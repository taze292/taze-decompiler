#include "Taze/Decompiler.hpp"
#include "WebSocketServer.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int main(int Argc, char **Argv)
{
    try
    {
        Taze::Options Settings;
        std::string Input, Output;
        bool Assembly = false;
        bool Server = false;
        unsigned Port = 8877;
        for (int I = 1; I < Argc; ++I)
        {
            std::string Arg = Argv[I];
            auto Value = [&]() -> std::string
            {
                if (++I == Argc)
                    throw Taze::Error("Missing value for " + Arg);
                return Argv[I];
            };
            if (Arg == "--help" || Arg == "-h")
            {
                std::cout << "Taze Luau decompiler\nUsage: taze INPUT.luac [-o OUTPUT.luau] [options]\n\n"
                             "  --serve                      Start the local WebSocket bridge (Windows)\n"
                             "  --port N                     Bridge port (default: 8877)\n"
                             "  --encoding auto|plain|roblox   Decode opcode bytes (default: auto)\n"
                             "  --disassemble                Print bytecode instructions\n"
                             "  --state-machine              Force explicit control-flow lowering\n"
                             "  --indent N                   Spaces per indentation level (1..16)\n"
                             "  --no-header                  Omit generator header\n"
                             "  --no-lines                   Omit function line comments\n"
                             "  --no-upvalues                Omit capture comments\n";
                return 0;
            }
            if (Arg == "-o" || Arg == "--output")
                Output = Value();
            else if (Arg == "--encoding")
            {
                auto Encoding = Value();
                if (Encoding == "plain")
                    Settings.Encoding = Taze::OpcodeEncoding::Plain;
                else if (Encoding == "roblox")
                    Settings.Encoding = Taze::OpcodeEncoding::Roblox;
                else if (Encoding != "auto")
                    throw Taze::Error("Unknown encoding " + Encoding);
            }
            else if (Arg == "--indent")
                Settings.IndentWidth = unsigned(std::stoul(Value()));
            else if (Arg == "--no-header")
                Settings.IncludeHeader = false;
            else if (Arg == "--no-lines")
                Settings.IncludeLineComments = false;
            else if (Arg == "--no-upvalues")
                Settings.IncludeUpvalueComments = false;
            else if (Arg == "--state-machine")
                Settings.ForceStateMachine = true;
            else if (Arg == "--disassemble")
                Assembly = true;
            else if (Arg == "--serve")
                Server = true;
            else if (Arg == "--port")
            {
                auto Text = Value();
                std::size_t Used = 0;
                auto Number = std::stoul(Text, &Used);
                if (Used != Text.size() || Number == 0 || Number > 65535)
                    throw Taze::Error("Invalid port");
                Port = unsigned(Number);
            }
            else if (Arg.starts_with('-'))
                throw Taze::Error("Unknown option " + Arg);
            else if (Input.empty())
                Input = Arg;
            else
                throw Taze::Error("Expected exactly one input file");
        }
        if (Server)
        {
            if (!Input.empty() || !Output.empty() || Assembly)
                throw Taze::Error("--serve cannot be combined with file input/output or --disassemble");
            if (Settings.IndentWidth < 1 || Settings.IndentWidth > 16)
                throw Taze::Error("Indent must be between 1 and 16");
            return Taze::ServeWebSocket(Port, Settings);
        }
        if (Input.empty())
            throw Taze::Error("Usage: taze INPUT.luac [-o OUTPUT.luau]; see --help");
        if (std::filesystem::file_size(Input) > Settings.MaxInputBytes)
            throw Taze::Error("Input exceeds 64 MiB limit");
        std::ifstream File(Input, std::ios::binary);
        if (!File)
            throw Taze::Error("Cannot open input: " + Input);
        std::string Bytes((std::istreambuf_iterator<char>(File)), {});
        std::string Source;
        if (Assembly)
            Source = Taze::Disassemble(Bytes, Settings);
        else
        {
            auto Result = Taze::Decompile(Bytes, Settings);
            Source = std::move(Result.Source);
            for (const auto &Diagnostic : Result.Diagnostics)
                std::cerr << "taze: " << Diagnostic << '\n';
        }
        if (Output.empty())
            std::cout << Source;
        else
        {
            if (std::filesystem::exists(Output) && std::filesystem::equivalent(Input, Output))
                throw Taze::Error("Input and output must be different files");
            std::ofstream Out(Output, std::ios::binary);
            if (!Out || !Out.write(Source.data(), std::streamsize(Source.size())))
                throw Taze::Error("Cannot write output: " + Output);
        }
        return 0;
    }
    catch (const std::exception &Error)
    {
        std::cerr << "taze: " << Error.what() << '\n';
        return 1;
    }
}
