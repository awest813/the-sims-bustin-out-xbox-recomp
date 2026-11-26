<#
.SYNOPSIS
    One-click interactive setup wizard for The Sims: Bustin' Out (USA)
    Native Windows x64 Static Recompilation.
.DESCRIPTION
    1. Checks Python 3.10+, capstone (auto-installs if missing), CMake, and Visual Studio C++ Build Tools.
    2. Detects or prompts (via Windows File Explorer) for your legally dumped Xbox ISO / extracted files,
       and extracts into game\ without modifying your original ISO.
    3. Runs the xboxrecomp static recompilation pipeline to generate src\recomp\gen\.
    4. Configures and builds build\Release\sims_bustin_out_recomp.exe.
    5. Optionally launches the native Launcher & Settings GUI.
.EXAMPLE
    .\setup.bat
    .\scripts\setup.ps1 -IsoPath "D:\Xbox\SimsBustinOut.iso"
    .\scripts\setup.ps1 -CheckOnly
    .\scripts\setup.ps1 -SkipLaunch -NoPrompt
#>
param(
    [string]$IsoPath = "",
    [switch]$CheckOnly,
    [switch]$SkipLaunch,
    [switch]$ForceRecomp,
    [switch]$ForceBuild,
    [switch]$NoPrompt
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot

function Write-Banner {
    Write-Host ""
    Write-Host "======================================================================" -ForegroundColor Cyan
    Write-Host "  The Sims: Bustin' Out (USA) - Native Windows x64 Setup Wizard" -ForegroundColor White
    Write-Host "======================================================================" -ForegroundColor Cyan
    Write-Host ""
}

function Write-Ok($msg)   { Write-Host "  [OK]   $msg" -ForegroundColor Green }
function Write-Info($msg) { Write-Host "  [INFO] $msg" -ForegroundColor Cyan }
function Write-Warn($msg) { Write-Host "  [WARN] $msg" -ForegroundColor Yellow }
function Write-Err($msg)  { Write-Host "  [ERR]  $msg" -ForegroundColor Red }

function Find-Python {
    foreach ($cand in @("python", "python3", "py")) {
        $cmd = Get-Command $cand -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        # Skip Microsoft Store App Execution Aliases (they open the Store or fail)
        if ($cmd.Source -like "*\WindowsApps\*") { continue }
        try {
            $ver = if ($cand -eq "py") {
                & $cmd.Source -3 --version 2>&1
            } else {
                & $cmd.Source --version 2>&1
            }
            if ($LASTEXITCODE -eq 0 -and $ver -match "Python (\d+)\.(\d+)") {
                $maj = [int]$Matches[1]; $min = [int]$Matches[2]
                if ($maj -gt 3 -or ($maj -eq 3 -and $min -ge 10)) {
                    return @{
                        Exe          = $cmd.Source
                        IsPyLauncher = ($cand -eq "py")
                        Ver          = ($ver -join " ").Trim()
                    }
                }
            }
        } catch {}
    }
    return $null
}

function Invoke-Py($pyInfo, [string[]]$PyArgs) {
    if ($pyInfo.IsPyLauncher) {
        & $pyInfo.Exe -3 @PyArgs
    } else {
        & $pyInfo.Exe @PyArgs
    }
}

function Find-VisualStudio {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $ver = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion 2>$null | Select-Object -First 1)
    $path = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1)
    if (-not $ver -or -not $path) { return $null }
    $major = [int](($ver -split '\.')[0])
    $gen = switch ($major) {
        18 { "Visual Studio 18 2026" }
        17 { "Visual Studio 17 2022" }
        16 { "Visual Studio 16 2019" }
        default { "Visual Studio 17 2022" }
    }
    return @{ Version = $ver.Trim(); Path = $path.Trim(); Generator = $gen }
}

