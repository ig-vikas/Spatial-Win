param([string]$Version = '1.1.4', [string]$MakeNsis)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
cmake -S windows -B windows/build -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'Windows configuration failed' }
cmake --build windows/build --config Release
if ($LASTEXITCODE -ne 0) { throw 'Windows build failed' }
$stage = Join-Path $PWD "dist/Spatial-$Version-Windows-x64"
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -Path windows/build/Release/Spatial.exe,windows/build/Release/phonon.dll,windows/README.md -Destination $stage
Copy-Item -Path licenses/STEAM-AUDIO-LICENSE.txt,licenses/STEAM-AUDIO-THIRDPARTY.txt -Destination $stage
Compress-Archive -LiteralPath $stage -DestinationPath "$stage.zip" -CompressionLevel Optimal -Force
Get-FileHash -LiteralPath "$stage.zip" -Algorithm SHA256

if (-not $MakeNsis) {
    $MakeNsis = (Get-Command makensis.exe -ErrorAction SilentlyContinue).Source
}
if (-not $MakeNsis) {
    $MakeNsis = Join-Path ${env:ProgramFiles(x86)} 'NSIS/makensis.exe'
}
if (-not (Test-Path -LiteralPath $MakeNsis -PathType Leaf)) {
    Write-Warning 'The portable ZIP is ready. Pass the NSIS compiler path with -MakeNsis to also create Setup.'
    return
}
$setup = "$stage-Setup.exe"
& $MakeNsis /V2 "/DSPATIAL_VERSION=$Version" "/DPAYLOAD_DIR=$stage" "/DSETUP_PATH=$setup" 'windows/setup.nsi'
if ($LASTEXITCODE -ne 0) { throw 'Windows installer build failed' }
Get-FileHash -LiteralPath $setup -Algorithm SHA256
