#include "Bytecode.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
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
bool Atomic(const std::string &Text)
{
    if (Identifier(Text))
        return true;
    auto References = Tokens(Text);
    return References.size() == 1 && Text == Token(References[0]);
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
    bool Keep = false, Parameter = false, Cell = false;
    unsigned Owner = 0;
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
    unsigned Id, Depth;
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
    void BuildDataflow();

    unsigned SymbolAt(unsigned R, unsigned Pc, bool Write = false) const;
    std::string Read(unsigned R, unsigned Pc) const;
    std::string WriteName(unsigned R, unsigned Pc) const;
    std::string ConstantAt(unsigned Index, unsigned Recursion = 0) const;
    std::string Member(const std::string &Object, unsigned Index) const;
    std::string Global(unsigned Index) const;
    Statement Assign(unsigned R, const Instruction &I, std::string Value, bool Pure = false, bool Call = false);
    void Simple(const Instruction &I, std::vector<Statement> &Out);
    std::string Condition(const Instruction &I) const;
    std::vector<Statement> Region(unsigned Start, unsigned End, int BreakTarget = -1, int ContinueTarget = -1, unsigned Level = 0,
                                  bool IgnoreBackedge = false);
    std::vector<Statement> StateMachine();
    std::string Closure(const Instruction &I);
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
    }
    unsigned New(std::string Name, unsigned Owner, bool Keep = false, bool Parameter = false, bool Cell = false)
    {
        if (Name.starts_with("Value") && Name.size() > 5 &&
            std::all_of(Name.begin() + 5, Name.end(), [](unsigned char C) { return std::isdigit(C); }))
            Name = "Value" + std::to_string(++NextValue);
        Name = Pascal(Name);
        std::string Base = Name;
        unsigned Suffix = 2;
        while (Names.count(Name))
            Name = Base + std::to_string(Suffix++);
        Names.insert(Name);
        unsigned Id = unsigned(Symbols.size());
        Symbols.push_back({Name, Keep, Parameter, Cell, Owner});
        return Id;
    }
    std::string Resolve(std::string Text) const
    {
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
    : E(Owner), P(E.C.Prototypes.at(Proto)), Id(Proto), Depth(Nesting), Upvalues(std::move(Captures))
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

std::string Function::Closure(const Instruction &I)
{
    unsigned Child = I.Op == LOP_NEWCLOSURE ? P.Children.at(unsigned(I.D)) : P.Constants.at(unsigned(I.D)).Index;
    std::vector<Upvalue> Captures;
    std::vector<std::string> Arguments, Bindings;
    std::string Self;
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
                Self = Token(E.New(U.Name, Child, true, true));
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
        unsigned Capture = E.New(U.Name, Child, true, true, U.Reference);
        Bindings.push_back(Token(Capture));
        Arguments.push_back(Argument);
        U.Cell = U.Reference ? Token(Capture) : "";
        U.Value = Token(Capture) + (U.Reference ? "[1]" : "");
        Captures.push_back(std::move(U));
    }
    Function Nested(E, Child, std::move(Captures), Depth + 1);
    std::string Body = Nested.Generate();
    std::string Text = "function" + Nested.Signature();
    if (E.Settings.IncludeLineComments)
        Text += " -- Line: " + (E.C.Prototypes[Child].Line ? std::to_string(E.C.Prototypes[Child].Line) : "Unknown");
    Text += "\n" + Body + "end";
    if (!Self.empty())
    {
        Text = "local function " + Self + Text.substr(8) + "\nreturn " + Self;
        std::string Wrapped = "(function(" + Join(Bindings) + ")\n";
        std::size_t Start = 0;
        do
        {
            auto End = Text.find('\n', Start);
            Wrapped += std::string(E.Settings.IndentWidth, ' ') + Text.substr(Start, End == std::string::npos ? End : End - Start) + "\n";
            if (End == std::string::npos)
                break;
            Start = End + 1;
        } while (Start < Text.size());
        return Wrapped + "end)(" + Join(Arguments) + ")";
    }
    if (!Arguments.empty())
    {
        std::string Wrapped = "(function(" + Join(Bindings) + ")\n";
        Wrapped += std::string(E.Settings.IndentWidth, ' ') + "return " + Text;
        // Indent all lines of the returned function by the wrapper indentation.
        auto FirstNewline = Wrapped.find('\n') + 1;
        auto Next = Wrapped.find('\n', FirstNewline);
        while (Next != std::string::npos)
        {
            Wrapped.insert(Next + 1, E.Settings.IndentWidth, ' ');
            Next = Wrapped.find('\n', Next + 1 + E.Settings.IndentWidth);
        }
        return Wrapped + "\nend)(" + Join(Arguments) + ")";
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
        if (!E.C.Prototypes[Child].Name.empty() && E.Symbols[S].Name.starts_with("Value"))
        {
            unsigned Nice = E.New(E.C.Prototypes[Child].Name, Id, true);
            E.Symbols[S].Name = E.Symbols[Nice].Name;
        }
        Set(Closure(I));
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
    while (Pos < Limit)
    {
        const auto &I = P.Instructions[Pos];
        // A backedge to this instruction defines a while/repeat region. Choose the outermost backedge.
        if (!IgnoreBackedge)
        {
            int Back = -1;
            for (unsigned J = Pos + 1; J < Limit; ++J)
                if (P.Instructions[J].Target() == int(I.Pc) && (Jump(P.Instructions[J].Op) || Conditional(P.Instructions[J].Op)))
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
                if (Target <= int(I.Pc) || Target > int(End))
                    throw Unstructured("conditional crosses region");
                unsigned ElseIndex = Index(unsigned(Target));
                int JoinPc = Target;
                unsigned ThenEnd = unsigned(Target);
                if (ElseIndex > Pos + 1)
                {
                    const auto &BeforeElse = P.Instructions[ElseIndex - 1];
                    if (Jump(BeforeElse.Op) && BeforeElse.Target() > Target && BeforeElse.Target() <= int(End))
                    {
                        JoinPc = BeforeElse.Target();
                        ThenEnd = BeforeElse.Pc;
                    }
                }
                Branch.Text = "not (" + Branch.Text + ")";
                Branch.Body = Region(I.Next, ThenEnd, BreakTarget, ContinueTarget, Level + 1);
                if (JoinPc != Target)
                    Branch.Else = Region(unsigned(Target), unsigned(JoinPc), BreakTarget, ContinueTarget, Level + 1);
                Out.push_back(std::move(Branch));
                Pos = Index(unsigned(JoinPc));
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
            if (Target < int(I.Next) || Target > int(End))
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
                        if (E.Symbols[Dependency].Cell || WritesCount[Dependency] > 1)
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
                                    std::string(".:[").find(Nodes[J].Text[Position]) != std::string::npos && !Atomic(Replacement) &&
                                    (Replacement.empty() || Replacement.front() != '('))
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
                        if (E.Symbols[Dependency].Cell)
                            CanReorder = false;
                    bool Ordered = CanReorder || BeforeUse.find_first_of(")[.:+-*/%^#") == std::string::npos;
                    // Adjacent substitution cannot cross a side effect. Do not change a call's arity by inlining a multi-result call.
                    if (Ordered && std::count(Uses.begin(), Uses.end(), Id) == 1 && Next.Type != Kind::While && Next.Type != Kind::Repeat &&
                        Next.Type != Kind::Function)
                    {
                        std::string Replacement = S.Text;
                        if (S.Call)
                            Replacement = "(" + Replacement + ")";
                        auto Position = Next.Text.find(Token(Id)) + Token(Id).size();
                        if (Position < Next.Text.size() && std::string(".:[").find(Next.Text[Position]) != std::string::npos &&
                            !Atomic(Replacement) && (Replacement.empty() || Replacement.front() != '('))
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
            while (N + 1 < List.size() && List[N + 1].Type == Kind::Raw)
            {
                auto Text = List[N + 1].Text;
                if (!Text.starts_with(Table))
                    break;
                auto Equal = Text.find(" = ");
                if (Equal == std::string::npos || Text.find('\n') != std::string::npos)
                    break;
                auto Right = Text.substr(Equal + 3);
                if (Right.find(Table) != std::string::npos)
                    break;
                auto Key = Text.substr(Table.size(), Equal - Table.size());
                if (Key.starts_with('.'))
                    Key.erase(0, 1);
                else if (!(Key.starts_with('[') && Key.ends_with(']')))
                    break;
                Fields.push_back(Key + " = " + Right);
                List.erase(List.begin() + std::ptrdiff_t(N + 1));
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
        // A statement beginning with '(' can otherwise attach to the previous call in Luau's grammar.
        if (!Text.empty() && Text.front() == '(' && !Out.empty())
        {
            auto End = Out.find_last_not_of("\n ");
            if (End != std::string::npos && Out[End] != ';')
                Out.insert(End + 1, ";");
        }
        std::size_t Start = 0;
        do
        {
            auto End = Text.find('\n', Start);
            Out += Pad + Text.substr(Start, End == std::string::npos ? End : End - Start) + '\n';
            if (End == std::string::npos)
                break;
            Start = End + 1;
        } while (Start < Text.size());
    };
    for (std::size_t N = 0; N < Nodes.size(); ++N)
    {
        const auto &S = Nodes[N];
        bool Block = !S.Body.empty() || S.Text.find("function(") != std::string::npos;
        if (Block && !Out.empty() && !Out.ends_with("\n\n"))
            Out += '\n';
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
                Line((AllNew ? "local " : "") + Join(Names) + " = " + S.Text);
            for (auto Id : S.Left)
                Declared.insert(Id);
            break;
        }
        case Kind::Raw:
            Line(S.Text);
            break;
        case Kind::Return:
            Line(S.Text.empty() ? "return" : "return " + S.Text);
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
                Line("if " + S.Text + " then");
            if (S.Type == Kind::While)
                Line("while " + S.Text + " do");
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
                Line("else");
                InnerDeclared = Declared;
                Out += Render(S.Else, Indent + 1, InnerDeclared, Hoist);
            }
            Line(S.Type == Kind::Repeat ? "until " + S.Text : "end");
            break;
        }
        default:
            throw Error("Internal: unsupported source statement");
        }
        if (Block && N + 1 < Nodes.size())
            Out += '\n';
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
    Optimize(Nodes);
    // Remove the compiler's implicit empty return at the end of a chunk/function.
    if (!Nodes.empty() && Nodes.back().Type == Kind::Return && Nodes.back().Text.empty())
        Nodes.pop_back();

    std::set<unsigned> Referenced, Assigned, NestedAssigned, TopAssigned, ReadBeforeWrite;
    std::function<void(const std::vector<Statement> &, unsigned)> Inspect = [&](const auto &List, unsigned Level)
    {
        for (const auto &S : List)
        {
            for (auto U : Tokens(S.Text))
            {
                Referenced.insert(U);
                if (!Assigned.count(U))
                    ReadBeforeWrite.insert(U);
            }
            for (auto U : S.Left)
            {
                Assigned.insert(U);
                Referenced.insert(U);
                (Level ? NestedAssigned : TopAssigned).insert(U);
            }
            Inspect(S.Body, Level + 1);
            Inspect(S.Else, Level + 1);
        }
    };
    Inspect(Nodes, 0);
    std::set<unsigned> Hoist, Declared;
    for (auto U : Parameters)
        Declared.insert(U);
    for (auto U : Referenced)
    {
        const auto &S = E.Symbols[U];
        if (S.Owner == Id && !S.Parameter && (NestedAssigned.count(U) || ReadBeforeWrite.count(U) || S.Cell))
            Hoist.insert(U);
    }
    if (Dispatch)
        for (auto U : Registers)
            if (Referenced.count(U) && !E.Symbols[U].Parameter)
                Hoist.insert(U);
    unsigned Indent = Main ? 0 : 1;
    std::string Pad(Indent * E.Settings.IndentWidth, ' '), Out;
    if (E.Settings.IncludeUpvalueComments && !Upvalues.empty())
    {
        Out += Pad + "--[[\n";
        for (unsigned J = 0; J < Upvalues.size(); ++J)
            Out += Pad + std::string(E.Settings.IndentWidth, ' ') + std::to_string(J + 1) + ": " + Upvalues[J].Name + " (type \"" +
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
    if (!Out.empty() && !Out.ends_with("\n\n"))
        Out += '\n';
    Out += Render(Nodes, Indent, Declared, Hoist);
    return Out;
}
} // namespace

Result Decompile(std::string_view Bytecode, const Options &Settings)
{
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
