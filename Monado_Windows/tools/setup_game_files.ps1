# [XCDE] Select immutable extracted input; never change or replace old target links.
[CmdletBinding()]
param([string]$Folder)
$ErrorActionPreference = 'Stop'
$portRoot = Split-Path -Parent $PSScriptRoot
if (-not $Folder) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Choose your own already-extracted DE 2.0.0 folder containing exefs and romfs.'
    if ($dialog.ShowDialog() -ne 'OK') { exit 2 }
    $Folder = $dialog.SelectedPath
}
if (-not (Test-Path -LiteralPath $Folder -PathType Container)) { throw 'The selected folder does not exist.' }
$inputRoot = (Resolve-Path -LiteralPath $Folder).Path
$python = Join-Path $portRoot '.deps\python314\python.exe'
if (-not (Test-Path -LiteralPath $python -PathType Leaf)) { $python = Join-Path $portRoot '.venv\Scripts\python.exe' }
if (-not (Test-Path -LiteralPath $python -PathType Leaf)) { throw 'Prepare the project-local tools before selecting input.' }
& $python -B (Join-Path $portRoot 'tools\extract\check_dump.py') $inputRoot
if ($LASTEXITCODE -ne 0) { throw 'The selected already-extracted input failed validation. No previous selection changed.' }
$privateRoot = Join-Path $portRoot '.deps'
New-Item -ItemType Directory -Force -Path $privateRoot | Out-Null
$record = Join-Path $privateRoot 'selected-input.json'
$temp = Join-Path $privateRoot ('selected-input.' + [Guid]::NewGuid().ToString('N') + '.tmp')
$bytes = [Text.Encoding]::UTF8.GetBytes((@{schema=1;path=$inputRoot} | ConvertTo-Json))
$stream = [IO.File]::Open($temp, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
if (Test-Path -LiteralPath $record) { [IO.File]::Replace($temp, $record, $null) } else { [IO.File]::Move($temp, $record) }
Write-Host 'Validated input selected. Existing inputs, targets, settings and saves were preserved. Run the root Build_Monado_II.bat.'
