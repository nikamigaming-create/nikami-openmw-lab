param(
    [string]$FnvRoot = "D:\SteamLibrary\steamapps\common\Fallout New Vegas",
    [string]$FnvData = "",
    [string]$VcpkgRoot = "D:\code\c\FMODS\vcpkg",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 8
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
. (Join-Path $PSScriptRoot "fnv-runtime-settings.ps1")
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Join-Path $FnvRoot "Data"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-render-distance-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-FileContains([string]$Path, [string]$Pattern, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path)) { throw "Missing file for ${Label}: $Path" }
    if (!(Select-String -LiteralPath $Path -Pattern $Pattern -Quiet)) {
        throw "Missing ${Label}: $Pattern in $Path"
    }
    Write-ProofLine "OK ${Label}: $Pattern"
}

function Assert-FileNotContains([string]$Path, [string]$Pattern, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path)) { throw "Missing file for ${Label}: $Path" }
    $matches = @(Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue)
    if ($matches.Count -gt 0) {
        throw "Unexpected ${Label}: $($matches[0].Line.Trim())"
    }
    Write-ProofLine "OK absent ${Label}: $Pattern"
}

function Get-RequiredLogMatch([string]$Path, [string]$Pattern, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path)) { throw "Missing log for ${Label}: $Path" }
    $matchInfo = Select-String -LiteralPath $Path -Pattern $Pattern | Select-Object -First 1
    if ($null -eq $matchInfo) {
        throw "Missing ${Label}: $Pattern in $Path"
    }
    Write-ProofLine "OK ${Label}: $($matchInfo.Line.Trim())"
    return $matchInfo.Matches[0]
}

