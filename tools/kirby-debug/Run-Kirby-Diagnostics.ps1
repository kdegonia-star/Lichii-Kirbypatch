# Run next to lichii.exe to capture existing DKB frame/GPU diagnostics.
# No emulator settings or shader caches are deleted.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "lichii.exe"
if (!(Test-Path $exe)) { throw "Place this script next to lichii.exe." }
$env:EDEN_DKB_FRAME_TRACE = "1"
$env:EDEN_NX2_PRECOMPILED = "1"
$session = Join-Path $root ("kirby-debug-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Path $session | Out-Null
@"
Session: $(Get-Date -Format o)
Build: $exe
EDEN_DKB_FRAME_TRACE=1
EDEN_NX2_PRECOMPILED=1
"@ | Set-Content (Join-Path $session "session.txt")
Write-Host "Starting Lichii with DKB frame and GPU tracing enabled."
Write-Host "Reproduce the shader-related FPS collapse, exit Lichii, then send eden_log.txt."
Write-Host "Session details: $session"
Start-Process -FilePath $exe -WorkingDirectory $root -Wait
$logLocations = @(
  (Join-Path $env:APPDATA "eden\\log\\eden_log.txt"),
  (Join-Path $env:APPDATA "eden\\log\\eden_log.0.txt"),
  (Join-Path $env:APPDATA "eden\\log\\eden_log.1.txt")
)
foreach ($path in $logLocations) {
  if (Test-Path $path) {
    Copy-Item $path -Destination (Join-Path $session (Split-Path $path -Leaf)) -Force
  }
}
Write-Host "Captured available logs in $session"
