#include "Luau/Compiler.h"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int Argc, char **Argv)
{
    if (Argc < 3)
    {
        std::cerr << "Usage: taze-compile INPUT.luau OUTPUT.luac [optimization 0..2] [debug 0..2]\n";
        return 1;
    }
    try
    {
        std::ifstream In(Argv[1], std::ios::binary);
        if (!In)
            throw std::runtime_error("Cannot open source");
        std::string Source((std::istreambuf_iterator<char>(In)), {});
        Luau::CompileOptions Options;
        Options.optimizationLevel = Argc > 3 ? std::stoi(Argv[3]) : 1;
        Options.debugLevel = Argc > 4 ? std::stoi(Argv[4]) : 2;
        std::string Bytes = Luau::compile(Source, Options);
        if (Bytes.empty() || Bytes[0] == 0)
            throw std::runtime_error(Bytes.substr(1));
        std::ofstream Out(Argv[2], std::ios::binary);
        if (!Out || !Out.write(Bytes.data(), std::streamsize(Bytes.size())))
            throw std::runtime_error("Cannot write bytecode");
    }
    catch (const std::exception &Error)
    {
        std::cerr << Error.what() << '\n';
        return 1;
    }
}
