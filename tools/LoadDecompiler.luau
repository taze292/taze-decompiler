local Ok, Source = pcall(game.HttpGet, game, "https://raw.githubusercontent.com/taze292/taze-decompiler/v0.2.0/tools/Decompile.luau")
if not Ok or type(Source) ~= "string" then return end
local Chunk = loadstring(Source)
if type(Chunk) ~= "function" then return end
pcall(Chunk)