function Assert-NumberWithin([string]$Label, [double]$Actual, [double]$Expected, [double]$Tolerance) {
    if ([Math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "Unexpected ${Label}: actual=$Actual expected=$Expected tolerance=$Tolerance"
    }
    Write-ProofLine "OK ${Label}: actual=$Actual expected=$Expected tolerance=$Tolerance"
}

function Assert-RelativeNumberWithin([string]$Label, [double]$Actual, [double]$Expected, [double]$RelativeTolerance) {
    $scale = [Math]::Max(1.0, [Math]::Abs($Expected))
    $delta = [Math]::Abs($Actual - $Expected) / $scale
    if ($delta -gt $RelativeTolerance) {
        throw "Unexpected ${Label}: actual=$Actual expected=$Expected relativeDelta=$delta tolerance=$RelativeTolerance"
    }
    Write-ProofLine "OK ${Label}: actual=$Actual expected=$Expected relativeDelta=$delta tolerance=$RelativeTolerance"
}

function Get-SettingsViewingDistance([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }
    $match = Select-String -LiteralPath $Path `
        -Pattern "^\s*viewing distance\s*=\s*(?<value>[-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+))\s*$" |
        Select-Object -First 1
    if ($null -eq $match) {
        return $null
    }
    return [double]$match.Matches[0].Groups["value"].Value
}

function Add-SettingsAuditRow(
    [System.Collections.Generic.List[object]]$Rows,
    [System.Collections.Generic.List[string]]$Failures,
    [string]$Path,
    [string]$Scope,
    [double]$ExpectedViewingDistance,
    [bool]$Required,
    [bool]$MustMatchExpected,
    [string]$MismatchClassification,
    [string]$Proof
) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    $exists = Test-Path -LiteralPath $Path -PathType Leaf
    $value = if ($exists) { Get-SettingsViewingDistance $Path } else { $null }
    $matchesExpected = $false
    if ($null -ne $value) {
        $matchesExpected = [Math]::Abs($value - $ExpectedViewingDistance) -le 0.01
    }

    $classification = "runtime-supported"
    $passesCurrentRequirement = $true
    $rowProof = $Proof
    if (!$exists) {
        $classification = if ($Required) { "known-blocked" } else { "intentionally-excluded-with-proof" }
        $passesCurrentRequirement = !$Required
        $rowProof = if ($Required) { "Required current generated FNV settings.cfg is missing." } else { $Proof }
    }
    elseif ($null -eq $value) {
        $classification = if ($MustMatchExpected) { "known-blocked" } else { $MismatchClassification }
        $passesCurrentRequirement = !$MustMatchExpected
        $rowProof = "settings.cfg has no parseable Camera/viewing distance line."
    }
    elseif (!$matchesExpected -or $value -le 10000) {
        $classification = if ($MustMatchExpected) { "known-blocked" } else { $MismatchClassification }
        $passesCurrentRequirement = !$MustMatchExpected
    }

    if (!$passesCurrentRequirement) {
        $Failures.Add("$Scope path=$Path viewingDistance=$value expected=$ExpectedViewingDistance")
    }

    $Rows.Add([ordered]@{
        scope = $Scope
        path = $Path
        exists = $exists
        viewingDistance = $value
        expectedViewingDistance = $ExpectedViewingDistance
        matchesExpected = $matchesExpected
        required = $Required
        mustMatchExpected = $MustMatchExpected
        classification = $classification
        proof = $rowProof
    })
}

Write-ProofLine "FNV render distance contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

if (!(Test-Path -LiteralPath $FnvData -PathType Container)) {
    throw "Missing FNV data directory: $FnvData"
}

$FnvRootFromData = Get-NikamiFnvRootFromData -FnvData $FnvData
$BlockDistance = Get-NikamiFnvIniNumericSetting -FnvRoot $FnvRootFromData -SettingName "fBlockLoadDistance"
$ExpectedViewingDistance = Get-NikamiFnvViewingDistance -FnvData $FnvData
if ($ExpectedViewingDistance -le 10000) {
    throw "Harvested viewing distance is not beyond the old low fallback: $ExpectedViewingDistance"
}

Write-ProofLine "Harvested fBlockLoadDistance: $($BlockDistance.value)"
Write-ProofLine "Harvest source: $($BlockDistance.source)"
Write-ProofLine "Expected generated OpenMW viewing distance: $ExpectedViewingDistance"
Write-ProofLine ""

$HelperScript = Join-Path $RepoRoot "scripts/nikami/fnv-runtime-settings.ps1"
$FlatScript = Join-Path $RepoRoot "scripts/nikami/run-fnv-flat.ps1"
$FlatProofScript = Join-Path $RepoRoot "scripts/nikami/run-fnv-flat-proof.ps1"
$PcvrProofScript = Join-Path $RepoRoot "scripts/nikami/run-fnv-pcvr-proof.ps1"
$VrDeployScript = Join-Path $RepoRoot "scripts/nikami/deploy-fnv-vr-headset.ps1"
$RenderingManagerCpp = Join-Path $RepoRoot "apps/openmw/mwrender/renderingmanager.cpp"
$SkyCpp = Join-Path $RepoRoot "apps/openmw/mwrender/sky.cpp"

Assert-FileContains $HelperScript "fBlockLoadDistance" "runtime settings helper harvests FNV block load distance"
Assert-FileContains $FlatScript "Get-NikamiFnvViewingDistance" "flat runner derives viewing distance from helper"
Assert-FileContains $FlatProofScript "OPENMW_FNV_RENDER_DISTANCE_DIAG" "flat proof enables render-distance runtime diagnostics"
Assert-FileContains $RenderingManagerCpp "FNV/ESM4 proof: render distance viewDistance=" "renderer logs consumed render distance"
Assert-FileContains $SkyCpp "targetRadius=" "FNV sky wrap logs target radius"
Assert-FileContains $VrDeployScript "Get-NikamiFnvViewingDistance" "VR/headset deploy derives viewing distance from helper"
Assert-FileContains $VrDeployScript '\[string\]\$ProofRoot = ""' "VR/headset deploy accepts external proof root"
Assert-FileContains $VrDeployScript 'Split-Path \$RepoRoot -Parent' "VR/headset deploy defaults generated output outside repo"
Assert-FileContains $FlatScript "distant terrain = true" "flat runner enables generated distant terrain"
Assert-FileContains $FlatScript "object paging = true" "flat runner enables generated object paging"
Assert-FileContains $PcvrProofScript "distant terrain = true" "PCVR runner enables generated distant terrain"
Assert-FileContains $PcvrProofScript "object paging active grid = true" "PCVR runner enables generated active-grid object paging"
Assert-FileContains $VrDeployScript "distant terrain = true" "VR/headset deploy enables generated distant terrain"
Assert-FileContains $VrDeployScript "object paging min size = 0.01" "VR/headset deploy uses known-good object paging min size"
Assert-FileNotContains $FlatScript "viewing distance = 10000" "flat runner hardcoded low viewing distance"
Assert-FileNotContains $VrDeployScript "ViewingDistance = 10000|viewing distance = 10000" "VR/headset hardcoded low viewing distance"
Assert-FileNotContains $VrDeployScript 'Join-Path \$RepoRoot "proof/headset-fnv-vr"' "VR/headset repo-local generated proof root"

$flatProofRoot = Join-Path $ProofRoot "fnv-flat-proof"
$before = @()
if (Test-Path -LiteralPath $flatProofRoot) {
    $before = @(Get-ChildItem -LiteralPath $flatProofRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
}
$FlatRunSeconds = [Math]::Max($RunSeconds, 20)

& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $FlatRunSeconds `
    -NoSound

$after = @(Get-ChildItem -LiteralPath $flatProofRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending)
$latestFlatProof = $after | Where-Object { $before -notcontains $_.FullName } | Select-Object -First 1
if ($null -eq $latestFlatProof) {
    $latestFlatProof = $after | Select-Object -First 1
}
if ($null -eq $latestFlatProof) {
    throw "No FNV flat proof directory was produced"
}

$flatSettings = Join-Path $latestFlatProof.FullName "settings.cfg"
$flatOpenMwLog = Join-Path $latestFlatProof.FullName "openmw.log"
Write-ProofLine ""
Write-ProofLine "Flat proof: $($latestFlatProof.FullName)"
Write-ProofLine "Settings: $flatSettings"
Write-ProofLine "OpenMW log: $flatOpenMwLog"

Assert-FileContains $flatSettings "^viewing distance = $ExpectedViewingDistance$" "generated flat viewing distance"
Assert-FileContains $flatSettings "^distant terrain = true$" "generated flat distant terrain"
Assert-FileContains $flatSettings "^object paging = true$" "generated flat object paging"
Assert-FileContains $flatSettings "^object paging active grid = true$" "generated flat active-grid object paging"
Assert-FileContains $flatSettings "^object paging min size = 0\.01$" "generated flat object paging min size"
Assert-FileNotContains $flatSettings "^viewing distance = 10000$" "generated low fallback viewing distance"
Assert-FileNotContains $flatOpenMwLog "Failed to compile|failed to compile|linking failed|GLSL.*error|shader.*error" "shader/blocker line"

$settingsAuditRows = [System.Collections.Generic.List[object]]::new()
$settingsAuditFailures = [System.Collections.Generic.List[string]]::new()
Add-SettingsAuditRow $settingsAuditRows $settingsAuditFailures `
    (Join-Path $ProofRoot "configs/fnv-flat-clean/settings.cfg") `
    "current-generated-flat-config" $ExpectedViewingDistance $true $true "known-blocked" `
    "Current generated PC-flat config must use harvested FNV fBlockLoadDistance."
Add-SettingsAuditRow $settingsAuditRows $settingsAuditFailures `
    $flatSettings `
    "current-flat-proof-copy" $ExpectedViewingDistance $true $true "known-blocked" `
    "Current flat proof copy must match the generated PC-flat launch config."
Add-SettingsAuditRow $settingsAuditRows $settingsAuditFailures `
    (Join-Path $ProofRoot "configs/fnv-pcvr-clean/settings.cfg") `
    "optional-current-pcvr-config" $ExpectedViewingDistance $false $true "known-blocked" `
    "If a generated PCVR config exists, it must use harvested FNV fBlockLoadDistance."

$pcvrProofRoot = Join-Path $ProofRoot "fnv-pcvr-proof"
$latestPcvrProof = $null
if (Test-Path -LiteralPath $pcvrProofRoot -PathType Container) {
    $latestPcvrProof = Get-ChildItem -LiteralPath $pcvrProofRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
if ($null -ne $latestPcvrProof) {
    Add-SettingsAuditRow $settingsAuditRows $settingsAuditFailures `
        (Join-Path $latestPcvrProof.FullName "settings.cfg") `
        "optional-latest-pcvr-proof-copy" $ExpectedViewingDistance $false $true "known-blocked" `
        "If a PCVR proof copy exists, it must match harvested FNV fBlockLoadDistance."
}

Add-SettingsAuditRow $settingsAuditRows $settingsAuditFailures `
    (Join-Path $ProofRoot "headset-fnv-vr/stage/config/settings.cfg") `
    "optional-current-external-headset-stage" $ExpectedViewingDistance $false $true "known-blocked" `
    "Current headset deploy staging now lives outside the repo and must use harvested FNV fBlockLoadDistance when present."
Add-SettingsAuditRow $settingsAuditRows $settingsAuditFailures `
    (Join-Path $RepoRoot "proof/headset-fnv-vr/stage/config/settings.cfg") `
    "stale-repo-local-headset-stage-output" $ExpectedViewingDistance $false $false "intentionally-excluded-with-proof" `
    "Ignored generated stage output left from the old repo-local deploy path; not current publish evidence after deploy script externalized ProofRoot."

$localModlistSettings = "D:\Modlists\fnv\openmw-config\settings.cfg"
Add-SettingsAuditRow $settingsAuditRows $settingsAuditFailures `
    $localModlistSettings `
    "external-local-modlist-config" $ExpectedViewingDistance $false $false "loaded-pending-runtime" `
    "External local modlist settings are inventoried but are not the generated launch config used by current proof runners."

$flatUiProofRoot = "D:\Modlists\fnv\openmw-config\flat-ui-proof"
if (Test-Path -LiteralPath $flatUiProofRoot -PathType Container) {
    Get-ChildItem -LiteralPath $flatUiProofRoot -Recurse -Filter "settings.cfg" -File -ErrorAction SilentlyContinue |
        ForEach-Object {
            Add-SettingsAuditRow $settingsAuditRows $settingsAuditFailures `
                $_.FullName `
                "historical-external-flat-ui-proof-output" $ExpectedViewingDistance $false $false "intentionally-excluded-with-proof" `
                "Historical external flat UI proof output is inventoried but not current publish evidence."
        }
}