function Find-CMake($vsInfo) {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $cands = @(
        "C:\Program Files\CMake\bin\cmake.exe",
        "C:\Program Files (x86)\CMake\bin\cmake.exe"
    )
    if ($vsInfo -and $vsInfo.Path) {
        $cands += (Join-Path $vsInfo.Path "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
    }
    foreach ($c in $cands) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

function Find-NearbyIso {
    $searchDirs = @($RepoRoot)
    $parent = Split-Path -Parent $RepoRoot
    if ($parent -and (Test-Path $parent)) {
        $searchDirs += $parent
        Get-ChildItem $parent -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            if ($_.FullName -ne $RepoRoot) { $searchDirs += $_.FullName }
        }
    }
    foreach ($d in $searchDirs) {
        $hit = Get-ChildItem $d -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in @(".iso", ".xiso") } | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

function Prompt-IsoFileDialog {
    try {
        Add-Type -AssemblyName System.Windows.Forms | Out-Null
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title = "Select your Original Xbox ISO for The Sims: Bustin' Out (USA)"
        $dlg.Filter = "Xbox ISO Images (*.iso;*.xiso)|*.iso;*.xiso|All Files (*.*)|*.*"
        $dlg.InitialDirectory = (Split-Path -Parent $RepoRoot)
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            return $dlg.FileName
        }
    } catch {
        Write-Warn "File dialog unavailable; please type the full path to your .iso file:"
        $typed = Read-Host "  ISO path"
        if ($typed) { return $typed.Trim('"') }
    }
    return $null
}

function Get-XbeTitleId($pyInfo, $xbePath) {
    $code = "import struct,sys; b=open(sys.argv[1],'rb').read(8192); assert b[:4]==b'XBEH'; base=struct.unpack_from('<I',b,0x104)[0]; cva=struct.unpack_from('<I',b,0x118)[0]; off=cva-base; tid=struct.unpack_from('<I',b,off+8)[0]; print(f'0x{tid:08X}')"
    $out = Invoke-Py $pyInfo @("-c", $code, $xbePath) 2>$null
    if ($LASTEXITCODE -eq 0 -and $out) { return ($out -join "").Trim() }
    return $null
}

try {
    Write-Banner

    # ── Step 1: Check Prerequisites ──────────────────────────────────────────
    Write-Host "[Step 1/4] Checking prerequisites..." -ForegroundColor White
    $missing = @()

    $py = Find-Python
    if ($py) {
        Write-Ok "Python found: $($py.Ver) ($($py.Exe))"
    } else {
        Write-Err "Python 3.10+ not found."
        Write-Host "         Install with: winget install Python.Python.3.12" -ForegroundColor Gray
        Write-Host "         Or download:  https://www.python.org/downloads/ (check 'Add python.exe to PATH')" -ForegroundColor Gray
        $missing += "Python 3.10+"
    }

    $vs = Find-VisualStudio
    if ($vs) {
        Write-Ok "Visual Studio C++ found: v$($vs.Version) ($($vs.Generator))"
    } else {
        Write-Err "Visual Studio 2022 (or 2019) C++ Build Tools not found."
        Write-Host '         Install with: winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"' -ForegroundColor Gray
        $missing += "Visual Studio C++ Build Tools"
    }

    $cmake = Find-CMake $vs
    if ($cmake) {
        $cver = (& $cmake --version 2>$null | Select-Object -First 1)
        Write-Ok "CMake found: $cver ($cmake)"
    } else {
        Write-Err "CMake 3.20+ not found."
        Write-Host "         Install with: winget install Kitware.CMake" -ForegroundColor Gray
        $missing += "CMake 3.20+"
    }

    if ($py) {
        Invoke-Py $py @("-c", "import capstone") 2>$null
        if ($LASTEXITCODE -eq 0) {
            Write-Ok "Python 'capstone' package is installed."
        } elseif ($CheckOnly) {
            Write-Warn "Python 'capstone' package not installed yet (setup will auto-install it via pip)."
        } else {
            Write-Info "Installing required Python package 'capstone'..."
            Invoke-Py $py @("-m", "pip", "install", "--quiet", "capstone")
            if ($LASTEXITCODE -ne 0) {
                Write-Err "Failed to install capstone via pip. Run: pip install capstone"
                $missing += "Python capstone"
            } else {
                Write-Ok "Installed Python 'capstone' package."
            }
        }
    }

    if ($missing.Count -gt 0) {
        Write-Host ""
        Write-Err "Please install the missing prerequisite(s) above, then re-run setup.bat."
        exit 1
    }

    if ($CheckOnly) {
        Write-Host ""
        Write-Ok "All prerequisites satisfied! Ready to run setup.bat."
        exit 0
    }

    # ── Step 2: Game Files / ISO Extraction ──────────────────────────────────
    Write-Host ""
    Write-Host "[Step 2/4] Checking game files (game\default.xbe)..." -ForegroundColor White
    $xbePath = Join-Path $RepoRoot "game\default.xbe"
    $gameFiles = if (Test-Path (Join-Path $RepoRoot "game")) {
        (Get-ChildItem (Join-Path $RepoRoot "game") -File -ErrorAction SilentlyContinue).Count
    } else { 0 }

    if ((Test-Path $xbePath) -and $gameFiles -ge 5) {
        $tid = Get-XbeTitleId $py $xbePath
        Write-Ok "Found extracted game files in game\ ($gameFiles files, Title ID: $tid)"
        if ($tid -and $tid -ne "0x45410043") {
            Write-Warn "Expected Title ID 0x45410043 (The Sims: Bustin' Out USA), got $tid."
        }
    } else {
        $chosenIso = $IsoPath
        if (-not $chosenIso) {
            $detected = Find-NearbyIso
            if ($detected) {
                Write-Info "Detected nearby Xbox ISO: $detected"
                if ($NoPrompt) {
                    $chosenIso = $detected
                } else {
                    $ans = Read-Host "  Extract this ISO into game\? (Original ISO is never modified) [Y/n]"
                    if (-not $ans -or $ans -match "^[Yy]") {
                        $chosenIso = $detected
                    }
                }
            }
        }
        if (-not $chosenIso -and -not $NoPrompt) {
            Write-Info "Opening Windows File Explorer — select your The Sims: Bustin' Out (.iso / .xiso.iso)..."
            $chosenIso = Prompt-IsoFileDialog
        }
        if (-not $chosenIso -or -not (Test-Path $chosenIso)) {
            Write-Err "No Xbox ISO selected and game\default.xbe was not found."
            Write-Host "         Place your extracted disc files in game\ or re-run setup.bat and pick your .iso." -ForegroundColor Gray
            exit 1
        }

        Write-Info "Extracting Xbox ISO -> game\ (this takes ~10-30 seconds; original ISO is untouched)..."
        $env:PYTHONPATH = Join-Path $RepoRoot "third_party\xboxrecomp"
        New-Item -ItemType Directory -Force -Path (Join-Path $RepoRoot "game") | Out-Null
        Invoke-Py $py @("-m", "tools.xiso", "unpack", $chosenIso, "-o", "game")
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $xbePath)) {
            Write-Err "ISO extraction failed or default.xbe was not found inside the image."
            exit 1
        }
        $tid = Get-XbeTitleId $py $xbePath
        $gameFiles = (Get-ChildItem (Join-Path $RepoRoot "game") -File).Count
        Write-Ok "Extracted $gameFiles files into game\ (Title ID: $tid)"
    }

    # ── Step 3: Static Recompilation (default.xbe -> src\recomp\gen\) ────────
    Write-Host ""
    Write-Host "[Step 3/4] Static recompilation (x86 -> C)..." -ForegroundColor White
    $dispatchC = Join-Path $RepoRoot "src\recomp\gen\recomp_dispatch.c"
    if ((Test-Path $dispatchC) -and -not $ForceRecomp) {
        Write-Ok "Recompiled C sources already present in src\recomp\gen\ (pass -ForceRecomp to regenerate)."
    } else {
        Write-Info "Lifting 25,891 functions from game\default.xbe (takes ~1-2 minutes on first run)..."
        & (Join-Path $RepoRoot "scripts\recomp.ps1") -XbePath $xbePath
        if ($LASTEXITCODE -ne 0) {
            Write-Err "Static recompilation failed."
            exit 1
        }
        Write-Ok "Static recompilation completed."
    }

    # ── Step 4: Configure & Build Release Executable ─────────────────────────
    Write-Host ""
    Write-Host "[Step 4/4] Building native Windows x64 executable (Release)..." -ForegroundColor White
    $exePath = Join-Path $RepoRoot "build\Release\sims_bustin_out_recomp.exe"
    $needsBuild = $ForceBuild -or (-not (Test-Path $exePath))
    if (-not $needsBuild) {
        $exeTime = (Get-Item $exePath).LastWriteTime
        $newer = Get-ChildItem (Join-Path $RepoRoot "src") -File | Where-Object { $_.LastWriteTime -gt $exeTime }
        if ($newer -or ((Get-Item (Join-Path $RepoRoot "CMakeLists.txt")).LastWriteTime -gt $exeTime)) {
            $needsBuild = $true
        }
    }

    if (-not $needsBuild) {
        Write-Ok "Executable is already up-to-date: build\Release\sims_bustin_out_recomp.exe"
    } else {
        Write-Info "Configuring with CMake ($($vs.Generator), x64)..."
        & $cmake -S . -B build -G $vs.Generator -A x64
        if ($LASTEXITCODE -ne 0) {
            Write-Err "CMake configure failed."
            exit 1
        }
        Write-Info "Compiling Release build in parallel (first build takes ~2-4 minutes)..."
        & $cmake --build build --config Release --parallel
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exePath)) {
            Write-Err "Build failed."
            exit 1
        }
        Write-Ok "Build succeeded: build\Release\sims_bustin_out_recomp.exe"
    }

    # ── Done! ────────────────────────────────────────────────────────────────
    Write-Host ""
    Write-Host "======================================================================" -ForegroundColor Green
    Write-Host "  Setup Complete! Double-click play.bat anytime to launch the game." -ForegroundColor White
    Write-Host "======================================================================" -ForegroundColor Green
    Write-Host ""

    if (-not $SkipLaunch -and -not $NoPrompt) {
        $launch = Read-Host "  Launch The Sims: Bustin' Out Launcher now? [Y/n]"
        if (-not $launch -or $launch -match "^[Yy]") {
            Start-Process -FilePath $exePath -WorkingDirectory $RepoRoot
        }
    }
    exit 0
} finally {
    Pop-Location
}
