param([string]$BuildDirectory = "$PSScriptRoot/../build/Release")
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ArtifactDirectory = Join-Path $ProjectRoot 'artifacts'
New-Item -ItemType Directory -Path $ArtifactDirectory -Force | Out-Null
$Fixture = Join-Path $ProjectRoot 'examples/RobloxFixture.luau'
& "$BuildDirectory/taze-compile.exe" $Fixture "$ArtifactDirectory/RobloxFixture.luac" 1 2
if ($LASTEXITCODE) { throw 'Fixture compilation failed.' }
& "$BuildDirectory/taze.exe" "$ArtifactDirectory/RobloxFixture.luac" -o "$ArtifactDirectory/RobloxFixture.decompiled.luau"
if ($LASTEXITCODE) { throw 'Fixture decompilation failed.' }
$Original = Get-Content -LiteralPath $Fixture -Raw
$Decompiled = Get-Content -LiteralPath "$ArtifactDirectory/RobloxFixture.decompiled.luau" -Raw
$Harness = @'
getgenv().TazeTestResult = { Running = true }
local Ok, Failure = pcall(function()
local function Run(Source)
    local Function, CompileError = loadstring(Source, "TazeFixture")
    assert(Function, CompileError)
    return table.pack(Function())
end

'@
$Harness += "`nlocal Expected = Run([====[`n" + $Original + "`n]====])`n"
$Harness += "local Actual = Run([====[`n" + $Decompiled + "`n]====])`n"
$Harness += @'
assert(Expected.n == Actual.n, "Return count differs")
for Index = 1, Expected.n do
    assert(Expected[Index] == Actual[Index], "Return value differs at " .. Index)
end
print("TAZE_FIXTURE_PASS", Actual.n, Actual[1], Actual[2], Actual[3], Actual[4])
'@
if (Test-Path -LiteralPath "$ArtifactDirectory/CameraModule.luac") {
    & "$BuildDirectory/taze.exe" "$ArtifactDirectory/CameraModule.luac" -o "$ArtifactDirectory/CameraModule.luau"
    if ($LASTEXITCODE) { throw 'CameraModule decompilation failed.' }
    $CameraSource = Get-Content -LiteralPath "$ArtifactDirectory/CameraModule.luau" -Raw
    $Harness += "`nlocal CameraFunction, CameraError = loadstring([====[`n" + $CameraSource + "`n]====], 'TazeCameraModule')`n"
    $Harness += "assert(CameraFunction, CameraError)`nprint('TAZE_CAMERA_COMPILE_PASS')`n"
}
$Harness += "`nend)`ngetgenv().TazeTestResult = { Passed = Ok, Error = not Ok and tostring(Failure) or nil, Version = version() }`n"
[IO.File]::WriteAllText("$ArtifactDirectory/RobloxTest.luau", $Harness)
Write-Output "$ArtifactDirectory/RobloxTest.luau"
