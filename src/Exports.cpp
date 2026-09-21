#include "Exports.hpp"
#include "ControlFlow.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>

namespace Taze
{
Exports::Value Exports::Make(Expr::Kind Type, unsigned Id, std::string Key, std::vector<Value> Args)
{
    return std::make_shared<Expr>(Expr{Type, Id, std::move(Key), std::move(Args)});
}
Exports::Value Exports::Ref(unsigned Symbol)
{
    return Make(Expr::Reference, Symbol);
}
bool Exports::Same(Value A, Value B) const
{
    return A && B && A->Type == B->Type && A->Id == B->Id && A->Key == B->Key && A->Type != Expr::Unknown;
}
void Exports::Define(unsigned Symbol, Value Input, unsigned Frame, unsigned Pc)
{
    auto Site = std::make_pair(Frame, Pc);
    auto [It, New] = Definitions.emplace(Symbol, Input);
    if (New)
        Sites[Symbol] = Site;
    else if (Sites[Symbol] == Site)
        It->second = Input;
    else
        It->second = Make(Expr::Unknown);
}
void Exports::Alias(unsigned Symbol, unsigned Target)
{
    Definitions[Symbol] = Ref(Target);
}
void Exports::Closure(unsigned Symbol, unsigned Frame, unsigned Owner, unsigned Pc)
{
    Define(Symbol, Make(Expr::Function, Frame), Owner, Pc);
    Frames[Frame].Owner = Owner;
    Frames[Frame].CreationPc = Pc;
}

unsigned Exports::Record(const Prototype &P, unsigned Proto, const std::vector<std::vector<unsigned>> &Reads,
                         const std::vector<std::vector<unsigned>> &Written, const std::vector<unsigned> &Upvalues,
                         const std::vector<unsigned> &Parameters)
{
    unsigned FrameId = unsigned(Frames.size());
    Frames.push_back({Proto, Parameters, {}});
    ControlFlow Flow(P);
    auto Certain = [&](unsigned Index)
    {
        for (unsigned J = 0; J < P.Instructions.size(); ++J)
            if (P.Instructions[J].Op == LOP_RETURN && !Flow.Dominates(Index, J))
                return false;
        return true;
    };
    auto Literal = [&](unsigned K) -> Value
    {
        const auto &C = P.Constants.at(K);
        if (C.Tag == LBC_CONSTANT_STRING)
            return Make(Expr::Literal, 0, "s" + C.Text);
        if (C.Tag == LBC_CONSTANT_NUMBER)
            return Make(Expr::Literal, 0, "n" + Number(C.Number));
        if (C.Tag == LBC_CONSTANT_TABLE || C.Tag == LBC_CONSTANT_TABLE_WITH_CONSTANTS)
            return Make(Expr::Table, ++NextTable);
        if (C.Tag == LBC_CONSTANT_IMPORT)
        {
            if ((C.Index >> 30) == 1)
                return Make(Expr::Global, 0, P.Constants[(C.Index >> 20) & 1023].Text);
        }
        return Make(Expr::Unknown);
    };
    for (unsigned N = 0; N < P.Instructions.size(); ++N)
    {
        const auto &I = P.Instructions[N];
        if (Reads[N].empty())
            continue;
        auto R = [&](unsigned Reg) { return Ref(Reads[N].at(Reg)); };
        Value Output = Make(Expr::Unknown);
        switch (I.Op)
        {
        case LOP_NEWTABLE:
            Output = Make(Expr::Table, ++NextTable);
            break;
        case LOP_LOADN:
            Output = Make(Expr::Literal, 0, "n" + std::to_string(I.D));
            break;
        case LOP_DUPTABLE:
        case LOP_LOADK:
            Output = Literal(unsigned(I.D));
            break;
        case LOP_LOADKX:
            Output = Literal(I.Aux);
            break;
        case LOP_MOVE:
            Output = R(I.B);
            break;
        case LOP_GETUPVAL:
            if (I.B < Upvalues.size() && Upvalues[I.B] != std::numeric_limits<unsigned>::max())
                Output = Ref(Upvalues[I.B]);
            break;
        case LOP_GETGLOBAL:
            Output = Make(Expr::Global, 0, P.Constants[I.Aux].Text);
            break;
        case LOP_GETIMPORT:
            Output = Literal(unsigned(I.D));
            break;
        case LOP_GETTABLEKS:
        case LOP_GETUDATAKS:
            Output = Make(Expr::Member, 0, "s" + P.Constants[I.Op == LOP_GETUDATAKS ? I.Aux & 65535 : I.Aux].Text, {R(I.B)});
            break;
        case LOP_GETTABLEN:
            Output = Make(Expr::Member, 0, "n" + std::to_string(I.C + 1), {R(I.B)});
            break;
        case LOP_GETTABLE:
            Output = Make(Expr::Member, 0, {}, {R(I.B), R(I.C)});
            break;
        case LOP_SETTABLEKS:
        case LOP_SETUDATAKS:
            Writes.push_back(
                {R(I.B), R(I.A), "s" + P.Constants[I.Op == LOP_SETUDATAKS ? I.Aux & 65535 : I.Aux].Text, FrameId, I.Pc, Certain(N)});
            break;
        case LOP_SETTABLEN:
            Writes.push_back({R(I.B), R(I.A), "n" + std::to_string(I.C + 1), FrameId, I.Pc, Certain(N)});
            break;
        case LOP_SETTABLE:
            Writes.push_back({R(I.B), R(I.A), "*", FrameId, I.Pc, Certain(N), R(I.C)});
            break;
        case LOP_SETLIST:
            if (I.C)
                for (unsigned J = 0; J < I.C - 1; ++J)
                    Writes.push_back({R(I.A), R(I.B + J), "n" + std::to_string(I.Aux + J), FrameId, I.Pc, Certain(N)});
            else
                Writes.push_back({R(I.A), Make(Expr::Unknown), "*", FrameId, I.Pc, false});
            break;
        case LOP_NAMECALL:
        case LOP_NAMECALLUDATA:
            Output = Make(Expr::Member, 0, "s" + P.Constants[I.Op == LOP_NAMECALLUDATA ? I.Aux & 65535 : I.Aux].Text, {R(I.B)});
            break;
        case LOP_CALL:
        case LOP_CALLFB:
        {
            std::vector<Value> Args{R(I.A)};
            if (I.B)
                for (unsigned J = 1; J < I.B; ++J)
                    Args.push_back(R(I.A + J));
            Output = Make(Expr::Call, FrameId, {}, std::move(Args));
            Calls.push_back(Output);
            break;
        }
        case LOP_RETURN:
            if (I.B == 2 || I.B == 0)
                Frames[FrameId].Returns.emplace_back(I.Pc, R(I.A));
            else
                Frames[FrameId].Returns.emplace_back(I.Pc, Make(Expr::Unknown));
            break;
        default:
            break;
        }
        if (I.Op == LOP_NEWCLOSURE || I.Op == LOP_DUPCLOSURE)
            continue; // Bound when the child frame is generated.
        auto Changed = Taze::Writes(I, P);
        for (auto Reg : Changed)
            Define(Written[N].at(Reg),
                   Reg == I.A ? Output
                              : ((I.Op == LOP_NAMECALL || I.Op == LOP_NAMECALLUDATA) && Reg == I.A + 1 ? R(I.B) : Make(Expr::Unknown)),
                   FrameId, I.Pc);
    }
    return FrameId;
}

Exports::Value Exports::Resolve(Value Input, const Arguments &Args, unsigned Depth)
{
    if (!Input || Depth > 48 || ++Work > 200000)
        return Make(Expr::Unknown);
    if (!Resolving.insert(Input.get()).second)
        return Make(Expr::Unknown);
    struct Release
    {
        std::set<const Expr *> &Set;
        const Expr *Item;
        ~Release()
        {
            Set.erase(Item);
        }
    } Guard{Resolving, Input.get()};
    if (Input->Type == Expr::Reference)
    {
        if (auto It = Args.find(Input->Id); It != Args.end())
            return Resolve(It->second, {}, Depth + 1);
        auto It = Definitions.find(Input->Id);
        if (Args.empty())
            if (auto Found = Cache.find(Input->Id); Found != Cache.end())
                return Found->second;
        auto Result = It == Definitions.end() ? Make(Expr::Unknown) : Resolve(It->second, Args, Depth + 1);
        if (Args.empty() && Result->Type != Expr::Unknown)
            Cache[Input->Id] = Result;
        return Result;
    }
    if (Input->Type == Expr::Member)
    {
        auto Key = Input->Key;
        if (Key.empty())
        {
            auto Index = Resolve(Input->Args[1], Args, Depth + 1);
            if (Index->Type != Expr::Literal)
                return Make(Expr::Unknown);
            Key = Index->Key;
        }
        return Field(Resolve(Input->Args[0], Args, Depth + 1), Key, Args, Depth + 1);
    }
    if (Input->Type != Expr::Call)
        return Input;
    auto Callee = Resolve(Input->Args[0], Args, Depth + 1);
    if (Callee->Type == Expr::Global && Callee->Key == "setmetatable" && Input->Args.size() == 3)
    {
        auto Object = Resolve(Input->Args[1], Args, Depth + 1);
        auto Meta = Resolve(Input->Args[2], Args, Depth + 1);
        if (Object->Type != Expr::Table)
            return Make(Expr::Unknown);
        return Make(Expr::Table, Object->Id, {}, {Meta});
    }
    if (Callee->Type != Expr::Function)
        return Make(Expr::Unknown);
    ActiveFrames.insert(Callee->Id);
    const auto &Frame = Frames[Callee->Id];
    Arguments Bound = Args;
    for (unsigned J = 0; J < Frame.Parameters.size(); ++J)
        Bound[Frame.Parameters[J]] = J + 1 < Input->Args.size() ? Resolve(Input->Args[J + 1], Args, Depth + 1) : Make(Expr::Unknown);
    Value Result;
    for (const auto &[Pc, Return] : Frame.Returns)
    {
        auto Candidate = Resolve(Return, Bound, Depth + 1);
        if (Result && !Same(Result, Candidate))
            return Make(Expr::Unknown);
        Result = Candidate;
    }
    return Result ? Result : Make(Expr::Unknown);
}

Exports::Value Exports::Field(Value Table, const std::string &Key, const Arguments &Args, unsigned Depth)
{
    if (!Table || Table->Type != Expr::Table || Depth > 48 || ++Work > 200000)
        return Make(Expr::Unknown);
    if (EscapedTables.count(Table->Id))
        return Make(Expr::Unknown);
    Value Result;
    for (const auto &Write : Writes)
    {
        auto WrittenKey = Write.Key;
        if (Write.Index)
        {
            auto Index = Resolve(Write.Index, Args, Depth + 1);
            if (Index->Type == Expr::Literal)
                WrittenKey = Index->Key;
        }
        if ((WrittenKey != Key && WrittenKey != "*") || !ActiveFrames.count(Write.Frame))
            continue;
        auto Base = Resolve(Write.Base, Args, Depth + 1);
        if (!Same(Base, Table))
            continue;
        if (!Write.Certain || WrittenKey == "*")
            return Make(Expr::Unknown);
        auto Content = Resolve(Write.Content, Args, Depth + 1);
        if (Result && !Same(Result, Content))
            return Make(Expr::Unknown);
        Result = Content;
    }
    if (Result)
        return Result;
    if (!Table->Args.empty())
    {
        auto Index = Field(Table->Args[0], "s__index", Args, Depth + 1);
        if (Index->Type == Expr::Table)
            return Field(Index, Key, Args, Depth + 1);
    }
    return Make(Expr::Unknown);
}

std::map<unsigned, std::string> Exports::Find(unsigned Main)
{
    Work = 0;
    Cache.clear();
    Resolving.clear();
    std::map<unsigned, std::string> Result;
    if (Main >= Frames.size())
        return Result;
    ActiveFrames = {Main};
    EscapedTables.clear();
    for (unsigned Pass = 0; Pass < Frames.size(); ++Pass)
    {
        auto Before = ActiveFrames.size();
        for (const auto &Call : Calls)
        {
            if (!ActiveFrames.count(Call->Id))
                continue;
            auto Callee = Resolve(Call->Args[0], {});
            if (Callee->Type == Expr::Function)
                ActiveFrames.insert(Callee->Id);
            if (Callee->Type == Expr::Global && Callee->Key == "setmetatable")
                continue;
            for (unsigned J = 1; J < Call->Args.size(); ++J)
            {
                auto Arg = Resolve(Call->Args[J], {});
                if (Arg->Type == Expr::Table)
                    EscapedTables.insert(Arg->Id);
            }
        }
        if (ActiveFrames.size() == Before)
            break;
    }
    Cache.clear();
    std::set<std::string> Keys;
    for (const auto &Write : Writes)
    {
        auto Key = Write.Key;
        if (Write.Index)
        {
            auto Index = Resolve(Write.Index, {}, 0);
            if (Index->Type == Expr::Literal)
                Key = Index->Key;
        }
        if (Key != "s__index" && Key != "*")
            Keys.insert(Key);
    }
    std::vector<std::pair<unsigned, Value>> Returns;
    for (auto [Pc, Return] : Frames[Main].Returns)
        Returns.emplace_back(Pc, Resolve(Return, {}));
    std::function<void(Value, std::string, std::vector<std::string>, std::set<unsigned>, unsigned)> Walk =
        [&](Value Object, std::string Path, std::vector<std::string> Steps, auto Seen, unsigned Depth)
    {
        if (Work > 200000 || Depth > 8 || !Object)
            return;
        if (Object->Type == Expr::Function)
        {
            unsigned Parent = Object->Id, Anchor = 0;
            for (unsigned N = 0; N < 48 && Parent < Frames.size(); ++N)
            {
                Anchor = Frames[Parent].CreationPc;
                Parent = Frames[Parent].Owner;
                if (Parent == Main)
                    break;
            }
            for (const auto &[Pc, Return] : Returns)
            {
                if (Parent == Main && Pc < Anchor)
                    continue;
                auto Other = Return;
                for (const auto &Step : Steps)
                    Other = Field(Other, Step, {}, 0);
                if (!Same(Other, Object))
                    return;
            }
            auto Proto = Frames[Object->Id].Proto;
            auto It = Result.find(Proto);
            if (It == Result.end() || Path.size() < It->second.size())
                Result[Proto] = Path;
            return;
        }
        if (Object->Type != Expr::Table || !Seen.insert(Object->Id).second)
            return;
        for (const auto &Key : Keys)
        {
            auto Child = Field(Object, Key, {}, 0);
            if (Child->Type != Expr::Function && Child->Type != Expr::Table)
                continue;
            auto Name = Key.substr(1);
            static const std::set<std::string> Keywords = {"and", "break",    "do",     "else", "elseif", "end",   "false",
                                                           "for", "function", "if",     "in",   "local",  "nil",   "not",
                                                           "or",  "repeat",   "return", "then", "true",   "until", "while"};
            bool Identifier = !Name.empty() && (std::isalpha(static_cast<unsigned char>(Name[0])) || Name[0] == '_') &&
                              std::all_of(Name.begin(), Name.end(), [](unsigned char C) { return std::isalnum(C) || C == '_'; }) &&
                              !Keywords.count(Name);
            std::string Suffix = Key.starts_with('n') ? "[" + Name + "]" : Identifier ? "." + Name : "[" + Quote(Name) + "]";
            auto ChildSteps = Steps;
            ChildSteps.push_back(Key);
            Walk(Child, Path + Suffix, std::move(ChildSteps), Seen, Depth + 1);
        }
    };
    for (auto [Pc, Return] : Returns)
        Walk(Return, {}, {}, {}, 0);
    // Exhausting the bound leaves uncertainty; do not publish partial claims.
    if (Work > 200000)
        Result.clear();
    return Result;
}
} // namespace Taze
