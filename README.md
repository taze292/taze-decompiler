# Taze Decompiler

A C++20 Luau bytecode decompiler with a command-line tool and a reusable static library. It reconstructs Luau source with PascalCase local names, four-space indentation, separated function blocks, starting-line comments, and numbered `Copy` / `Reference` upvalue comments.

The supplied Luau source stays on disk as a build dependency and is **excluded from Git**. Build products and extracted game scripts are excluded too.

## Download

Download [Taze v0.1.0 for Windows x64](https://github.com/taze292/taze-decompiler/releases/tag/v0.1.0). Extract the ZIP and run `Start-Decompiler.cmd` (or run `taze.exe --serve`). Keep it running, then execute the included `Decompile.luau` loader in your executor. The loader downloads the bridge pinned to the same release tag. The executable and loader are also available as individual assets, with SHA-256 checksums. The packaged executable includes the C++ runtime and does not require Clang or the Luau source checkout.

## Build on this Windows machine

```powershell
./build.ps1 -Test
```

The script finds CMake in PATH or the installed Visual Studio directories and builds with ClangCL. This workspace was built with Clang 22.1.3 and the Windows SDK. Outputs:

- `build/Release/taze.exe`: decompiler and disassembler
- `build/Release/TazeDecompiler.lib`: C++ library
- `build/Release/taze-compile.exe`: local fixture compiler
- `build/Release/taze-tests.exe`: regression tests

The default dependency locations are `luau/` or the existing `luau-master/luau-master/`. To use another checkout:

```powershell
./build.ps1 -Test -LuauDirectory C:/path/to/luau
```

For a fresh clone, obtain the [official Luau source](https://github.com/luau-lang/luau) separately. The snapshot used here declares opcodes through `FASTPCALL` and `NEWCLASS`, with a maximum ordinary bytecode version of 14. Its `Common/include/Luau/Bytecode.h` SHA-256 is:

```text
f65c8a45e3c6f3888c923b805b8698c74d612a27ca68918d3065995dbbfda669
```

On other systems with CMake and a C++20 compiler:

```sh
cmake -S . -B build -DTAZE_LUAU_DIR=/path/to/luau -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`-DTAZE_BUILD_TESTS=OFF` builds only the decompiler and library. These depend on Luau's bytecode-definition headers, not its compiler or VM libraries. The built decompiler does not need the Luau checkout at runtime.

## Use

### In-game WebSocket bridge (Windows)

Start the executable from this project directory and leave it running:

```powershell
./build/Release/taze.exe --serve
```

Run **`tools/Decompile.luau`** in your executor. It checks for `getscriptbytecode` and a supported WebSocket API, connects to `ws://127.0.0.1:8877/decompile`, and replaces `getgenv().decompile` only after connecting successfully. Then use:

```luau
local Source = decompile(game.Players.LocalPlayer.PlayerScripts.PlayerModule.CameraModule)
print(Source)
-- Optional: setclipboard(Source) or writefile("CameraModule.decompiled.luau", Source)
```

`decompile(Script, TimeoutSeconds)` returns source or raises an explanatory error. The default timeout is 30 seconds. Calls yield while waiting; concurrent calls are matched by request IDs. The next call reconnects after a disconnection. A timeout stops waiting for that request; it does not cancel work already running in C++.

For ModuleScripts, the bridge supplies a quoted instance path so exported functions can have copyable `require(...)` lookups. It does not require the module while decompiling. On installation it also probes `filtergc` with a known closure and both correct and incorrect line numbers, preferring verified `StartLine` support, then `Line`. Inconclusive probes default to `Line`. Inspect `getgenv().TazeBridge.FilterLineField` and `.LineProbe` (`"verified"` or `"fallback"`) for the result.

To change the port, run `taze.exe --serve --port 8878`, and set this **before** executing the bridge script:

```luau
getgenv().TazeConfig = {
    Url = "ws://127.0.0.1:8878/decompile",
    Timeout = 60,
}
```

Re-running the script replaces its previous connection cleanly. To disconnect and restore the original executor function:

```luau
getgenv().TazeBridge.Stop()
```

Bridge v3 also hooks existing decompiler references when `hookfunction` is available, so tools that cached the previous function can use Taze. It preserves these targets across reinstalls and restores their previous implementations on `Stop()`. This uses the executor's [hookfunction API](https://docs.chimera.best/Closures/hookfunction/); unsupported hooks fall back to global replacement. `TazeBridge.HookStatus` reports `installed`, `failed`, or `unavailable`. `TazeBridge.Decompile(Script)` is a direct entry point if another tool replaces the global later.

After a call, `TazeBridge.LastRequest` shows `ModulePath`, `PathStatus`, `ExportLookups`, `GcLookups`, and `FilterLineField`. Cloned instance references are compared with `compareinstances` when available. Missing or ambiguous module paths produce a warning instead of silently disabling `require` lookups. Copy the current `tools/Decompile.luau`; its installation message identifies version 3.

Supported executor APIs are `WebSocket.connect`, `websocket.connect`, or `syn.websocket.connect`, with `Send`, `Close`, `OnMessage`, and `OnClose`. The server binds only to IPv4 loopback, supports up to eight connected clients, and disconnects sockets after ten idle minutes; the bridge reconnects on demand. Stop the server with Ctrl+C in its terminal. Existing formatting options such as `--indent 2` and `--no-upvalues` also apply in server mode. Other platforms retain file-based decompilation.

The [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455) transport supports masked client frames, fragmented text messages, ping/pong, and closing frames. The bridge sends `RequestId\nTAZE2\nLineField\nHexModulePath\nHexBytecode`; the path field is empty when unavailable. Legacy `RequestId\nHexBytecode` requests remain supported. Replies are `RequestId\nok\nSource` or `RequestId\nerror\nMessage`. Hex preserves arbitrary bytecode bytes without depending on executor-specific base64 helpers. The service only decompiles supplied bytes; it does not execute them or access paths supplied by clients. No third-party networking dependency is required.

### File input

```powershell
./build/Release/taze.exe artifacts/CameraModule.luac -o artifacts/CameraModule.luau
./build/Release/taze.exe artifacts/CameraModule.luac --disassemble -o artifacts/CameraModule.asm
./build/Release/taze.exe --help
```

Input must be **raw, decompressed serialized Luau bytecode**, such as the string returned by `getscriptbytecode`. Source text, base64, hex text, encrypted/compressed asset containers, process memory, and native machine code are not bytecode inputs. Opcode encoding is detected by validating the entire instruction stream; override it with `--encoding plain` or `--encoding roblox`. The Roblox decoder multiplies encoded opcode bytes by 203 modulo 256, leaving AUX data unchanged. It accepts the observed 24-byte Roblox trailer only after validating Roblox-encoded instructions; the trailer is opaque and is not authenticated.

Other options: `--indent 2`, `--no-header`, `--no-lines`, `--no-filtergc`, `--no-upvalues`, and `--state-machine`. Failures produce a nonzero exit code and an explanatory message. Reconstruction finishes before the output file is opened, so an unsupported input does not leave a partial source file.

Use `--module-path 'game.ReplicatedStorage.MyModule'` to enable exported-function lookups for file input, and `--filter-line StartLine` to select that GC filter key manually. The default is `Line`. The corresponding C++ fields are `Options.ModulePath` and `Options.FilterLineField`.

### Local fixture

```powershell
./build/Release/taze-compile.exe examples/RobloxFixture.luau artifacts/Fixture.luac 1 2
./build/Release/taze.exe artifacts/Fixture.luac -o artifacts/Fixture.luau
```

The compiler's last two arguments are optimization level and debug level. Debug level 2 supplies local/function names; stripped bytecode requires inferred names. A typical recovered function looks like:

```luau
local UserInputService = game:GetService("UserInputService")
local RunService = game:GetService("RunService")

local function ServiceNames() -- Line: 4 | filtergc("function", { Line = 4, Name = "ServiceNames", Constants = { "Name" } }, true)
    --[[
        Upvalues:
        1: UserInputService (type "Copy")
        2: RunService (type "Copy")
    ]]

    return UserInputService.Name, RunService.Name
end
```

Global names, table fields, and method names retain their original spelling, because changing them would change program behavior. Generated locals and function bindings use PascalCase and avoid collisions with globals and enclosing locals. Names are allocated after optimization, so eliminated temporaries do not leave names such as `Value438` or unnecessary suffixes on imported modules. Anonymous locals with a known type use independent `Table1`, `Function1`, `String1`, `Number1`, `Boolean1`, `Vector1`, or `Integer1` sequences. Type propagation follows copies and combines branch/loop definitions; mixed or unknown types retain `Value1`, `Value2`, etc. Debug names and meaningful inferred names take precedence. A missing line number is reported as `Unknown`. Comments, type annotations, and names erased by the compiler cannot be recovered exactly.

Generated statements have no semicolons. Parenthesized call statements use a `do` block where needed to avoid Luau's ambiguous statement boundary; literal string contents remain intact. Consecutive locals stay together, with separate groups for services and module imports. Blank lines separate declaration groups, writes, calls, returns, multiline tables, and control-flow blocks. Initializers stay in their original evaluation order. Blank lines contain no trailing indentation.

Each recovered function's line comment includes an inline, copyable lookup. When its export path can be recovered and a module path is available, this becomes, for example, `-- Line: 4 | require(game.ReplicatedStorage.MyModule).Utilities.Read`. Nested tables, numeric or quoted keys, directly returned functions, and recognizable constructor/metatable exports are supported. A bounded static analysis follows returned values without executing the module. Conditional or conflicting exports, unknown mutations, and other unresolved cases retain GC lookups.

For `filtergc` lookups, `Name` is included when the prototype contains an original debug name; generated PascalCase names are never substituted for missing metadata. Constants come from that function's own bytecode constant pool: strings, booleans, numbers, vectors, integers, and import expressions such as `Enum.CameraType.Custom`. Strings and names are escaped so the entire lookup stays on one line. Upvalues and immediate instruction operands are not constant-pool entries. Nil/NaN and table/closure templates are omitted because an array filter cannot meaningfully represent nil/NaN or recreate live object identity. If a start line is absent, the displayed line is `Unknown` and the selected line filter is `nil`.

GC lookups depend on the executor's `filtergc` support and the function being alive; line/constants alone do not guarantee a unique match. The connected executor successfully matched all 31 emitted CameraModule lookups; the updated probe verified `StartLine`. Use `--no-filtergc` (or `Options.IncludeFunctionLocators = false`) to suppress either kind of lookup and keep just the line number; `--no-lines` hides the entire comment. The API's [function-filter documentation](https://docs.chimera.best/Environment/filtergc/FunctionFilterOptions/) describes constant matching.

## C++ API

```cpp
#include <Taze/Decompiler.hpp>

Taze::Options Options;
Options.Encoding = Taze::OpcodeEncoding::Auto;

Taze::Result Result = Taze::Decompile(BytecodeString, Options);
// Result.Source contains Luau source.
// Result.Diagnostics reports control-flow fallback decisions.
// Malformed or unsupported input throws Taze::Error.
```

`Taze::Disassemble` exposes prototypes, constants, local ranges, decoded instructions, capture descriptors, and branch targets. Resource limits cover input size, output size, prototype nesting, and dataflow analysis.

## Compatibility and tests

The reader supports serialized versions **3–14**, type metadata 1–3, and parses the experimental version-100 envelope. Execution tests cover compiler-produced versions **9, 11, 12, 13, and 14**, including Roblox opcode encoding. Older reader branches are not backed by historical compiler binaries in this repository.

Implemented reconstruction includes arithmetic, comparisons, short-circuit control flow, imports, table/member operations, method calls, fixed and open return tuples, varargs, closures, copy/reference captures, upvalue closing, numeric/generic loops, and fastcall fallback paths. Register dataflow separates reused temporary slots and merges live values at control-flow joins. Explicit local snapshots and reference cells preserve captured lifetimes without generated, immediately invoked closure factories. Recursive closures use named local functions where possible. Integer constants use `integer.fromstring`; vector constants use `vector.create`, so those outputs require the corresponding target-runtime libraries.

Control-flow structuring builds a successor/predecessor graph and computes dominators, postdominators, and register liveness. Natural loops and branch continuations become ordinary Luau blocks. Early returns retain their lexical continuation instead of duplicating the remainder of the function. Subsequent passes combine shared branches, recover short-circuit expressions and `elseif` chains, fold iterator setup into `for`, and place local declarations in the smallest shared scope. These transformations use bytecode information; the user-supplied CameraModule source is only a local comparison fixture.

The tests compile source, decompile it, recompile the result, and compare returned values in the Luau VM. They exercise optimization levels 0–2, debug levels 0/2, normal and forced state-machine output, mutable captures, nested loops, recursion, yielding, nil-containing tuples, metamethods, and evaluation order. Additional checks cover versioned serialization, encoded opcodes, trailers, malformed headers, and every truncated prefix of a valid fixture.

### Connected Roblox validation

Tested against client **0.739.0.7390687** using the specifically selected script:

```luau
getscriptbytecode(game.Players.LocalPlayer.PlayerScripts.PlayerModule.CameraModule)
```

The captured module contains **13,117 bytes**, bytecode version **12**, type version **3**, and **32 prototypes**. Its decompiled output compiled successfully both locally and through Roblox's `loadstring`. All 32 prototypes reconstruct without state-machine fallback. Control-flow improvements reduced the original 92,557-byte output to 25,048 bytes; with the requested spacing and inline lookup comments, the current bridge output is **32,900 bytes** using the verified `StartLine` key (32,745 with `Line`). It contains no generated capture-factory calls. CameraModule calls its constructor but returns an empty table, so its functions correctly retain GC lookups. The project fixture also executed in Roblox with identical results before and after decompilation, including services, mutable closures, loops, and trailing nil varargs.

CameraModule itself was **compiled, not executed or substituted for the running camera controller**. A successful compilation does not establish full behavioral equivalence for that module. Extracted bytecode, generated CameraModule source, and test harnesses live in the ignored `artifacts/` directory.

`tools/ExportCameraModule.luau` exports the selected module to an executor workspace when `writefile` is available. To regenerate the project fixture and the optional CameraModule compile harness:

```powershell
./tools/PrepareRobloxTest.ps1
```

Execute the resulting `artifacts/RobloxTest.luau` through the connected client. `getgenv().TazeTestResult` records success or the error. The helper does not start a server or replace any Roblox functions.

## Limits

This is a working decompiler, **not a guarantee of perfect source recovery for every Roblox script**.

- Control flow that cannot be safely structured is emitted as a readable `ProgramCounter` loop and reported in diagnostics. It is less concise than the original source. The fallback for generalized iteration uses a coroutine adapter; custom iterators that inspect coroutine identity or yield have different execution context in that fallback.
- Experimental Luau class instructions/constants, runtime-patched `NATIVECALL`, and nonzero fourth vector components are rejected explicitly. Unknown future versions/opcodes are rejected rather than guessed.
- Debug-stack inspection, closure identity, environment manipulation, and undefined table-length/iteration-order behavior are not promised to match the original compiled program. Generated helpers use standard library functions.
- Original formatting, comments, types, and stripped identifiers are unavailable. Heuristics infer useful service/module/member names where possible; remaining locals use generated PascalCase names.

The implementation studies the official `Bytecode.h`, `BytecodeUtils.h`, `lvmload.cpp`, and `lvmexecute.cpp`. See `THIRD_PARTY_NOTICES.md` for attribution.
