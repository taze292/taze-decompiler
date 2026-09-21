# Taze Decompiler

A C++20 Luau bytecode decompiler with a command-line tool and a reusable static library. It reconstructs Luau source with PascalCase local names, four-space indentation, separated function blocks, starting-line comments, and numbered `Copy` / `Reference` upvalue comments.

The supplied Luau source stays on disk as a build dependency and is **excluded from Git**. Build products and extracted game scripts are excluded too.

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

```powershell
./build/Release/taze.exe artifacts/CameraModule.luac -o artifacts/CameraModule.luau
./build/Release/taze.exe artifacts/CameraModule.luac --disassemble -o artifacts/CameraModule.asm
./build/Release/taze.exe --help
```

Input must be **raw, decompressed serialized Luau bytecode**, such as the string returned by `getscriptbytecode`. Source text, base64, hex text, encrypted/compressed asset containers, process memory, and native machine code are not bytecode inputs. Opcode encoding is detected by validating the entire instruction stream; override it with `--encoding plain` or `--encoding roblox`. The Roblox decoder multiplies encoded opcode bytes by 203 modulo 256, leaving AUX data unchanged. It accepts the observed 24-byte Roblox trailer only after validating Roblox-encoded instructions; the trailer is opaque and is not authenticated.

Other options: `--indent 2`, `--no-header`, `--no-lines`, `--no-upvalues`, and `--state-machine`. Failures produce a nonzero exit code and an explanatory message. Reconstruction finishes before the output file is opened, so an unsupported input does not leave a partial source file.

### Local fixture

```powershell
./build/Release/taze-compile.exe examples/RobloxFixture.luau artifacts/Fixture.luac 1 2
./build/Release/taze.exe artifacts/Fixture.luac -o artifacts/Fixture.luau
```

The compiler's last two arguments are optimization level and debug level. Debug level 2 supplies local/function names; stripped bytecode requires inferred names. A typical recovered function looks like:

```luau
local UserInputService = game:GetService("UserInputService")
local RunService = game:GetService("RunService")

local function ServiceNames() -- Line: 4
    --[[
        1: UserInputService (type "Copy")
        2: RunService (type "Copy")
    ]]

    return UserInputService.Name, RunService.Name
end
```

Global names, table fields, and method names retain their original spelling, because changing them would change program behavior. Generated locals and function bindings use PascalCase and avoid collisions with globals. A missing line number is reported as `Unknown`. Comments, type annotations, and names erased by the compiler cannot be recovered exactly.

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

Implemented reconstruction includes arithmetic, comparisons, short-circuit control flow, imports, table/member operations, method calls, fixed and open return tuples, varargs, closures, copy/reference captures, upvalue closing, numeric/generic loops, and fastcall fallback paths. Register dataflow separates reused temporary slots and merges values at control-flow joins. Copy snapshots and reference cells preserve captured lifetimes. Integer constants use `integer.fromstring`; vector constants use `vector.create`, so those outputs require the corresponding target-runtime libraries.

The tests compile source, decompile it, recompile the result, and compare returned values in the Luau VM. They exercise optimization levels 0–2, debug levels 0/2, normal and forced state-machine output, mutable captures, nested loops, recursion, yielding, nil-containing tuples, metamethods, and evaluation order. Additional checks cover versioned serialization, encoded opcodes, trailers, malformed headers, and every truncated prefix of a valid fixture.

### Connected Roblox validation

Tested against client **0.739.0.7390687** using the specifically selected script:

```luau
getscriptbytecode(game.Players.LocalPlayer.PlayerScripts.PlayerModule.CameraModule)
```

The captured module contains **13,117 bytes**, bytecode version **12**, type version **3**, and **32 prototypes**. Its decompiled output compiled successfully both locally and through Roblox's `loadstring`. Six functions use the explicit state-machine fallback. The project fixture also executed in Roblox with identical results before and after decompilation, including services, mutable closures, loops, and trailing nil varargs.

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
