<#
.SYNOPSIS
    Install RatCam Recorder: copy the application, make a desktop icon, and
    register it in Add/Remove Programs.

.DESCRIPTION
    Everything the application needs is copied into one folder -- Qt, FFmpeg
    and pylon's user-mode DLLs included -- so it no longer depends on PATH.

    What this CANNOT install is Basler's kernel driver. The cameras bind to
    'plnu3v' (oem66.inf), a signed kernel-mode driver that only Basler's own
    pylon installer can place, and which their licence governs. pylon must
    therefore already be installed on the machine. This script checks and says
    so plainly rather than installing something that then finds no cameras.

.PARAMETER Source
    Build output to install from. Defaults to the Release folder beside this
    script's repository.

.PARAMETER InstallPath
    Where to install. Defaults to "C:\Program Files\RatCam Recorder", which
    needs an elevated shell. Pass a path under your profile to avoid that.

.PARAMETER Uninstall
    Remove a previous installation instead of installing.

.EXAMPLE
    # From an elevated PowerShell:
    .\Install-RatCamRecorder.ps1

.EXAMPLE
    # No admin rights needed:
    .\Install-RatCamRecorder.ps1 -InstallPath "$env:LOCALAPPDATA\RatCamRecorder"
#>

[CmdletBinding()]
param(
    [string]$Source,
    [string]$InstallPath = "$env:ProgramFiles\RatCam Recorder",
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$AppName    = 'RatCam Recorder'
$ExeName    = 'RatCamRecorder.exe'
$RegKey     = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RatCamRecorder'

function Write-Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Write-Warn($text) { Write-Host "  ! $text" -ForegroundColor Yellow }
function Write-Fail($text) { Write-Host "  X $text" -ForegroundColor Red }

function Remove-Shortcuts {
    $desktop = [Environment]::GetFolderPath('Desktop')
    $start   = [Environment]::GetFolderPath('Programs')
    foreach ($p in @("$desktop\$AppName.lnk", "$start\$AppName.lnk")) {
        if (Test-Path $p) { Remove-Item $p -Force; Write-Host "  removed $p" }
    }
}

# ----------------------------------------------------------------- uninstall
if ($Uninstall) {
    Write-Step "Uninstalling $AppName"
    Remove-Shortcuts
    if (Test-Path $InstallPath) {
        Remove-Item $InstallPath -Recurse -Force
        Write-Host "  removed $InstallPath"
    }
    if (Test-Path $RegKey) { Remove-Item $RegKey -Recurse -Force }
    Write-Host ""
    Write-Host "Done. Recordings were not touched." -ForegroundColor Green
    return
}

# ------------------------------------------------------------------- source
if (-not $Source) {
    $Source = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\bin\Release'
}
if (-not (Test-Path (Join-Path $Source $ExeName))) {
    Write-Fail "$ExeName not found in $Source"
    Write-Host "  Build it first, or pass -Source <folder>."
    exit 1
}
Write-Step "Installing from $Source"

# ------------------------------------------------------- pylon prerequisite
$pylonDriver = Get-CimInstance Win32_SystemDriver -Filter "Name='plnu3v'" -ErrorAction SilentlyContinue
if (-not $pylonDriver) {
    Write-Warn "Basler's pylon USB3 driver (plnu3v) was not found."
    Write-Warn "The application will install and start, but no cameras will be"
    Write-Warn "detected until pylon is installed. This script cannot install"
    Write-Warn "it: a kernel driver has to come from Basler's own installer."
    $answer = Read-Host "  Continue anyway? (y/N)"
    if ($answer -ne 'y') { Write-Host "Cancelled."; exit 1 }
} else {
    Write-Host "  pylon driver present (plnu3v)"
}

# -------------------------------------------------------------------- copy
Write-Step "Copying to $InstallPath"
try {
    if (Test-Path $InstallPath) {
        # A running instance would block the copy and leave a half-updated
        # install, which is worse than refusing.
        $running = Get-Process RatCamRecorder -ErrorAction SilentlyContinue
        if ($running) {
            Write-Fail "$AppName is running (PID $($running.Id)). Close it and retry."
            exit 1
        }
        Remove-Item "$InstallPath\*" -Recurse -Force -ErrorAction SilentlyContinue
    }
    New-Item -ItemType Directory -Path $InstallPath -Force | Out-Null
    Copy-Item "$Source\*" $InstallPath -Recurse -Force
} catch {
    Write-Fail $_.Exception.Message
    Write-Host "  If this is a permissions error, either run PowerShell as"
    Write-Host "  Administrator, or install somewhere under your profile:"
    Write-Host "    .\Install-RatCamRecorder.ps1 -InstallPath `"`$env:LOCALAPPDATA\RatCamRecorder`""
    exit 1
}
$size = (Get-ChildItem $InstallPath -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("  {0:N0} file(s), {1:N1} MB" -f (Get-ChildItem $InstallPath -Recurse -File).Count, $size)

# --------------------------------------------------------------- shortcuts
Write-Step "Creating shortcuts"
Remove-Shortcuts
$target = Join-Path $InstallPath $ExeName
$icon   = Join-Path $InstallPath 'ratcam.ico'
$shell  = New-Object -ComObject WScript.Shell

foreach ($dir in @([Environment]::GetFolderPath('Desktop'),
                   [Environment]::GetFolderPath('Programs'))) {
    $lnk = $shell.CreateShortcut((Join-Path $dir "$AppName.lnk"))
    $lnk.TargetPath       = $target
    $lnk.WorkingDirectory = $InstallPath
    $lnk.Description      = 'Synchronised multi-camera recorder'
    if (Test-Path $icon) { $lnk.IconLocation = $icon }
    $lnk.Save()
    Write-Host "  $(Join-Path $dir "$AppName.lnk")"
}

# ------------------------------------------------------- add/remove programs
New-Item -Path $RegKey -Force | Out-Null
$uninstallCmd = "powershell -ExecutionPolicy Bypass -File `"$InstallPath\Install-RatCamRecorder.ps1`" -Uninstall -InstallPath `"$InstallPath`""
Set-ItemProperty $RegKey DisplayName     $AppName
Set-ItemProperty $RegKey DisplayVersion  '1.0.0'
Set-ItemProperty $RegKey Publisher       'Peter Gombkoto'
Set-ItemProperty $RegKey InstallLocation $InstallPath
Set-ItemProperty $RegKey DisplayIcon     $icon
Set-ItemProperty $RegKey UninstallString $uninstallCmd
Set-ItemProperty $RegKey NoModify 1 -Type DWord
Set-ItemProperty $RegKey NoRepair 1 -Type DWord

# Keep a copy of this script so uninstall works after the source is gone.
Copy-Item $PSCommandPath (Join-Path $InstallPath 'Install-RatCamRecorder.ps1') -Force

Write-Host ""
Write-Host "$AppName installed." -ForegroundColor Green
Write-Host "  Desktop icon and Start Menu entry created."
Write-Host "  Uninstall from Settings > Apps, or run the script with -Uninstall."
