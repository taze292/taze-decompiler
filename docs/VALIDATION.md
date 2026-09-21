# Validation record

The final Windows Release build used ClangCL 22.1.3, CMake, and the installed Windows SDK.

`build/Release/taze-tests.exe` reported **1,527 passed, 0 failed**. CTest also passed. These are individual fixture/configuration and input-validation checks across 55 source fixtures, not 1,527 distinct source programs. The suite compares execution results across optimization levels 0–2, stripped/full debug data, structured/fallback emission, versions 9 and 11–14, and plain/Roblox opcode encodings.

New regression fixtures cover chained enum tests, shared branch suffixes, early returns, mixed boolean precedence, false/nil values, side effects in conditions, loop exits, scoped captures, method installation, field/method evaluation order, naming collisions, and parenthesized call statements. These fixtures explicitly reject state-machine fallback in normal mode. The output for the fixtures is also checked for statement semicolons.

The Luau dependency header checksum and supported/unsupported features are documented in `README.md`.

## Live Roblox check

- Client version: `0.739.0.7390687`.
- User-selected input: `game.Players.LocalPlayer.PlayerScripts.PlayerModule.CameraModule` through `getscriptbytecode`.
- Input: 13,117 bytes, bytecode version 12, type metadata version 3, 32 prototypes.
- The input parsed, decompiled, and recompiled locally.
- Roblox's `loadstring` accepted the generated CameraModule source.
- `examples/RobloxFixture.luau` and its decompiled counterpart executed in the connected client and returned identical values, including the count of trailing nil results.
- Final harness result: `{ Passed = true, Version = "0.739.0.7390687" }`.

All 32 CameraModule prototypes now use structured output; none requires a state machine. The previous output was 92,557 bytes; the revised output is 25,048 bytes, with no semicolons or `ProgramCounter` dispatch. Both measurements include the standard header, line comments, and upvalue comments.

The user-supplied, slightly older `CameraModule.luau` comparison source was also compiled at optimization levels 0–2 and debug levels 0/2. All six versions decompiled without fallback and their recovered source compiled successfully. Generated sizes ranged from 24,935 to 26,520 bytes. This source is excluded from Git, and no module-specific source template or replacement is built into the decompiler.

CameraModule's top-level code was not executed, and the running camera controller was not replaced. These CameraModule checks establish parsing and compilation compatibility; they do not prove its entire behavior is equivalent. Behavioral equivalence checks apply to the executable regression fixtures.

The captured bytecode, generated game-module source, and local harness are deliberately excluded from Git under `artifacts/`.
