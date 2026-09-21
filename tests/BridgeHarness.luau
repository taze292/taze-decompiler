local function Install(Environment)
    local getgenv = function() return Environment end
    local getscriptbytecode = function(Target) return Target.Bytecode end
    local compareinstances = function(A, B) return (A.Canonical or A) == (B.Canonical or B) end
    local hookfunction = Environment.Hook
    local newcclosure = function(Function) return Function end
    local filtergc = Environment.Filter
    local debug = { info = function(Function) Environment.Probe = Function return 123 end }
    local WebSocket = Environment.Provider
    local game = Environment.Game
    local print = function() end
    local warn = function(Message) Environment.Warning = Message end
    local typeof = function(Value) return if type(Value) == "table" and Value.IsA then "Instance" else type(Value) end
    local task = { spawn = function(Function) Function() end, wait = function() Environment.Time += 0.1 end }
    local os = { clock = function() return Environment.Time end }
    --!BRIDGE_SOURCE!
end

local function Instance(Name, Class, Parent)
    local Value = { Name = Name, Class = Class, Parent = Parent, Children = {} }
    function Value:IsA(Expected) return self.Class == Expected end
    function Value:FindFirstChild(Key) return self.Children[Key] end
    if Parent then Parent.Children[Name] = Value end
    return Value
end

local function Signal()
    local Value = { Listeners = {} }
    function Value:Connect(Callback)
        self.Listeners[Callback] = true
        return { Disconnect = function() self.Listeners[Callback] = nil end }
    end
    function Value:Fire(...)
        for Callback in self.Listeners do Callback(...) end
    end
    return Value
end

local Passed = 0
for _, Mode in { "Line", "StartLine", "Both", "Ignored", "Missing", "Hidden" } do
    local Environment = { Time = 0, Connects = 0 }
    Environment.Game = Instance("Game", "DataModel")
    local Players = Instance("Players", "Players", Environment.Game)
    Players.LocalPlayer = Instance("TestPlayer", "Player", Players)
    function Environment.Game:GetService(Name) return self.Children[Name] end
    local Folder = Instance("PlayerScripts", "Folder", Players.LocalPlayer)
    local Target = Instance('Quoted"\n\255', "ModuleScript", Folder)
    Target.Bytecode = "\009\003\000\255"
    local Implementations = {}
    local Original
    Original = function(...) return Implementations[Original](...) end
    Implementations[Original] = function() return "original" end
    Environment.decompile = Original
    if Mode ~= "Missing" then
        Environment.Hook = function(Target, Replacement)
            assert(Mode ~= "Hidden", "mock executor rejected hook")
            assert(Implementations[Target], "mock only hooks registered closures")
            local Backup = Implementations[Target]
            Implementations[Target] = Replacement
            return Backup
        end
    end

    if Mode ~= "Missing" then
        Environment.Filter = function(Kind, Options, One)
            assert(Kind == "function" and One == false)
            assert(Options.IgnoreExecutor == false and Options.IgnoreSyn == false)
            if Mode == "Hidden" then return {} end
            local Line
            if Mode == "Both" then Line = Options.StartLine or Options.Line
            elseif Mode ~= "Ignored" then Line = Options[Mode] end
            return if Line == nil or Line == 123 then { Environment.Probe } else {}
        end
    end

    Environment.Provider = { connect = function()
        Environment.Connects += 1
        local Socket = { OnMessage = Signal(), OnClose = Signal() }
        function Socket:Close() self.OnClose:Fire() end
        function Socket:Send(Message)
            Environment.Message = Message
            local Id = Message:match("^(%d+)\n")
            self.OnMessage:Fire(Id .. "\nok\nreturn 1")
        end
        Environment.Socket = Socket
        return Socket
    end }

    local Bridge = Install(Environment)
    local Expected = if Mode == "StartLine" or Mode == "Both" then "StartLine" else "Line"
    assert(Bridge.FilterLineField == Expected, "Wrong detected line field for " .. Mode)
    assert(Bridge.LineProbe == (if Mode == "Ignored" or Mode == "Missing" or Mode == "Hidden" then "fallback" else "verified"))
    assert(Environment.decompile(Target) == "return 1")
    assert(Bridge.Version == 3 and Bridge.Decompile == Environment.decompile)
    if Mode ~= "Missing" and Mode ~= "Hidden" then
        assert(Bridge.HookStatus == "installed" and Original(Target) == "return 1")
    else
        assert(Original(Target) == "original")
    end
    local Field, HexPath, Bytecode = Environment.Message:match("TAZE2\n([^\n]+)\n([^\n]*)\n(.+)$")
    assert(Field == Expected and Bytecode == "090300ff")
    local Path = HexPath:gsub("..", function(Pair) return string.char(tonumber(Pair, 16)) end)
    assert(Path == 'game:GetService("Players").LocalPlayer:FindFirstChild("PlayerScripts"):FindFirstChild("Quoted\\\"\\010\\255")', Path)
    assert(Bridge.LastRequest.ModulePath == Path and Bridge.LastRequest.PathStatus == "available")
    local Clone = setmetatable({ Canonical = Target }, { __index = Target })
    assert(Environment.decompile(Clone) == "return 1")
    assert(Bridge.LastRequest.ModulePath == Path, "A cloned instance reference lost its module path")
    local Detached = Instance("Detached", "ModuleScript")
    Detached.Bytecode = Target.Bytecode
    assert(Environment.decompile(Detached) == "return 1")
    assert(Bridge.LastRequest.ModulePath == "" and Environment.Warning:find("not parented", 1, true))
    Target.Class = "LocalScript"
    assert(Environment.decompile(Target) == "return 1")
    assert(Environment.Message:find("\n" .. Expected .. "\n\n", 1, true))
    Target.Class = "ModuleScript"
    Environment.Socket:Close()
    assert(not Bridge.Connected)
    assert(Environment.decompile(Target) == "return 1" and Environment.Connects == 2)
    local SavedWrapper
    SavedWrapper = function(...) return Implementations[SavedWrapper](...) end
    Implementations[SavedWrapper] = function() return "wrapper" end
    Environment.decompile = SavedWrapper
    local Replacement = Install(Environment)
    assert(Environment.TazeBridge == Replacement and Replacement.Original == Original)
    Replacement = Install(Environment)
    if Mode ~= "Missing" and Mode ~= "Hidden" then
        assert(SavedWrapper(Target) == "return 1", "Reinstallation lost a previously hooked cached wrapper")
    end
    Replacement.Stop()
    assert(Environment.decompile == Original and Environment.TazeBridge == nil)
    assert(Original(Target) == "original", "Stop did not restore the cached decompiler")
    assert(SavedWrapper(Target) == "wrapper", "Stop did not restore the cached wrapper")
    Passed += 1
end
return Passed
