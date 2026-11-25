# make-sample.ps1: makes a small, made-up BRODALF database to take
# screenshots of. Test tool only; run on a throwaway Windows machine (it
# makes virtual disks with diskpart, so it needs to run as administrator).
#
# What it makes, under C:\BrodalfDemo:
#   Data\Documents, Data\Photos, Data\Projects   three backup roots
#   Drives\Blue                                  a folder used as a backup drive
#   Drives\Gray                                  one marked broken afterwards
#   Cloud\OneDrive                               an extra place for starred files
#   My backups.brodalf                           the database
# and two virtual disks under C:\BrodalfVHD, both left unplugged:
#   red.vhdx    a backup drive (R:), backed up to, then unplugged
#   spare.vhdx  a disk BRODALF has never seen (N:)
# After the backups, 40 photos change, two letters are deleted, a file is
# damaged in place and a large video is added, so there is something to
# back up and something to warn about.
param(
    [Parameter(Mandatory = $true)] [string] $Cli,
    [Parameter(Mandatory = $true)] [string] $Passphrase,
    [string] $Root = "C:\BrodalfDemo",
    [string] $VhdDir = "C:\BrodalfVHD"
)
$ErrorActionPreference = "Stop"
$Db = Join-Path $Root "My backups.brodalf"
$Data = Join-Path $Root "Data"

function Run-Cli {
    param([string[]] $CliArgs, [int[]] $Ok = @(0))
    Write-Host ("brodalf-cli " + ($CliArgs -join " "))
    & $Cli @CliArgs
    if ($Ok -notcontains $LASTEXITCODE) { throw "brodalf-cli $($CliArgs[0]) failed with exit code $LASTEXITCODE" }
    $global:LASTEXITCODE = 0
}

$Rng = New-Object System.Random 20261125
function New-File {
    param([string] $Path, [int] $Bytes)
    New-Item -ItemType Directory -Force -Path (Split-Path $Path) | Out-Null
    $b = New-Object byte[] $Bytes
    $Rng.NextBytes($b)
    [System.IO.File]::WriteAllBytes($Path, $b)
}

function Diskpart {
    param([string] $Name, [string[]] $Lines)
    $path = Join-Path $VhdDir $Name
    Set-Content -Path $path -Value $Lines -Encoding Ascii
    $out = & diskpart.exe /s $path
    Write-Host ($out -join "`n")
    if ($LASTEXITCODE -ne 0) { throw "diskpart $Name failed with exit code $LASTEXITCODE" }
}

if (Test-Path $Root) { Remove-Item -Recurse -Force $Root }
New-Item -ItemType Directory -Force -Path $Root, $VhdDir, "$Root\Drives\Blue", "$Root\Drives\Gray", "$Root\Cloud\OneDrive" | Out-Null

# ---- The files ------------------------------------------------------------
$docs = Join-Path $Data "Documents"
foreach ($y in 2021, 2022) { foreach ($n in "Tax return", "Pay slips", "Bank statements") { New-File "$docs\Taxes\$y\$n $y.pdf" (60KB + $Rng.Next(200KB)) } }
New-File "$docs\Taxes\2023\Tax return 2023.pdf" 240KB
foreach ($i in 1..6) { New-File ("$docs\Taxes\2023\Receipt {0:00} dentist and pharmacy.pdf" -f $i) (20KB + $Rng.Next(40KB)) }
foreach ($n in "Lease agreement", "Letter to the landlord", "Car insurance", "Job offer", "Reference from Anna", "K$([char]0xFC)ndigung Fitnessstudio") { New-File "$docs\Letters\$n.docx" (15KB + $Rng.Next(30KB)) }
foreach ($n in "Pancakes", "Lentil soup", "Grandma's apple cake") { New-File "$docs\Recipes\$n.txt" 4KB }
New-File "$docs\Household inventory.xlsx" 48KB
New-File "$docs\Passport scan.jpg" 900KB

$photos = Join-Path $Data "Photos"
foreach ($i in 1..40) { New-File ("$photos\2023\Holiday\IMG_{0:0000}.jpg" -f $i) (150KB + $Rng.Next(150KB)) }
foreach ($i in 1..12) { New-File ("$photos\2024\Garden\IMG_{0:0000}.jpg" -f (1000 + $i)) (150KB + $Rng.Next(150KB)) }
foreach ($i in 1..5) { New-File ("$photos\2024\Birthday\IMG_{0:0000}.jpg" -f (2000 + $i)) (150KB + $Rng.Next(150KB)) }

$projects = Join-Path $Data "Projects"
New-File "$projects\Website\index.html" 12KB
New-File "$projects\Website\style.css" 6KB
New-File "$projects\Thesis\Thesis draft 3.docx" 420KB
New-File "$projects\Thesis\Sources.bib" 30KB

