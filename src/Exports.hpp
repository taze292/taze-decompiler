#pragma once

#include "Bytecode.hpp"
#include <memory>
#include <set>

namespace Taze
{
// A bounded symbolic graph for export annotations only. It never executes module code.
class Exports
{
    struct Expr;
    using Value = std::shared_ptr<Expr>;
    struct Expr
    {
        enum Kind
        {
            Unknown,
            Reference,
            Table,
            Function,
            Global,
            Literal,
            Member,
            Call
        } Type = Unknown;
        unsigned Id = 0;
        std::string Key;
        std::vector<Value> Args;
    };
    struct Frame
    {
        unsigned Proto;
        std::vector<unsigned> Parameters;
        std::vector<std::pair<unsigned, Value>> Returns;
        unsigned Owner = ~0u, CreationPc = 0;
    };
    struct Write
    {
        Value Base, Content;
        std::string Key;
        unsigned Frame, Pc;
        bool Certain = true;
        Value Index = {};
    };
    std::vector<Frame> Frames;
    std::map<unsigned, Value> Definitions;
    std::map<unsigned, std::pair<unsigned, unsigned>> Sites;
    std::vector<Write> Writes;
    std::vector<Value> Calls;
    std::set<unsigned> ActiveFrames, EscapedTables;
    unsigned NextTable = 0;
    unsigned Work = 0;
    std::set<const Expr *> Resolving;
    std::map<unsigned, Value> Cache;
    using Arguments = std::map<unsigned, Value>;
    Value Make(Expr::Kind Type, unsigned Id = 0, std::string Key = {}, std::vector<Value> Args = {});
    Value Ref(unsigned Symbol);
    Value Resolve(Value Input, const Arguments &Args, unsigned Depth = 0);
    Value Field(Value Table, const std::string &Key, const Arguments &Args, unsigned Depth);
    void Define(unsigned Symbol, Value Input, unsigned Frame, unsigned Pc);
    bool Same(Value A, Value B) const;

  public:
    unsigned Record(const Prototype &P, unsigned Proto, const std::vector<std::vector<unsigned>> &Reads,
                    const std::vector<std::vector<unsigned>> &Writes, const std::vector<unsigned> &Upvalues,
                    const std::vector<unsigned> &Parameters);
    void Closure(unsigned Symbol, unsigned Frame, unsigned Owner, unsigned Pc);
    void Alias(unsigned Symbol, unsigned Target);
    std::map<unsigned, std::string> Find(unsigned Main);
};
} // namespace Taze
