#pragma once

#include "Bytecode.hpp"
#include <bitset>

namespace Taze
{
// Instruction indices are graph nodes; the final node is the common function exit.
struct ControlFlow
{
    std::vector<std::vector<unsigned>> Successors, Predecessors;
    std::vector<int> Dominators, PostDominators;
    std::vector<std::bitset<256>> LiveIn;
    std::vector<unsigned> Blocks;

    explicit ControlFlow(const Prototype &Proto);
    bool Dominates(unsigned Header, unsigned Node) const;
};

std::vector<unsigned> Reads(const Instruction &I, const Prototype &P);
} // namespace Taze
