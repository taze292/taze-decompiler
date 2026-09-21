# Validation record

The final Windows Release build used ClangCL 22.1.3, CMake, and the installed Windows SDK.

`build/Release/taze-tests.exe` reported **1,537 passed, 0 failed**. CTest also passed. These are individual fixture/configuration, metadata and input-validation checks, not 1,537 distinct source programs. The suite compares execution results across 55 behavioral fixtures, optimization levels 0–2, stripped/full debug data, structured/fallback emission, versions 9 and 11–14, and plain/Roblox opcode encodings.

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

All 32 CameraModule prototypes use structured output; none requires a state machine. Control-flow structuring reduced output from 92,557 to 25,048 bytes. The current output is 32,734 bytes with the subsequently added spacing, `Upvalues:` labels, and inline function lookup comments; it still contains no semicolons or `ProgramCounter` dispatch.

The user-supplied, slightly older `CameraModule.luau` comparison source was also compiled at optimization levels 0–2 and debug levels 0/2. All six versions decompiled without fallback and their recovered source compiled successfully. Generated sizes ranged from 24,935 to 26,520 bytes. This source is excluded from Git, and no module-specific source template or replacement is built into the decompiler.

CameraModule's top-level code was not executed, and the running camera controller was not replaced. These CameraModule checks establish parsing and compilation compatibility; they do not prove its entire behavior is equivalent. Behavioral equivalence checks apply to the executable regression fixtures.

The captured bytecode, generated game-module source, and local harness are deliberately excluded from Git under `artifacts/`.

## WebSocket bridge

The Windows integration test starts a temporary loopback server and runs **16 transport checks**, including the RFC handshake hash, CLI/source parity, fragmented requests, invalid hex/bytecode followed by valid requests on the same socket, payloads above 65,535 bytes, simultaneous clients, reconnection, ping/pong, closing frames, missing masks, and oversized frame rejection. CTest runs this test alongside the decompiler checks when PowerShell is available.

The executor bridge was installed through the connected Roblox client. Its replacement `decompile` called `getscriptbytecode` on the authorized CameraModule, sent the result to the C++ server, and returned **25,048 bytes**. Roblox's `loadstring` accepted the returned source, which contained no semicolons. The native executor sends a `ws://127.0.0.1:port` Origin header; this is accepted, while browser HTTP(S) origins are rejected.

The live bridge also recovered after the server process was restarted, restored the original executor function through `TazeBridge.Stop()`, and completed two concurrent CameraModule requests with matching response lengths. It was reinstalled and left connected after validation.

## Formatting and function lookups

Ten additional assertions check inline lookup metadata, string escaping (including newlines, quotes, backslashes and NUL), constant imports, exclusion of upvalues, local grouping, return separation, whitespace-free blank lines, comment suppression options, and start-line retention with stripped local names. Extracted lookup expressions are compiled and evaluated against a stub to verify their actual values.

The updated bridge returned the 32,734-byte CameraModule source in-game. It compiled successfully, and all **31** emitted `filtergc` expressions compiled and returned live function matches without errors. The `ShouldUseVehicleCamera` lookup was additionally checked for its exact debug name and start line 320. The root prototype has no function declaration comment, so it does not produce a lookup.
