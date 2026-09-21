#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Taze
{
enum class OpcodeEncoding
{
    Plain,
    Roblox,
    Auto
};

struct Options
{
    OpcodeEncoding Encoding = OpcodeEncoding::Auto;
    unsigned IndentWidth = 4;
    bool IncludeHeader = true;
    bool IncludeLineComments = true;
    bool IncludeUpvalueComments = true;
    bool IncludeFunctionLocators = true;
    bool ForceStateMachine = false;
    std::string FilterLineField = "Line";
    std::string ModulePath;
    std::size_t MaxInputBytes = 64 * 1024 * 1024;
    std::size_t MaxOutputBytes = 64 * 1024 * 1024;
};

struct Result
{
    std::string Source;
    std::vector<std::string> Diagnostics;
    unsigned BytecodeVersion = 0;
    unsigned TypeVersion = 0;
    std::size_t FunctionCount = 0;
    std::size_t StateMachineFunctions = 0;
    OpcodeEncoding Encoding = OpcodeEncoding::Plain;
};

class Error : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

Result Decompile(std::string_view Bytecode, const Options &Settings = {});
std::string Disassemble(std::string_view Bytecode, const Options &Settings = {});
} // namespace Taze
