-- Start build/Release/taze.exe --serve before running this script.
-- Optional: getgenv().TazeConfig = { Url = "ws://127.0.0.1:8877/decompile", Timeout = 30 }
local Environment = getgenv and getgenv() or getfenv(0)
local Config = Environment.TazeConfig or {}
local Url = Config.Url or "ws://127.0.0.1:8877/decompile"
local DefaultTimeout = Config.Timeout or 30
local GetBytecode = getscriptbytecode or Environment.getscriptbytecode
assert(type(GetBytecode) == "function", "Taze requires getscriptbytecode")
assert(type(DefaultTimeout) == "number" and DefaultTimeout > 0 and DefaultTimeout < math.huge, "Taze timeout must be positive and finite")

local Provider = WebSocket or websocket or (syn and syn.websocket)
local Connect = Provider and (Provider.connect or Provider.Connect)
assert(type(Connect) == "function", "Taze requires WebSocket.connect, websocket.connect, or syn.websocket.connect")

local Previous = Environment.TazeBridge
local PreviousDecompile = Environment.decompile or decompile
local Original = if Previous and Previous.Marker == "TazeBridge1" then Previous.Original else Environment.decompile or decompile
local Bridge = { Marker = "TazeBridge1", Version = 3, Original = Original, Url = Url, Connected = false }
local HookTargets = { Original, PreviousDecompile }
if Previous and Previous.Marker == "TazeBridge1" and Previous.HookTargets then
    for _, Target in Previous.HookTargets do
        table.insert(HookTargets, Target)
    end
end
Bridge.HookTargets = {}
local CompareInstances = compareinstances or Environment.compareinstances

local function SameInstance(Left, Right)
    if Left == Right then
        return true
    end
    if Left and Right and type(CompareInstances) == "function" then
        local Ok, Same = pcall(CompareInstances, Left, Right)
        return Ok and Same == true
    end
    return false
end

local function DetectLineField()
    local Filter = filtergc or Environment.filtergc
    if type(Filter) ~= "function" or type(debug.info) ~= "function" then
        return "Line", "fallback"
    end

    local function TazeLineProbe()
        return "TazeBridge.LineProbe.20260921"
    end

    local Ok, Line = pcall(debug.info, TazeLineProbe, "l")
    if not Ok or type(Line) ~= "number" or Line < 1 then
        return "Line", "fallback"
    end

    local function ContainsProbe(Field, Value)
        local Options = {
            Constants = { "TazeBridge.LineProbe.20260921" },
            IgnoreExecutor = false,
            IgnoreSyn = false,
        }
        Options[Field] = Value

        local Success, Matches = pcall(Filter, "function", Options, false)
        if not Success then
            return nil
        end
        if type(Matches) == "function" then
            return Matches == TazeLineProbe
        end
        if type(Matches) == "table" then
            for _, Match in Matches do
                if Match == TazeLineProbe then
                    return true
                end
            end
        end
        return false
    end

    for _, Field in { "StartLine", "Line" } do
        -- The negative control rejects executors that silently ignore an unknown option.
        if ContainsProbe(Field, Line) == true and ContainsProbe(Field, Line + 1000000) == false then
            return Field, "verified"
        end
    end
    return "Line", "fallback"
end

Bridge.FilterLineField, Bridge.LineProbe = DetectLineField()

local function QuoteName(Name)
    return '"' .. Name:gsub(".", function(Character)
        local Byte = string.byte(Character)
        if Character == '"' or Character == "\\" then
            return "\\" .. Character
        elseif Byte < 32 or Byte >= 127 then
            return string.format("\\%03d", Byte)
        end
        return Character
    end) .. '"'
end

