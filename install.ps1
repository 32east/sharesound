<#
    sharesound installer.

    Copies the helper to %LOCALAPPDATA%\sharesound, starts it, registers it to
    run at logon, and opens the page that installs the userscript.

    Usage:   powershell -ExecutionPolicy Bypass -File install.ps1
    Remove:  powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$NoAutostart,
    # Capture only this process tree (e.g. "game.exe"), or everything except it
    # with -Exclude. Both need Windows build 20348+; ignored otherwise.
    [string]$Only,
    [string]$Exclude
)

$ErrorActionPreference = 'Stop'
$AppName   = 'sharesound'
$InstallTo = Join-Path $env:LOCALAPPDATA $AppName
$RunKey    = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$Port      = 47823

function Stop-Helper {
    Get-Process sharesound, sharesound-cli -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
}

if ($Uninstall) {
    Stop-Helper
    Remove-ItemProperty -Path $RunKey -Name $AppName -ErrorAction SilentlyContinue
    if (Test-Path $InstallTo) { Remove-Item $InstallTo -Recurse -Force }
    Write-Host "sharesound removed. The userscript stays in Tampermonkey - delete it there if you want it gone." -ForegroundColor Yellow
    return
}

$src = $PSScriptRoot
foreach ($f in @('sharesound.exe', 'sharesound-cli.exe')) {
    if (-not (Test-Path (Join-Path $src $f))) {
        throw "$f not found next to the installer. Build it first (see README) or use a release download."
    }
}

Stop-Helper
New-Item -ItemType Directory -Force -Path $InstallTo | Out-Null
Copy-Item (Join-Path $src 'sharesound.exe')     $InstallTo -Force
Copy-Item (Join-Path $src 'sharesound-cli.exe') $InstallTo -Force

$exe = Join-Path $InstallTo 'sharesound.exe'
$args = @()
if ($Only)    { $args += @('--only', $Only) }
elseif ($Exclude) { $args += @('--exclude', $Exclude) }

if ($NoAutostart) {
    Remove-ItemProperty -Path $RunKey -Name $AppName -ErrorAction SilentlyContinue
} else {
    $cmd = '"{0}"{1}' -f $exe, $(if ($args) { ' ' + ($args -join ' ') } else { '' })
    Set-ItemProperty -Path $RunKey -Name $AppName -Value $cmd
}

if ($args) { Start-Process -FilePath $exe -ArgumentList $args -WindowStyle Hidden }
else       { Start-Process -FilePath $exe -WindowStyle Hidden }

Start-Sleep -Milliseconds 800
try {
    $health = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 3
    Write-Host "helper running (capture: $($health.mode))" -ForegroundColor Green
} catch {
    throw "helper did not answer on port $Port. Is something else using that port?"
}

Write-Host ''
& (Join-Path $InstallTo 'sharesound-cli.exe') --doctor
Write-Host ''
Write-Host 'Next: install the userscript from the page that just opened, then reload the Discord tab.' -ForegroundColor Cyan
Start-Process "http://127.0.0.1:$Port/"
