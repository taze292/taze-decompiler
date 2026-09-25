#include "Luau/BytecodeBuilder.h"
#include "Luau/BytecodeUtils.h"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include "Taze/Decompiler.hpp"
#include "lua.h"
#include "lualib.h"
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

class RobloxEncoder : public Luau::BytecodeEncoder
{
  public:
    void encode(uint32_t *Words, size_t Count) override
    {
        for (size_t Pc = 0; Pc < Count;)
        {
            auto Opcode = LuauOpcode(Words[Pc] & 255);
            Words[Pc] = (Words[Pc] & ~255u) | ((unsigned(Opcode) * 227) & 255);
            Pc += Luau::getOpLength(Opcode);
        }
    }
};

std::string Execute(const std::string &Bytes)
{
    std::unique_ptr<lua_State, decltype(&lua_close)> L(luaL_newstate(), lua_close);
    luaL_openlibs(L.get());
    unsigned Budget = 100000;
    lua_callbacks(L.get())->userdata = &Budget;
    lua_callbacks(L.get())->interrupt = [](lua_State *State, int Gc)
    {
        if (Gc < 0 && --*static_cast<unsigned *>(lua_callbacks(State)->userdata) == 0)
            luaL_error(State, "Fixture execution budget exceeded");
    };
    if (luau_load(L.get(), "test", Bytes.data(), Bytes.size(), 0))
        throw std::runtime_error(lua_tostring(L.get(), -1));
    if (lua_pcall(L.get(), 0, LUA_MULTRET, 0))
        throw std::runtime_error(lua_tostring(L.get(), -1));
    std::string Values;
    for (int I = 1; I <= lua_gettop(L.get()); ++I)
    {
        Values += std::to_string(lua_type(L.get(), I)) + ":";
        if (lua_isboolean(L.get(), I))
            Values += lua_toboolean(L.get(), I) ? "true" : "false";
        else if (lua_isnumber(L.get(), I))
        {
            char Buffer[64];
            std::snprintf(Buffer, sizeof(Buffer), "%.17g", lua_tonumber(L.get(), I));
            Values += Buffer;
        }
        else if (lua_isstring(L.get(), I))
        {
            size_t Length = 0;
            auto Text = lua_tolstring(L.get(), I, &Length);
            Values.append(Text, Length);
        }
        else if (!lua_isnil(L.get(), I))
            throw std::runtime_error("Fixture must return scalar values");
        Values += "|";
    }
    return Values;
}