$settingsAuditPath = Join-Path $ProofDir "settings-distance-audit.json"
$settingsAuditRows | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $settingsAuditPath -Encoding UTF8
Write-ProofLine "Settings distance audit: $settingsAuditPath"
Write-ProofLine "Settings audit rows: $($settingsAuditRows.Count)"
if ($settingsAuditFailures.Count -gt 0) {
    foreach ($failure in $settingsAuditFailures) {
        Write-ProofLine "FAIL settings audit: $failure"
    }
    throw "FNV render distance settings audit failed. See $settingsAuditPath"
}
Write-ProofLine "OK settings audit current generated FNV configs match harvested viewing distance"

$renderMatch = Get-RequiredLogMatch $flatOpenMwLog `
    "FNV/ESM4 proof: render distance viewDistance=(?<view>[0-9.eE+-]+) near=(?<near>[0-9.eE+-]+) fov=(?<fov>[0-9.eE+-]+) aspect=(?<aspect>[0-9.eE+-]+) projectionFar=(?<projectionFar>[0-9.eE+-]+) sharedFar=(?<sharedFar>[0-9.eE+-]+) terrainViewDistance=(?<terrain>[0-9.eE+-]+)" `
    "runtime render distance consumption"
$runtimeViewDistance = [double]$renderMatch.Groups["view"].Value
$runtimeProjectionFar = [double]$renderMatch.Groups["projectionFar"].Value
$runtimeSharedFar = [double]$renderMatch.Groups["sharedFar"].Value
$runtimeFov = [double]$renderMatch.Groups["fov"].Value
$runtimeTerrainDistance = [double]$renderMatch.Groups["terrain"].Value
$expectedTerrainDistance = [double]$ExpectedViewingDistance / [Math]::Cos(([Math]::Min($runtimeFov, 140.0) * [Math]::PI / 180.0) / 2.0)
Assert-NumberWithin "runtime view distance" $runtimeViewDistance $ExpectedViewingDistance 0.01
Assert-NumberWithin "runtime projection far distance" $runtimeProjectionFar $ExpectedViewingDistance 0.01
Assert-NumberWithin "runtime shared far distance" $runtimeSharedFar $ExpectedViewingDistance 0.01
Assert-NumberWithin "runtime terrain view distance" $runtimeTerrainDistance $expectedTerrainDistance 1.0

