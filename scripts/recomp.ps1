<#
.SYNOPSIS
    Runs the full xboxrecomp static recompilation pipeline on game\default.xbe
    to generate src\recomp\gen\ for The Sims: Bustin' Out (USA).
.EXAMPLE
    .\scripts\recomp.ps1
    .\scripts\recomp.ps1 -XbePath "C:\path\to\default.xbe"
#>
param(
    [string]$XbePath = "game\default.xbe",
    [int]$SplitSize = 250
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot

try {
    if (-not (Test-Path $XbePath)) {
        Write-Error "XBE not found at '$XbePath'. Place your legally dumped The Sims: Bustin' Out (USA) files in game\ first."
        exit 1
    }

    $env:PYTHONPATH = Join-Path $RepoRoot "third_party\xboxrecomp"

    Write-Host "[1/4] Disassembling $XbePath -> analysis\disasm ..." -ForegroundColor Cyan
    python -m tools.disasm $XbePath -o analysis\disasm
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "[2/4] Identifying functions -> analysis\func_id ..." -ForegroundColor Cyan
    python -m tools.func_id $XbePath `
        --functions analysis\disasm\functions.json `
        --strings analysis\disasm\strings.json `
        --xrefs analysis\disasm\xrefs.json `
        -o analysis\func_id
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "[3/4] Running ABI analysis -> analysis\abi ..." -ForegroundColor Cyan
    python -m tools.abi_analysis $XbePath `
        --disasm-dir analysis\disasm `
        --func-id-dir analysis\func_id `
        --output-dir analysis\abi
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "[4/4] Lifting x86 -> C into src\recomp\gen (excluding manual overrides) ..." -ForegroundColor Cyan
    python -m tools.recomp $XbePath `
        --game-name "The Sims Bustin Out" `
        --all `
        --split $SplitSize `
        --gen-dir src\recomp\gen `
        --disasm-dir analysis\disasm `
        --func-id-dir analysis\func_id `
        --abi-dir analysis\abi `
        --exclude-manual src\recomp_manual.c
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "`nRecompilation complete! Next run:" -ForegroundColor Green
    Write-Host '  cmake -S . -B build -G "Visual Studio 17 2022" -A x64'
    Write-Host '  cmake --build build --config Release --parallel'
} finally {
    Pop-Location
}
