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
        {"yield", "local f=coroutine.wrap(function() for i=1,3 do coroutine.yield(i) end return 4 end); return f(),f(),f(),f()"}};
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
                        Generated = Taze::Decompile(Original, Settings).Source;
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
