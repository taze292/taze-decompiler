-- Run in the connected client to export the explicitly selected test module.
-- The destination is relative to the executor's workspace, not this repository.
local CameraModule = game.Players.LocalPlayer.PlayerScripts.PlayerModule.CameraModule
local Bytecode = getscriptbytecode(CameraModule)

assert(type(Bytecode) == "string" and #Bytecode > 0, "CameraModule bytecode was unavailable")
assert(type(writefile) == "function", "This helper requires the executor's writefile function")

writefile("CameraModule.luac", Bytecode)
print("Exported CameraModule.luac", #Bytecode, "bytes; version", string.byte(Bytecode, 1))
