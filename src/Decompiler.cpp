#include "Bytecode.hpp"
#include "ControlFlow.hpp"
#include "Exports.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_map>

namespace Taze
{
namespace
{
bool Identifier(std::string_view Text)
{
    static const std::set<std::string> Keywords = {"and", "break",    "do",     "else", "elseif", "end",   "false",
                                                   "for", "function", "if",     "in",   "local",  "nil",   "not",
                                                   "or",  "repeat",   "return", "then", "true",   "until", "while"};
    if (Text.empty() || Keywords.count(std::string(Text)))
        return false;
    if (!(std::isalpha(static_cast<unsigned char>(Text[0])) || Text[0] == '_'))
        return false;
    for (unsigned char C : Text)
        if (!(std::isalnum(C) || C == '_'))
            return false;
    return true;
}

std::string Pascal(std::string_view Text)
{
    std::string Out;
    bool Upper = true;
    for (unsigned char C : Text)
    {
        if (C >= 128 || !std::isalnum(C))
        {
            Upper = true;
            continue;
        }
        Out += Upper ? char(std::toupper(C)) : char(C);
        Upper = false;
    }
    if (Out.empty())
        Out = "Value";
    if (std::isdigit(static_cast<unsigned char>(Out[0])))
        Out = "Value" + Out;
    return Out;
}

std::string Token(unsigned Id)
{
    return '\x1f' + std::to_string(Id) + '\x1e';
}
std::vector<unsigned> Tokens(const std::string &Text)
{
    std::vector<unsigned> Out;
    for (std::size_t Pos = 0; (Pos = Text.find('\x1f', Pos)) != std::string::npos;)
    {
        auto End = Text.find('\x1e', Pos);
        if (End == std::string::npos)
            throw Error("Internal: unterminated source symbol");
        Out.push_back(unsigned(std::stoul(Text.substr(Pos + 1, End - Pos - 1))));
        Pos = End + 1;
    }
    return Out;
}
void Replace(std::string &Text, const std::string &From, const std::string &To)
{
    for (std::size_t Pos = 0; (Pos = Text.find(From, Pos)) != std::string::npos; Pos += To.size())
        Text.replace(Pos, From.size(), To);
}
std::string Join(const std::vector<std::string> &Items, const std::string &Separator = ", ")
{
    std::string Out;
    for (const auto &Item : Items)
    {
        if (!Out.empty())
            Out += Separator;
        Out += Item;
    }
    return Out;
}
std::size_t Closing(const std::string &Text, std::size_t Start)
{
    char Open = Text[Start], Close = Open == '(' ? ')' : ']';
    unsigned Depth = 0;
    char QuoteChar = 0;
    for (std::size_t I = Start; I < Text.size(); ++I)
    {
        char C = Text[I];
        if (QuoteChar)
        {
            if (C == '\\')
                ++I;
            else if (C == QuoteChar)
                QuoteChar = 0;
            continue;
        }
        if (C == '\'' || C == '"')
        {
            QuoteChar = C;
            continue;
        }
        if (C == Open)
            ++Depth;
        if (C == Close && --Depth == 0)
            return I;
    }
    return std::string::npos;
}

std::string Unwrap(std::string Text)
{
    while (Text.size() >= 2 && Text.front() == '(' && Closing(Text, 0) == Text.size() - 1)
        Text = Text.substr(1, Text.size() - 2);
    return Text;
}

bool PrefixExpression(const std::string &Text)
{
    std::size_t Pos = 0;
    auto Name = [&]()
    {
        auto Start = Pos;
        if (Pos < Text.size() && Text[Pos] == '\x1f')
        {
            auto End = Text.find('\x1e', Pos);
            if (End == std::string::npos)
                return false;
            Pos = End + 1;
            return true;
        }
        while (Pos < Text.size() && (std::isalnum(static_cast<unsigned char>(Text[Pos])) || Text[Pos] == '_'))
            ++Pos;
        return Identifier(Text.substr(Start, Pos - Start));
    };
    if (!Name())
        return false;
    while (Pos < Text.size())
    {
        if (Text[Pos] == '.' || Text[Pos] == ':')
        {
            ++Pos;
            if (!Name())
                return false;
        }
        else if (Text[Pos] == '(' || Text[Pos] == '[')
        {
            auto End = Closing(Text, Pos);
            if (End == std::string::npos)
                return false;
            Pos = End + 1;
        }
        else
            return false;
    }
    return true;
}

std::string CleanExpression(std::string Text)
{
    if (Text.find('\n') != std::string::npos)
        return Text;
    char QuoteChar = 0;
    for (std::size_t Pos = 0; Pos < Text.size(); ++Pos)
    {
        if (QuoteChar)
        {
            if (Text[Pos] == '\\')
                ++Pos;
            else if (Text[Pos] == QuoteChar)
                QuoteChar = 0;
            continue;
        }
        if (Text[Pos] == '\'' || Text[Pos] == '"')
        {
            QuoteChar = Text[Pos];
            continue;
        }
        if (Text[Pos] != '(')
            continue;
        auto End = Closing(Text, Pos);
        if (End == std::string::npos || End + 1 == Text.size())
            continue;
        if (std::string(".:[(").find(Text[End + 1]) == std::string::npos)
            continue;
        auto Inner = Text.substr(Pos + 1, End - Pos - 1);
        if (PrefixExpression(Inner))
        {
            Text.erase(End, 1);
            Text.erase(Pos, 1);
            if (Pos)
                --Pos;
        }
    }
    return Text;
}

std::string Negate(std::string Text)
{
    Text = Unwrap(Text);
    if (Text.starts_with("not ") && (PrefixExpression(Text.substr(4)) || (Text[4] == '(' && Closing(Text, 4) == Text.size() - 1)))
        return Unwrap(Text.substr(4));
    return "not " + (PrefixExpression(Text) ? Text : "(" + Text + ")");
}

std::string CleanCondition(std::string Text)
{
    Text = Unwrap(CleanExpression(Text));
    if (Text.starts_with("not "))
    {
        auto Operand = Text.substr(4);
        if (!PrefixExpression(Operand) && !(Operand.starts_with('(') && Closing(Operand, 0) == Operand.size() - 1) &&
            !Operand.starts_with("not "))
            return Text;
        auto Inner = Unwrap(Text.substr(4));
        if (Inner.starts_with("not "))
            return CleanCondition(Inner.substr(4));
        // Equality negation is safe for metamethods and NaN; ordering negation is not.
        unsigned Depth = 0;
        char QuoteChar = 0;
        for (std::size_t I = 0; I + 3 < Inner.size(); ++I)
        {
            char C = Inner[I];
            if (QuoteChar)
            {
                if (C == '\\')
                    ++I;
                else if (C == QuoteChar)
                    QuoteChar = 0;
                continue;
            }
            if (C == '\'' || C == '"')
            {
                QuoteChar = C;
                continue;
            }
            if (C == '(' || C == '[')
                ++Depth;
            else if (C == ')' || C == ']')
                --Depth;
            else if (!Depth && Inner.find(" and ") == std::string::npos && Inner.find(" or ") == std::string::npos &&
                     (Inner.substr(I, 4) == " == " || Inner.substr(I, 4) == " ~= "))
            {
                Inner.replace(I, 4, Inner.substr(I, 4) == " == " ? " ~= " : " == ");
                return Inner;
            }
        }
        return Negate(Inner);
    }
    return Text;
}

// Locate operators outside nested expressions and string literals, preserving precedence.
std::vector<std::string> SplitLogical(const std::string &Text, const std::string &Operator)
{
    unsigned Depth = 0;
    char QuoteChar = 0;
    std::size_t Start = 0;
    std::vector<std::string> Parts;
    for (std::size_t I = 0; I < Text.size(); ++I)
    {
        char C = Text[I];
        if (QuoteChar)
        {
            if (C == '\\')
                ++I;
            else if (C == QuoteChar)
                QuoteChar = 0;
            continue;
        }
        if (C == '\'' || C == '"')
        {
            QuoteChar = C;
            continue;
        }
        if (C == '(' || C == '[' || C == '{')
            ++Depth;
        else if (C == ')' || C == ']' || C == '}')
            --Depth;
        else if (!Depth && Text.compare(I, Operator.size(), Operator) == 0)
        {
            Parts.push_back(Text.substr(Start, I - Start));
            I += Operator.size() - 1;
            Start = I + 1;
        }
    }
    Parts.push_back(Text.substr(Start));
    return Parts;
}

std::string FormatCondition(std::string Text, unsigned ParentPrecedence = 0)
{
    Text = CleanCondition(Text);
    if (Text.starts_with("if ") || Text.find('\n') != std::string::npos)
        return Text;
    for (unsigned Precedence = 1; Precedence <= 2; ++Precedence)
    {
        std::string Operator = Precedence == 1 ? " or " : " and ";
        auto Parts = SplitLogical(Text, Operator);
        if (Parts.size() == 1)
            continue;
        for (auto &Part : Parts)
            Part = FormatCondition(Part, Precedence);
        Text = Join(Parts, Operator);
        return Precedence < ParentPrecedence ? "(" + Text + ")" : Text;
    }
    return Text;
}

enum class Kind
{
    Assign,
    Raw,
    Return,
    If,
    While,
    Repeat,
    For,
    Break,
    Continue,
    Function
};
struct Statement
{
    Kind Type = Kind::Raw;
    std::vector<unsigned> Left;
    std::string Text;
    std::vector<Statement> Body, Else;
    bool Pure = false, Call = false;
    Statement(Kind K = Kind::Raw, std::vector<unsigned> L = {}, std::string T = {}, std::vector<Statement> B = {},
              std::vector<Statement> Other = {}, bool P = false, bool C = false)
        : Type(K), Left(std::move(L)), Text(std::move(T)), Body(std::move(B)), Else(std::move(Other)), Pure(P), Call(C)
    {
    }
};
struct Symbol
{
    std::string Name;
    std::string Base;
    bool Keep = false, Parameter = false, Cell = false;
    unsigned Owner = 0;
    bool ReferenceAlias = false;
    std::string Category = {};
};
struct Upvalue
{
    std::string Value;
    std::string Cell;
    std::string Name;
    bool Reference = false;
};
struct Unstructured : Error
{
    using Error::Error;
};

class Engine;
class Function
{
    Engine &E;
    const Prototype &P;
    ControlFlow Flow;
    unsigned Id, Depth;
    unsigned ExportFrame = std::numeric_limits<unsigned>::max();
    std::vector<Upvalue> Upvalues;
    std::vector<unsigned> Registers;
    std::vector<unsigned> LocalSymbols;
    std::vector<unsigned> LocalStarts;
    std::vector<unsigned> Parameters;
    std::vector<std::vector<unsigned>> ReadSymbols, WriteSymbols;
    std::set<unsigned> Cells;
    std::vector<Statement> Prefix;
    unsigned PackSymbol, TopSymbol, VarargsSymbol;
    bool NeedsPack = false, NeedsVarargs = false;
    bool Dispatch = false;
    std::string PendingMethod, PendingReceiver;
    unsigned PendingBase = 0;
    std::string OpenExpression;
    unsigned OpenExpressionBase = 0;
    std::string OpenHint;
    std::map<unsigned, std::string> Hints;
    std::vector<unsigned> RegionVisits;
    std::map<const std::vector<Statement> *, std::set<unsigned>> ScopedDeclarations;
    void BuildDataflow();

    unsigned SymbolAt(unsigned R, unsigned Pc, bool Write = false) const;
    std::string Read(unsigned R, unsigned Pc) const;
    std::string WriteName(unsigned R, unsigned Pc) const;
    std::string ConstantAt(unsigned Index, unsigned Recursion = 0) const;
    std::string Locator() const;
    std::string Member(const std::string &Object, unsigned Index) const;
    std::string Global(unsigned Index) const;
    Statement Assign(unsigned R, const Instruction &I, std::string Value, bool Pure = false, bool Call = false);
    void Simple(const Instruction &I, std::vector<Statement> &Out);
    std::string Condition(const Instruction &I) const;
    std::vector<Statement> Region(unsigned Start, unsigned End, int BreakTarget = -1, int ContinueTarget = -1, unsigned Level = 0,
                                  bool IgnoreBackedge = false);
    std::vector<Statement> StateMachine();
    std::string Closure(const Instruction &I, std::vector<Statement> &Out);
    unsigned Index(unsigned Pc) const;
    void Optimize(std::vector<Statement> &Nodes);
    std::string Render(const std::vector<Statement> &Nodes, unsigned Indent, std::set<unsigned> &Declared, const std::set<unsigned> &Hoist);

  public:
    Function(Engine &Owner, unsigned Proto, std::vector<Upvalue> Captures, unsigned Nesting);
    std::string Generate(bool Main = false);
    std::string Signature() const;
};

class Engine
{
  public:
    const Chunk &C;
    const Options &Settings;
    Result Output;
    std::vector<Symbol> Symbols;
    std::set<std::string> Names;
    std::set<std::string> GlobalNames;
    std::map<unsigned, std::set<unsigned>> Parents;
    Exports ExportGraph;
    unsigned MainExportFrame = std::numeric_limits<unsigned>::max();
    std::map<unsigned, std::string> Locators;
    std::size_t Work = 0;
    unsigned NextValue = 0;

