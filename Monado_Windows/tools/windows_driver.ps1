# [XCDE] Native source fallback. Build never opens a launcher or starts a game.
[CmdletBinding()]
param(
    [ValidateSet('Build', 'Play')][string]$Action = 'Build',
    [string]$InputFolder,
    [ValidateRange(1, 6)][int]$Jobs = 4,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$BuildArguments
)
$ErrorActionPreference = 'Stop'
$portRoot = Split-Path -Parent $PSScriptRoot
$python = Join-Path $portRoot '.venv\Scripts\python.exe'
if (Test-Path -LiteralPath (Join-Path $portRoot '.deps\python314\python.exe') -PathType Leaf) {
    $python = Join-Path $portRoot '.deps\python314\python.exe'
}
if (-not (Test-Path -LiteralPath $python -PathType Leaf)) {
    throw 'Project-local Python is missing. Run Monado_Windows\tools\setup_windows.ps1 -PythonPath <CPython 3.14 python.exe>; this never installs system tools.'
}
if ($Action -eq 'Build') {
    $arguments = @('-B', (Join-Path $portRoot 'scripts\build_windows.py'), '--jobs', $Jobs.ToString())
    if ($InputFolder) { $arguments += @('--input', $InputFolder) }
    $arguments += $BuildArguments
    & $python @arguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    exit 0
}
if ($BuildArguments -or $InputFolder) { throw 'Play accepts only -Action Play. Select input during Build.' }
$activeFile = Join-Path $portRoot 'build\active-windows-build.json'
if (-not (Test-Path -LiteralPath $activeFile -PathType Leaf)) {
    throw 'No checked native game/launcher pair has been selected. Complete Build_Monado_II.bat first.'
}
# The Python verifier checks the complete PE DLL closure and exact owned receipt
# before opening the existing launcher. It does not press Play.
& $python -B (Join-Path $portRoot 'scripts\build_windows.py') --launch-current
exit $LASTEXITCODE
