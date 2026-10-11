# SM64 VR one-click setup: installs the build tools, takes your own Super Mario 64 (USA) ROM,
# builds the game and puts shortcuts on your desktop. Safe to run again (it only redoes what's needed).
#
#   setup.bat                      interactive (asks for the ROM with a file picker)
#   setup.bat -Rom "C:\path\rom"   non-interactive ROM path (.z64 / .n64 / .v64 / .zip)
#   setup.bat -NoShortcuts         skip the desktop shortcuts

param(
    [string]$Rom = "",
    [switch]$NoShortcuts,
    [switch]$Yes
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$Msys = "C:\msys64"
$Bash = Join-Path $Msys "usr\bin\bash.exe"
$UsSha1 = "9bef1128717f958171a4afac3ed78ee2bb4e86ce"
$Exe = Join-Path $Repo "build\us_pc\sm64.us.f3dex2e.exe"

function Step($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Fail($msg) {
    Write-Host ""
    Write-Host "SETUP FAILED: $msg" -ForegroundColor Red
    Write-Host ""
    if (-not $Yes) { Read-Host "Press Enter to close" | Out-Null }
    exit 1
}
function Ask($question) {
    if ($Yes) { return $true }
    $a = Read-Host "$question [Y/n]"
    return ($a -eq "" -or $a -match "^[yY]")
}

# Run a command inside the MSYS2 MinGW64 shell, in the repo folder.
function Invoke-Msys($cmd) {
    $env:MSYSTEM = "MINGW64"
    $env:CHERE_INVOKING = "1"
    Push-Location $Repo
    try {
        & $Bash -lc $cmd | Out-Host
        return $LASTEXITCODE
    } finally {
        Pop-Location
    }
}

Write-Host "Super Mario 64 VR - setup" -ForegroundColor Yellow
Write-Host "Folder: $Repo"

# ---------------------------------------------------------------------------------------------
Step "1/4  Build tools (MSYS2)"
if (-not (Test-Path $Bash)) {
    Write-Host "MSYS2 (free compiler toolkit, about 1.5 GB) is needed to build the game."
    if (-not (Ask "Install MSYS2 to C:\msys64 now?")) { Fail "MSYS2 is required. Get it from https://www.msys2.org and run setup again." }
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        winget install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements
    } else {
        Write-Host "winget not found - downloading the MSYS2 installer..."
        $installer = Join-Path $env:TEMP "msys2-installer.exe"
        Invoke-WebRequest "https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-x86_64-latest.exe" -OutFile $installer
        Start-Process $installer -ArgumentList "install", "--root", $Msys, "--confirm-command", "--accept-messages" -Wait
    }
    if (-not (Test-Path $Bash)) { Fail "MSYS2 did not install to $Msys. Install it from https://www.msys2.org and run setup again." }
    # first start initialises the keyring
    Invoke-Msys "true" | Out-Null
}
Write-Host "Installing compiler packages (first run takes a few minutes)..."
$code = Invoke-Msys "pacman -Sy --noconfirm >/dev/null && pacman -S --needed --noconfirm make python mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 mingw-w64-x86_64-glew mingw-w64-x86_64-pkgconf"
if ($code -ne 0) { Fail "Could not install the MSYS2 packages (check your internet connection)." }

# ---------------------------------------------------------------------------------------------
Step "2/4  Your Super Mario 64 ROM"
$baserom = Join-Path $Repo "baserom.us.z64"

function Get-Sha1($path) { (Get-FileHash $path -Algorithm SHA1).Hash.ToLower() }

function Import-Rom($src) {
    if (-not (Test-Path $src)) { Fail "ROM not found: $src" }
    $tmp = $null
    if ($src -match "\.zip$") {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [System.IO.Compression.ZipFile]::OpenRead($src)
        try {
            $entry = $zip.Entries | Where-Object { $_.Name -match "\.(z64|n64|v64)$" } | Select-Object -First 1
            if ($null -eq $entry) { Fail "No .z64/.n64/.v64 file inside $src" }
            $tmp = Join-Path $env:TEMP ("sm64vr_" + $entry.Name)
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $tmp, $true)
            $src = $tmp
        } finally {
            $zip.Dispose()
        }
    }
    $bytes = [System.IO.File]::ReadAllBytes($src)
    if ($tmp) { Remove-Item $tmp -ErrorAction SilentlyContinue }
    # Convert byte-swapped (.v64) and little-endian (.n64) dumps to big-endian .z64
    if ($bytes[0] -eq 0x37 -and $bytes[1] -eq 0x80) {
        for ($i = 0; $i -lt $bytes.Length; $i += 2) { $t = $bytes[$i]; $bytes[$i] = $bytes[$i + 1]; $bytes[$i + 1] = $t }
    } elseif ($bytes[0] -eq 0x40 -and $bytes[1] -eq 0x12) {
        for ($i = 0; $i -lt $bytes.Length; $i += 4) {
            $a = $bytes[$i]; $b = $bytes[$i + 1]
            $bytes[$i] = $bytes[$i + 3]; $bytes[$i + 1] = $bytes[$i + 2]; $bytes[$i + 2] = $b; $bytes[$i + 3] = $a
        }
    } elseif (-not ($bytes[0] -eq 0x80 -and $bytes[1] -eq 0x37)) {
        Fail "That file is not an N64 ROM."
    }
    [System.IO.File]::WriteAllBytes($baserom, $bytes)
    if ((Get-Sha1 $baserom) -ne $UsSha1) {
        Remove-Item $baserom
        Fail "That ROM is not Super Mario 64 (USA). Only the unmodified US version is supported."
    }
}

