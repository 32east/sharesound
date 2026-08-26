<#
    Builds both binaries from one source:
      sharesound.exe      GUI subsystem - runs windowless at logon
      sharesound-cli.exe  console       - --doctor / --list / --tone

    Needs Visual Studio Build Tools 2022 (C++ workload) and the Windows SDK.
#>
[CmdletBinding()]
param([switch]$SkipEmbed)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not $SkipEmbed) {
    if (Get-Command python -ErrorAction SilentlyContinue) {
        python tools\embed.py
    } else {
        Write-Warning 'python not found; building with the existing assets.h'
    }
}

$candidates = @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
)
$vcvars = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw 'vcvars64.bat not found - install Visual Studio Build Tools 2022 with the C++ workload.' }

Get-Process sharesound, sharesound-cli -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

$libs = 'ole32.lib mmdevapi.lib ws2_32.lib'
$flags = '/nologo /O2 /EHsc /std:c++17 /W1'

# vcvars can write harmless notices to stderr (e.g. a missing vswhere); with
# ErrorActionPreference=Stop PowerShell would turn those into a failure, so
# judge the build by its exit code instead.
$ErrorActionPreference = 'Continue'

cmd /c "`"$vcvars`" >nul && cl $flags sharesound.cpp /Fe:sharesound.exe /link $libs /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup"
if ($LASTEXITCODE -ne 0) { throw 'GUI build failed' }

cmd /c "`"$vcvars`" >nul && cl $flags sharesound.cpp /Fe:sharesound-cli.exe /link $libs"
if ($LASTEXITCODE -ne 0) { throw 'CLI build failed' }

$ErrorActionPreference = 'Stop'

Remove-Item *.obj -ErrorAction SilentlyContinue
Get-ChildItem sharesound.exe, sharesound-cli.exe | ForEach-Object {
    '{0,-20} {1,8:N0} bytes' -f $_.Name, $_.Length
}
