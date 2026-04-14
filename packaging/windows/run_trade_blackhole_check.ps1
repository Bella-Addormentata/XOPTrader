param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$ConfigPath,
    [string]$LogPath,
    [ValidateSet("text", "json")]
    [string]$Format = "text",
    [ValidateSet("none", "medium", "high")]
    [string]$FailOn = "high"
)

if (-not $ConfigPath) {
    $ConfigPath = Join-Path $RepoRoot "config.yaml"
}
if (-not $LogPath) {
    $LogPath = Join-Path $RepoRoot "logs\trade-blackhole-check.log"
}

$scriptPath = Join-Path $RepoRoot "scripts\check_trade_blackholes.py"
if (-not (Test-Path $scriptPath)) {
    Write-Error "Missing checker script: $scriptPath"
    exit 1
}

$logDir = Split-Path -Parent $LogPath
if ($logDir -and -not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}

$pythonExe = $null
$pythonArgs = @()
if (Get-Command py -ErrorAction SilentlyContinue) {
    $pythonExe = "py"
    $pythonArgs = @("-3")
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    $pythonExe = "python"
}

if (-not $pythonExe) {
    Write-Error "Python 3 launcher not found. Install Python or adjust the wrapper."
    exit 1
}

$stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
"[$stamp] Running trade blackhole audit" | Tee-Object -FilePath $LogPath -Append

& $pythonExe @pythonArgs $scriptPath --config $ConfigPath --format $Format --fail-on $FailOn 2>&1 |
    Tee-Object -FilePath $LogPath -Append

$exitCode = $LASTEXITCODE
$stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
"[$stamp] ExitCode=$exitCode" | Tee-Object -FilePath $LogPath -Append
exit $exitCode