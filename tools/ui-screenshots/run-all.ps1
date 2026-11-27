# run-all.ps1: takes the screenshots in each language and appearance.
# Test tool only, for a throwaway Windows machine (see make-sample.ps1).
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $Cli,
    [Parameter(Mandatory = $true)] [string] $Shots,
    [Parameter(Mandatory = $true)] [string] $Ids,
    [Parameter(Mandatory = $true)] [string] $Out,
    [string] $Combos = "en-light,en-dark,de-light,de-dark,es-light,es-dark",
    [int] $MinutesEach = 30
)
$ErrorActionPreference = "Stop"
$Root = "C:\BrodalfDemo"
$Pristine = "C:\BrodalfPristine"
$VhdDir = "C:\BrodalfVHD"
$Passphrase = "correct horse battery staple"

& (Join-Path $PSScriptRoot "make-sample.ps1") -Cli $Cli -Passphrase $Passphrase -Root $Root -VhdDir $VhdDir
robocopy $Root $Pristine /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "could not keep a copy of the sample" }
Copy-Item "$VhdDir\red.vhdx" "$VhdDir\red.pristine"
Copy-Item "$VhdDir\spare.vhdx" "$VhdDir\spare.pristine"
$global:LASTEXITCODE = 0

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$failed = @()
foreach ($combo in $Combos.Split(",")) {
    $lang, $theme = $combo.Trim().Split("-")
    Write-Host "::group::$combo"
    # Back to the sample as it was made: unplug the virtual disks, put the files back.
    foreach ($s in "detach-red.txt", "detach-new.txt") { & diskpart.exe /s (Join-Path $VhdDir $s) | Out-Null }
    Get-Process brodalf -ErrorAction SilentlyContinue | Stop-Process -Force
    robocopy $Pristine $Root /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "could not put the sample back" }
    Copy-Item -Force "$VhdDir\red.pristine" "$VhdDir\red.vhdx"
    Copy-Item -Force "$VhdDir\spare.pristine" "$VhdDir\spare.vhdx"
    $global:LASTEXITCODE = 0
    # Where the restore goes: it already holds a different IMG_0001.jpg, which the restore leaves alone.
    if (Test-Path C:\BrodalfRestore) { Remove-Item -Recurse -Force C:\BrodalfRestore }
    New-Item -ItemType Directory -Force -Path C:\BrodalfRestore\2023\Holiday | Out-Null
    Set-Content -Path C:\BrodalfRestore\2023\Holiday\IMG_0001.jpg -Value "a different picture"

    $dir = Join-Path $Out $combo
    $list = @("--exe", $Exe, "--db", "$Root\My backups.brodalf", "--ids", $Ids, "--out", $dir,
              "--lang", $lang, "--theme", $theme, "--pass", $Passphrase, "--vhd-dir", $VhdDir)
    if ($combo -eq "en-light") { $list += "--extras" }
    # Start-Process does not quote arguments with spaces in them: quote each.
    $line = ($list | ForEach-Object { '"' + $_ + '"' }) -join " "
    $p = Start-Process -FilePath $Shots -ArgumentList $line -PassThru -NoNewWindow
    $null = $p.Handle # without this, PowerShell never learns the exit code
    if (-not $p.WaitForExit($MinutesEach * 60000)) {
        Write-Host "$combo took longer than $MinutesEach minutes: stopping it"
        $p.Kill()
        Get-Process brodalf -ErrorAction SilentlyContinue | Stop-Process -Force
        $failed += $combo
    } elseif ($p.ExitCode -ne 0) {
        $failed += $combo
    }
    $n = (Get-ChildItem -Path $dir -Filter *.png -ErrorAction SilentlyContinue).Count
    Write-Host "$combo`: $n screenshots"
    Write-Host "::endgroup::"
}
foreach ($s in "detach-red.txt", "detach-new.txt") { & diskpart.exe /s (Join-Path $VhdDir $s) | Out-Null }
if ($failed.Count) { Write-Host "Did not finish: $($failed -join ', ')"; exit 1 }
exit 0