if ((Test-Path $baserom) -and ((Get-Sha1 $baserom) -eq $UsSha1)) {
    Write-Host "ROM already in place and verified."
} else {
    if ($Rom -eq "") {
        Write-Host "Pick your Super Mario 64 (USA) ROM, dumped from a cartridge you own."
        Write-Host "(.z64, .n64, .v64, or a .zip containing one)"
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title = "Select your Super Mario 64 (USA) ROM"
        $dlg.Filter = "N64 ROM (*.z64;*.n64;*.v64;*.zip)|*.z64;*.n64;*.v64;*.zip|All files (*.*)|*.*"
        $dlg.InitialDirectory = [Environment]::GetFolderPath("UserProfile") + "\Downloads"
        if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail "No ROM selected." }
        $Rom = $dlg.FileName
    }
    Import-Rom $Rom
    Write-Host "ROM verified (Super Mario 64 USA)." -ForegroundColor Green
}

# ---------------------------------------------------------------------------------------------
Step "3/4  Building the game (5-10 minutes the first time)"
$jobs = [Environment]::ProcessorCount
$code = Invoke-Msys "make -j$jobs VERSION=us > build.log 2>&1"
if ($code -ne 0 -or -not (Test-Path $Exe)) {
    Get-Content (Join-Path $Repo "build.log") -Tail 25
    Fail "The build failed. The full log is in build.log - please attach it if you open an issue."
}
Write-Host "Build OK: $Exe" -ForegroundColor Green

# ---------------------------------------------------------------------------------------------
Step "4/4  Shortcuts"
if (-not $NoShortcuts) {
    $desktop = [Environment]::GetFolderPath("Desktop")
    $shell = New-Object -ComObject WScript.Shell
    $links = @(
        @{ Name = "Super Mario 64 VR"; Args = "--skip-intro" },
        @{ Name = "Super Mario 64 (desktop)"; Args = "--novr" }
    )
    foreach ($l in $links) {
        $lnk = $shell.CreateShortcut((Join-Path $desktop ($l.Name + ".lnk")))
        $lnk.TargetPath = $Exe
        $lnk.Arguments = $l.Args
        $lnk.WorkingDirectory = Split-Path $Exe
        $lnk.IconLocation = "$Exe,0"
        $lnk.Save()
        Write-Host "Created desktop shortcut: $($l.Name)"
    }
}

Write-Host ""
Write-Host "All done!" -ForegroundColor Green
Write-Host "Start SteamVR (or Oculus / Virtual Desktop), put on your headset, then open 'Super Mario 64 VR'."
Write-Host "No headset connected? It starts as the normal desktop game."
Write-Host ""
if (-not $Yes) { Read-Host "Press Enter to close" | Out-Null }
