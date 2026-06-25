param(
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Runner = Join-Path $PSScriptRoot "run-fnv-skinning-fixture-sweep.ps1"
if (!(Test-Path -LiteralPath $Runner -PathType Leaf)) {
    throw "Missing fixture sweep runner: $Runner"
}

$runRoot = Join-Path $ProofRoot "fnv-skinning-fixture-sweep"
if (Test-Path -LiteralPath $runRoot -PathType Container) {
    $before = @(Get-ChildItem -LiteralPath $runRoot -Directory | Select-Object -ExpandProperty FullName)
} else {
    $before = @()
}

& $Runner `
    -ProofRoot $ProofRoot `
    -Fixtures @(
        "label=contract-pete;target=FormId:0x00104c7f;kind=npc",
        "label=contract-gecko;target=NVCrV19FireGeckoRanged;kind=creature"
    ) `
    -Modes current,auto `
    -DryRun 2>&1

$after = @(Get-ChildItem -LiteralPath $runRoot -Directory | Sort-Object LastWriteTime -Descending)
$runDir = $after | Where-Object { $before -notcontains $_.FullName } | Select-Object -First 1
if ($null -eq $runDir) {
    $runDir = $after | Select-Object -First 1
}

$jsonPath = Join-Path $runDir.FullName "skinning-fixture-sweep.json"
if (!(Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
    throw "Missing fixture sweep JSON: $jsonPath"
}

$summaryPath = Join-Path (Split-Path $jsonPath -Parent) "summary.md"
if (!(Test-Path -LiteralPath $summaryPath -PathType Leaf)) {
    throw "Missing fixture sweep summary: $summaryPath"
}

$doc = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
if ($doc.schema -ne "nikami-fnv-skinning-fixture-sweep-v1") {
    throw "Unexpected fixture sweep schema: $($doc.schema)"
}
if ($doc.promoteToCpp -ne $false) {
    throw "Fixture sweep dry-run must not promote C++ changes."
}
if ($doc.fixtureCount -ne 2) {
    throw "Expected two dry-run fixtures, got $($doc.fixtureCount)."
}
if ($doc.payloadPolicy -notlike "*no retail assets are committed*") {
    throw "Fixture sweep missing no-retail payload policy."
}

$fixtures = @($doc.fixtures)
foreach ($expected in @("contract-pete", "contract-gecko")) {
    $row = $fixtures | Where-Object { $_.label -eq $expected } | Select-Object -First 1
    if ($null -eq $row) {
        throw "Missing fixture row: $expected"
    }
    if ($row.firstFailingGate -ne "dry-run-no-runtime-proof") {
        throw "Dry-run fixture row did not classify as pending runtime proof: $expected"
    }
}

Write-Host "FNV skinning fixture sweep contract PASS"
Write-Host "ProofDir: $(Split-Path $jsonPath -Parent)"