int main()
{
    const std::vector<std::pair<std::string, std::string>> Fixtures = {
        {"arithmetic", "local x, y = 12, 5; return x+y,x-y,x*y,x/y,x%y,x^y,x//y,-x,not y"},
        {"branch",
         "local function f(x) local result; if x < 3 then result = x * 2 else result = x + 5 end return result end return f(2), f(8)"},
        {"numeric_for", "local sum=0; for i=1,10 do sum+=i end; for i=5,1,-2 do sum+=i end return sum"},
        {"while", "local n=0; local sum=0; while n<9 do n+=1; if n==3 then continue end; if n==7 then break end; sum+=n end return n,sum"},
        {"repeat", "local n=0; repeat n+=1 until n>=5; return n"},
        {"generic", "local sum=0; for k,v in ipairs({3,4,5}) do sum+=k*v end; for k,v in {a=2,b=8} do sum+=v end return sum"},
        {"closure_copy", "local seed=17; local function add(x) return seed+x end return add(5)"},
        {"closure_ref", "local n=0; local function inc(x) n+=x; return n end return inc(3),inc(4),n"},
        {"close_scope", "local a,b; do local x=10; a=function() x+=1 return x end end do local x=20; b=function() x+=2 return x end end "
                        "return a(),b(),a(),b()"},
        {"loop_captures", "local f={}; for i=1,4 do local x=i; f[i]=function() x+=1 return x end end return f[1](),f[2](),f[1](),f[4]()"},
        {"recursive", "local function fact(n) if n<=1 then return 1 end return n*fact(n-1) end return fact(6)"},
        {"varargs", "local function f(a,...) return a,... end local function g(...) return f(9,...) end return g(2,nil,7,nil)"},
        {"multret", "local function f() return 2,nil,4,nil end local t={1,f()}; local a,b,c,d=f(); return t[1],t[2],t[3],t[4],a,b,c,d"},
        {"methods", "local t={x=3}; function t:add(n) self.x+=n return self.x end return t:add(5),t:add(2)"},
        {"shortcircuit", "local function f(a,b) return a and b or 9, a==b, a~=b, a<b, a<=b, not(a<b) end return f(2,3)"},
        {"strings", "local s='quote\\\" slash\\\\ nul\\000 byte\\255'; local t={['not-key']=s}; return t['not-key'],s..'end',#s"},
        {"side_effects", "local n=0; local function f() n+=1 return n end return f()+f()*f(),n"},
        {"metamethod",
         "local n=0; local t=setmetatable({}, {__index=function(_,k) n+=1 return n end}); local a=t.x; local b=t.y; return a+b,n"},
        {"iter_meta",
         "local t=setmetatable({}, {__iter=function() return next,{a=2,b=4},nil end}); local sum=0; for k,v in t do sum+=v end return sum"},
        {"shadowing", "local function f(x) local result=x; do local x=7; result+=x end return result+x end return f(3)"},
        {"nested_loops",
         "local sum=0; for i=1,4 do for j=1,5 do if j==2 then continue end if i==3 then break end sum+=i*j end end return sum"},
        {"mutable_parameter", "local function f(n) local function g() n+=1 return n end local a,b=g(),g() return a,b,n end return f(9)"},
        {"multi_capture",
         "local a,b=0,0; local function f() a+=1 b+=2 end local function pair() return 5,8 end a,b=pair(); f(); return a,b"},
        {"helper_collision",
         "local function f(...) local Index={...}; local TupleIndex=Index[1]; return TupleIndex,Index[2] end return f(4,5)"},
        {"capture_read_order",
         "local x=0; local function inc() x+=1 end; local function f() local before=x; inc(); return before,x end return f()"},
        {"capture_forward",
         "local x=4; local function outer() return function() x+=3 return x end end local a,b=outer(),outer(); return a(),b(),x"},
        {"copy_each_iteration", "local t={}; for i=1,4 do t[i]=function() return i end end return t[1](),t[2](),t[3](),t[4]()"},
        {"repeat_continue", "local n,sum=0,0; repeat n+=1; if n%2==0 then continue end; sum+=n until n>8; return sum,n"},
        {"while_nested", "local n,sum=0,0; while n<8 do n+=1; if n<4 then sum+=2 else sum+=3 end end return sum,n"},
        {"branch_returns",
         "local function f(x) if x==1 then return 3 elseif x==2 then return 4 else return 5 end end return f(1),f(2),f(3)"},
        {"call_order", "local log=''; local function f(x) log..=x return x end local function sink(...) return ... end local a=f('a'); "
                       "local b=f('b'); sink(b,a); return log"},
        {"argument_order", "local log=''; local function f(x) log..=x return x end local function sink(...) return ... end local "
                           "a,b,c=sink(f('a'),f('b'),f('c')); return log,a,b,c"},
        {"empty_multret", "local function f() end local function g(...) return select('#',...),... end return g(1,f())"},
        {"zero_varargs", "local function f(...) return select('#',...),... end return f()"},
        {"vararg_fixed", "local function f(...) local a,b,c=... return a,b,c end return f(1,nil,3,9)"},
        {"pcall", "local ok,n=pcall(function(x) return x+4 end,3); local no=pcall(function() error('test') end); return ok,n,no"},
        {"builtin", "local x=4; return math.max(x,9),math.abs(-x),bit32.band(7,3),string.sub('abcd',2,3)"},
        {"not_nan", "local function f(x) return x<0,x<=0,not(x<0),not(x<=0) end return f(0/0)"},
        {"function_reuse", "local t={}; for i=1,3 do t[i]=function(x) return x+1 end end return t[1](3),t[2](4)"},
        {"member_effects", "local n=0; local t=setmetatable({}, {__index=function(_,k) n+=1 return function() n+=10 return n end end}); "
                           "local f=t.a; local g=t.b; return f(),g(),n"},
        {"binary_concat", "local function f(x,y,z) return x..y..z end return f('a','b','c')"},
        {"yield", "local f=coroutine.wrap(function() for i=1,3 do coroutine.yield(i) end return 4 end); return f(),f(),f(),f()"},
        {"cf_enum_chain",
         "local function f(x) local result; if x==1 or x==2 or x==3 then result=8 elseif x==4 then result=9 else return -1 end "
         "return result+10 end return f(0),f(1),f(2),f(3),f(4),f(5)"},
        {"cf_early_return", "local function f(x,y) if x==1 then return 7 end local result=0; if x==2 or x==3 then "
                            "if y then result=4 else return 8 end elseif x==4 then result=6 else return 9 end return result+2 end "
                            "return f(1,false),f(2,false),f(2,true),f(3,true),f(4,false),f(5,true)"},
        {"cf_boolean_values",
         "local function f(a,b,c) local x=a and b and c; local y=a or b or c; "
         "return x,y,if a then b else nil,if a then b else false end "
         "local out={}; for _,a in {false,0,1} do for _,b in {false,2} do "
         "local x,y,z,w=f(a,b,3); out[#out+1]=tostring(x)..tostring(y)..tostring(z)..tostring(w) end end return table.concat(out,'|')"},
        {"cf_condition_effects", "local log=''; local function test(x) log..=tostring(x); return x==2 or x==4 end "
                                 "for i=1,5 do if test(i) or test(i+1) or test(i+2) then log..='T' else log..='F' end "
                                 "if test(i) and test(i+1) then log..='Y' end end return log"},
        {"cf_shared_suffix", "local function f(x) local result=0; if x<5 then if x<3 then result=1 else result=2 end "
                             "result+=8 else result=3; result+=8 end return result end return f(1),f(4),f(7)"},
        {"cf_scope_capture", "local function f(x) local r; if x then local value=7; r=function() return value end "
                             "else local value=9; r=function() return value end end return r() end return f(false),f(true)"},
        {"cf_loop_exit", "local sum=0; for i=1,7 do if i==1 or i==2 then continue end "
                         "if i==5 or i==6 then break end sum+=i end return sum"},
        {"cf_parenthesized_call", "local log=0; local t=setmetatable({}, {__call=function() log+=1 end}); "
                                  "local f=function() return t end; (f())(); (f())(); return log"},
        {"cf_names", "local Value1=3; local function f(Argument1) local UserInputService=Argument1; "
                     "return function(Value2) local Value3=Value2+UserInputService; return Value3+Value1 end end return f(7)(9)"},
        {"cf_method_install", "local t={x=5}; function t:read() return self.x end function t:set(x) self.x=x end "
                              "local a=t:read(); t:set(9); return a,t:read()"},
        {"cf_method_order", "local log=''; local t=setmetatable({}, {__index=function(_,key) log..='lookup'; "
                            "return function(_,x) log..=x end end}); local function f() log..='arg'; return 'call' end "
                            "local x=f(); t:method(x); t:method(f()); return log"},
        {"cf_field_order", "local log=''; local t=setmetatable({}, {__newindex=function(_,key,x) log..=key..x end}); "
                           "local function f() log..='rhs'; return 'value' end local x=f(); t.field=x; t.field=f(); return log"},
        {"cf_mixed_logic", "local function f(a,b,c,d) if not a and b or c and not d then return 1 else return 2 end end "
                           "local result=''; for _,a in {false,true} do for _,b in {false,true} do for _,c in {false,true} do "
                           "for _,d in {false,true} do result..=f(a,b,c,d) end end end end return result"}};
    unsigned Passed = 0, Failed = 0;
    for (const auto &[Name, Source] : Fixtures)
        for (int Optimization = 0; Optimization <= 2; ++Optimization)
            for (int Debug : {0, 2})
                for (bool StateMachine : {false, true})
                {
                    std::string Generated;
                    try
                    {
                        Luau::CompileOptions Compile;
                        Compile.optimizationLevel = Optimization;
                        Compile.debugLevel = Debug;
                        auto Original = Luau::compile(Source, Compile);
                        auto Expected = Execute(Original);
                        Taze::Options Settings;
                        Settings.ForceStateMachine = StateMachine;
                        auto Result = Taze::Decompile(Original, Settings);
                        Generated = Result.Source;
                        if (Generated.find(';') != std::string::npos)
                            throw std::runtime_error("Generated a statement semicolon");
                        if (Generated.find("end)(") != std::string::npos)
                            throw std::runtime_error("Generated an inline capture factory wrapper");
                        if (!StateMachine && Name.starts_with("cf_") && Result.StateMachineFunctions)
                            throw std::runtime_error("Reducible control flow used state-machine fallback");
                        auto Recompiled = Luau::compile(Generated, Compile);
                        if (Recompiled[0] == 0)
                            throw std::runtime_error("Output does not compile: " + Recompiled.substr(1));
                        auto Actual = Execute(Recompiled);
                        if (Expected != Actual)
                            throw std::runtime_error("Expected " + Expected + ", got " + Actual);
                        Passed++;
                    }
                    catch (const std::exception &Error)
                    {
                        Failed++;
                        std::cerr << "FAIL " << Name << " O" << Optimization << " D" << Debug << " S" << StateMachine << ": "
                                  << Error.what() << '\n';
                        std::ofstream("failure-" + Name + "-" + std::to_string(Optimization) + "-" + std::to_string(Debug) + "-" +
                                      std::to_string(StateMachine) + ".luau")
                            << Generated;
                    }
                }
    // Function locators are executable metadata: verify escaped literals, imports and per-prototype membership.
    try
    {
        auto Check = [&](bool Condition, const char *Message)
        {
            if (!Condition)
                throw std::runtime_error(Message);
            ++Passed;
        };
        Luau::CompileOptions Compile;
        Compile.debugLevel = 2;
        Compile.optimizationLevel = 1;
        const std::string StyleSource = "local Shared = 7\n"
                                        "local function Probe(Input)\n"
                                        "    local First = Input.Name\n"
                                        "    local Second = Input.Value\n"
                                        "    Input.Value = First\n"
                                        "    Input.Name = Second\n"
                                        "    return Shared, math.abs(Input.Count), \"Marker\\n\\\"\\\\\\000\", 12345.75\n"
                                        "end\nreturn Probe";
        auto Bytecode = Luau::compile(StyleSource, Compile);
        auto Source = Taze::Decompile(Bytecode).Source;
        Check(Source.find("-- Line: 2 | filtergc(\"function\", { Line = 2, Name = \"Probe\" }, true)") != std::string::npos,
              "Missing inline locator or serialized function name");
        auto Start = Source.find("filtergc(");
        auto Lookup = Source.substr(Start, Source.find('\n', Start) - Start);
        auto LookupTest = Luau::compile("local function filtergc(Kind, Options, One) "
                                        "assert(Kind=='function' and One==true) "
                                        "return Options.Name,Options.Line,Options.Constants end return " +
                                        Lookup);
        Check(Execute(LookupTest) == Execute(Luau::compile("return 'Probe',2,nil")),
              "Named locator should use the original name and line without constants");
        Check(Source.find("Upvalues:\n        1: Shared (type \"Copy\")") != std::string::npos, "Missing Upvalues label");
        Check(Source.find("local First = Input.Name\n    local Second = Input.Value\n\n    Input.Value = First") != std::string::npos,
              "Consecutive locals were not grouped and separated from writes");
        Check(Source.find("\n\n    return ") != std::string::npos, "Missing return separation");
        Check(Source.find("\n    \n") == std::string::npos, "Blank lines contain indentation whitespace");
        Taze::Options Settings;
        Settings.IncludeFunctionLocators = false;
        auto WithoutLookup = Taze::Decompile(Bytecode, Settings).Source;
        Check(WithoutLookup.find("filtergc(") == std::string::npos && WithoutLookup.find("-- Line: 2") != std::string::npos,
              "Locator suppression removed line comments");
        Settings.IncludeFunctionLocators = true;
        Settings.IncludeLineComments = false;
        Check(Taze::Decompile(Bytecode, Settings).Source.find("filtergc(") == std::string::npos,
              "Disabled line comments emitted a locator");
        Settings.IncludeUpvalueComments = false;
        Check(Taze::Decompile(Bytecode, Settings).Source.find("Upvalues:") == std::string::npos,
              "Disabled upvalue comments emitted a label");
        Compile.debugLevel = 0;
        auto Stripped = Taze::Decompile(Luau::compile(StyleSource, Compile)).Source;
        Check(Stripped.find("-- Line: 2 | filtergc(\"function\", { Line = 2,") != std::string::npos,
              "Stripped local debug names should preserve the serialized function start line");
        Check(Stripped.find(", Name = ") == std::string::npos, "Stripped function acquired an invented GC name");
        auto StrippedStart = Stripped.find("filtergc(");
        auto StrippedLookup = Stripped.substr(StrippedStart, Stripped.find('\n', StrippedStart) - StrippedStart);
        auto StrippedTest = Luau::compile("local function filtergc(_, Options) "
                                          "return Options.Line,Options.Name,#Options.Constants end return " +
                                          StrippedLookup);
        Check(Execute(StrippedTest) == Execute(Luau::compile("return 2,nil,5")), "Unnamed locator must limit constants to five");
        Compile.debugLevel = 2;
        auto PreserveSource = Taze::Decompile(Luau::compile("local function lowerCamel() return 7 end return lowerCamel", Compile)).Source;
        Check(PreserveSource.find("function lowerCamel(") != std::string::npos, "Function name casing changed");
        Compile.debugLevel = 0;
        auto TableSource = Taze::Decompile(Luau::compile("local A,B={},{} A[1]=0 B[1]=1 return A,B", Compile)).Source;
        Check(TableSource.find("return { [1] = 0 }, { [1] = 1 }") != std::string::npos,
              "Interleaved small tables were not folded and redundant locals removed");
        auto AliasSource = Taze::Decompile(Luau::compile("local A={}; local B=A; print(B,B); return B", Compile)).Source;
        Check(AliasSource.find("print(Table1, Table1)") != std::string::npos && AliasSource.find("local Table2") == std::string::npos,
              "Repeated stable aliases were not removed");
        auto MutableSource = Taze::Decompile(Luau::compile("local A=1; local B=A; A=2; return B,A", Compile)).Source;
        Check(MutableSource.find("return 1, 2") != std::string::npos || MutableSource.find("return Number1, 2") != std::string::npos,
              "Mutable source alias lost its snapshot value");
    }
    catch (const std::exception &Error)
    {
        ++Failed;
        std::cerr << "FAIL formatting/locator: " << Error.what() << '\n';
    }
    try
    {
        auto Read = [](const std::string &Path)
        {
            std::ifstream In(Path, std::ios::binary);
            if (!In)
                throw std::runtime_error("Cannot read bridge fixture: " + Path);
            return std::string(std::istreambuf_iterator<char>(In), {});
        };
        auto Harness = Read(std::string(TAZE_SOURCE_DIR) + "/tests/BridgeHarness.luau");
        auto Bridge = Read(std::string(TAZE_SOURCE_DIR) + "/tools/Decompile.luau");
        auto Marker = Harness.find("--!BRIDGE_SOURCE!");
        Harness.replace(Marker, std::string("--!BRIDGE_SOURCE!").size(), Bridge);
        if (Execute(Luau::compile(Harness)) != Execute(Luau::compile("return 6")))
            throw std::runtime_error("Bridge probe modes failed");
        Passed += 6;
    }
    catch (const std::exception &Error)
    {
        ++Failed;
        std::cerr << "FAIL bridge probe: " << Error.what() << '\n';
    }
    const std::vector<std::tuple<std::string, std::string, std::string>> Modules = {
        {"direct", "local M={}; function M.Read() return 17 end return M", ".Read"},
        {"nested", "local M={Inner={}}; function M.Inner.Read() return 17 end return M", ".Inner.Read"},
        {"numeric", "local M={function() return 17 end}; return M", "[1]"},
        {"quoted_key", "local M={}; M['#1']=function() return 17 end return M", "[\"#1\"]"},
        {"factory",
         "local M={}; M.__index=M; function M.new() return setmetatable({},M) end function M.Read() return 17 end return M.new()", ".Read"},
        {"returned_function", "return function() return 17 end", ""},
        {"discarded", "local M={}; function M.Read() return 17 end return {}", "NONE"},
        {"ambiguous",
         "local A,B={},{}; function A.Read() return 17 end function B.Read() return 18 end if getfenv().Flag then return A else return B "
         "end",
         "NONE"},
        {"conditional", "local M={}; if getfenv().Flag then function M.Read() return 17 end end return M", "NONE"},
        {"overwritten", "local M={}; function M.Read() return 17 end M.Read=false return M", "NONE"},
        {"escaped", "local M={}; function M.Read() return 17 end UnknownMutator(M) return M", "NONE"},
        {"cycle", "local M={}; M.Self=M; function M.Read() return 17 end return M", ".Read"},
    };
    for (const auto &[Name, Source, Suffix] : Modules)
        for (int Optimization : {0, 1, 2})
        {
            try
            {
                Luau::CompileOptions Compile;
                Compile.optimizationLevel = Optimization;
                Compile.debugLevel = 2;
                Taze::Options Settings;
                Settings.ModulePath = "game.ReplicatedStorage.Test";
                Settings.FilterLineField = "StartLine";
                auto Result = Taze::Decompile(Luau::compile(Source, Compile), Settings).Source;
                auto Expected = " | require(" + Settings.ModulePath + ")" + Suffix;
                if (Suffix == "NONE" ? Result.find(" | require(") != std::string::npos : Result.find(Expected) == std::string::npos)
                    throw std::runtime_error("Incorrect module export locator\n" + Result);
                auto Bytes = Luau::compile(Result);
                if (Bytes[0] == 0)
                    throw std::runtime_error(Bytes.substr(1));
                if (Result.find("filtergc(\"function\", { Line =") != std::string::npos)
                    throw std::runtime_error("StartLine option ignored");
                if (Suffix != "NONE")
                {
                    auto Query = "local Module = (function() " + Source +
                                 " end)()\n"
                                 "local game = {ReplicatedStorage={Test={}}}\n"
                                 "local function require(Path) assert(Path==game.ReplicatedStorage.Test) return Module end\n"
                                 "local Target = require(game.ReplicatedStorage.Test)" +
                                 Suffix + "\nreturn type(Target),Target()";
                    if (Execute(Luau::compile(Query)) != Execute(Luau::compile("return 'function',17")))
                        throw std::runtime_error("Export locator did not reach the expected returned function");
                }
                ++Passed;
            }
            catch (const std::exception &Error)
            {
                ++Failed;
                std::cerr << "FAIL module " << Name << " O" << Optimization << ": " << Error.what() << '\n';
            }
        }
    for (const auto &[Source, Expected] : std::vector<std::pair<std::string, std::string>>{
             {"local A,B={},{}; print(A,B); return A,B", "local Table1 = {}"},
             {"local A=123; print(A); return A", "local Number1 = 123"},
             {"local A='hello'; print(A); return A", "local String1 = \"hello\""},
             {"local A=true; print(A); return A", "local Boolean1 = true"},
             {"local A=function() return 1 end; print(A); return A", "local function Function1("},
             {"local A={}; if ... then A=123 end print(A); return A", "local Value1"},
             {"local A={}; print(Table1,A); return A", "local Table2 = {}"}})
    {
        try
        {
            Luau::CompileOptions Compile;
            Compile.optimizationLevel = 0;
            Compile.debugLevel = 0;
            auto Result = Taze::Decompile(Luau::compile(Source, Compile)).Source;
            if (Result.find(Expected) == std::string::npos)
                throw std::runtime_error("Missing categorized name: " + Expected + "\n" + Result);
            auto Bytes = Luau::compile(Result);
            if (Bytes[0] == 0)
                throw std::runtime_error(Bytes.substr(1));
            ++Passed;
        }
        catch (const std::exception &Error)
        {
            ++Failed;
            std::cerr << "FAIL categorized names: " << Error.what() << '\n';
        }
    }
    // Every proper prefix of a valid chunk must fail without crashing or producing partial output.
    auto Sample = Luau::compile("return 123");
    for (std::size_t Size = 0; Size < Sample.size(); ++Size)
    {
        try
        {
            Taze::Decompile(std::string_view(Sample).substr(0, Size));
            Failed++;
        }
        catch (const Taze::Error &)
        {
            Passed++;
        }
    }
    RobloxEncoder Encoder;
    const std::vector<std::pair<unsigned, std::string>> Versions = {{9, ""},
                                                                    {11, "LuauEmitCallFeedback"},
                                                                    {12, "LuauBytecodeCostModel"},
                                                                    {13, "LuauCompileEmitVectorDouble"},
                                                                    {14, "LuauCompileFastpcall"}};
    for (const auto &[Version, FlagName] : Versions)
    {
        Luau::FValue<bool> *Enabled = nullptr;
        bool Old = false;
        for (auto *Flag = Luau::FValue<bool>::list; Flag; Flag = Flag->next)
            if (Flag->name == FlagName)
            {
                Enabled = Flag;
                Old = Flag->value;
                Flag->value = true;
            }
        if (!FlagName.empty() && !Enabled)
        {
            std::cerr << "Missing version flag " << FlagName << '\n';
            Failed++;
            continue;
        }
        for (const auto &[Name, Source] : Fixtures)
        {
            try
            {
                Luau::CompileOptions Options;
                Options.debugLevel = 2;
                auto Plain = Luau::compile(Source, Options);
                auto Encoded = Luau::compile(Source, Options, {}, &Encoder);
                auto Expected = Execute(Plain);
                for (bool Roblox : {false, true})
                {
                    auto Result = Taze::Decompile(Roblox ? Encoded : Plain);
                    if (Result.BytecodeVersion != Version)
                        throw std::runtime_error("Wrong bytecode version");
                    if (Roblox && Result.Encoding != Taze::OpcodeEncoding::Roblox)
                        throw std::runtime_error("Wrong opcode encoding");
                    auto Recompiled = Luau::compile(Result.Source, Options);
                    if (Execute(Recompiled) != Expected)
                        throw std::runtime_error("Versioned execution mismatch");
                    Passed++;
                }
                if (Taze::Decompile(Encoded + std::string(24, '\0')).Source != Taze::Decompile(Encoded).Source)
                    throw std::runtime_error("Roblox trailer affected source");
                Passed++;
            }
            catch (const std::exception &Error)
            {
                std::cerr << "FAIL v" << Version << ' ' << Name << ": " << Error.what() << '\n';
                Failed++;
            }
        }
        if (Enabled)
            Enabled->value = Old;
    }
    // Malformed headers and length fields must be rejected, including overflow and unknown versions.
    for (const std::string &Bad : {std::string("\xff", 1), std::string("\x09\x03\xff\xff\xff\xff\xff", 7), Sample + "extra"})
    {
        try
        {
            Taze::Decompile(Bad);
            Failed++;
        }
        catch (const Taze::Error &)
        {
            Passed++;
        }
    }
    std::cout << Passed << " passed, " << Failed << " failed\n";
    return Failed ? 1 : 0;
}
