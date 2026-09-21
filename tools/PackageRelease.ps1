param([string]$Version = '0.1.0', [string]$BuildDirectory = 'build-release')
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Version must use major.minor.patch format.' }
$Project = Split-Path $PSScriptRoot -Parent
$Binary = Join-Path $Project "$BuildDirectory/Release/taze.exe"
$Loader = Join-Path $PSScriptRoot 'LoadDecompiler.luau'
$Url = "https://raw.githubusercontent.com/taze292/taze-decompiler/v$Version/tools/Decompile.luau"
if (-not (Test-Path -LiteralPath $Binary)) { throw 'Build the Release executable first.' }
if (-not (Get-Content -LiteralPath $Loader -Raw).Contains($Url)) { throw 'Update LoadDecompiler.luau to the release tag first.' }
$Output = Join-Path $Project "artifacts/releases/v$Version"
$Package = Join-Path $Output 'package'
New-Item -ItemType Directory -Path $Package -Force | Out-Null
Copy-Item -LiteralPath $Binary -Destination (Join-Path $Output 'taze.exe')
Copy-Item -LiteralPath $Loader -Destination (Join-Path $Output 'Decompile.luau')
Copy-Item -LiteralPath $Binary -Destination (Join-Path $Package 'taze.exe')
Copy-Item -LiteralPath $Loader -Destination (Join-Path $Package 'Decompile.luau')
Copy-Item -LiteralPath (Join-Path $Project 'THIRD_PARTY_NOTICES.md') -Destination $Package
Copy-Item -LiteralPath (Join-Path $Project 'docs/Luau-LICENSE.txt') -Destination $Package
@'
@echo off
"%~dp0taze.exe" --serve
pause
'@ | Set-Content -LiteralPath (Join-Path $Package 'Start-Decompiler.cmd') -Encoding ascii
@"
Taze Decompiler v$Version - Windows x64

1. Extract all files from the ZIP.
2. Run Start-Decompiler.cmd, or run: taze.exe --serve
3. Keep the server running. The default port is 8877 on 127.0.0.1.
4. Execute Decompile.luau in your Roblox executor. It downloads the bridge
   from the v$Version GitHub tag and replaces the executor's decompile function.
5. Call decompile(YourScriptInstance) to obtain source.

The executor must provide getscriptbytecode and a compatible WebSocket API.
The loader also requires loadstring and game:HttpGet.
If you already have a Taze server running, close it before starting this one.
Use getgenv().TazeBridge.Stop() to restore the previous decompiler.

Offline bridge script: tools/Decompile.luau in the tagged GitHub source.
Loader URL: $Url
CLI help: taze.exe --help
File input: taze.exe input.luac -o output.luau

The executable includes the C++ runtime. Clang and the Luau source are not
needed to run it. Inputs must be raw decompressed serialized Luau bytecode.
This release does not guarantee perfect recovery of every Roblox script.
Luau attribution and license are included alongside this file.

Documentation: https://github.com/taze292/taze-decompiler/tree/v$Version
"@ | Set-Content -LiteralPath (Join-Path $Package 'README.txt') -Encoding utf8
$Files = @('taze.exe', 'Decompile.luau', 'Start-Decompiler.cmd', 'README.txt', 'THIRD_PARTY_NOTICES.md', 'Luau-LICENSE.txt') |
    ForEach-Object { Join-Path $Package $_ }
$Archive = "Taze-Decompiler-v$Version-windows-x64.zip"
Compress-Archive -LiteralPath $Files -DestinationPath (Join-Path $Output $Archive) -Force
@('taze.exe', 'Decompile.luau', $Archive) | ForEach-Object {
    $Hash = (Get-FileHash -LiteralPath (Join-Path $Output $_) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$Hash  $_"
} | Set-Content -LiteralPath (Join-Path $Output 'SHA256SUMS.txt') -Encoding ascii
Write-Output $Output