    Engine(const Chunk &Bytecode, const Options &Options) : C(Bytecode), Settings(Options)
    {
        // A generated local must never shadow an original global, even when PascalCase changes a debug name.
        for (const auto &P : C.Prototypes)
            for (const auto &I : P.Instructions)
            {
                if (I.Op == LOP_GETGLOBAL || I.Op == LOP_SETGLOBAL)
                    Names.insert(P.Constants[I.Aux].Text);
                if (I.Op == LOP_GETIMPORT)
                    Names.insert(P.Constants[(I.Aux >> 20) & 1023].Text);
            }
        for (const char *Name : {"game", "workspace", "script", "math", "string", "table", "coroutine", "vector", "integer", "getfenv",
                                 "setmetatable", "next", "type", "select", "error", "tonumber"})
            Names.insert(Name);
        GlobalNames = Names;
    }
    unsigned New(std::string Name, unsigned Owner, bool Keep = false, bool Parameter = false, bool Cell = false, bool Preserve = false)
    {
        if (Name.starts_with("Value") && Name.size() > 5 &&
            std::all_of(Name.begin() + 5, Name.end(), [](unsigned char C) { return std::isdigit(C); }))
            Name = "Value" + std::to_string(++NextValue);
        Name = Preserve && Identifier(Name) ? Name : Pascal(Name);
        std::string Base = Name;
        unsigned Suffix = 2;
        while (Names.count(Name))
            Name = Base + std::to_string(Suffix++);
        Names.insert(Name);
        unsigned Id = unsigned(Symbols.size());
        Symbols.push_back({Name, Base, Keep, Parameter, Cell, Owner});
        return Id;
    }
    std::string Resolve(std::string Text)
    {
        auto Paths = Settings.ModulePath.empty() ? std::map<unsigned, std::string>{} : ExportGraph.Find(MainExportFrame);
        for (const auto &[Proto, Fallback] : Locators)
        {
            auto It = Paths.find(Proto);
            auto Lookup = It == Paths.end() ? Fallback : "require(" + Settings.ModulePath + ")" + It->second;
            Replace(Text, "\x1d" + std::to_string(Proto) + "\x1c", Lookup);
        }
        auto References = Tokens(Text);
        std::map<unsigned, std::vector<unsigned>> ByOwner;
        std::set<unsigned> Seen;
        for (auto Id : References)
            if (Seen.insert(Id).second)
                ByOwner[Symbols[Id].Owner].push_back(Id);
        std::map<unsigned, std::set<std::string>> Used;
        std::set<unsigned> Ready;
        std::function<void(unsigned)> Allocate = [&](unsigned Owner)
        {
            if (!Ready.insert(Owner).second)
                return;
            auto &Taken = Used[Owner];
            Taken = GlobalNames;
            for (auto Parent : Parents[Owner])
            {
                Allocate(Parent);
                Taken.insert(Used[Parent].begin(), Used[Parent].end());
            }
            std::map<std::string, unsigned> Counters;
            for (auto Id : ByOwner[Owner])
            {
                auto &S = Symbols[Id];
                std::string Base = S.Base;
                bool Generic = Base.starts_with("Value") && Base.size() > 5 &&
                               std::all_of(Base.begin() + 5, Base.end(), [](unsigned char C) { return std::isdigit(C); });
                if (Generic)
                {
                    const auto Prefix = S.Category.empty() ? "Value" : S.Category;
                    do
                    {
                        S.Name = Prefix + std::to_string(++Counters[Prefix]);
                    } while (Taken.count(S.Name));
                }
                else
                {
                    S.Name = Base;
                    unsigned Suffix = 2;
                    while (Taken.count(S.Name))
                        S.Name = Base + std::to_string(Suffix++);
                }
                Taken.insert(S.Name);
            }
        };
        for (auto &[Owner, Ids] : ByOwner)
            Allocate(Owner);
        for (auto Id : Tokens(Text))
            Replace(Text, Token(Id), Symbols.at(Id).Name);
        return Text;
    }
    void Budget(std::size_t Amount)
    {
        Work += Amount;
        if (Work > Settings.MaxOutputBytes * 8)
            throw Error("Source reconstruction exceeds output/work limit");
    }
};

Function::Function(Engine &Owner, unsigned Proto, std::vector<Upvalue> Captures, unsigned Nesting)
    : E(Owner), P(E.C.Prototypes.at(Proto)), Flow(P), Id(Proto), Depth(Nesting), Upvalues(std::move(Captures))
{
    if (Depth > 128)
        throw Error("Function nesting exceeds 128 levels");
    for (const auto &I : P.Instructions)
        for (auto [Kind, R] : I.Captures)
            if (Kind == LCT_REF)
                Cells.insert(R);
    for (unsigned R = 0; R < P.Stack; ++R)
    {
        std::string Name = R < P.Parameters ? "Argument" + std::to_string(R + 1) : "Value" + std::to_string(R + 1);
        for (const auto &L : P.Locals)
            if (L.Register == R && ((R < P.Parameters && L.Start <= 1) || Cells.count(R)))
            {
                Name = L.Name;
                break;
            }
        Registers.push_back(E.New(Name, Id, R < P.Parameters, R < P.Parameters, Cells.count(R) != 0));
        if (R < P.Parameters)
            Parameters.push_back(Registers.back());
    }
    for (const auto &L : P.Locals)
    {
        if (L.Start <= 1 && L.Register < P.Parameters)
        {
            LocalSymbols.push_back(Registers[L.Register]);
            LocalStarts.push_back(0);
            continue;
        }
        unsigned Start = L.Start;
        // Debug ranges start after initialization. Include the last defining instruction in this local's lifetime.
        for (auto It = P.Instructions.rbegin(); It != P.Instructions.rend(); ++It)
        {
            if (It->Pc >= L.Start)
                continue;
            auto Written = Writes(*It, P);
            if (std::find(Written.begin(), Written.end(), L.Register) != Written.end())
            {
                Start = It->Pc;
                break;
            }
        }
        for (const auto &I : P.Instructions)
            if ((I.Op == LOP_FORGPREP || I.Op == LOP_FORGPREP_NEXT || I.Op == LOP_FORGPREP_INEXT) && I.Next == L.Start &&
                L.Register >= I.A + 3)
                Start = L.Start;
        LocalStarts.push_back(Start);
        LocalSymbols.push_back(Cells.count(L.Register) ? Registers[L.Register] : E.New(L.Name, Id, true));
    }
    PackSymbol = E.New("Results", Id, true);
    TopSymbol = E.New("ResultBase", Id, true);
    VarargsSymbol = E.New("Arguments", Id, true);
    while (Upvalues.size() < P.Upvalues)
        throw Error("Main prototype has unbound upvalues");
    BuildDataflow();
    if (!E.Settings.ModulePath.empty())
    {
        std::vector<unsigned> Captures;
        for (const auto &U : Upvalues)
        {
            auto Refs = Tokens(U.Value);
            Captures.push_back(!U.Reference && Refs.size() == 1 ? Refs.front() : std::numeric_limits<unsigned>::max());
        }
        ExportFrame = E.ExportGraph.Record(P, Id, ReadSymbols, WriteSymbols, Captures, Parameters);
        if (Depth == 0)
            E.MainExportFrame = ExportFrame;
    }
}

void Function::BuildDataflow()
{
    const unsigned Count = unsigned(P.Instructions.size());
    if (std::uint64_t(Count) * P.Stack > 16000000)
        throw Error("Function dataflow exceeds analysis limit");
    std::vector<unsigned> Parent;
    auto New = [&]()
    {
        auto Id = unsigned(Parent.size());
        Parent.push_back(Id);
        return Id;
    };
    std::function<unsigned(unsigned)> Find = [&](unsigned Id) -> unsigned
    {
        unsigned Root = Id;
        while (Parent[Root] != Root)
            Root = Parent[Root];
        while (Parent[Id] != Id)
        {
            auto Next = Parent[Id];
            Parent[Id] = Root;
            Id = Next;
        }
        return Root;
    };
    auto Union = [&](unsigned A, unsigned B)
    {
        A = Find(A);
        B = Find(B);
        if (A != B)
            Parent[std::max(A, B)] = std::min(A, B);
    };
    std::vector<unsigned> Initial;
    for (unsigned R = 0; R < P.Stack; ++R)
        Initial.push_back(New());
    std::vector<std::vector<std::pair<unsigned, unsigned>>> Definitions(Count);
    for (unsigned J = 0; J < Count; ++J)
        for (unsigned R : Writes(P.Instructions[J], P))
        {
            auto D = New();
            Definitions[J].emplace_back(R, D);
            if (Cells.count(R))
                Union(D, Initial[R]);
        }
    ReadSymbols.resize(Count);
    WriteSymbols.resize(Count);
    std::vector<bool> Visited(Count, false);
    ReadSymbols[0] = Initial;
    Visited[0] = true;
    std::queue<unsigned> Queue;
    Queue.push(0);
    while (!Queue.empty())
    {
        unsigned J = Queue.front();
        Queue.pop();
        auto State = ReadSymbols[J];
        for (auto [R, D] : Definitions[J])
            State[R] = D;
        WriteSymbols[J] = State;
        const auto &I = P.Instructions[J];
        std::vector<unsigned> Successors;
        if (I.Target() >= 0)
            Successors.push_back(Index(unsigned(I.Target())));
        bool Unconditional = I.Op == LOP_JUMP || I.Op == LOP_JUMPBACK || I.Op == LOP_JUMPX || I.Op == LOP_RETURN || I.Op == LOP_FORGPREP ||
                             I.Op == LOP_FORGPREP_NEXT || I.Op == LOP_FORGPREP_INEXT || (I.Op == LOP_LOADB && I.C);
        if (!Unconditional && J + 1 < Count)
            Successors.push_back(J + 1);
        for (unsigned Next : Successors)
        {
            if (Next >= Count)
                continue;
            if (!Visited[Next])
            {
                ReadSymbols[Next] = State;
                Visited[Next] = true;
                Queue.push(Next);
            }
            else
                for (unsigned R = 0; R < P.Stack; ++R)
                    if (Flow.LiveIn[Next].test(R))
                        Union(ReadSymbols[Next][R], State[R]);
        }
    }
    std::map<unsigned, unsigned> RootSymbol;
    for (unsigned R = 0; R < P.Stack; ++R)
        if (R < P.Parameters || Cells.count(R))
            RootSymbol[Find(Initial[R])] = Registers[R];
    // Recover debug names from the values alive in their ranges, rather than treating a register as one permanent variable.
    for (unsigned J = 0; J < P.Locals.size(); ++J)
    {
        const auto &L = P.Locals[J];
        auto It = P.PcToInstruction.lower_bound(L.Start);
        if (It == P.PcToInstruction.end() || ReadSymbols[It->second].empty())
            continue;
        auto Root = Find(ReadSymbols[It->second][L.Register]);
        if (!RootSymbol.count(Root))
            RootSymbol[Root] = LocalSymbols[J];
    }
    auto Resolve = [&](unsigned Definition, unsigned R)
    {
        unsigned Root = Find(Definition);
        auto It = RootSymbol.find(Root);
        if (It != RootSymbol.end())
            return It->second;
        unsigned S = E.New("Value" + std::to_string(R + 1), Id);
        RootSymbol.emplace(Root, S);
        return S;
    };
    for (unsigned J = 0; J < Count; ++J)
    {
        if (!Visited[J])
        {
            ReadSymbols[J] = Initial;
            WriteSymbols[J] = Initial;
        }
        for (unsigned R = 0; R < P.Stack; ++R)
        {
            ReadSymbols[J][R] = Resolve(ReadSymbols[J][R], R);
            WriteSymbols[J][R] = Resolve(WriteSymbols[J][R], R);
        }
    }

    // Union possible types over all definitions, including loop/branch joins. Only
    // specialize anonymous names when every reaching definition agrees.
    std::map<unsigned, unsigned> Types;
    std::map<unsigned, std::vector<unsigned>> Copies;
    std::queue<unsigned> Pending;
    auto Add = [&](unsigned S, unsigned Mask)
    {
        if ((Types[S] | Mask) != Types[S])
        {
            Types[S] |= Mask;
            Pending.push(S);
        }
    };
    auto ConstantType = [&](unsigned K)
    {
        switch (P.Constants[K].Tag)
        {
        case LBC_CONSTANT_TABLE:
        case LBC_CONSTANT_TABLE_WITH_CONSTANTS:
            return 2u;
        case LBC_CONSTANT_CLOSURE:
            return 4u;
        case LBC_CONSTANT_STRING:
            return 8u;
        case LBC_CONSTANT_NUMBER:
            return 16u;
        case LBC_CONSTANT_BOOLEAN:
            return 32u;
        case LBC_CONSTANT_VECTOR:
            return 64u;
        case LBC_CONSTANT_INTEGER:
            return 128u;
        default:
            return 1u;
        }
    };
    for (unsigned R = 0; R < P.Stack; ++R)
        Add(Resolve(Initial[R], R), 1);
    for (unsigned J = 0; J < Count; ++J)
    {
        if (!Visited[J])
            continue;
        const auto &I = P.Instructions[J];
        for (auto R : Writes(I, P))
        {
            auto S = WriteSymbols[J][R];
            if (I.Op == LOP_MOVE)
            {
                Copies[ReadSymbols[J][I.B]].push_back(S);
                continue;
            }
            unsigned Mask = 1;
            if (R == I.A)
                switch (I.Op)
                {
                case LOP_NEWTABLE:
                    Mask = 2;
                    break;
                case LOP_NEWCLOSURE:
                case LOP_DUPCLOSURE:
                    Mask = 4;
                    break;
                case LOP_LOADK:
                case LOP_DUPTABLE:
                    Mask = ConstantType(unsigned(I.D));
                    break;
                case LOP_LOADKX:
                    Mask = ConstantType(I.Aux);
                    break;
                case LOP_LOADN:
                    Mask = 16;
                    break;
                case LOP_LOADB:
                case LOP_NOT:
                    Mask = 32;
                    break;
                default:
                    break;
                }
            Add(S, Mask);
        }
    }
    while (!Pending.empty())
    {
        auto S = Pending.front();
        Pending.pop();
        for (auto Destination : Copies[S])
            Add(Destination, Types[S]);
    }
    const std::map<unsigned, std::string> Categories = {{2, "Table"},    {4, "Function"}, {8, "String"},   {16, "Number"},
                                                        {32, "Boolean"}, {64, "Vector"},  {128, "Integer"}};
    for (const auto &[S, Mask] : Types)
        if (auto It = Categories.find(Mask); It != Categories.end())
            E.Symbols[S].Category = It->second;
    // Reference captures are represented by an actual one-element table in
    // generated source, regardless of the type of the captured value.
    for (auto &S : E.Symbols)
        if (S.Owner == Id && S.Cell)
            S.Category = "Table";
}

unsigned Function::SymbolAt(unsigned R, unsigned Pc, bool Write) const
{
    if (R >= P.Stack)
        throw Error("Invalid register during reconstruction");
    if (Cells.count(R) || Dispatch)
        return Registers[R];
    if (!ReadSymbols.empty())
        return (Write ? WriteSymbols : ReadSymbols).at(Index(Pc)).at(R);
    for (unsigned J = unsigned(P.Locals.size()); J-- > 0;)
        if (P.Locals[J].Register == R && Pc >= LocalStarts[J] && Pc < P.Locals[J].End &&
            (Write || Pc != LocalStarts[J] || P.Locals[J].Start == LocalStarts[J] || E.Symbols[LocalSymbols[J]].Parameter))
            return LocalSymbols[J];
    return Registers[R];
}
std::string Function::Read(unsigned R, unsigned Pc) const
{
    unsigned S = SymbolAt(R, Pc);
    return Token(S) + (E.Symbols[S].Cell ? "[1]" : "");
}
std::string Function::WriteName(unsigned R, unsigned Pc) const
{
    return Read(R, Pc);
}
unsigned Function::Index(unsigned Pc) const
{
    if (Pc == P.Code.size())
        return unsigned(P.Instructions.size());
    auto It = P.PcToInstruction.find(Pc);
    if (It == P.PcToInstruction.end())
        throw Unstructured("invalid region boundary");
    return It->second;
}

std::string Function::ConstantAt(unsigned Index, unsigned Recursion) const
{
    if (Recursion > 32)
        throw Error("Constant nesting too deep");
    const auto &K = P.Constants.at(Index);
    switch (K.Tag)
    {
    case LBC_CONSTANT_NIL:
        return "nil";
    case LBC_CONSTANT_BOOLEAN:
        return K.Index ? "true" : "false";
    case LBC_CONSTANT_NUMBER:
        return Number(K.Number);
    case LBC_CONSTANT_STRING:
        return Quote(K.Text);
    case LBC_CONSTANT_VECTOR:
    case LBC_CONSTANT_VECTORD:
        if (K.Vector[3] != 0)
            throw Error("Four-component vector constants require a four-component target runtime");
        return "vector.create(" + Number(K.Vector[0]) + ", " + Number(K.Vector[1]) + ", " + Number(K.Vector[2]) + ")";
    case LBC_CONSTANT_INTEGER:
        return "integer.fromstring(" + Quote((K.Negative ? "-" : "") + std::to_string(K.Integer)) + ")";
    case LBC_CONSTANT_TABLE:
    case LBC_CONSTANT_TABLE_WITH_CONSTANTS:
    {
        std::vector<std::string> Entries;
        for (auto [Key, Value] : K.Entries)
        {
            auto Name = P.Constants.at(Key);
            std::string Field =
                Name.Tag == LBC_CONSTANT_STRING && Identifier(Name.Text) ? Name.Text : "[" + ConstantAt(Key, Recursion + 1) + "]";
            Entries.push_back(Field + " = " + (Value < 0 ? "0" : ConstantAt(unsigned(Value), Recursion + 1)));
        }
        return Entries.empty() ? "{}" : "{ " + Join(Entries) + " }";
    }
    case LBC_CONSTANT_IMPORT:
    {
        std::string Path;
        for (unsigned J = 0; J < (K.Index >> 30); ++J)
        {
            unsigned Part = (K.Index >> (20 - 10 * J)) & 1023;
            Path = J ? Member(Path, Part) : Global(Part);
        }
        return Path;
    }
    case LBC_CONSTANT_CLASS_SHAPE:
        throw Error("Experimental Luau class constants cannot be represented in the stable Roblox language");
    default:
        throw Error("Constant is not a source literal");
    }
}
std::string Function::Locator() const
{
    std::vector<std::string> Constants;
    bool HasLineAndName = P.Line && !P.Name.empty();
    for (unsigned Index = 0; !HasLineAndName && Index < P.Constants.size() && Constants.size() < 5; ++Index)
    {
        const auto &K = P.Constants[Index];
        switch (K.Tag)
        {
        case LBC_CONSTANT_NUMBER:
            // NaN cannot be matched by equality. Nil is not an element of a Luau array.
            if (!std::isnan(K.Number))
                Constants.push_back(ConstantAt(Index));
            break;
        case LBC_CONSTANT_BOOLEAN:
        case LBC_CONSTANT_STRING:
        case LBC_CONSTANT_IMPORT:
        case LBC_CONSTANT_INTEGER:
        case LBC_CONSTANT_VECTOR:
        case LBC_CONSTANT_VECTORD:
            Constants.push_back(ConstantAt(Index));
            break;
        default:
            // Serialized table templates and closure prototypes do not identify live objects.
            break;
        }
    }
    auto Text = "filtergc(\"function\", { " + E.Settings.FilterLineField + " = " + (P.Line ? std::to_string(P.Line) : "nil") +
                (P.Name.empty() ? "" : ", Name = " + Quote(P.Name)) +
                (HasLineAndName ? "" : ", Constants = {" + (Constants.empty() ? "" : " " + Join(Constants) + " ") + "}") + " }, true)";
    E.Locators[Id] = Text;
    return "\x1d" + std::to_string(Id) + "\x1c";
}

std::string Function::Member(const std::string &Object, unsigned Index) const
{
    const auto &K = P.Constants.at(Index);
    return Object + (K.Tag == LBC_CONSTANT_STRING && Identifier(K.Text) ? "." + K.Text : "[" + ConstantAt(Index) + "]");
}
std::string Function::Global(unsigned Index) const
{
    const auto &Name = P.Constants.at(Index).Text;
    return Identifier(Name) ? Name : "getfenv()[" + Quote(Name) + "]";
}
Statement Function::Assign(unsigned R, const Instruction &I, std::string Value, bool Pure, bool Call)
{
    unsigned S = SymbolAt(R, I.Pc, true);
    if (E.Symbols[S].Cell)
        return {Kind::Raw, {}, Token(S) + "[1] = " + Value};
    return {Kind::Assign, {S}, std::move(Value), {}, {}, Pure, Call};
}

std::string Function::Signature() const
{
    std::vector<std::string> Args;
    for (auto S : Parameters)
        Args.push_back(Token(S));
    if (P.Vararg)
        Args.push_back("...");
    return "(" + Join(Args) + ")";
}

std::string Function::Closure(const Instruction &I, std::vector<Statement> &Out)
{
    unsigned Child = I.Op == LOP_NEWCLOSURE ? P.Children.at(unsigned(I.D)) : P.Constants.at(unsigned(I.D)).Index;
    E.Parents[Child].insert(Id);
    std::vector<Upvalue> Captures;
    std::string Self;
    unsigned SelfSymbol = std::numeric_limits<unsigned>::max();
    for (unsigned J = 0; J < I.Captures.size(); ++J)
    {
        auto [Kind, R] = I.Captures[J];
        Upvalue U;
        std::string Argument;
        if (Kind == LCT_UPVAL)
        {
            U = Upvalues.at(R);
            Argument = U.Reference ? U.Cell : U.Value;
        }
        else
        {
            unsigned S = SymbolAt(R, I.Pc, R == I.A);
            U.Name = E.Symbols[S].Name;
            U.Reference = Kind == LCT_REF;
            Argument = U.Reference ? Token(Registers[R]) : Read(R, I.Pc);
        }
        if (Kind == LCT_VAL && R == I.A)
        {
            if (Self.empty())
            {
                auto Destination = SymbolAt(I.A, I.Pc, true);
                unsigned Definitions = E.Symbols[Destination].Parameter ? 1u : 0u;
                for (const auto &Code : P.Instructions)
                    for (auto Reg : Writes(Code, P))
                        if (SymbolAt(Reg, Code.Pc, true) == Destination)
                            ++Definitions;
                SelfSymbol = !Dispatch && !E.Symbols[Destination].Cell && Definitions == 1
                                 ? Destination
                                 : E.New(E.C.Prototypes[Child].Name.empty() ? "RecursiveFunction" : E.C.Prototypes[Child].Name, Id, true,
                                         false, false, true);
                Self = Token(SelfSymbol);
            }
            U.Value = Self;
            U.Cell.clear();
            Captures.push_back(std::move(U));
            continue;
        }
        if (Kind == LCT_VAL && !Cells.count(R))
        {
            unsigned S = SymbolAt(R, I.Pc);
            unsigned Definitions = E.Symbols[S].Parameter ? 1u : 0u;
            for (const auto &Code : P.Instructions)
                for (unsigned Reg : Writes(Code, P))
                    if (SymbolAt(Reg, Code.Pc, true) == S)
                        Definitions++;
            bool Loop = false;
            for (const auto &Code : P.Instructions)
                if (Code.Target() >= 0 && Code.Target() <= int(I.Pc) && Code.Pc >= I.Pc && Code.Target() < int(Code.Pc))
                    Loop = true;
            if (Definitions == 1 && !Loop)
            {
                U.Value = Read(R, I.Pc);
                U.Cell.clear();
                E.Symbols[S].Keep = true;
                Captures.push_back(std::move(U));
                continue;
            }
        }
        // Snapshot copy captures and reference-cell identities at closure creation, including per-iteration captures.
        unsigned Capture = E.New(U.Name + "Capture", Id, true);
        E.Symbols[Capture].ReferenceAlias = U.Reference;
        Out.push_back({Kind::Assign, {Capture}, Argument});
        auto References = Tokens(Argument);
        if (!U.Reference && References.size() == 1 && Argument == Token(References.front()))
            E.ExportGraph.Alias(Capture, References.front());
        U.Cell = U.Reference ? Token(Capture) : "";
        U.Value = Token(Capture) + (U.Reference ? "[1]" : "");
        Captures.push_back(std::move(U));
    }
    Function Nested(E, Child, std::move(Captures), Depth + 1);
    if (!E.Settings.ModulePath.empty())
        E.ExportGraph.Closure(SymbolAt(I.A, I.Pc, true), Nested.ExportFrame, ExportFrame, I.Pc);
    auto Next = Index(I.Next);
    if (!Nested.Parameters.empty() && E.C.Prototypes[Child].Locals.empty() && Next < P.Instructions.size())
    {
        const auto &Install = P.Instructions[Next];
        if (Install.Op == LOP_SETTABLEKS && Install.A == I.A &&
            std::any_of(Nested.P.Instructions.begin(), Nested.P.Instructions.end(), [](const auto &Code)
                        { return Code.B == 0 && (Code.Op == LOP_NAMECALL || Code.Op == LOP_SETTABLEKS || Code.Op == LOP_GETTABLEKS); }))
            E.Symbols[Nested.Parameters.front()].Base = "Self";
    }
    std::string Body = Nested.Generate();
    std::string Text = "function" + Nested.Signature();
    if (E.Settings.IncludeLineComments)
    {
        Text += " -- Line: " + (E.C.Prototypes[Child].Line ? std::to_string(E.C.Prototypes[Child].Line) : "Unknown");
        if (E.Settings.IncludeFunctionLocators)
            Text += " | " + Nested.Locator();
    }
    Text += "\n" + Body + "end";
    if (!Self.empty() && SelfSymbol != SymbolAt(I.A, I.Pc, true))
    {
        Out.push_back({Kind::Assign, {SelfSymbol}, Text});
        return Self;
    }
    return Text;
}

std::string Function::Condition(const Instruction &I) const
{
    std::string Left = Read(I.A, I.Pc), Right, Op;
    bool Invert = false;
    switch (I.Op)
    {
    case LOP_JUMPIF:
        return Left;
    case LOP_JUMPIFNOT:
        return "not " + Left;
    case LOP_JUMPIFEQ:
        Op = "==";
        Right = Read(I.Aux, I.Pc);
        break;
    case LOP_JUMPIFLE:
        Op = "<=";
        Right = Read(I.Aux, I.Pc);
        break;
    case LOP_JUMPIFLT:
        Op = "<";
        Right = Read(I.Aux, I.Pc);
        break;
    case LOP_JUMPIFNOTEQ:
        Op = "==";
        Right = Read(I.Aux, I.Pc);
        Invert = true;
        break;
    case LOP_JUMPIFNOTLE:
        Op = "<=";
        Right = Read(I.Aux, I.Pc);
        Invert = true;
        break;
    case LOP_JUMPIFNOTLT:
        Op = "<";
        Right = Read(I.Aux, I.Pc);
        Invert = true;
        break;
    case LOP_JUMPXEQKNIL:
        Right = "nil";
        Op = "==";
        Invert = (I.Aux >> 31) != 0;
        break;
    case LOP_JUMPXEQKB:
        Right = (I.Aux & 1) ? "true" : "false";
        Op = "==";
        Invert = (I.Aux >> 31) != 0;
        break;
    case LOP_JUMPXEQKN:
    case LOP_JUMPXEQKS:
        Right = ConstantAt(I.Aux & 0xffffff);
        Op = "==";
        Invert = (I.Aux >> 31) != 0;
        break;
    default:
        throw Unstructured("unsupported conditional branch");
    }
    auto Expression = Left + " " + Op + " " + Right;
    return Invert ? "not (" + Expression + ")" : Expression;
}

void Function::Simple(const Instruction &I, std::vector<Statement> &Out)
{
    auto R = [&](unsigned Reg) { return Read(Reg, I.Pc); };
    auto Emit = [&](std::string Text) { Out.push_back({Kind::Raw, {}, std::move(Text)}); };
    auto Set = [&](std::string Text, bool Pure = false, bool Call = false) { Out.push_back(Assign(I.A, I, std::move(Text), Pure, Call)); };
    auto Hint = [&](std::string Name, bool Keep = false)
    {
        unsigned S = SymbolAt(I.A, I.Pc, true);
        Hints[S] = Name;
        if (!Name.empty() && !E.Symbols[S].Keep)
        {
            auto Nice = E.New(Name, Id, Keep);
            E.Symbols[S].Name = E.Symbols[Nice].Name;
            E.Symbols[S].Base = E.Symbols[Nice].Base;
            E.Symbols[S].Keep = Keep;
        }
    };
    auto Many = [&](unsigned Base, unsigned Count, const std::string &Text)
    {
        std::vector<unsigned> Lhs;
        bool Box = false;
        for (unsigned J = 0; J < Count; ++J)
        {
            auto S = SymbolAt(Base + J, I.Pc, true);
            Lhs.push_back(S);
            Box |= E.Symbols[S].Cell;
        }
        if (!Count)
            Emit(Text);
        else if (Box)
        {
            std::vector<std::string> Names;
            for (auto S : Lhs)
                Names.push_back(Token(S) + (E.Symbols[S].Cell ? "[1]" : ""));
            Emit(Join(Names) + " = " + Text);
        }
        else
            Out.push_back({Kind::Assign, std::move(Lhs), Text, {}, {}, false, true});
    };
    auto CanInlineOpen = [&]()
    {
        auto Next = Index(I.Next);
        if (Dispatch || Next >= P.Instructions.size())
            return false;
        const auto &Consumer = P.Instructions[Next];
        return ((Consumer.Op == LOP_CALL || Consumer.Op == LOP_CALLFB) && !Consumer.B && Consumer.A < I.A) ||
               (Consumer.Op == LOP_RETURN && !Consumer.B && Consumer.A <= I.A);
    };
    switch (I.Op)
    {
    case LOP_NOP:
    case LOP_BREAK:
    case LOP_COVERAGE:
    case LOP_PREPVARARGS:
    case LOP_FASTCALL:
    case LOP_FASTCALL1:
    case LOP_FASTCALL2:
    case LOP_FASTCALL2K:
    case LOP_FASTCALL3:
    case LOP_FASTPCALL:
        break; // Execute the compiler's ordinary fallback path for optimization hints.
    case LOP_NATIVECALL:
        throw Error("NATIVECALL is a runtime patch; supply original serialized bytecode");
    case LOP_LOADNIL:
        Set("nil", true);
        break;
    case LOP_LOADB:
        Set(I.B ? "true" : "false", true);
        break;
    case LOP_LOADN:
        Set(std::to_string(I.D), true);
        break;
    case LOP_LOADK:
        if (P.Constants.at(unsigned(I.D)).Tag == LBC_CONSTANT_STRING)
            Hints[SymbolAt(I.A, I.Pc, true)] = P.Constants[unsigned(I.D)].Text;
        Set(ConstantAt(unsigned(I.D)), true);
        break;
    case LOP_LOADKX:
        Set(ConstantAt(I.Aux), true);
        break;
    case LOP_MOVE:
        Hints[SymbolAt(I.A, I.Pc, true)] = Hints[SymbolAt(I.B, I.Pc)];
        Set(R(I.B), true);
        break;
    case LOP_GETGLOBAL:
        Hints[SymbolAt(I.A, I.Pc, true)] = P.Constants[I.Aux].Text;
        Set(Global(I.Aux));
        break;
    case LOP_SETGLOBAL:
        Emit(Global(I.Aux) + " = " + R(I.A));
        break;
    case LOP_GETUPVAL:
        Set(Upvalues.at(I.B).Value, true);
        break;
    case LOP_SETUPVAL:
        if (!Upvalues.at(I.B).Reference)
            throw Error("SETUPVAL targets a copy capture");
        Emit(Upvalues[I.B].Value + " = " + R(I.A));
        break;
    case LOP_CLOSEUPVALS:
        for (unsigned Reg : Cells)
            if (Reg >= I.A)
                Emit(Token(Registers[Reg]) + " = { " + Token(Registers[Reg]) + "[1] }");
        break;
    case LOP_GETIMPORT:
    {
        std::string Path;
        for (unsigned J = 0; J < (I.Aux >> 30); ++J)
        {
            unsigned Part = (I.Aux >> (20 - 10 * J)) & 1023;
            Path = J ? Member(Path, Part) : Global(Part);
        }
        Hints[SymbolAt(I.A, I.Pc, true)] = Path;
        Set(Path);
        break;
    }
    case LOP_GETTABLE:
        Set(R(I.B) + "[" + R(I.C) + "]");
        break;
    case LOP_SETTABLE:
        Emit(R(I.B) + "[" + R(I.C) + "] = " + R(I.A));
        break;
    case LOP_GETTABLEKS:
    case LOP_GETUDATAKS:
    {
        unsigned Key = I.Op == LOP_GETUDATAKS ? I.Aux & 65535 : I.Aux;
        auto Name = P.Constants[Key].Text;
        Hint(Name == "new" ? Hints[SymbolAt(I.B, I.Pc)] + ".new" : Name);
        Set(Member(R(I.B), Key));
        break;
    }
    case LOP_SETTABLEKS:
    case LOP_SETUDATAKS:
        Emit(Member(R(I.B), I.Op == LOP_SETUDATAKS ? I.Aux & 65535 : I.Aux) + " = " + R(I.A));
        break;
    case LOP_GETTABLEN:
        Set(R(I.B) + "[" + std::to_string(I.C + 1) + "]");
        break;
    case LOP_SETTABLEN:
        Emit(R(I.B) + "[" + std::to_string(I.C + 1) + "] = " + R(I.A));
        break;
    case LOP_NEWCLOSURE:
    case LOP_DUPCLOSURE:
    {
        unsigned S = SymbolAt(I.A, I.Pc, true);
        E.Symbols[S].Keep = true;
        unsigned Child = I.Op == LOP_NEWCLOSURE ? P.Children.at(unsigned(I.D)) : P.Constants.at(unsigned(I.D)).Index;
        const auto &FunctionName = E.C.Prototypes[Child].Name;
        if (!FunctionName.empty() &&
            (E.Symbols[S].Name.starts_with("Value") || (Identifier(FunctionName) && E.Symbols[S].Base == Pascal(FunctionName))))
        {
            unsigned Nice = E.New(FunctionName, Id, true, false, false, true);
            E.Symbols[S].Name = E.Symbols[Nice].Name;
            E.Symbols[S].Base = E.Symbols[Nice].Base;
        }
        Set(Closure(I, Out));
        break;
    }
    case LOP_NAMECALL:
    case LOP_NAMECALLUDATA:
    {
        unsigned Key = I.Op == LOP_NAMECALLUDATA ? I.Aux & 65535 : I.Aux;
        PendingMethod = P.Constants.at(Key).Text;
        PendingReceiver = R(I.B);
        PendingBase = I.A;
        // NAMECALL is paired with CALL; preserve the colon syntax for Roblox __namecall.
        break;
    }
    case LOP_CALL:
    case LOP_CALLFB:
    {
        bool Method = !PendingMethod.empty() && PendingBase == I.A;
        unsigned Start = I.A + (Method ? 2 : 1);
        std::vector<std::string> Args;
        if (I.B)
            for (unsigned J = Start; J < I.A + I.B; ++J)
                Args.push_back(R(J));
        else if (!OpenExpression.empty())
        {
            for (unsigned J = Start; J < OpenExpressionBase; ++J)
                Args.push_back(R(J));
            Args.push_back(std::move(OpenExpression));
            OpenExpression.clear();
        }
        else
        {
            // Open arguments consist of fixed registers followed by the most recent open result tuple.
            NeedsPack = true;
            unsigned Arguments = E.New("CallArguments", Id, true);
            auto TupleIndex = Token(E.New("TupleIndex", Id, true, true));
            Emit("local " + Token(Arguments) + " = {}");
            // Dynamic prefixes are expanded by a statically generated choice, preserving nils.
            for (unsigned J = Start; J < P.Stack; ++J)
                Emit("if " + Token(TopSymbol) + " > " + std::to_string(J) + " then " + Token(Arguments) + "[" +
                     std::to_string(J - Start + 1) + "] = " + R(J) + " end");
            Emit("for " + TupleIndex + " = 1, " + Token(PackSymbol) + ".n do\n    " + Token(Arguments) + "[" + Token(TopSymbol) + " - " +
                 std::to_string(Start) + " + " + TupleIndex + "] = " + Token(PackSymbol) + "[" + TupleIndex + "]\nend");
            Args.push_back("table.unpack(" + Token(Arguments) + ", 1, " + Token(TopSymbol) + " - " + std::to_string(Start) + " + " +
                           Token(PackSymbol) + ".n)");
        }
        std::string Callable = Method ? PendingReceiver + ":" + PendingMethod : R(I.A);
        if (Method && !Identifier(PendingMethod))
            throw Error("NAMECALL contains a method name that cannot be expressed with colon syntax");
        std::string Call = Callable + "(" + Join(Args) + ")";
        std::string ResultHint;
        if (Method && (PendingMethod == "GetService" || PendingMethod == "WaitForChild" || PendingMethod == "FindFirstChild") && I.B > 2)
            ResultHint = Hints[SymbolAt(Start, I.Pc)];
        else if (!Method)
        {
            auto CallableHint = Hints[SymbolAt(I.A, I.Pc)];
            if (CallableHint == "require")
                ResultHint = I.B ? Hints[SymbolAt(I.A + 1, I.Pc)] : OpenHint;
            else if (CallableHint.ends_with(".new"))
                ResultHint = CallableHint.substr(0, CallableHint.size() - 4);
        }
        if (!ResultHint.empty())
            Hint(ResultHint, I.C == 2);
        if (Method && PendingMethod == "GetService" && I.C == 2 && I.B == 3)
        {
            auto Previous = Index(I.Pc);
            if (Previous)
            {
                const auto &Load = P.Instructions[Previous - 1];
                if (Load.Op == LOP_NAMECALL && Previous > 1)
                    Previous--;
                const auto &Arg = P.Instructions[Previous - 1];
                if (Arg.Op == LOP_LOADK && P.Constants[unsigned(Arg.D)].Tag == LBC_CONSTANT_STRING)
                {
                    unsigned S = SymbolAt(I.A, I.Pc, true);
                    if (!E.Symbols[S].Keep)
                    {
                        auto New = E.New(P.Constants[unsigned(Arg.D)].Text, Id, true);
                        E.Symbols[S].Name = E.Symbols[New].Name;
                        E.Symbols[S].Base = E.Symbols[New].Base;
                        E.Symbols[S].Keep = true;
                    }
                }
            }
        }
        PendingMethod.clear();
        PendingReceiver.clear();
        if (I.C)
            Many(I.A, I.C - 1, Call);
        else if (CanInlineOpen())
        {
            OpenExpression = Call;
            OpenExpressionBase = I.A;
            OpenHint = ResultHint;
        }
        else
        {
            NeedsPack = true;
            Emit(Token(PackSymbol) + " = table.pack(" + Call + ")");
            Emit(Token(TopSymbol) + " = " + std::to_string(I.A));
            // Fixed consumers can read the prefix of an open result tuple.
            for (unsigned J = I.A; J < P.Stack; ++J)
                Out.push_back(Assign(J, I, Token(PackSymbol) + "[" + std::to_string(J - I.A + 1) + "]"));
        }
        break;
    }
    case LOP_RETURN:
    {
        std::vector<std::string> Values;
        if (I.B)
            for (unsigned J = 0; J + 1 < I.B; ++J)
                Values.push_back(R(I.A + J));
        else if (!OpenExpression.empty())
        {
            for (unsigned J = I.A; J < OpenExpressionBase; ++J)
                Values.push_back(R(J));
            Values.push_back(std::move(OpenExpression));
            OpenExpression.clear();
        }
        else
        {
            NeedsPack = true;
            unsigned ReturnValues = E.New("ReturnValues", Id, true);
            auto TupleIndex = Token(E.New("TupleIndex", Id, true, true));
            Emit("local " + Token(ReturnValues) + " = {}");
            for (unsigned J = I.A; J < P.Stack; ++J)
                Emit("if " + Token(TopSymbol) + " > " + std::to_string(J) + " then " + Token(ReturnValues) + "[" +
                     std::to_string(J - I.A + 1) + "] = " + R(J) + " end");
            Emit("for " + TupleIndex + " = 1, " + Token(PackSymbol) + ".n do\n    " + Token(ReturnValues) + "[" + Token(TopSymbol) + " - " +
                 std::to_string(I.A) + " + " + TupleIndex + "] = " + Token(PackSymbol) + "[" + TupleIndex + "]\nend");
            Values.push_back("table.unpack(" + Token(ReturnValues) + ", 1, " + Token(TopSymbol) + " - " + std::to_string(I.A) + " + " +
                             Token(PackSymbol) + ".n)");
        }
        Out.push_back({Kind::Return, {}, Join(Values)});
        break;
    }
    case LOP_ADD:
    case LOP_SUB:
    case LOP_MUL:
    case LOP_DIV:
    case LOP_MOD:
    case LOP_POW:
    case LOP_ADDK:
    case LOP_SUBK:
    case LOP_MULK:
    case LOP_DIVK:
    case LOP_MODK:
    case LOP_POWK:
    case LOP_AND:
    case LOP_OR:
    case LOP_ANDK:
    case LOP_ORK:
    case LOP_IDIV:
    case LOP_IDIVK:
    case LOP_SUBRK:
    case LOP_DIVRK:
    {
        std::string Left = R(I.B < P.Stack ? I.B : 0), Right, Op;
        static const char *Arithmetic[] = {"+", "-", "*", "/", "%", "^"};
        if (I.Op >= LOP_ADD && I.Op <= LOP_POW)
        {
            Op = Arithmetic[I.Op - LOP_ADD];
            Right = R(I.C);
        }
        else if (I.Op >= LOP_ADDK && I.Op <= LOP_POWK)
        {
            Op = Arithmetic[I.Op - LOP_ADDK];
            Right = ConstantAt(I.C);
        }
        else if (I.Op == LOP_IDIV || I.Op == LOP_IDIVK)
        {
            Op = "//";
            Right = I.Op == LOP_IDIV ? R(I.C) : ConstantAt(I.C);
        }
        else if (I.Op == LOP_SUBRK || I.Op == LOP_DIVRK)
        {
            Op = I.Op == LOP_SUBRK ? "-" : "/";
            Left = ConstantAt(I.B);
            Right = R(I.C);
        }
        else
        {
            Op = (I.Op == LOP_AND || I.Op == LOP_ANDK) ? "and" : "or";
            Right = (I.Op == LOP_ANDK || I.Op == LOP_ORK) ? ConstantAt(I.C) : R(I.C);
        }
        Set("(" + Left + " " + Op + " " + Right + ")");
        break;
    }
    case LOP_NOT:
        Set("(not " + R(I.B) + ")", true);
        break;
    case LOP_MINUS:
        Set("(-" + R(I.B) + ")");
        break;
    case LOP_LENGTH:
        Set("(#" + R(I.B) + ")");
        break;
    case LOP_CONCAT:
    {
        std::vector<std::string> Parts;
        for (unsigned J = I.B; J <= I.C; ++J)
            Parts.push_back(R(J));
        Set("(" + Join(Parts, " .. ") + ")");
        break;
    }
    case LOP_NEWTABLE:
        Set("{}");
        break;
    case LOP_DUPTABLE:
        Set(ConstantAt(unsigned(I.D)));
        break;
    case LOP_SETLIST:
        if (I.C)
            for (unsigned J = 0; J + 1 < I.C; ++J)
                Emit(R(I.A) + "[" + std::to_string(I.Aux + J) + "] = " + R(I.B + J));
        else
        {
            NeedsPack = true;
            auto TupleIndex = Token(E.New("TupleIndex", Id, true, true));
            for (unsigned J = I.B; J < P.Stack; ++J)
                Emit("if " + Token(TopSymbol) + " > " + std::to_string(J) + " then " + R(I.A) + "[" + std::to_string(I.Aux + J - I.B) +
                     "] = " + R(J) + " end");
            Emit("for " + TupleIndex + " = 1, " + Token(PackSymbol) + ".n do\n    " + R(I.A) + "[" + std::to_string(I.Aux) + " + " +
                 Token(TopSymbol) + " - " + std::to_string(I.B) + " + " + TupleIndex + " - 1] = " + Token(PackSymbol) + "[" + TupleIndex +
                 "]\nend");
        }
        break;
    case LOP_GETVARARGS:
        if (!I.B && CanInlineOpen())
        {
            OpenExpression = "...";
            OpenExpressionBase = I.A;
            break;
        }
        NeedsVarargs = true;
        if (I.B)
            Many(I.A, I.B - 1, "table.unpack(" + Token(VarargsSymbol) + ", 1, " + std::to_string(I.B - 1) + ")");
        else
        {
            NeedsPack = true;
            Emit(Token(PackSymbol) + " = " + Token(VarargsSymbol));
            Emit(Token(TopSymbol) + " = " + std::to_string(I.A));
            for (unsigned J = I.A; J < P.Stack; ++J)
                Out.push_back(Assign(J, I, Token(PackSymbol) + "[" + std::to_string(J - I.A + 1) + "]"));
        }
        break;
    case LOP_NEWCLASS:
    case LOP_NEWCLASSMEMBER:
        throw Error("Experimental class bytecode is parsed, but stable Roblox has no equivalent source construct");
    default:
        throw Unstructured("unhandled control instruction " + OpcodeName(I.Op));
    }
}

bool Jump(LuauOpcode Op)
{
    return Op == LOP_JUMP || Op == LOP_JUMPBACK || Op == LOP_JUMPX;
}
bool Conditional(LuauOpcode Op)
{
    return (Op >= LOP_JUMPIF && Op <= LOP_JUMPIFNOTLT) || (Op >= LOP_JUMPXEQKNIL && Op <= LOP_JUMPXEQKS);
}

std::vector<Statement> Function::Region(unsigned Start, unsigned End, int BreakTarget, int ContinueTarget, unsigned Level,
                                        bool IgnoreBackedge)
{
    if (Level > 150)
        throw Unstructured("control-flow nesting too deep");
    std::vector<Statement> Out;
    unsigned Pos = Index(Start), Limit = Index(End);
    while (Pos != Limit && Pos < P.Instructions.size())
    {
        if (RegionVisits.empty())
            RegionVisits.resize(P.Instructions.size());
        if (++RegionVisits[Pos] > 12)
            throw Unstructured("shared region exceeds duplication budget");
        const auto &I = P.Instructions[Pos];
        // A backedge to this instruction defines a while/repeat region. Choose the outermost backedge.
        if (!IgnoreBackedge)
        {
            int Back = -1;
            for (unsigned J = Pos + 1; J < Limit; ++J)
                if (P.Instructions[J].Target() == int(I.Pc) && Flow.Dominates(Pos, J) &&
                    (Jump(P.Instructions[J].Op) || Conditional(P.Instructions[J].Op)))
                    Back = int(J);
            if (Back >= 0)
            {
                const auto &Tail = P.Instructions[unsigned(Back)];
                Statement Loop;
                Loop.Type = Kind::While;
                Loop.Text = "true";
                Loop.Body = Region(I.Pc, Tail.Pc, int(Tail.Next), int(I.Pc), Level + 1, true);
                if (Conditional(Tail.Op))
                {
                    Loop.Type = Kind::Repeat;
                    Loop.Text = "not (" + Condition(Tail) + ")";
                }
                Out.push_back(std::move(Loop));
                Pos = unsigned(Back) + 1;
                continue;
            }
        }
        IgnoreBackedge = false;
        if (I.Op == LOP_FORNPREP)
        {
            unsigned Finish = Index(unsigned(I.Target()));
            if (Finish == 0 || Finish > Limit)
                throw Unstructured("numeric loop leaves region");
            const auto &Tail = P.Instructions[Finish - 1];
            if (Tail.Op != LOP_FORNLOOP || Tail.A != I.A || Tail.Target() != int(I.Next))
                throw Unstructured("noncanonical numeric loop");
            unsigned Counter = E.New("Index", Id, true, true);
            Statement Loop{Kind::For};
            Loop.Left = {Counter};
            Loop.Text = Token(Counter) + " = " + Read(I.A + 2, I.Pc) + ", " + Read(I.A, I.Pc) + ", " + Read(I.A + 1, I.Pc);
            Loop.Body = Region(I.Next, Tail.Pc, I.Target(), int(Tail.Pc), Level + 1);
            Loop.Body.insert(Loop.Body.begin(), Assign(I.A + 2, P.Instructions[Pos + 1], Token(Counter), true));
            Out.push_back(std::move(Loop));
            Pos = Finish;
            continue;
        }
        if (I.Op == LOP_FORGPREP || I.Op == LOP_FORGPREP_NEXT || I.Op == LOP_FORGPREP_INEXT)
        {
            unsigned Finish = Index(unsigned(I.Target()));
            if (Finish >= Limit)
                throw Unstructured("generic loop leaves region");
            const auto &Tail = P.Instructions[Finish];
            if (Tail.Op != LOP_FORGLOOP || Tail.A != I.A || Tail.Target() != int(I.Next))
                throw Unstructured("noncanonical generic loop");
            Statement Loop{Kind::For};
            std::vector<std::string> Vars;
            for (unsigned J = 0; J < (Tail.Aux & 255); ++J)
            {
                unsigned Var = E.New(J == 0 ? "Key" : J == 1 ? "Value" : "Item" + std::to_string(J + 1), Id, true, true);
                Vars.push_back(Token(Var));
                Loop.Left.push_back(Var);
            }
            Loop.Text = Join(Vars) + " in " + Read(I.A, I.Pc) + ", " + Read(I.A + 1, I.Pc) + ", " + Read(I.A + 2, I.Pc);
            Loop.Body = Region(I.Next, Tail.Pc, int(Tail.Next), int(Tail.Pc), Level + 1);
            for (unsigned J = unsigned(Vars.size()); J-- > 0;)
                Loop.Body.insert(Loop.Body.begin(), Assign(I.A + 3 + J, P.Instructions[Pos + 1], Vars[J], true));
            Out.push_back(std::move(Loop));
            Pos = Finish + 1;
            continue;
        }
        if (Conditional(I.Op))
        {
            auto Target = I.Target();
            // Comparisons lowered to LOADB/skip/LOADB form a single boolean value.
            if (Pos + 2 < Limit)
            {
                const auto &False = P.Instructions[Pos + 1];
                const auto &True = P.Instructions[Pos + 2];
                if (False.Op == LOP_LOADB && True.Op == LOP_LOADB && False.A == True.A && False.B != True.B && Target == int(True.Pc) &&
                    False.Target() == int(True.Next) && !True.C)
                {
                    std::string Value = Condition(I);
                    if (!True.B)
                        Value = "not (" + Value + ")";
                    else if (I.Op == LOP_JUMPIF)
                        Value = "not not (" + Value + ")";
                    Out.push_back(Assign(False.A, False, "(" + Value + ")"));
                    Pos += 3;
                    continue;
                }
            }
            Statement Branch{Kind::If, {}, Condition(I)};
            if (Target == BreakTarget)
                Branch.Body.push_back({Kind::Break});
            else if (Target == ContinueTarget)
                Branch.Body.push_back({Kind::Continue});
            else
            {
                if (Target <= int(I.Pc))
                    throw Unstructured("conditional crosses region");
                // The lexical false arm is also a valid continuation when paths in the first
                // arm terminate early. A strict postdominator would move that continuation to
                // the function exit and duplicate the remainder of the function.
                unsigned JoinPc = unsigned(Target);
                for (unsigned Scan = Pos + 1; Scan < Index(std::min(JoinPc, End)); ++Scan)
                {
                    const auto &Edge = P.Instructions[Scan];
                    if (Edge.Target() <= Target || Edge.Target() == BreakTarget || Edge.Target() == ContinueTarget)
                        continue;
                    const auto &Destination = P.Instructions[Index(unsigned(Edge.Target()))];
                    if (Destination.Op == LOP_RETURN)
                        continue;
                    JoinPc = std::max(JoinPc, unsigned(Edge.Target()));
                }
                int Post = Flow.PostDominators[Pos];
                if (Post >= 0 && unsigned(Post) < P.Instructions.size())
                    JoinPc = std::min(JoinPc, P.Instructions[unsigned(Post)].Pc);
                JoinPc = std::min(JoinPc, End);
                if (JoinPc <= I.Pc)
                    throw Unstructured("branch rejoins a loop header at " + std::to_string(I.Pc) + " -> " + std::to_string(JoinPc) +
                                       " end " + std::to_string(End));
                Branch.Text = "not (" + Branch.Text + ")";
                Branch.Body = Region(I.Next, JoinPc, BreakTarget, ContinueTarget, Level + 1);
                if (JoinPc != unsigned(Target))
                    Branch.Else = Region(unsigned(Target), JoinPc, BreakTarget, ContinueTarget, Level + 1);
                Out.push_back(std::move(Branch));
                Pos = Index(JoinPc);
                continue;
            }
            Out.push_back(std::move(Branch));
            ++Pos;
            continue;
        }
        if (Jump(I.Op) || (I.Op == LOP_LOADB && I.C) || I.Op == LOP_CMPPROTO)
        {
            if (I.Op == LOP_LOADB)
                Simple(I, Out);
            auto Target = I.Target();
            if (Target == BreakTarget)
            {
                Out.push_back({Kind::Break});
                break;
            }
            if (Target == ContinueTarget)
            {
                Out.push_back({Kind::Continue});
                break;
            }
            if (Target < int(I.Next))
                throw Unstructured("jump crosses region");
            // CMPPROTO's generic fallback is always valid, independent of runtime-specific proto ids.
            Pos = Index(unsigned(Target));
            continue;
        }
        Simple(I, Out);
        if (I.Op == LOP_RETURN)
            break;
        ++Pos;
    }
    return Out;
}

std::vector<Statement> Function::StateMachine()
{
    Dispatch = true;
    PendingMethod.clear();
    unsigned PcSymbol = E.New("ProgramCounter", Id, true);
    std::vector<Statement> Out{{Kind::Raw, {}, "-- Control flow required an explicit state machine."}, {Kind::Assign, {PcSymbol}, "0"}};
    Statement Loop{Kind::While, {}, "true"};
    std::map<unsigned, unsigned> IteratorSymbols;
    // Generic iteration uses native Luau iteration inside a resumable iterator, including __iter.
    for (const auto &I : P.Instructions)
        if (I.Op == LOP_FORGPREP || I.Op == LOP_FORGPREP_NEXT || I.Op == LOP_FORGPREP_INEXT)
        {
            unsigned S = E.New("Iterator", Id, true);
            IteratorSymbols[I.A] = S;
            Out.push_back({Kind::Raw, {}, "local " + Token(S)});
        }
    for (const auto &I : P.Instructions)
    {
        Statement Branch{Kind::If, {}, Token(PcSymbol) + " == " + std::to_string(I.Pc)};
        auto SetPc = [&](int Target) { Branch.Body.push_back({Kind::Raw, {}, Token(PcSymbol) + " = " + std::to_string(Target)}); };
        auto R = [&](unsigned Reg) { return Read(Reg, I.Pc); };
        if (Conditional(I.Op))
        {
            Statement Test{Kind::If, {}, Condition(I)};
            Test.Body.push_back({Kind::Raw, {}, Token(PcSymbol) + " = " + std::to_string(I.Target())});
            Test.Else.push_back({Kind::Raw, {}, Token(PcSymbol) + " = " + std::to_string(I.Next)});
            Branch.Body.push_back(std::move(Test));
        }
        else if (Jump(I.Op) || I.Op == LOP_CMPPROTO)
            SetPc(I.Target());
        else if (I.Op == LOP_FORNPREP || I.Op == LOP_FORNLOOP)
        {
            if (I.Op == LOP_FORNPREP)
                for (unsigned J = 0; J < 3; ++J)
                    Branch.Body.push_back({Kind::Raw, {}, R(I.A + J) + " = tonumber(" + R(I.A + J) + ")"});
            else
                Branch.Body.push_back({Kind::Raw, {}, R(I.A + 2) + " = " + R(I.A + 2) + " + " + R(I.A + 1)});
            std::string Condition = "(" + R(I.A + 1) + " > 0 and " + R(I.A + 2) + " <= " + R(I.A) + ") or (not (" + R(I.A + 1) +
                                    " > 0) and " + R(I.A) + " <= " + R(I.A + 2) + ")";
            Statement Test{Kind::If, {}, Condition};
            Test.Body.push_back({Kind::Raw, {}, Token(PcSymbol) + " = " + std::to_string(I.Op == LOP_FORNPREP ? int(I.Next) : I.Target())});
            Test.Else.push_back({Kind::Raw, {}, Token(PcSymbol) + " = " + std::to_string(I.Op == LOP_FORNPREP ? I.Target() : int(I.Next))});
            Branch.Body.push_back(std::move(Test));
        }
        else if (I.Op == LOP_FORGPREP || I.Op == LOP_FORGPREP_NEXT || I.Op == LOP_FORGPREP_INEXT)
        {
            const auto &Tail = P.Instructions.at(Index(unsigned(I.Target())));
            if (Tail.Op != LOP_FORGLOOP)
                throw Error("Generic preparation does not target FORGLOOP");
            std::vector<std::string> Vars;
            for (unsigned J = 0; J < (Tail.Aux & 255); ++J)
                Vars.push_back("Item" + std::to_string(J + 1));
            auto Text = Token(IteratorSymbols.at(I.A)) + " = coroutine.wrap(function()\n    for " + Join(Vars) + " in " + R(I.A) + ", " +
                        R(I.A + 1) + ", " + R(I.A + 2) + " do\n        coroutine.yield(true, " + Join(Vars) +
                        ")\n    end\n    return false\nend)";
            Branch.Body.push_back({Kind::Raw, {}, Text});
            SetPc(I.Target());
        }
        else if (I.Op == LOP_FORGLOOP)
        {
            auto It = IteratorSymbols.find(I.A);
            if (It == IteratorSymbols.end())
                throw Error("Generic loop has no preparation");
            unsigned HasNext = E.New("HasNext", Id, true);
            Branch.Body.push_back({Kind::Raw, {}, "local " + Token(HasNext)});
            std::vector<std::string> Vars{Token(HasNext)};
            for (unsigned J = 0; J < (I.Aux & 255); ++J)
                Vars.push_back(R(I.A + 3 + J));
            Branch.Body.push_back({Kind::Raw, {}, Join(Vars) + " = " + Token(It->second) + "()"});
            Branch.Body.push_back({Kind::Raw, {}, R(I.A + 2) + " = " + R(I.A + 3)});
            Statement Test{Kind::If, {}, Token(HasNext)};
            Test.Body.push_back({Kind::Raw, {}, Token(PcSymbol) + " = " + std::to_string(I.Target())});
            Test.Else.push_back({Kind::Raw, {}, Token(PcSymbol) + " = " + std::to_string(I.Next)});
            Branch.Body.push_back(std::move(Test));
        }
        else if (I.Op == LOP_NAMECALL || I.Op == LOP_NAMECALLUDATA)
        {
            // Keep preparation and call in a single state so __namecall survives the lowering.
            Simple(I, Branch.Body);
            unsigned J = Index(I.Next);
            for (; J < P.Instructions.size(); ++J)
            {
                auto Op = P.Instructions[J].Op;
                if (Conditional(Op) || Jump(Op) || Op == LOP_RETURN)
                    throw Error("NAMECALL is not paired with CALL");
                Simple(P.Instructions[J], Branch.Body);
                if (Op == LOP_CALL || Op == LOP_CALLFB)
                {
                    SetPc(int(P.Instructions[J].Next));
                    break;
                }
            }
        }
        else
        {
            Simple(I, Branch.Body);
            if (I.Op != LOP_RETURN)
                SetPc(I.Op == LOP_LOADB && I.C ? I.Target() : int(I.Next));
        }
        if (I.Op != LOP_RETURN)
            Branch.Body.push_back({Kind::Continue});
        Loop.Body.push_back(std::move(Branch));
    }
    Loop.Body.push_back({Kind::Raw, {}, "error(\"Invalid decompiled control-flow state\")"});
    Out.push_back(std::move(Loop));
    return Out;
}

void Function::Optimize(std::vector<Statement> &Nodes)
{
    std::function<bool(const Statement &, const Statement &)> Equal = [&](const auto &A, const auto &B)
    {
        return A.Type == B.Type && A.Left == B.Left && A.Text == B.Text && A.Body.size() == B.Body.size() &&
               A.Else.size() == B.Else.size() && std::equal(A.Body.begin(), A.Body.end(), B.Body.begin(), Equal) &&
               std::equal(A.Else.begin(), A.Else.end(), B.Else.begin(), Equal);
    };
    auto Same = [&](const auto &A, const auto &B) { return A.size() == B.size() && std::equal(A.begin(), A.end(), B.begin(), Equal); };
    std::function<void(std::vector<Statement> &)> Structure = [&](auto &List)
    {
        for (auto &S : List)
        {
            Structure(S.Body);
            Structure(S.Else);
            if (S.Type != Kind::If)
                continue;
            S.Text = CleanCondition(S.Text);
            if (S.Body.empty() && !S.Else.empty())
            {
                S.Text = CleanCondition(Negate(S.Text));
                S.Body.swap(S.Else);
            }
            if (S.Body.size() == 1 && S.Body[0].Type == Kind::If && Same(S.Else, S.Body[0].Else))
            {
                auto Inner = std::move(S.Body[0]);
                S.Text = "(" + S.Text + ") and (" + Inner.Text + ")";
                S.Body = std::move(Inner.Body);
            }
            if (S.Body.size() == 1 && S.Body[0].Type == Kind::If && !S.Else.empty())
            {
                S.Text = CleanCondition(Negate(S.Text));
                S.Body.swap(S.Else);
            }
            if (S.Else.size() == 1 && S.Else[0].Type == Kind::If && Same(S.Body, S.Else[0].Body))
            {
                auto Inner = std::move(S.Else[0]);
                S.Text = "(" + S.Text + ") or (" + Inner.Text + ")";
                S.Else = std::move(Inner.Else);
            }
        }
        for (std::size_t N = 0; N < List.size(); ++N)
        {
            auto &S = List[N];
            if (S.Type != Kind::If)
                continue;
            std::vector<Statement> Tail;
            while (!S.Body.empty() && !S.Else.empty() && Equal(S.Body.back(), S.Else.back()))
            {
                Tail.push_back(std::move(S.Body.back()));
                S.Body.pop_back();
                S.Else.pop_back();
            }
            if (!Tail.empty())
            {
                std::reverse(Tail.begin(), Tail.end());
                List.insert(List.begin() + std::ptrdiff_t(N + 1), Tail.begin(), Tail.end());
            }
        }
        for (std::size_t N = 0; N < List.size(); ++N)
        {
            auto &S = List[N];
            if (S.Type != Kind::If || S.Body.size() != 1 || S.Body[0].Type != Kind::Assign || S.Body[0].Left.size() != 1)
                continue;
            auto U = S.Body[0].Left[0];
            if (S.Else.empty() && N && List[N - 1].Type == Kind::Assign && List[N - 1].Left == S.Body[0].Left &&
                S.Body[0].Text.find(Token(U)) == std::string::npos)
            {
                auto Test = CleanCondition(S.Text);
                if (Test == Token(U) || Test == "not " + Token(U))
                {
                    auto &Previous = List[N - 1];
                    Previous.Text =
                        "(" + Unwrap(Previous.Text) + ")" + (Test == Token(U) ? " and " : " or ") + "(" + Unwrap(S.Body[0].Text) + ")";
                    Previous.Pure = false;
                    Previous.Call = false;
                    List.erase(List.begin() + std::ptrdiff_t(N--));
                    continue;
                }
            }
            if (S.Else.size() == 1 && S.Else[0].Type == Kind::Assign && S.Else[0].Left == S.Body[0].Left &&
                (S.Else[0].Text == "false" || S.Else[0].Text == "nil" || S.Body[0].Text == "false" || S.Body[0].Text == "nil") &&
                S.Body[0].Text.find('\n') == std::string::npos && S.Else[0].Text.find('\n') == std::string::npos)
            {
                Statement Value = S.Body[0];
                Value.Text = "(if " + CleanCondition(S.Text) + " then " + Unwrap(Value.Text) + " else " + Unwrap(S.Else[0].Text) + ")";
                Value.Pure = false;
                Value.Call = false;
                S = std::move(Value);
            }
        }
    };
    Structure(Nodes);
    std::unordered_map<unsigned, unsigned> Reads, WritesCount;
    std::function<void(const std::vector<Statement> &)> Count = [&](const auto &List)
    {
        for (const auto &S : List)
        {
            for (auto Id : Tokens(S.Text))
                Reads[Id]++;
            for (auto Id : S.Left)
                WritesCount[Id]++;
            Count(S.Body);
            Count(S.Else);
        }
    };
    Count(Nodes);
    std::function<void(std::vector<Statement> &)> Apply = [&](std::vector<Statement> &Nodes)
    {
        for (auto &S : Nodes)
        {
            Apply(S.Body);
            Apply(S.Else);
        }
        for (std::size_t N = 0; N < Nodes.size();)
        {
            auto &S = Nodes[N];
            // A temporary that merely copies a stable local can be replaced at
            // every straight-line use, even when the copy is read more than once.
            if (S.Type == Kind::Assign && S.Left.size() == 1 && !E.Symbols[S.Left[0]].Keep && WritesCount[S.Left[0]] == 1)
            {
                auto From = Tokens(S.Text);
                if (From.size() == 1 && S.Text == Token(From.front()) && From.front() != S.Left[0] && !E.Symbols[From.front()].Cell &&
                    !E.Symbols[From.front()].ReferenceAlias)
                {
                    bool Initialized = E.Symbols[From.front()].Parameter && WritesCount[From.front()] == 0;
                    if (WritesCount[From.front()] == 1)
                        for (std::size_t J = 0; J < N; ++J)
                            Initialized |= std::find(Nodes[J].Left.begin(), Nodes[J].Left.end(), From.front()) != Nodes[J].Left.end();
                    unsigned Uses = 0;
                    std::size_t End = N + 1;
                    for (; Initialized && End < Nodes.size(); ++End)
                    {
                        const auto &Later = Nodes[End];
                        if (!Later.Body.empty() || !Later.Else.empty() || Later.Type == Kind::While || Later.Type == Kind::Repeat ||
                            Later.Type == Kind::For || Later.Type == Kind::Function || Later.Text.find('\n') != std::string::npos)
                            break;
                        auto References = Tokens(Later.Text);
                        Uses += unsigned(std::count(References.begin(), References.end(), S.Left[0]));
                    }
                    if (Initialized && Uses && Uses == Reads[S.Left[0]])
                    {
                        for (std::size_t J = N + 1; J < End; ++J)
                            Replace(Nodes[J].Text, Token(S.Left[0]), Token(From.front()));
                        Nodes.erase(Nodes.begin() + std::ptrdiff_t(N));
                        continue;
                    }
                }
            }
            if (S.Type == Kind::Assign && S.Left.size() == 3 && N + 1 < Nodes.size() && Nodes[N + 1].Type == Kind::For)
            {
                auto &Loop = Nodes[N + 1];
                std::vector<std::string> Vars;
                bool Once = true;
                for (auto U : S.Left)
                {
                    Vars.push_back(Token(U));
                    Once &= Reads[U] == 1 && WritesCount[U] == 1;
                }
                auto Suffix = " in " + Join(Vars);
                if (Once && Loop.Text.ends_with(Suffix))
                {
                    Loop.Text.replace(Loop.Text.size() - Suffix.size(), Suffix.size(), " in " + S.Text);
                    Nodes.erase(Nodes.begin() + std::ptrdiff_t(N));
                    continue;
                }
            }
            // Recover a method declaration from the closure and its table installation.
            if (S.Type == Kind::Assign && S.Left.size() == 1 && S.Text.starts_with("function(") && Reads[S.Left[0]] == 1 &&
                N + 1 < Nodes.size() && Nodes[N + 1].Type == Kind::Raw)
            {
                auto &Next = Nodes[N + 1];
                auto Suffix = " = " + Token(S.Left[0]);
                if (Next.Text.ends_with(Suffix))
                {
                    auto Target = Next.Text.substr(0, Next.Text.size() - Suffix.size());
                    if (Target.find('.') != std::string::npos && Target.find('[') == std::string::npos && PrefixExpression(Target))
                    {
                        S.Text = "function " + Target + S.Text.substr(8);
                        S.Type = Kind::Function;
                        S.Left.clear();
                        Nodes.erase(Nodes.begin() + std::ptrdiff_t(N + 1));
                    }
                }
            }
            if (S.Type == Kind::Assign && S.Left.size() == 1 && !E.Symbols[S.Left[0]].Keep && WritesCount[S.Left[0]] == 1)
            {
                auto Id = S.Left[0];
                if (!Reads[Id] && S.Pure)
                {
                    Nodes.erase(Nodes.begin() + std::ptrdiff_t(N));
                    continue;
                }
                if (Reads[Id] == 1 && S.Pure && S.Text.find('\n') == std::string::npos)
                {
                    bool Stable = true;
                    for (auto Dependency : Tokens(S.Text))
                        if (E.Symbols[Dependency].Cell || E.Symbols[Dependency].ReferenceAlias || WritesCount[Dependency] > 1)
                            Stable = false;
                    if (Stable)
                    {
                        bool Removed = false;
                        for (std::size_t J = N + 1; J < Nodes.size(); ++J)
                        {
                            if (!Nodes[J].Body.empty() || !Nodes[J].Else.empty() || Nodes[J].Type == Kind::Repeat ||
                                Nodes[J].Type == Kind::While)
                                break;
                            auto Uses = Tokens(Nodes[J].Text);
                            if (std::count(Uses.begin(), Uses.end(), Id) == 1)
                            {
                                auto Replacement = S.Text;
                                auto Position = Nodes[J].Text.find(Token(Id)) + Token(Id).size();
                                if (Position < Nodes[J].Text.size() &&
                                    std::string(".:[").find(Nodes[J].Text[Position]) != std::string::npos &&
                                    !PrefixExpression(Replacement) && (Replacement.empty() || Replacement.front() != '('))
                                    Replacement = "(" + Replacement + ")";
                                Replace(Nodes[J].Text, Token(Id), Replacement);
                                Nodes.erase(Nodes.begin() + std::ptrdiff_t(N));
                                Removed = true;
                                break;
                            }
                        }
                        if (Removed)
                        {
                            if (N)
                                --N;
                            continue;
                        }
                    }
                }
                if (N + 1 < Nodes.size() && Reads[Id] == 1 && S.Text.find('\n') == std::string::npos)
                {
                    auto &Next = Nodes[N + 1];
                    auto Uses = Tokens(Next.Text);
                    auto UsePosition = Next.Text.find(Token(Id));
                    auto BeforeUse = Next.Text.substr(0, UsePosition);
                    bool CanReorder = S.Pure;
                    for (auto Dependency : Tokens(S.Text))
                        if (E.Symbols[Dependency].Cell || E.Symbols[Dependency].ReferenceAlias)
                            CanReorder = false;
                    bool Ordered = CanReorder || BeforeUse.find_first_of(")[.:+-*/%^#") == std::string::npos;
                    auto PrefixSymbols = Tokens(BeforeUse);
                    if (PrefixSymbols.size() == 1 && !E.Symbols[PrefixSymbols[0]].Cell && BeforeUse.starts_with(Token(PrefixSymbols[0])))
                    {
                        auto Tail = BeforeUse.substr(Token(PrefixSymbols[0]).size());
                        // Assigning a constant field on a stable local has no effect before the RHS is evaluated.
                        if (Tail.starts_with('.') && Tail.ends_with(" = ") && Identifier(Tail.substr(1, Tail.size() - 4)))
                            Ordered = true;
                        // Luau evaluates method arguments before NAMECALL; the receiver here is already a stable local.
                        if (Tail.starts_with(':') && Tail.ends_with('(') && Identifier(Tail.substr(1, Tail.size() - 2)))
                            Ordered = true;
                    }
                    // Adjacent substitution cannot cross a side effect. Do not change a call's arity by inlining a multi-result call.
                    if (Ordered && std::count(Uses.begin(), Uses.end(), Id) == 1 && Next.Type != Kind::While && Next.Type != Kind::Repeat &&
                        Next.Type != Kind::Function)
                    {
                        std::string Replacement = S.Text;
                        if (S.Call)
                            Replacement = "(" + Replacement + ")";
                        auto Position = Next.Text.find(Token(Id)) + Token(Id).size();
                        if (Position < Next.Text.size() && std::string(".:[").find(Next.Text[Position]) != std::string::npos &&
                            !PrefixExpression(Replacement) && (Replacement.empty() || Replacement.front() != '('))
                            Replacement = "(" + Replacement + ")";
                        Replace(Next.Text, Token(Id), Replacement);
                        Next.Pure = Next.Pure && S.Pure;
                        Next.Call = Next.Call || S.Call;
                        Nodes.erase(Nodes.begin() + std::ptrdiff_t(N));
                        if (N)
                            --N;
                        continue;
                    }
                }
            }
            ++N;
        }
    };
    Apply(Nodes);
    std::function<void(std::vector<Statement> &)> Tables = [&](auto &List)
    {
        for (auto &S : List)
        {
            Tables(S.Body);
            Tables(S.Else);
        }
        for (std::size_t N = 0; N < List.size(); ++N)
        {
            auto &S = List[N];
            if (S.Type != Kind::Assign || S.Left.size() != 1 || S.Text != "{}")
                continue;
            auto Table = Token(S.Left[0]);
            std::vector<std::string> Fields;
            for (std::size_t J = N + 1; J < List.size();)
            {
                // Independent empty table allocations may be interleaved with
                // the first writes to this table.
                if (List[J].Type == Kind::Assign && List[J].Left.size() == 1 && List[J].Text == "{}")
                {
                    ++J;
                    continue;
                }
                if (List[J].Type != Kind::Raw)
                    break;
                auto Text = List[J].Text;
                if (!Text.starts_with(Table))
                    break;
                auto Equal = Text.find(" = ");
                if (Equal == std::string::npos || Text.find('\n') != std::string::npos)
                    break;
                auto Right = Text.substr(Equal + 3);
                if (Right.find(Table) != std::string::npos)
                    break;
                auto Key = Text.substr(Table.size(), Equal - Table.size());
                if (J > N + 1 && (!Tokens(Key).empty() || !Tokens(Right).empty() ||
                                  std::any_of(Right.begin(), Right.end(), [](unsigned char C) { return std::isalpha(C); })))
                    break;
                if (Key.starts_with('.'))
                    Key.erase(0, 1);
                else if (!(Key.starts_with('[') && Key.ends_with(']')))
                    break;
                Fields.push_back(Key + " = " + Right);
                List.erase(List.begin() + std::ptrdiff_t(J));
            }
            if (Fields.size() > 3)
            {
                S.Text = "{\n";
                for (const auto &F : Fields)
                    S.Text += std::string(E.Settings.IndentWidth, ' ') + F + ",\n";
                S.Text += "}";
            }
            else if (!Fields.empty())
                S.Text = "{ " + Join(Fields) + " }";
        }
    };
    Tables(Nodes);
}

std::string Function::Render(const std::vector<Statement> &Nodes, unsigned Indent, std::set<unsigned> &Declared,
                             const std::set<unsigned> &Hoist)
{
    std::string Out;
    std::string Pad(Indent * E.Settings.IndentWidth, ' ');
    auto Line = [&](const std::string &Text)
    {
        std::size_t Start = 0;
        do
        {
            auto End = Text.find('\n', Start);
            auto Part = Text.substr(Start, End == std::string::npos ? End : End - Start);
            Out += (Part.find_first_not_of(" \t\r") == std::string::npos ? "" : Pad + Part) + '\n';
            if (End == std::string::npos)
                break;
            Start = End + 1;
        } while (Start < Text.size());
    };
    auto Separate = [&]()
    {
        if (!Out.empty() && !Out.ends_with("\n\n"))
            Out += '\n';
    };
    enum class Group
    {
        None,
        Locals,
        Services,
        Modules,
        Table,
        Writes,
        Calls,
        Exit,
        Block
    };
    Group Previous = Group::None;
    std::string PreviousReceiver;
    if (auto Found = ScopedDeclarations.find(&Nodes); Found != ScopedDeclarations.end())
    {
        std::vector<std::string> Names;
        for (auto Id : Found->second)
            if (Declared.insert(Id).second)
                Names.push_back(Token(Id));
        if (!Names.empty())
        {
            Line("local " + Join(Names));
            Separate();
        }
    }
    for (std::size_t N = 0; N < Nodes.size(); ++N)
    {
        const auto &S = Nodes[N];
        bool Block = S.Type == Kind::If || S.Type == Kind::While || S.Type == Kind::Repeat || S.Type == Kind::For ||
                     S.Type == Kind::Function || S.Text.find("function(") != std::string::npos;
        Group Current = Group::Calls;
        std::string Receiver;
        if (Block)
            Current = Group::Block;
        else if (S.Type == Kind::Assign)
        {
            bool New = std::any_of(S.Left.begin(), S.Left.end(), [&](auto U) { return !Declared.count(U); });
            auto Text = Unwrap(S.Text);
            Current = New ? Group::Locals : Group::Writes;
            if (New && Text.find(":GetService(") != std::string::npos)
                Current = Group::Services;
            else if (New && Text.starts_with("require("))
                Current = Group::Modules;
            else if (Text.starts_with('{') && Text.find('\n') != std::string::npos)
                Current = Group::Table;
        }
        else if (S.Type == Kind::Return || S.Type == Kind::Break || S.Type == Kind::Continue)
            Current = Group::Exit;
        else if (S.Type == Kind::Raw)
        {
            auto Assignment = S.Text.find(" = ");
            Current = Assignment != std::string::npos && PrefixExpression(S.Text.substr(0, Assignment)) ? Group::Writes : Group::Calls;
            auto References = Tokens(S.Text);
            if (!References.empty() && S.Text.starts_with(Token(References.front())))
                Receiver = Token(References.front());
        }
        bool Boundary =
            Block || Previous == Group::Block || Current == Group::Exit || Previous == Group::Exit || Current == Group::Table ||
            Previous == Group::Table ||
            ((Current != Previous) && (Current == Group::Locals || Previous == Group::Locals || Current == Group::Services ||
                                       Previous == Group::Services || Current == Group::Modules || Previous == Group::Modules)) ||
            (!Receiver.empty() && !PreviousReceiver.empty() && Receiver != PreviousReceiver);
        if (Previous != Group::None && Boundary)
            Separate();
        switch (S.Type)
        {
        case Kind::Assign:
        {
            std::vector<std::string> Names, New;
            for (auto Id : S.Left)
            {
                Names.push_back(Token(Id));
                if (!Declared.count(Id))
                    New.push_back(Token(Id));
            }
            bool AllNew = New.size() == Names.size();
            if (!AllNew && !New.empty())
                Line("local " + Join(New));
            if (S.Left.size() == 1 && S.Text.starts_with("function("))
            {
                std::string FunctionText = (AllNew ? "local function " : "function ") + Names[0] + S.Text.substr(8);
                Line(FunctionText);
            }
            else
                Line((AllNew ? "local " : "") + Join(Names) + " = " + CleanExpression(S.Left.size() == 1 ? Unwrap(S.Text) : S.Text));
            for (auto Id : S.Left)
                Declared.insert(Id);
            break;
        }
        case Kind::Raw:
        {
            auto Text = CleanExpression(S.Text);
            auto Equal = Text.find(" = ");
            if (Equal != std::string::npos && PrefixExpression(Text.substr(0, Equal)))
                Text = Text.substr(0, Equal + 3) + CleanExpression(Unwrap(Text.substr(Equal + 3)));
            // A lexical block separates a parenthesized call without a statement semicolon.
            if (Text.starts_with('('))
            {
                Line("do");
                Line(std::string(E.Settings.IndentWidth, ' ') + Text);
                Line("end");
            }
            else
                Line(Text);
            break;
        }
        case Kind::Function:
            Line(S.Text);
            break;
        case Kind::Return:
            Line(S.Text.empty() ? "return" : "return " + CleanExpression(S.Text));
            break;
        case Kind::Break:
            Line("break");
            break;
        case Kind::Continue:
            Line("continue");
            break;
        case Kind::If:
        case Kind::While:
        case Kind::Repeat:
        case Kind::For:
        {
            if (S.Type == Kind::If)
            {
                auto Test = FormatCondition(S.Text);
                if (Test.size() + Pad.size() > 110)
                    for (const auto *Operator : {" or ", " and "})
                    {
                        auto Parts = SplitLogical(Test, Operator);
                        if (Parts.size() < 2)
                            continue;
                        Test = Join(Parts, "\n" + std::string(E.Settings.IndentWidth, ' ') + std::string(Operator).substr(1));
                        break;
                    }
                Line("if " + Test + " then");
            }
            if (S.Type == Kind::While)
                Line("while " + FormatCondition(S.Text) + " do");
            if (S.Type == Kind::Repeat)
                Line("repeat");
            if (S.Type == Kind::For)
                Line("for " + S.Text + " do");
            auto InnerDeclared = Declared;
            for (auto Id : S.Left)
                InnerDeclared.insert(Id);
            Out += Render(S.Body, Indent + 1, InnerDeclared, Hoist);
            if (!S.Else.empty())
            {
                InnerDeclared = Declared;
                if (S.Type == Kind::If && S.Else.size() == 1 && S.Else.front().Type == Kind::If && ScopedDeclarations[&S.Else].empty())
                {
                    auto ElseText = Render(S.Else, Indent, InnerDeclared, Hoist);
                    ElseText.replace(Pad.size(), 2, "elseif");
                    Out += ElseText;
                    break; // The flattened elseif already supplies the final end.
                }
                Line("else");
                Out += Render(S.Else, Indent + 1, InnerDeclared, Hoist);
            }
            Line(S.Type == Kind::Repeat ? "until " + FormatCondition(S.Text) : "end");
            break;
        }
        default:
            throw Error("Internal: unsupported source statement");
        }
        Previous = Current;
        PreviousReceiver = Receiver;
        if ((Block || Current == Group::Table) && N + 1 < Nodes.size())
            Separate();
    }
    E.Budget(Out.size());
    return Out;
}

std::string Function::Generate(bool Main)
{
    std::vector<Statement> Nodes;
    try
    {
        if (E.Settings.ForceStateMachine)
            throw Unstructured("requested by caller");
        Nodes = Region(0, unsigned(P.Code.size()));
    }
    catch (const Unstructured &Reason)
    {
        E.Output.Diagnostics.push_back("Function " + std::to_string(Id) + ": state machine (" + Reason.what() + ")");
        E.Output.StateMachineFunctions++;
        Nodes = StateMachine();
    }
    for (unsigned Pass = 0; Pass < 5; ++Pass)
        Optimize(Nodes);
    // Remove the compiler's implicit empty return at the end of a chunk/function.
    if (!Nodes.empty() && Nodes.back().Type == Kind::Return && Nodes.back().Text.empty())
        Nodes.pop_back();

    using Scope = const std::vector<Statement> *;
    std::map<unsigned, std::vector<Scope>> Occurrences;
    std::set<unsigned> Referenced;
    std::vector<Scope> Path;
    auto Occur = [&](unsigned U)
    {
        Referenced.insert(U);
        auto [It, Inserted] = Occurrences.emplace(U, Path);
        if (!Inserted)
        {
            auto &Common = It->second;
            std::size_t N = 0;
            while (N < Common.size() && N < Path.size() && Common[N] == Path[N])
                ++N;
            Common.resize(N);
        }
    };
    std::function<void(const std::vector<Statement> &)> Inspect = [&](const auto &List)
    {
        Path.push_back(&List);
        for (const auto &S : List)
        {
            for (auto U : Tokens(S.Text))
                Occur(U);
            for (auto U : S.Left)
                Occur(U);
            Inspect(S.Body);
            Inspect(S.Else);
        }
        Path.pop_back();
    };
    Inspect(Nodes);
    std::function<bool(const Statement &, unsigned)> Contains = [&](const auto &S, unsigned U)
    {
        if (S.Text.find(Token(U)) != std::string::npos || std::find(S.Left.begin(), S.Left.end(), U) != S.Left.end())
            return true;
        for (const auto &Child : S.Body)
            if (Contains(Child, U))
                return true;
        for (const auto &Child : S.Else)
            if (Contains(Child, U))
                return true;
        return false;
    };
    std::set<unsigned> Hoist, Declared;
    for (auto U : Parameters)
        Declared.insert(U);
    for (auto U : Referenced)
    {
        const auto &S = E.Symbols[U];
        if (S.Owner != Id || S.Parameter)
            continue;
        if (S.Cell)
        {
            Hoist.insert(U);
            continue;
        }
        const auto *Scope = Occurrences[U].back();
        for (const auto &First : *Scope)
        {
            if (!Contains(First, U))
                continue;
            bool Defines = std::find(First.Left.begin(), First.Left.end(), U) != First.Left.end();
            if (!(Defines && (First.Type == Kind::For || (First.Type == Kind::Assign && (First.Text.find(Token(U)) == std::string::npos ||
                                                                                         First.Text.starts_with("function("))))))
                ScopedDeclarations[Scope].insert(U);
            break;
        }
    }
    Hoist.insert(ScopedDeclarations[&Nodes].begin(), ScopedDeclarations[&Nodes].end());
    if (Dispatch)
        for (auto U : Registers)
            if (Referenced.count(U) && !E.Symbols[U].Parameter)
                Hoist.insert(U);
    unsigned Indent = Main ? 0 : 1;
    std::string Pad(Indent * E.Settings.IndentWidth, ' '), Out;
    if (E.Settings.IncludeUpvalueComments && !Upvalues.empty())
    {
        Out += Pad + "--[[\n" + Pad + std::string(E.Settings.IndentWidth, ' ') + "Upvalues:\n";
        for (unsigned J = 0; J < Upvalues.size(); ++J)
            Out += Pad + std::string(E.Settings.IndentWidth, ' ') + std::to_string(J + 1) + ": " +
                   (Tokens(Upvalues[J].Value).empty() ? Upvalues[J].Name : Token(Tokens(Upvalues[J].Value).front())) + " (type \"" +
                   (Upvalues[J].Reference ? "Reference" : "Copy") + "\")\n";
        Out += Pad + "]]\n\n";
    }
    std::vector<std::string> Hoisted;
    for (auto U : Hoist)
    {
        if (U == PackSymbol || U == TopSymbol || U == VarargsSymbol)
            continue;
        if (E.Symbols[U].Cell)
            Out += Pad + "local " + Token(U) + " = {}\n";
        else
            Hoisted.push_back(Token(U));
        Declared.insert(U);
    }
    for (std::size_t N = 0; N < Hoisted.size(); N += 8)
        Out += Pad + "local " +
               Join(std::vector<std::string>(Hoisted.begin() + std::ptrdiff_t(N),
                                             Hoisted.begin() + std::ptrdiff_t(std::min(N + 8, Hoisted.size())))) +
               "\n";
    for (unsigned R : Cells)
        if (R < P.Parameters)
            Out += Pad + Token(Registers[R]) + " = { " + Token(Registers[R]) + " }\n";
    if (NeedsVarargs)
    {
        if (!P.Vararg)
            throw Error("GETVARARGS in a non-variadic prototype");
        Out += Pad + "local " + Token(VarargsSymbol) + " = table.pack(...)\n";
        Declared.insert(VarargsSymbol);
    }
    if (NeedsPack)
    {
        Out += Pad + "local " + Token(PackSymbol) + " = { n = 0 }\n";
        Out += Pad + "local " + Token(TopSymbol) + " = 0\n";
        Declared.insert(PackSymbol);
        Declared.insert(TopSymbol);
    }
    bool FollowsLocals = !Nodes.empty() && Nodes.front().Type == Kind::Assign && !Nodes.front().Text.starts_with("function(");
    if (!Out.empty() && !Out.ends_with("\n\n") && !FollowsLocals)
        Out += '\n';
    Out += Render(Nodes, Indent, Declared, Hoist);
    return Out;
}
} // namespace

Result Decompile(std::string_view Bytecode, const Options &Settings)
{
    if (Settings.FilterLineField != "Line" && Settings.FilterLineField != "StartLine")
        throw Error("Filter field must be Line or StartLine");
    if (Settings.ModulePath.size() > 8192 ||
        std::any_of(Settings.ModulePath.begin(), Settings.ModulePath.end(), [](unsigned char C) { return C < 32 || C >= 127; }))
        throw Error("Module path must be a single-line ASCII expression with escaped names (maximum 8192 bytes)");
    if (Settings.IndentWidth < 1 || Settings.IndentWidth > 16)
        throw Error("Indent width must be between 1 and 16");
    auto C = ReadBytecode(Bytecode, Settings);
    Engine E(C, Settings);
    Function Main(E, C.Main, {}, 0);
    auto Source = Main.Generate(true);
    if (Settings.IncludeHeader)
    {
        Source = "-- Decompiled by Taze\n-- Luau bytecode v" + std::to_string(C.Version) + " | Types v" + std::to_string(C.Types) + "\n\n" +
                 Source;
    }
    E.Output.Source = E.Resolve(std::move(Source));
    if (E.Output.Source.size() > Settings.MaxOutputBytes)
        throw Error("Generated source exceeds output limit");
    E.Output.BytecodeVersion = C.Version;
    E.Output.TypeVersion = C.Types;
    E.Output.FunctionCount = C.Prototypes.size();
    E.Output.Encoding = C.Encoding;
    return std::move(E.Output);
}
} // namespace Taze