local function ModulePath(Target)
    if not Target:IsA("ModuleScript") then
        return "", "not a ModuleScript"
    end

    local Parts = {}
    local Current = Target
    local LocalPlayer = game:GetService("Players").LocalPlayer
    local Base = "game"
    while not SameInstance(Current, game) do
        if SameInstance(Current, LocalPlayer) then
            Base = 'game:GetService("Players").LocalPlayer'
            break
        end
        local Parent = Current.Parent
        if not Parent then
            return "", "module is not parented under game"
        end
        if #Parts >= 128 then
            return "", "module hierarchy exceeds 128 levels"
        end
        if not SameInstance(Parent:FindFirstChild(Current.Name), Current) then
            return "", "ambiguous child name or uncomparable instance reference: " .. Current.Name
        end
        table.insert(Parts, ':FindFirstChild(' .. QuoteName(Current.Name) .. ')')
        Current = Parent
    end
    for Index = #Parts, 1, -1 do
        Base ..= Parts[Index]
    end
    if #Base > 8192 then
        return "", "module path exceeds 8192 bytes"
    end
    return Base, "available"
end
local Socket
local Connections = {}
local Pending = {}
local NextId = 0
local Connecting = false
local Stopped = false
local Installed
local Hook = hookfunction or Environment.hookfunction
local Wrap = newcclosure or Environment.newcclosure
local Hooks = {}

local function Disconnect(Reason)
    Bridge.Connected = false
    local OldSocket = Socket
    Socket = nil
    for _, Connection in Connections do
        pcall(function() Connection:Disconnect() end)
    end
    table.clear(Connections)
    for _, Request in Pending do
        Request.Error = Reason or "Taze WebSocket disconnected"
        Request.Done = true
    end
    if OldSocket then
        pcall(function() OldSocket:Close() end)
    end
end

local function EnsureConnected(Deadline)
    while Connecting do
        assert(os.clock() < Deadline, "Taze connection timed out")
        task.wait()
    end
    assert(not Stopped, "Taze bridge has been stopped")
    if Socket then
        return Socket
    end

    Connecting = true
    local Attempt = {}
    task.spawn(function()
        Attempt.Ok, Attempt.Result = pcall(Connect, Url)
        Attempt.Done = true
        if Attempt.Abandoned and Attempt.Ok and Attempt.Result then
            pcall(function() Attempt.Result:Close() end)
        end
    end)
    while not Attempt.Done and os.clock() < Deadline and not Stopped do
        task.wait()
    end
    Connecting = false
    if not Attempt.Done then
        Attempt.Abandoned = true
        error("Taze connection timed out or the bridge was stopped", 2)
    end
    local Ok, Result = Attempt.Ok, Attempt.Result
    assert(Ok and Result, "Cannot connect to Taze. Start taze.exe --serve first. " .. tostring(Result))
    if Stopped then
        pcall(function() Result:Close() end)
        error("Taze bridge has been stopped", 2)
    end
    Socket = Result
    local Current = Result
    local SetupOk, SetupError = pcall(function()
        Connections[1] = Current.OnMessage:Connect(function(Message)
            if Socket ~= Current or type(Message) ~= "string" then
                return
            end
            local Id, Status, Body = Message:match("^(%d+)\n([^\n]+)\n(.*)$")
            local Request = Id and Pending[Id]
            if not Request then
                return
            end
            if Status == "ok" then
                Request.Source = Body
            else
                Request.Error = Body or "Invalid Taze response"
            end
            Request.Done = true
        end)
        Connections[2] = Current.OnClose:Connect(function()
            if Socket == Current then
                Disconnect("Taze server disconnected. The next call will reconnect.")
            end
        end)
    end)
    if not SetupOk then
        Disconnect("Unsupported executor WebSocket event API")
        error(tostring(SetupError), 2)
    end
    Bridge.Connected = true
    return Current
end

local Hex = {}
for Byte = 0, 255 do
    Hex[string.char(Byte)] = string.format("%02x", Byte)
end