$skyPattern = "FNV/ESM4: wrapped sky mesh (?<label>[^()]+) \((?<model>[^)]+)\) radius=(?<radius>[0-9.eE+-]+) viewDistance=(?<view>[0-9.eE+-]+) targetRadius=(?<target>[0-9.eE+-]+) scale=(?<scale>[0-9.eE+-]+) fitApplied=1"
$skyLines = @(Select-String -LiteralPath $flatOpenMwLog -Pattern $skyPattern -AllMatches)
if ($skyLines.Count -lt 4) {
    throw "Expected at least four FNV sky wrap fit diagnostics, got $($skyLines.Count). See $flatOpenMwLog"
}
$expectedSkyTargetRadius = [Math]::Max(1024.0, [double]$ExpectedViewingDistance * 0.82)
$skySamples = @()
foreach ($line in $skyLines) {
    foreach ($match in $line.Matches) {
        $label = $match.Groups["label"].Value.Trim()
        $radius = [double]$match.Groups["radius"].Value
        $view = [double]$match.Groups["view"].Value
        $target = [double]$match.Groups["target"].Value
        $scale = [double]$match.Groups["scale"].Value
        $expectedScale = $expectedSkyTargetRadius / $radius
        Assert-NumberWithin "sky $label view distance" $view $ExpectedViewingDistance 0.01
        Assert-NumberWithin "sky $label target radius" $target $expectedSkyTargetRadius 0.01
        Assert-RelativeNumberWithin "sky $label scale" $scale $expectedScale 0.01
        $skySamples += [ordered]@{
            label = $label
            model = $match.Groups["model"].Value
            radius = $radius
            viewDistance = $view
            targetRadius = $target
            scale = $scale
            expectedScale = $expectedScale
        }
    }
}

