#pragma once

#include <cstdint>

#include "Luau/Bytecode.h"
#include "Luau/BytecodeUtils.h"
#include "Taze/Decompiler.hpp"
#include <array>
#include <map>
#include <utility>

namespace Taze
{
struct Constant
{
    unsigned Tag = 0;
    std::string Text;
    double Number = 0;
    std::array<double, 4> Vector{};
    std::uint64_t Integer = 0;
    bool Negative = false;
    std::uint32_t Index = 0;
    std::vector<std::pair<unsigned, int>> Entries;
};

struct Local
{
    std::string Name;
    unsigned Start = 0, End = 0, Register = 0;
};

struct Instruction
{
    LuauOpcode Op = LOP_NOP;
    unsigned Pc = 0, Next = 0;
    unsigned A = 0, B = 0, C = 0;
    int D = 0, E = 0;
    std::uint32_t Aux = 0;
    std::vector<std::pair<unsigned, unsigned>> Captures;
    int Target() const;
};

struct Prototype
{
    unsigned Stack = 0, Parameters = 0, Upvalues = 0, Flags = 0;
    bool Vararg = false;
    unsigned Line = 0;
    std::string Name;
    std::vector<std::uint32_t> Code;
    std::vector<Constant> Constants;
    std::vector<unsigned> Children;
    std::vector<unsigned> Lines;
    std::vector<Local> Locals;
    std::vector<std::string> UpvalueNames;
    std::vector<Instruction> Instructions;
    std::map<unsigned, unsigned> PcToInstruction;
};

struct Chunk
{
    unsigned Version = 0, Types = 0, Main = 0;
    OpcodeEncoding Encoding = OpcodeEncoding::Plain;
    std::vector<Prototype> Prototypes;
};

Chunk ReadBytecode(std::string_view Data, const Options &Settings);
std::string Quote(std::string_view Text);
std::string Number(double Value);
std::string OpcodeName(LuauOpcode Op);
std::vector<unsigned> Writes(const Instruction &I, const Prototype &P);
} // namespace Taze
