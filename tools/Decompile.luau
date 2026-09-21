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
local Original = if Previous and Previous.Marker == "TazeBridge1" then Previous.Original else Environment.decompile or decompile
local Bridge = { Marker = "TazeBridge1", Original = Original, Url = Url, Connected = false }
local Socket
local Connections = {}
local Pending = {}
local NextId = 0
local Connecting = false
local Stopped = false
local Installed

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
    assert(os.clock() < Deadline, "Taze timed out while reading bytecode")
    NextId += 1
    local Id = tostring(NextId)
    local Request = {}
    Pending[Id] = Request
    local Sent, SendError = pcall(function()
        Current:Send(Id .. "\n" .. Encoded)
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
    return Request.Source
end

function Bridge.Stop()
    if Stopped then
        return
    end
    Stopped = true
    Disconnect("Taze bridge stopped")
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
Environment.decompile = Installed
Environment.TazeBridge = Bridge
print("Taze decompile installed: " .. Url)
return Bridge
