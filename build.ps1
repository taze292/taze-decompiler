param([switch]$Test, [string]$Configuration = 'Release', [string]$LuauDirectory = '')
$ErrorActionPreference = 'Stop'
$ProjectRoot = $PSScriptRoot
$CMakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$CMakePath = if ($CMakeCommand) { $CMakeCommand.Source } else {
    $Candidates = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    )
    $Candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $CMakePath) { throw 'CMake was not found. Install CMake or use a Visual Studio developer shell.' }
$ConfigureArguments = @('-S', $ProjectRoot, '-B', "$ProjectRoot/build", '-G', 'Visual Studio 17 2022', '-T', 'ClangCL', '-A', 'x64')
if ($LuauDirectory) { $ConfigureArguments += "-DTAZE_LUAU_DIR=$LuauDirectory" }
& $CMakePath @ConfigureArguments
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
& $CMakePath --build "$ProjectRoot/build" --config $Configuration --parallel 8
if ($LASTEXITCODE) { throw 'Build failed.' }
if ($Test) {
    & "$([System.IO.Path]::GetDirectoryName($CMakePath))/ctest.exe" --test-dir "$ProjectRoot/build" -C $Configuration --output-on-failure
    if ($LASTEXITCODE) { throw 'Tests failed.' }
}
