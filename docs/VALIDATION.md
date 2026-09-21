# Validation record

The final Windows Release build used ClangCL 22.1.3, CMake, and the installed Windows SDK.

`build/Release/taze-tests.exe` reported **1,176 passed, 0 failed**. CTest also passed. These are individual fixture/configuration and input-validation checks, not 1,176 distinct source programs. The suite compares execution results across optimization levels 0–2, stripped/full debug data, structured/fallback emission, versions 9 and 11–14, and plain/Roblox opcode encodings.

The Luau dependency header checksum and supported/unsupported features are documented in `README.md`.

## Live Roblox check

- Client version: `0.739.0.7390687`.
- User-selected input: `game.Players.LocalPlayer.PlayerScripts.PlayerModule.CameraModule` through `getscriptbytecode`.
- Input: 13,117 bytes, bytecode version 12, type metadata version 3, 32 prototypes.
- The input parsed, decompiled, and recompiled locally.
- Roblox's `loadstring` accepted the generated CameraModule source.
- `examples/RobloxFixture.luau` and its decompiled counterpart executed in the connected client and returned identical values, including the count of trailing nil results.
- Final harness result: `{ Passed = true, Version = "0.739.0.7390687" }`.

CameraModule functions 10, 11, 12, 18, 19, and 28 (zero-based prototype IDs) used explicit state-machine output. CameraModule's top-level code was not executed, and the running camera controller was not replaced. This check establishes parsing and compilation compatibility for that live module; it does not prove its entire behavior is equivalent.

The captured bytecode, generated game-module source, and local harness are deliberately excluded from Git under `artifacts/`.
