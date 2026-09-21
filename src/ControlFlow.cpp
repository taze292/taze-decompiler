#include "ControlFlow.hpp"

#include <algorithm>
#include <queue>

namespace Taze
{
std::vector<unsigned> Reads(const Instruction &I, const Prototype &P)
{
    std::vector<unsigned> Result;
    auto Range = [&](unsigned First, unsigned End)
    {
        for (unsigned R = First; R < std::min(End, P.Stack); ++R)
            Result.push_back(R);
    };
    switch (I.Op)
    {
    case LOP_MOVE:
    case LOP_GETTABLEKS:
    case LOP_GETUDATAKS:
    case LOP_GETTABLEN:
    case LOP_NOT:
    case LOP_MINUS:
    case LOP_LENGTH:
    case LOP_NAMECALL:
    case LOP_NAMECALLUDATA:
        return {I.B};
    case LOP_SETGLOBAL:
    case LOP_SETUPVAL:
    case LOP_JUMPIF:
    case LOP_JUMPIFNOT:
    case LOP_JUMPXEQKNIL:
    case LOP_JUMPXEQKB:
    case LOP_JUMPXEQKN:
    case LOP_JUMPXEQKS:
    case LOP_CMPPROTO:
        return {I.A};
    case LOP_GETTABLE:
    case LOP_ADD:
    case LOP_SUB:
    case LOP_MUL:
    case LOP_DIV:
    case LOP_MOD:
    case LOP_POW:
    case LOP_AND:
    case LOP_OR:
    case LOP_IDIV:
        return {I.B, I.C};
    case LOP_SETTABLE:
        return {I.A, I.B, I.C};
    case LOP_SETTABLEKS:
    case LOP_SETUDATAKS:
    case LOP_SETTABLEN:
        return {I.A, I.B};
    case LOP_ADDK:
    case LOP_SUBK:
    case LOP_MULK:
    case LOP_DIVK:
    case LOP_MODK:
    case LOP_POWK:
    case LOP_ANDK:
    case LOP_ORK:
    case LOP_IDIVK:
        return {I.B};
    case LOP_SUBRK:
    case LOP_DIVRK:
        return {I.C};
    case LOP_JUMPIFEQ:
    case LOP_JUMPIFLE:
    case LOP_JUMPIFLT:
    case LOP_JUMPIFNOTEQ:
    case LOP_JUMPIFNOTLE:
    case LOP_JUMPIFNOTLT:
        return {I.A, I.Aux};
    case LOP_CONCAT:
        Range(I.B, I.C + 1);
        break;
    case LOP_CALL:
    case LOP_CALLFB:
        Range(I.A, I.B ? I.A + I.B : P.Stack);
        break;
    case LOP_RETURN:
        Range(I.A, I.B ? I.A + I.B - 1 : P.Stack);
        break;
    case LOP_SETLIST:
        Result.push_back(I.A);
        Range(I.B, I.C ? I.B + I.C - 1 : P.Stack);
        break;
    case LOP_FORNPREP:
    case LOP_FORNLOOP:
    case LOP_FORGPREP:
    case LOP_FORGPREP_NEXT:
    case LOP_FORGPREP_INEXT:
    case LOP_FORGLOOP:
        Range(I.A, I.A + 3);
        break;
    case LOP_NEWCLOSURE:
    case LOP_DUPCLOSURE:
        for (auto [Kind, Register] : I.Captures)
            if (Kind != LCT_UPVAL && !(Kind == LCT_VAL && Register == I.A))
                Result.push_back(Register);
        break;
    case LOP_CLOSEUPVALS:
        Range(I.A, P.Stack);
        break;
    case LOP_NEWCLASSMEMBER:
        return {I.A, I.C};
    case LOP_NEWCLASS:
        if (I.B != 255)
            Result.push_back(I.B);
        break;
    default:
        break;
    }
    return Result;
}

namespace
{
std::vector<int> ImmediateDominators(const std::vector<std::vector<unsigned>> &Edges, const std::vector<std::vector<unsigned>> &Reverse,
                                     unsigned Root)
{
    std::vector<bool> Seen(Edges.size());
    std::vector<unsigned> Order;
    std::vector<std::pair<unsigned, unsigned>> Stack{{Root, 0}};
    Seen[Root] = true;
    while (!Stack.empty())
    {
        auto &[Node, Next] = Stack.back();
        if (Next == Edges[Node].size())
        {
            Order.push_back(Node);
            Stack.pop_back();
            continue;
        }
        unsigned Target = Edges[Node][Next++];
        if (!Seen[Target])
        {
            Seen[Target] = true;
            Stack.emplace_back(Target, 0);
        }
    }
    std::reverse(Order.begin(), Order.end());
    std::vector<unsigned> Rank(Edges.size());
    for (unsigned I = 0; I < Order.size(); ++I)
        Rank[Order[I]] = I;
    std::vector<int> Parent(Edges.size(), -1);
    Parent[Root] = int(Root);
    auto Intersect = [&](unsigned A, unsigned B)
    {
        while (A != B)
        {
            while (Rank[A] > Rank[B])
                A = unsigned(Parent[A]);
            while (Rank[B] > Rank[A])
                B = unsigned(Parent[B]);
        }
        return A;
    };
    bool Changed = true;
    while (Changed)
    {
        Changed = false;
        for (unsigned Node : Order)
        {
            if (Node == Root)
                continue;
            int Common = -1;
            for (unsigned Before : Reverse[Node])
                if (Parent[Before] != -1)
                    Common = Common == -1 ? int(Before) : int(Intersect(unsigned(Common), Before));
            if (Parent[Node] != Common)
            {
                Parent[Node] = Common;
                Changed = true;
            }
        }
    }
    return Parent;
}
} // namespace

ControlFlow::ControlFlow(const Prototype &P)
{
    unsigned Count = unsigned(P.Instructions.size());
    if (std::uint64_t(Count) * std::max(1u, P.Stack) > 16000000)
        throw Error("Function control-flow exceeds analysis limit");
    Successors.resize(Count + 1);
    Predecessors.resize(Count + 1);
    std::vector<bool> Leader(Count + 1);
    Leader[0] = true;
    for (unsigned J = 0; J < Count; ++J)
    {
        const auto &I = P.Instructions[J];
        auto Add = [&](unsigned Target)
        {
            Successors[J].push_back(Target);
            Predecessors[Target].push_back(J);
        };
        bool Stop = I.Op == LOP_JUMP || I.Op == LOP_JUMPBACK || I.Op == LOP_JUMPX || I.Op == LOP_RETURN || I.Op == LOP_FORGPREP ||
                    I.Op == LOP_FORGPREP_NEXT || I.Op == LOP_FORGPREP_INEXT || (I.Op == LOP_LOADB && I.C);
        if (I.Target() >= 0)
        {
            auto Target = P.PcToInstruction.at(unsigned(I.Target()));
            Add(Target);
            Leader[Target] = true;
            Leader[J + 1] = true;
        }
        if (I.Op == LOP_RETURN)
            Add(Count);
        else if (!Stop)
            Add(J + 1);
        if (Stop)
            Leader[J + 1] = true;
    }
    unsigned Block = 0;
    for (unsigned J = 0; J < Count; ++J)
    {
        if (J && Leader[J])
            ++Block;
        Blocks.push_back(Block);
    }
    Dominators = ImmediateDominators(Successors, Predecessors, 0);
    PostDominators = ImmediateDominators(Predecessors, Successors, Count);

    LiveIn.resize(Count + 1);
    std::vector<std::bitset<256>> Uses(Count), Definitions(Count);
    std::queue<unsigned> Queue;
    std::vector<bool> Queued(Count, true);
    for (unsigned J = 0; J < Count; ++J)
    {
        for (auto R : Reads(P.Instructions[J], P))
            Uses[J].set(R);
        for (auto R : Writes(P.Instructions[J], P))
            Definitions[J].set(R);
        Queue.push(Count - J - 1);
    }
    while (!Queue.empty())
    {
        auto J = Queue.front();
        Queue.pop();
        Queued[J] = false;
        std::bitset<256> Out;
        for (auto Next : Successors[J])
            Out |= LiveIn[Next];
        auto In = Uses[J] | (Out & ~Definitions[J]);
        if (In == LiveIn[J])
            continue;
        LiveIn[J] = In;
        for (auto Before : Predecessors[J])
            if (!Queued[Before])
            {
                Queued[Before] = true;
                Queue.push(Before);
            }
    }
}

bool ControlFlow::Dominates(unsigned Header, unsigned Node) const
{
    while (Node != Header && Dominators[Node] >= 0 && unsigned(Dominators[Node]) != Node)
        Node = unsigned(Dominators[Node]);
    return Node == Header;
}
} // namespace Taze