Installed = function(Target, Timeout)
    assert(typeof(Target) == "Instance" and (Target:IsA("LocalScript") or Target:IsA("ModuleScript") or Target:IsA("Script")),
        "decompile expects a script Instance")
    Timeout = Timeout or DefaultTimeout
    assert(type(Timeout) == "number" and Timeout > 0 and Timeout < math.huge, "decompile timeout must be positive and finite")
    local Deadline = os.clock() + Timeout
    local Current = EnsureConnected(Deadline)
    local Ok, Bytecode = pcall(GetBytecode, Target)
    assert(Ok, "getscriptbytecode failed: " .. tostring(Bytecode))
    assert(type(Bytecode) == "string" and #Bytecode > 0, "getscriptbytecode returned no bytecode for this script")
    assert(#Bytecode <= 64 * 1024 * 1024, "Bytecode exceeds the 64 MiB limit")
    local Encoded = Bytecode:gsub(".", Hex)
    local Expression, PathStatus = ModulePath(Target)
    local Path = Expression:gsub(".", Hex)
    assert(os.clock() < Deadline, "Taze timed out while reading bytecode")
    NextId += 1
    local Id = tostring(NextId)
    local Request = {}
    Pending[Id] = Request
    local Sent, SendError = pcall(function()
        Current:Send(Id .. "\nTAZE2\n" .. Bridge.FilterLineField .. "\n" .. Path .. "\n" .. Encoded)
    end)
    if not Sent then
        Pending[Id] = nil
        Disconnect("Taze send failed: " .. tostring(SendError))
        error("Taze send failed: " .. tostring(SendError), 2)
    end
    while not Request.Done and os.clock() < Deadline do
        task.wait()
    end
    Pending[Id] = nil
    assert(Request.Done, "Taze decompile timed out after " .. Timeout .. " seconds")
    if Request.Error then
        error("Taze: " .. Request.Error, 2)
    end
    local _, ExportCount = Request.Source:gsub(" | require%(", "")
    local _, GcCount = Request.Source:gsub(" | filtergc%(", "")
    Bridge.LastRequest = {
        ModulePath = Expression,
        PathStatus = PathStatus,
        ExportLookups = ExportCount,
        GcLookups = GcCount,
        FilterLineField = Bridge.FilterLineField,
    }
    if Target:IsA("ModuleScript") and Expression == "" then
        warn("Taze: require lookups unavailable: " .. PathStatus)
    end
    return Request.Source
end

function Bridge.Stop()
    if Stopped then
        return
    end
    Stopped = true
    Disconnect("Taze bridge stopped")
    for Index = #Hooks, 1, -1 do
        local Entry = Hooks[Index]
        local Ok, Error = pcall(Hook, Entry.Target, Entry.Backup)
        if not Ok then
            warn("Taze could not restore a decompile hook: " .. tostring(Error))
        end
    end
    table.clear(Hooks)
    if Environment.decompile == Installed then
        Environment.decompile = Original
    end
    if Environment.TazeBridge == Bridge then
        Environment.TazeBridge = nil
    end
end

-- Only replace the existing decompiler once the server connection succeeds.
EnsureConnected(os.clock() + DefaultTimeout)
if Previous and Previous.Marker == "TazeBridge1" then
    Previous.Stop()
end
-- Global assignment alone misses tools that saved the executor's old function.
Bridge.HookStatus = "unavailable"
if type(Hook) == "function" then
    local Replacement = Installed
    if type(Wrap) == "function" then
        local Ok, Wrapped = pcall(Wrap, Installed)
        if Ok then Replacement = Wrapped end
    end
    local Seen = {}
    for _, Target in HookTargets do
        if type(Target) == "function" and not Seen[Target] then
            Seen[Target] = true
            local Ok, Backup = pcall(Hook, Target, Replacement)
            if Ok and type(Backup) == "function" then
                table.insert(Hooks, { Target = Target, Backup = Backup })
                table.insert(Bridge.HookTargets, Target)
            else
                Bridge.HookError = tostring(Backup)
            end
        end
    end
    Bridge.HookStatus = if #Hooks > 0 then "installed" else "failed"
end
Environment.decompile = Installed
Bridge.Decompile = Installed
Environment.TazeBridge = Bridge
print("Taze decompile v3 installed: " .. Url .. " | require export lookups | " .. Bridge.FilterLineField .. " (" .. Bridge.LineProbe .. ") | cached references: " .. Bridge.HookStatus)
return Bridge
