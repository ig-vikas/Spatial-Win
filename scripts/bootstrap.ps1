$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
New-Item -ItemType Directory -Force '.tools','third_party' | Out-Null
if (!(Test-Path 'third_party/steamaudio/include/phonon.h')) {
    Invoke-WebRequest 'https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip' -OutFile '.tools/steamaudio.zip'
    $hash = (Get-FileHash '.tools/steamaudio.zip' -Algorithm SHA256).Hash
    if ($hash -ne '4A0AA5EC1176F38F0B0993A37C2259D9E86F27E22D5E24F83EC4C3CB9A1D5449') { throw 'Steam Audio archive checksum mismatch' }
    Expand-Archive -LiteralPath '.tools/steamaudio.zip' -DestinationPath 'third_party' -Force
}
Write-Output 'Steam Audio is ready. Run scripts/build-windows.ps1 to build Spatial.'
