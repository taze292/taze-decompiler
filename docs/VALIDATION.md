# Validation record

The final Windows Release build used ClangCL 22.1.3, CMake, and the installed Windows SDK.

`build/Release/taze-tests.exe` reported **1,587 passed, 0 failed**. CTest also passed. These are individual fixture/configuration, metadata and input-validation checks, not 1,587 distinct source programs. The suite compares execution results across 55 behavioral fixtures, optimization levels 0–2, stripped/full debug data, structured/fallback emission, versions 9 and 11–14, and plain/Roblox opcode encodings.

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

All 32 CameraModule prototypes use structured output; none requires a state machine. Control-flow structuring reduced output from 92,557 to 25,048 bytes. The current bridge output is 32,900 bytes with spacing, `Upvalues:` labels, and inline `StartLine` lookup comments (32,745 with `Line`); it still contains no semicolons or `ProgramCounter` dispatch.

The user-supplied, slightly older `CameraModule.luau` comparison source was also compiled at optimization levels 0–2 and debug levels 0/2. All six versions decompiled without fallback and their recovered source compiled successfully. Generated sizes ranged from 24,935 to 26,520 bytes. This source is excluded from Git, and no module-specific source template or replacement is built into the decompiler.

CameraModule's top-level code was not executed, and the running camera controller was not replaced. These CameraModule checks establish parsing and compilation compatibility; they do not prove its entire behavior is equivalent. Behavioral equivalence checks apply to the executable regression fixtures.

The captured bytecode, generated game-module source, and local harness are deliberately excluded from Git under `artifacts/`.

## WebSocket bridge

The Windows integration test starts a temporary loopback server and runs **20 transport checks**, including the RFC handshake hash, CLI/source parity, fragmented requests, invalid hex/bytecode followed by valid requests on the same socket, payloads above 65,535 bytes, simultaneous clients, reconnection, ping/pong, closing frames, missing masks, and oversized frame rejection. Metadata tests cover module export paths, the `StartLine` override, invalid filter keys, and newline rejection in module paths. CTest runs this test alongside the decompiler checks when PowerShell is available.

The executor bridge was installed through the connected Roblox client. Its replacement `decompile` called `getscriptbytecode` on the authorized CameraModule, sent the result to the C++ server, and returned **25,048 bytes**. Roblox's `loadstring` accepted the returned source, which contained no semicolons. The native executor sends a `ws://127.0.0.1:port` Origin header; this is accepted, while browser HTTP(S) origins are rejected.

The live bridge also recovered after the server process was restarted, restored the original executor function through `TazeBridge.Stop()`, and completed two concurrent CameraModule requests with matching response lengths. It was reinstalled and left connected after validation.

## Formatting and function lookups

Ten additional assertions check inline lookup metadata, string escaping (including newlines, quotes, backslashes and NUL), constant imports, exclusion of upvalues, local grouping, return separation, whitespace-free blank lines, comment suppression options, and start-line retention with stripped local names. Extracted lookup expressions are compiled and evaluated against a stub to verify their actual values.

The previous formatting revision returned the 32,734-byte CameraModule source in-game. It compiled successfully, and all **31** emitted `filtergc` expressions compiled and returned live function matches without errors. The `ShouldUseVehicleCamera` lookup was additionally checked for its exact debug name and start line 320. The root prototype has no function declaration comment, so it does not produce a lookup.

## Export lookups, capture cleanup, and line probing

Twelve module fixtures at optimization levels 0–2 add 36 checks: direct exports, nested tables, numeric and quoted keys, constructor/metatable instances, directly returned functions, discarded instances, ambiguous returns, conditional or overwritten fields, escaped tables, and cycles. Positive lookup expressions are executed against the module value and must resolve the expected callable function. Ambiguous cases must retain `filtergc`.

Six mock executor modes exercise the actual bridge source: `Line`, `StartLine`, both keys, ignored keys, missing `filtergc`, and hidden executor closures. The probe requires a matching closure at the correct line and no match at a deliberately wrong line, then defaults to `Line` if neither key is verified. These tests also exercise request metadata, path escaping, LocalScript handling, reconnection, reinstallation, and restoring the original function.

The behavioral fixtures now reject generated immediately invoked capture factories. Existing scoped-capture, recursive-function, reference-mutation, and evaluation-order cases continue to execute equivalently after replacing factories with explicit local snapshots.

The latest live bridge reported `{ FilterLineField = "StartLine", LineProbe = "verified" }`. Its 32,900-byte CameraModule result compiled in Roblox, contained no capture-factory calls, and the `ShouldUseVehicleCamera` lookup returned the expected named function. CameraModule calls its constructor and returns `{}`, so none of its methods is reachable through `require`; retaining GC locators here is intentional. The project behavioral harness was rerun and returned `{ Passed = true, Version = "0.739.0.7390687" }`. The updated server and bridge were left connected.

## BulletHandler and categorized names

In the user's subsequent game session, reinstalling the current bridge enabled the existing export analysis for `game.ReplicatedStorage.ModuleScripts.GunModules.BulletHandler`. The emitted `require(...).Fire` expression was evaluated and compared by identity with the module's exported `Fire` function; they matched. No export-analysis change was needed for this case. The bridge now exposes `Version = 2` and prints its export-lookup mode and detected line field during installation.

Seven additional checks cover stripped table, number, string, boolean, and anonymous function names, mixed-type branch fallback, and collisions with global names. The live BulletHandler output compiled successfully and contained `local Table1 = {}`, `local Table2 = {}`, `function Table1.Fire(Self)` with a `require(...).Fire` locator, and `return Table1`. The exported function was inspected, not called. The updated server and bridge were left running in that session.

## Bridge v3 and named GC lookups

The next live investigation found a version-2 bridge present but a different global decompiler wrapper returning an HTTP/TLS error. Global reassignment did not cover saved references. Version 3 hooks the original executor function and the currently installed wrapper where supported, preserves successful hook targets across reinstallation, and restores prior implementations on stop. The mock harness checks unavailable/rejected hooks, cached native and wrapper calls, repeated installation, restoration, cloned references, and detached module path diagnostics.

Live BulletHandler decompilation through the global function, direct bridge entry point, saved wrapper, and saved native function all emitted `require(...).Fire`, including after repeated installation. A cloned BulletHandler reference retained the same export path; evaluating the locator returned the identical exported function. BulletHandler and CameraModule output compiled. Projectile and ProjectileRender also compiled, each with two recovered export lookups.

GC locators now include original serialized function names when available. The executable metadata test verifies `Options.Name == "Probe"`, and an additional assertion rejects invented names in stripped bytecode. In-game, the CameraModule lookup containing `StartLine = 335, Name = "ShouldUseVehicleCamera"` returned the expected named function. The v3 bridge and updated executable were left running.
