# Point the project at the player's own game files (modelled on LASRecomp's start-screen game-file setup).
# The player picks a folder; exefs (the folder with main and main.npdm) and romfs (the folder with bdat/, the game's
# data root) are found in it or a few levels below (some layouts nest them). Game packages (.xci / .nsp / .nca / .nsz)
# are refused without being read. The version (2.0.0) and the romfs contents are checked by tools/extract/check_dump.py,
# then targets\exefs and targets\romfs are linked to the folders found (junctions: no 14 GB copy).
#   powershell -ExecutionPolicy Bypass -File tools\setup_game_files.ps1 [folder]      exit 0 = ready
param([string]$Folder)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$PickHint = "Choose the folder that has the game's exefs and romfs folders in it."

function Find-Game([string]$dir, [int]$depth, [hashtable]$found) {
    if ($depth -gt 4 -or ($found.exefs -and $found.romfs)) { return }
    if (-not $found.exefs -and (Test-Path "$dir\main" -PathType Leaf) -and (Test-Path "$dir\main.npdm" -PathType Leaf)) { $found.exefs = $dir }
    if (-not $found.romfs -and (Test-Path "$dir\bdat" -PathType Container)) { $found.romfs = $dir }
    foreach ($f in Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue) {
        if ($f.Extension -match '^\.(xci|nsp|nca|nsz|xcz)$') { $found.packages = $true }
    }
    foreach ($d in Get-ChildItem -LiteralPath $dir -Directory -ErrorAction SilentlyContinue) {
        Find-Game $d.FullName ($depth + 1) $found
    }
}

if (-not $Folder) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "$PickHint (Xenoblade Chronicles: Definitive Edition, version 2.0.0)"
    if ($dlg.ShowDialog() -ne 'OK') { Write-Host 'Cancelled.'; exit 2 }
    $Folder = $dlg.SelectedPath
}
if (-not (Test-Path -LiteralPath $Folder -PathType Container)) { Write-Host "No such folder: $Folder"; exit 1 }

$found = @{ exefs = $null; romfs = $null; packages = $false }
Find-Game (Resolve-Path -LiteralPath $Folder).Path 0 $found
if (-not $found.exefs -or -not $found.romfs) {
    if ($found.packages) {
        Write-Host 'Those are game packages, not game files.'
        Write-Host 'Monado needs the game''s own files (the exefs and romfs folders), not .xci / .nsp / .nca files.'
    } else {
        Write-Host 'No game files found in that folder.'
        Write-Host $PickHint
    }
    exit 1
}
Write-Host "exefs: $($found.exefs)"
Write-Host "romfs: $($found.romfs)"

# version (build-ids) and romfs contents
$py = "$root\.venv\Scripts\python.exe"
& $py -X utf8 "$root\tools\extract\check_dump.py" --exefs $found.exefs --romfs $found.romfs
if ($LASTEXITCODE -ne 0) {
    if ($LASTEXITCODE -eq 4) { Write-Host 'Monado needs version 2.0.0: the base game with the 2.0.0 update applied.' }
    elseif ($LASTEXITCODE -eq 5) { Write-Host 'Some romfs files are missing or different, usually from a copy or extraction that stopped partway, or a romfs without the 2.0.0 update. Get the game files again, then choose the folder again.' }
    exit 1
}

# targets\exefs, targets\romfs -> the folders found (an existing real folder is never deleted)
New-Item -ItemType Directory -Force "$root\targets" | Out-Null
foreach ($k in 'exefs', 'romfs') {
    $t = "$root\targets\$k"
    if (Test-Path -LiteralPath $t) {
        $item = Get-Item -LiteralPath $t -Force
        if (-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            Write-Host "targets\$k is a real folder (game files set up earlier). Move or rename it, then run this again."
            exit 1
        }
        $item.Delete()  # (removes the link only)
    }
    New-Item -ItemType Junction -Path $t -Target $found[$k] | Out-Null
}
Write-Host 'Game files ready: targets\ points to them.'
exit 0