$result = [ordered]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    proofDir = $ProofDir
    flatProofDir = $latestFlatProof.FullName
    classification = "runtime-supported"
    harvestedSetting = "fBlockLoadDistance"
    harvestedSource = $BlockDistance.source
    harvestedValue = $BlockDistance.value
    generatedViewingDistance = $ExpectedViewingDistance
    settingsAuditPath = $settingsAuditPath
    settingsAuditRows = $settingsAuditRows.Count
    settingsAuditFailures = $settingsAuditFailures.Count
    runtimeRenderDistance = [ordered]@{
        viewDistance = $runtimeViewDistance
        projectionFar = $runtimeProjectionFar
        sharedFar = $runtimeSharedFar
        fov = $runtimeFov
        terrainViewDistance = $runtimeTerrainDistance
        expectedTerrainViewDistance = $expectedTerrainDistance
    }
    skyFitSamples = $skySamples
    checked = @(
        "retail FNV INI fBlockLoadDistance harvested from disk",
        "flat generated settings.cfg uses harvested viewing distance",
        "flat generated settings.cfg enables distant terrain and object paging from the known-good FNV PCVR profile",
        "current generated flat/PCVR/headset settings are audited against harvested viewing distance when present",
        "stale repo-local and external historical FNV settings outputs are explicitly classified instead of silently used as proof",
        "renderer consumed harvested viewing distance as projection/shared far distance",
        "terrain view distance derived from harvested viewing distance and FOV",
        "FNV sky mesh radius/scale derived from harvested viewing distance",
        "old 10000 viewing distance fallback absent",
        "shader/blocker log lines absent"
    )
}
$resultPath = Join-Path $ProofDir "fnv-render-distance-contract.json"
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Contract JSON: $resultPath"
Write-ProofLine "FNV render distance contract PASS"
