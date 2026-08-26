<#
    sharesound installer.

    Copies the helper to %LOCALAPPDATA%\sharesound, starts it, registers it to
    run at logon, and opens the page that installs the userscript. Needs no
    administrator rights and changes no audio settings.

    Double-click Install.bat, or run:
      powershell -ExecutionPolicy Bypass -File install.ps1
    Remove with Uninstall.bat, or:
      powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$NoAutostart,
    # Capture only this process tree (e.g. "game.exe"), or everything except it.
    # Both need Windows build 20348+; ignored on older builds.
    [string]$Only,
    [string]$Exclude
)

$ErrorActionPreference = 'Stop'
$AppName   = 'sharesound'
$InstallTo = Join-Path $env:LOCALAPPDATA $AppName
$RunKey    = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$Port      = 47823

# Speak the user's language; most people running this are not developers.
$ru = (Get-UICulture).TwoLetterISOLanguageName -eq 'ru'
function Say($en, $ruText, $color = 'Gray') {
    Write-Host $(if ($ru) { $ruText } else { $en }) -ForegroundColor $color
}

function Stop-Helper {
    Get-Process sharesound, sharesound-cli -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
}

if ($Uninstall) {
    Stop-Helper
    Remove-ItemProperty -Path $RunKey -Name $AppName -ErrorAction SilentlyContinue
    if (Test-Path $InstallTo) { Remove-Item $InstallTo -Recurse -Force }
    Say "sharesound removed." "sharesound удалён." 'Green'
    Say "The browser script stays in Tampermonkey - delete it there if you want it gone." `
        "Скрипт остался в Tampermonkey - удалите его там, если он больше не нужен." 'Yellow'
    return
}

foreach ($f in @('sharesound.exe', 'sharesound-cli.exe')) {
    if (-not (Test-Path (Join-Path $PSScriptRoot $f))) {
        Say "$f is missing next to this installer. Download the release archive and unpack it whole." `
            "Рядом с установщиком нет файла $f. Скачайте архив релиза и распакуйте его целиком." 'Red'
        exit 1
    }
}

Say "Installing..." "Устанавливаю..." 'Cyan'
Stop-Helper
New-Item -ItemType Directory -Force -Path $InstallTo | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'sharesound.exe')     $InstallTo -Force
Copy-Item (Join-Path $PSScriptRoot 'sharesound-cli.exe') $InstallTo -Force

$exe = Join-Path $InstallTo 'sharesound.exe'
$argList = @()
if ($Only) { $argList += @('--only', $Only) }
elseif ($Exclude) { $argList += @('--exclude', $Exclude) }

if ($NoAutostart) {
    Remove-ItemProperty -Path $RunKey -Name $AppName -ErrorAction SilentlyContinue
    Say "Autostart skipped." "Автозапуск не настраивался." 'Yellow'
} else {
    $cmd = '"{0}"{1}' -f $exe, $(if ($argList) { ' ' + ($argList -join ' ') } else { '' })
    Set-ItemProperty -Path $RunKey -Name $AppName -Value $cmd
    Say "Will start automatically when you log in." "Будет запускаться сам при входе в Windows." 'Green'
}

if ($argList) { Start-Process -FilePath $exe -ArgumentList $argList -WindowStyle Hidden }
else          { Start-Process -FilePath $exe -WindowStyle Hidden }

$ok = $false
foreach ($i in 1..10) {
    Start-Sleep -Milliseconds 300
    try { Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 2 | Out-Null; $ok = $true; break }
    catch { }
}
if (-not $ok) {
    Say "The helper did not start. Another program may be using port $Port, or an antivirus blocked it." `
        "Программа не запустилась. Возможно, порт $Port занят другой программой или её заблокировал антивирус." 'Red'
    exit 1
}
Say "Helper is running." "Программа работает." 'Green'

Write-Host ''
& (Join-Path $InstallTo 'sharesound-cli.exe') --doctor
Write-Host ''
Say "A page just opened in your browser - click the blue button there to install the script, then reload your Discord tab." `
    "В браузере открылась страница - нажмите там синюю кнопку, чтобы поставить скрипт, затем перезагрузите вкладку Discord." 'Cyan'
Start-Process "http://127.0.0.1:$Port/"
