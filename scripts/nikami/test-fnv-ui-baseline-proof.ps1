param(
    [Parameter(Mandatory = $true)]
    [string]$ProofDir
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ProofDir = (Resolve-Path -LiteralPath $ProofDir).Path

$requiredScreenshots = @("hud.png", "status.png", "items.png", "map.png", "data.png")
$requiredLogs = @("hud_openmw.log", "status_openmw.log", "items_openmw.log", "map_openmw.log", "data_openmw.log")
$knownMissingMeshes = @(
    "meshes/sky_atmosphere.nif",
    "meshes/sky_night_01.nif",
    "meshes/sky_clouds_01.nif",
    "meshes/ashcloud.nif",
    "meshes/blightcloud.nif",
    "meshes/snow.nif",
    "meshes/blizzard.nif",
    "meshes/xbase_anim.nif",
    "meshes/xbase_anim.1st.nif",
    "meshes/xbase_anim_female.nif",
    "meshes/xargonian_swimkna.nif"
)

function Assert-File([string]$Name, [int64]$MinBytes) {
    $path = Join-Path $ProofDir $Name
    if (!(Test-Path -LiteralPath $path)) {
        throw "Missing proof artifact: $Name"
    }

    $item = Get-Item -LiteralPath $path
    if ($item.Length -lt $MinBytes) {
        throw "Proof artifact too small: $Name length=$($item.Length) min=$MinBytes"
    }
}

function Assert-LogContains([string]$LogName, [string]$Pattern, [string]$Description) {
    $path = Join-Path $ProofDir $LogName
    if (!(Select-String -LiteralPath $path -Pattern $Pattern -Quiet -ErrorAction SilentlyContinue)) {
        throw "Missing log marker in ${LogName}: $Description"
    }
}

foreach ($screenshot in $requiredScreenshots) {
    Assert-File $screenshot 10000
}

foreach ($log in $requiredLogs) {
    Assert-File $log 10000
}

Assert-LogContains "hud_openmw.log" "flat Fallout HUD readouts active HP/AP/AMMO/text compass" "HUD readouts"
Assert-LogContains "hud_openmw.log" "HUD Fallout bars applied HP/AP green theme; fatigue hidden" "Fallout HUD bars"
Assert-LogContains "hud_openmw.log" "FACE CHECK GSEasyPete: .*head=OK.*mouth=OK.*hairAttached=OK" "Easy Pete face data"
Assert-LogContains "hud_openmw.log" "package procedure animation source .*GoodspringsMilitiaTravelPackage" "Easy Pete FNV package data"
Assert-LogContains "status_openmw.log" "pushed inventory GUI mode page=`"status`" activeWindow=3" "STATUS page selected"
Assert-LogContains "status_openmw.log" "Fallout stats area applied SPECIAL/SKILLS" "STATUS SPECIAL/SKILLS"
Assert-LogContains "items_openmw.log" "pushed inventory GUI mode page=`"items`" activeWindow=1" "ITEMS page selected"
Assert-LogContains "items_openmw.log" "inventory tabs applied ALL/WEAPONS/APPAREL/AID/MISC / AMMO" "ITEMS tabs"
Assert-LogContains "map_openmw.log" "pushed inventory GUI mode page=`"map`" activeWindow=0" "MAP page selected"
Assert-LogContains "map_openmw.log" "Fallout world map texture bound textures/interface/worldmap/wasteland_nv_1024_no_map.dds" "Mojave map texture"
Assert-LogContains "data_openmw.log" "pushed inventory GUI mode page=`"data`" activeWindow=2" "DATA page selected"
Assert-LogContains "data_openmw.log" "DATA pane placeholder active for QUESTS/NOTES/RADIO/PERKS/ALT AMMO" "DATA placeholders"

$realBlockers = @()
$knownNoise = @()
foreach ($log in $requiredLogs) {
    $path = Join-Path $ProofDir $log
    $matches = Select-String -LiteralPath $path -Pattern "Fatal error|Failed to start new game|unknown global|List of NPC classes|marker_error" -ErrorAction SilentlyContinue
    foreach ($match in $matches) {
        $isKnown = $false
        foreach ($mesh in $knownMissingMeshes) {
            if ($match.Line -like "*$mesh*") {
                $isKnown = $true
                break
            }
        }

        if ($isKnown) {
            $knownNoise += $match
        }
        else {
            $realBlockers += $match
        }
    }
}

if ($realBlockers.Count -gt 0) {
    Write-Host "FNV UI baseline proof FAILED: real blocker log lines follow."
    foreach ($line in ($realBlockers | Select-Object -First 40)) {
        Write-Host $line.Line
    }
    throw "FNV UI baseline proof failed with $($realBlockers.Count) real blocker line(s)."
}

Write-Host "FNV UI baseline proof PASS"
Write-Host "  ProofDir: $ProofDir"
Write-Host "  Screenshots: $($requiredScreenshots -join ', ')"
Write-Host "  Known tolerated missing default mesh lines: $($knownNoise.Count)"