# ---- The red drive: a virtual disk at R: ----------------------------------
$red = Join-Path $VhdDir "red.vhdx"
$spare = Join-Path $VhdDir "spare.vhdx"
foreach ($v in $red, $spare) { if (Test-Path $v) { Remove-Item -Force $v } }
Diskpart "create-red.txt" @(
    "create vdisk file=`"$red`" maximum=2048 type=expandable",
    "select vdisk file=`"$red`"", "attach vdisk", "create partition primary",
    "format fs=ntfs quick label=`"RED`"", "assign letter=R")
Set-Content -Path (Join-Path $VhdDir "attach-red.txt") -Encoding Ascii -Value @(
    "select vdisk file=`"$red`"", "attach vdisk", "select partition 1", "assign letter=R noerr")
Set-Content -Path (Join-Path $VhdDir "detach-red.txt") -Encoding Ascii -Value @(
    "select vdisk file=`"$red`"", "detach vdisk noerr")
for ($i = 0; $i -lt 40 -and -not (Test-Path "R:\"); $i++) { Start-Sleep -Milliseconds 500 }
if (-not (Test-Path "R:\")) { throw "R: did not appear" }

# ---- The database -----------------------------------------------------------
$env:BRODALF_NEW_PASSPHRASE = $Passphrase
Run-Cli @("new", $Db, $docs, $photos, $projects)
Run-Cli @("passphrase", $Db)
Remove-Item Env:\BRODALF_NEW_PASSPHRASE
$env:BRODALF_PASSPHRASE = $Passphrase
Run-Cli @("scan", $Db) @(0, 2)
Run-Cli @("roots", $Db)
Run-Cli @("encrypt-folder", $Db, "Documents", "Taxes")
Run-Cli @("star", $Db, "Documents", "Letters")
Run-Cli @("star", $Db, "Photos", "2023")
Run-Cli @("drive", $Db, "$Root\Drives\Blue", "Blue", "--location", "Desk drawer at home")
Run-Cli @("drive", $Db, "R:\", "Red", "--location", "At my sister's")
Run-Cli @("drive", $Db, "$Root\Drives\Gray", "Gray", "--location", "Office")
Run-Cli @("place", $Db, "$Root\Cloud\OneDrive", "OneDrive folder on this PC", "--limit", "2GB")
Run-Cli @("backup", $Db, "$Root\Drives\Blue", "R:\", "$Root\Drives\Gray", "$Root\Cloud\OneDrive") @(0, 2)
Run-Cli @("drive-lost", $Db, "Gray", "broken", "--yes")

# ---- Changes since then -------------------------------------------------------
# A file damaged in place: same size and date, different contents.
$cake = "$docs\Recipes\Grandma's apple cake.txt"
$when = (Get-Item -LiteralPath $cake).LastWriteTimeUtc
$b = [System.IO.File]::ReadAllBytes($cake)
for ($i = 100; $i -lt 140; $i++) { $b[$i] = $b[$i] -bxor 0x5A }
[System.IO.File]::WriteAllBytes($cake, $b)
(Get-Item -LiteralPath $cake).LastWriteTimeUtc = $when
# 40 photos rewritten (as a photo app or ransomware might), two letters deleted, new files.
foreach ($i in 1..40) { New-File ("$photos\2023\Holiday\IMG_{0:0000}.jpg" -f $i) (150KB + $Rng.Next(150KB)) }
Remove-Item -LiteralPath "$docs\Letters\Car insurance.docx", "$docs\Letters\Job offer.docx"
New-File "$docs\Taxes\2024\Receipt 01 new glasses.pdf" 35KB
New-File "$docs\Letters\Letter to the bank.docx" 22KB
New-File "$photos\2024\Birthday\Birthday party.mp4" 300MB
Run-Cli @("scan", $Db, "--bitrot") @(0, 2)
Run-Cli @("status", $Db) @(0, 2)
Remove-Item Env:\BRODALF_PASSPHRASE

Diskpart "detach-red-now.txt" @("select vdisk file=`"$red`"", "detach vdisk")

# ---- A spare disk BRODALF has never seen, at N: --------------------------------
Diskpart "create-spare.txt" @(
    "create vdisk file=`"$spare`" maximum=1024 type=expandable",
    "select vdisk file=`"$spare`"", "attach vdisk", "create partition primary",
    "format fs=ntfs quick label=`"SPARE`"", "assign letter=N", "detach vdisk")
Set-Content -Path (Join-Path $VhdDir "attach-new.txt") -Encoding Ascii -Value @(
    "select vdisk file=`"$spare`"", "attach vdisk", "select partition 1", "assign letter=N noerr")
Set-Content -Path (Join-Path $VhdDir "detach-new.txt") -Encoding Ascii -Value @(
    "select vdisk file=`"$spare`"", "detach vdisk noerr")
Write-Host "Sample database ready: $Db"
