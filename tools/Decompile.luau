-- Start build/Release/taze.exe --serve before running this script.
-- Optional: getgenv().TazeConfig = { Url = "ws://127.0.0.1:8877/decompile", Timeout = 30 }
local Environment = getgenv and getgenv() or getfenv(0)
local Config = if type(Environment.TazeConfig) == "table" then Environment.TazeConfig else {}
local Url = Config.Url or "ws://127.0.0.1:8877/decompile"
local DefaultTimeout = Config.Timeout or 30
local GetBytecode = getscriptbytecode or Environment.getscriptbytecode
if type(GetBytecode) ~= "function" or type(DefaultTimeout) ~= "number" or DefaultTimeout <= 0 or DefaultTimeout >= math.huge then
    return nil
end

local Provider = WebSocket or websocket or (syn and syn.websocket)
local Connect = Provider and (Provider.connect or Provider.Connect)
if type(Connect) ~= "function" then
    return nil
end

local Previous = Environment.TazeBridge
local PreviousDecompile = Environment.decompile or decompile
local IsPreviousBridge = type(Previous) == "table" and Previous.Marker == "TazeBridge1"
local Original = if IsPreviousBridge then Previous.Original else Environment.decompile or decompile
local Bridge = { Marker = "TazeBridge1", Version = 4, Original = Original, Url = Url, Connected = false }
local HookTargets = { Original, PreviousDecompile }
if IsPreviousBridge and type(Previous.HookTargets) == "table" then
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
    if type(Filter) ~= "function" or type(debug) ~= "table" or type(debug.info) ~= "function" then
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
        if os.clock() >= Deadline then
            return nil
        end
        task.wait()
    end
    if Stopped then
        return nil
    end
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
        return nil
    end
    local Ok, Result = Attempt.Ok, Attempt.Result
    if not Ok or not Result then
        return nil
    end
    if Stopped then
        pcall(function() Result:Close() end)
        return nil
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
        return nil
    end
    Bridge.Connected = true
    return Current
end

local Hex = {}
for Byte = 0, 255 do
    Hex[string.char(Byte)] = string.format("%02x", Byte)
end

local function Decompile(Target, Timeout)
    if typeof(Target) ~= "Instance" or not (Target:IsA("LocalScript") or Target:IsA("ModuleScript") or Target:IsA("Script")) then
        return nil
    end
    Timeout = Timeout or DefaultTimeout
    if type(Timeout) ~= "number" or Timeout <= 0 or Timeout >= math.huge then
        return nil
    end
    local Deadline = os.clock() + Timeout
    local Current = EnsureConnected(Deadline)
    if not Current then
        return nil
    end
    local Ok, Bytecode = pcall(GetBytecode, Target)
    if not Ok or type(Bytecode) ~= "string" or #Bytecode == 0 or #Bytecode > 64 * 1024 * 1024 then
        return nil
    end
    local Encoded = Bytecode:gsub(".", Hex)
    local Expression, PathStatus = ModulePath(Target)
    local Path = Expression:gsub(".", Hex)
    if os.clock() >= Deadline then
        return nil
    end
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
        return nil
    end
    while not Request.Done and os.clock() < Deadline do
        task.wait()
    end
    Pending[Id] = nil
    if not Request.Done or Request.Error or type(Request.Source) ~= "string" then
        return nil
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
    return Request.Source
end

Installed = function(...)
    local Ok, Result = pcall(Decompile, ...)
    return if Ok then Result else nil
end

function Bridge.Stop()
    if Stopped then
        return
    end
    Stopped = true
    Disconnect("Taze bridge stopped")
    for Index = #Hooks, 1, -1 do
        local Entry = Hooks[Index]
        pcall(Hook, Entry.Target, Entry.Backup)
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
if not EnsureConnected(os.clock() + DefaultTimeout) then
    return nil
end
if IsPreviousBridge and type(Previous.Stop) == "function" then
    pcall(Previous.Stop)
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
pcall(print, "Taze decompile v4 installed: " .. Url .. " | require export lookups | " .. Bridge.FilterLineField .. " (" .. Bridge.LineProbe .. ") | cached references: " .. Bridge.HookStatus)
return Bridge
