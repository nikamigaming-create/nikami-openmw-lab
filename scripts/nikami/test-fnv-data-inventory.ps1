param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$BsaTool = $env:NIKAMI_BSATOOL,
    [string]$ProofRoot = "",
    [switch]$StrictNoSilentDrop
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($FnvRoot) -and [string]::IsNullOrWhiteSpace($FnvData)) {
    throw "Set -FnvRoot, -FnvData, NIKAMI_FNV_ROOT, or NIKAMI_FNV_DATA before running this proof."
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Join-Path $FnvRoot "Data"
}
if ([string]::IsNullOrWhiteSpace($FnvRoot)) {
    $FnvRoot = Split-Path $FnvData -Parent
}
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($BsaTool)) {
    $candidate = Join-Path $RepoRoot "build-clean/Release/bsatool.exe"
    if (Test-Path -LiteralPath $candidate) {
        $BsaTool = $candidate
    }
    elseif (Test-Path -LiteralPath "D:\Modlists\fnv\openmw-source\MSVC2022_64\Release\bsatool.exe") {
        $BsaTool = "D:\Modlists\fnv\openmw-source\MSVC2022_64\Release\bsatool.exe"
    }
    else {
        $BsaTool = "bsatool.exe"
    }
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-data-inventory/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-File([string]$Path, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing ${Description}: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    Write-ProofLine "OK file: $Description -> $Path ($($item.Length) bytes)"
}

function Assert-Directory([string]$Path, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Missing ${Description}: $Path"
    }
    Write-ProofLine "OK dir: $Description -> $Path"
}

function Get-BsaList([string]$BsaPath) {
    $name = Split-Path $BsaPath -Leaf
    $cachePath = Join-Path $ProofDir "$name.list.txt"
    & $BsaTool list $BsaPath | Set-Content -LiteralPath $cachePath
    if ($LASTEXITCODE -ne 0) {
        throw "bsatool failed for $BsaPath exit=$LASTEXITCODE"
    }
    Write-ProofLine "OK BSA listed: $name -> $cachePath"
    Get-Content -LiteralPath $cachePath
}

function Assert-BsaEntry([string[]]$List, [string]$Entry, [string]$Description) {
    $match = $List | Where-Object { $_ -ieq $Entry } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($match)) {
        throw "Missing ${Description}: $Entry"
    }
    Write-ProofLine "OK BSA entry: $Description -> $Entry"
}

function Assert-IniValue([string[]]$Ini, [string]$Key, [string]$Expected = "") {
    $line = $Ini | Where-Object { $_ -match "^\s*$([Regex]::Escape($Key))\s*=" } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw "Missing INI key: $Key"
    }
    Write-ProofLine "OK INI key: $line"
    if (![string]::IsNullOrWhiteSpace($Expected) -and $line -notmatch "=\s*$([Regex]::Escape($Expected))\s*$") {
        throw "INI key $Key did not match expected value $Expected; actual line: $line"
    }
}

function ConvertTo-FourCC([uint32]$Value) {
    $bytes = [BitConverter]::GetBytes($Value)
    [Text.Encoding]::ASCII.GetString($bytes)
}

function Get-Esm4RecordInventory([string]$EsmPath) {
    $stream = [IO.File]::OpenRead($EsmPath)
    $reader = [IO.BinaryReader]::new($stream)
    $counts = @{}

    function Read-Range([IO.BinaryReader]$Reader, [int64]$EndOffset, [hashtable]$Counts) {
        while ($Reader.BaseStream.Position + 24 -le $EndOffset) {
            $recordStart = $Reader.BaseStream.Position
            $type = [Text.Encoding]::ASCII.GetString($Reader.ReadBytes(4))
            $size = $Reader.ReadUInt32()
            [void]$Reader.ReadUInt32()
            [void]$Reader.ReadUInt32()
            [void]$Reader.ReadUInt32()
            [void]$Reader.ReadUInt16()
            [void]$Reader.ReadUInt16()

            if ($type -eq "GRUP") {
                $groupEnd = $recordStart + [int64]$size
                if ($groupEnd -le $recordStart -or $groupEnd -gt $EndOffset) {
                    throw "Invalid GRUP range in ${EsmPath}: start=$recordStart size=$size end=$groupEnd limit=$EndOffset"
                }
                Read-Range $Reader $groupEnd $Counts
                $Reader.BaseStream.Position = $groupEnd
                continue
            }

            if (!$Counts.ContainsKey($type)) {
                $Counts[$type] = 0
            }
            $Counts[$type] = [int]$Counts[$type] + 1
            $next = $Reader.BaseStream.Position + [int64]$size
            if ($next -lt $Reader.BaseStream.Position -or $next -gt $EndOffset) {
                throw "Invalid ESM4 record range in ${EsmPath}: type=$type start=$recordStart size=$size next=$next limit=$EndOffset"
            }
            $Reader.BaseStream.Position = $next
        }
    }

    try {
        Read-Range $reader $stream.Length $counts
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }

    $counts.GetEnumerator() | Sort-Object Name | ForEach-Object {
        [pscustomobject]@{ Type = $_.Name; Count = [int]$_.Value }
    }
}

function Get-Esm4CodeCoverage {
    $loaderByRecord = @{}
    $typeByRecord = @{}
    Get-ChildItem -LiteralPath (Join-Path $RepoRoot "components/esm4") -Filter "load*.hpp" | ForEach-Object {
        $content = Get-Content -LiteralPath $_.FullName -Raw
        $lines = $content -split "`r?`n"
        for ($i = 0; $i -lt $lines.Count; ++$i) {
            $recordMatch = [regex]::Match($lines[$i], "sRecordId\s*=\s*(?:ESM::RecNameInts::|ESM::)REC_(?<record>[A-Z0-9_]+)4")
            if (!$recordMatch.Success) {
                continue
            }

            $type = ""
            for ($j = $i; $j -ge 0; --$j) {
                $typeMatch = [regex]::Match($lines[$j], "^\s{4}struct\s+(?<type>[A-Za-z0-9_]+)(?:\s|:|$)")
                if ($typeMatch.Success) {
                    $type = $typeMatch.Groups["type"].Value
                    break
                }
            }
            if ([string]::IsNullOrWhiteSpace($type)) {
                continue
            }

            $record = $recordMatch.Groups["record"].Value
            $loaderByRecord[$record] = $_.FullName
            $typeByRecord[$record] = $type
        }
    }

    $recordsHeader = Get-Content -LiteralPath (Join-Path $RepoRoot "components/esm4/records.hpp") -Raw
    $storeHeader = Get-Content -LiteralPath (Join-Path $RepoRoot "apps/openmw/mwworld/esmstore.hpp") -Raw
    $storeCpp = Get-Content -LiteralPath (Join-Path $RepoRoot "apps/openmw/mwworld/store.cpp") -Raw
    $esmstoreCpp = Get-Content -LiteralPath (Join-Path $RepoRoot "apps/openmw/mwworld/esmstore.cpp") -Raw
    $rawPendingRecords = @{}
    $rawPendingMatch = [regex]::Match(
        $esmstoreCpp,
        "static bool isLoadedPendingEsm4Record(?<body>[\s\S]*?)static bool readLoadedPendingEsm4Record"
    )
    if ($rawPendingMatch.Success) {
        foreach ($match in [regex]::Matches($rawPendingMatch.Groups["body"].Value, "case\s+ESM::REC_(?<record>[A-Z0-9_]+)4\s*:")) {
            $rawPendingRecords[$match.Groups["record"].Value] = $true
        }
    }

    @{
        LoaderByRecord = $loaderByRecord
        TypeByRecord = $typeByRecord
        RecordsHeader = $recordsHeader
        StoreHeader = $storeHeader
        StoreCpp = $storeCpp
        EsmStoreCpp = $esmstoreCpp
        RawPendingRecords = $rawPendingRecords
    }
}

function Write-Esm4CoverageInventory([string]$EsmPath) {
    $inventory = @(Get-Esm4RecordInventory $EsmPath)
    $coverage = Get-Esm4CodeCoverage
    $runtimeClaims = @{
        "AMMO" = "SpellWindow DATA alternate-ammo list"
        "ACHR" = "cell actor refs / placed NPC proof"
        "ACRE" = "cell actor refs / placed creature proof"
        "CELL" = "world/cell loading"
        "LAND" = "terrain loading"
        "LTEX" = "terrain layer textures"
        "NPC_" = "actor rendering/animation"
        "CREA" = "creature rendering/animation"
        "REFR" = "placed object refs"
        "SOUN" = "sound buffer baseline"
        "SNDR" = "sound reference baseline"
        "STAT" = "static world geometry"
        "MSTT" = "movable static tumbleweed/collision gate"
        "SCOL" = "static collection placement"
        "TXST" = "texture set rendering"
        "WEAP" = "weapon data/HUD ammo source"
        "NOTE" = "DATA note source store"
        "TACT" = "DATA radio talking-activator source store"
        "GMST" = "ESM4-to-runtime game setting bridge"
        "GLOB" = "ESM4-to-runtime global variable bridge"
    }

    $knownBlocked = @()
    $loadedPendingRuntime = @()
    $runtimeSupported = @()
    $total = 0

    Write-ProofLine ""
    Write-ProofLine "FNV ESM4 record inventory discovery"
    Write-ProofLine "ESM: $EsmPath"

    foreach ($row in $inventory) {
        $total += $row.Count
        $type = $row.Type
        $loader = $coverage.LoaderByRecord.ContainsKey($type)
        $loaderPath = if ($loader) { $coverage.LoaderByRecord[$type] } else { "" }
        $cppType = if ($coverage.TypeByRecord.ContainsKey($type)) { $coverage.TypeByRecord[$type] } else { "" }
        $recordsHeader = $loader -and $coverage.RecordsHeader -match [regex]::Escape((Split-Path $loaderPath -Leaf).Replace(".hpp", ".hpp"))
        $store = (-not [string]::IsNullOrWhiteSpace($cppType)) -and $coverage.StoreHeader -match "Store<ESM4::$([regex]::Escape($cppType))>"
        $instantiated = (-not [string]::IsNullOrWhiteSpace($cppType)) -and $coverage.StoreCpp -match "TypedDynamicStore<ESM4::$([regex]::Escape($cppType))"
        $rawPending = $coverage.RawPendingRecords.ContainsKey($type)
        $runtime = $runtimeClaims.ContainsKey($type)

        if ($loader -and $recordsHeader -and $store -and $instantiated -and $runtime) {
            $runtimeSupported += $row
            Write-ProofLine "OK ESM4 record type runtime-supported: $type count=$($row.Count) loader=1 records.hpp=1 store=1 instantiation=1 runtime=$($runtimeClaims[$type])"
        }
        elseif ($rawPending) {
            $loadedPendingRuntime += $row
            Write-ProofLine "WARN ESM4 record type loaded-pending-runtime: $type count=$($row.Count) rawPending=1 runtime=0"
        }
        elseif ($loader -and $recordsHeader -and $store -and $instantiated) {
            $loadedPendingRuntime += $row
            Write-ProofLine "WARN ESM4 record type loaded-pending-runtime: $type count=$($row.Count) loader=1 records.hpp=1 store=1 instantiation=1 runtime=0"
        }
        elseif ($loader) {
            $knownBlocked += $row
            Write-ProofLine "FAIL ESM4 record type known-blocked: $type count=$($row.Count) loader=1 records.hpp=$([int]$recordsHeader) store=$([int]$store) instantiation=$([int]$instantiated) runtime=$([int]$runtime)"
        }
        else {
            $knownBlocked += $row
            Write-ProofLine "FAIL ESM4 record type known-blocked: $type count=$($row.Count) loader=0 records.hpp=0 store=0 instantiation=0 runtime=0"
        }
    }

    Write-ProofLine "OK ESM4 inventory parsed: topLevelRecordTypes=$($inventory.Count) totalRecords=$total"
    Write-ProofLine "FNV ESM4 no-silent-drop discovery summary knownBlockedTypes=$($knownBlocked.Count) loadedPendingRuntimeTypes=$($loadedPendingRuntime.Count) runtimeSupportedTypes=$($runtimeSupported.Count)"

    if ($StrictNoSilentDrop -and $knownBlocked.Count -gt 0) {
        throw "FNV ESM4 no-silent-drop strict proof failed knownBlockedTypes=$($knownBlocked.Count). See $SummaryFile."
    }
}

Write-ProofLine "FNV data inventory proof $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "BsaTool: $BsaTool"
Write-ProofLine ""

Assert-Directory $FnvRoot "FNV root"
Assert-Directory $FnvData "FNV data"
$mainEsm = Join-Path $FnvData "FalloutNV.esm"
Assert-File $mainEsm "main ESM"
Write-Esm4CoverageInventory $mainEsm
Assert-File (Join-Path $FnvRoot "Fallout_default.ini") "default INI"
Assert-Directory (Join-Path $FnvData "Video") "loose video directory"
Assert-File (Join-Path $FnvData "Video/FNVIntro.bik") "loose FNV intro movie"

$ini = Get-Content -LiteralPath (Join-Path $FnvRoot "Fallout_default.ini")
Assert-IniValue $ini "sIntroMovie" "Fallout INTRO Vsk.bik"
Assert-IniValue $ini "sWelcomeScreen1" "loading_screen_legal"
Assert-IniValue $ini "sWelcomeScreen2" "loading_screen_bethsoft"
Assert-IniValue $ini "sWelcomeScreen3" "loading_screen_BGS"
Assert-IniValue $ini "sWelcomeScreen4" "loading_screen01"
Assert-IniValue $ini "sMainMenuBackground" "main_background"
Assert-IniValue $ini "iSystemColorHUDMainRed" "26"
Assert-IniValue $ini "iSystemColorHUDMainGreen" "255"
Assert-IniValue $ini "iSystemColorHUDMainBlue" "128"
Assert-IniValue $ini "bUsePipboyMode" "1"

$iniMovie = Join-Path (Join-Path $FnvData "Video") "Fallout INTRO Vsk.bik"
if (Test-Path -LiteralPath $iniMovie) {
    Write-ProofLine "OK INI movie resolves as loose file: $iniMovie"
}
else {
    Write-ProofLine "WARN INI movie path is not a loose file: $iniMovie"
    Write-ProofLine "WARN local loose intro candidate remains: $(Join-Path $FnvData "Video/FNVIntro.bik")"
}

$miscBsa = Join-Path $FnvData "Fallout - Misc.bsa"
$textures2Bsa = Join-Path $FnvData "Fallout - Textures2.bsa"
$texturesBsa = Join-Path $FnvData "Fallout - Textures.bsa"
Assert-File $miscBsa "misc BSA"
Assert-File $textures2Bsa "textures2 BSA"
Assert-File $texturesBsa "textures BSA"

$misc = Get-BsaList $miscBsa
Assert-BsaEntry $misc "menus\globals.xml" "menu globals"
Assert-BsaEntry $misc "menus\loading_menu.xml" "loading menu XML"
Assert-BsaEntry $misc "menus\main\hud_main_menu.xml" "HUD menu XML"
Assert-BsaEntry $misc "menus\main\inventory_menu.xml" "inventory menu XML"
Assert-BsaEntry $misc "menus\main\map_menu.xml" "map menu XML"
Assert-BsaEntry $misc "menus\main\stats_menu.xml" "stats menu XML"
Assert-BsaEntry $misc "menus\main\quickkeys_menu.xml" "quickkeys menu XML"
Assert-BsaEntry $misc "menus\prefabs\compassicon.xml" "compass prefab XML"
Assert-BsaEntry $misc "menus\prefabs\hudtemplates.xml" "HUD templates XML"

$textures2 = Get-BsaList $textures2Bsa
Assert-BsaEntry $textures2 "textures\interface\loading\loading_screen01.dds" "loading screen art"
Assert-BsaEntry $textures2 "textures\interface\loading\loading_screen_legal.dds" "legal loading art"
Assert-BsaEntry $textures2 "textures\interface\loading\loading_screen_bethsoft.dds" "Bethesda loading art"
Assert-BsaEntry $textures2 "textures\interface\loading\loading_screen_bgs.dds" "BGS loading art"
Assert-BsaEntry $textures2 "textures\interface\worldmap\wasteland_nv_2048_no_map.dds" "Mojave world map base 2048"
Assert-BsaEntry $textures2 "textures\interface\worldmap\wasteland_nv_1024_no_map.dds" "Mojave world map base 1024"
Assert-BsaEntry $textures2 "textures\interface\hud\glow_hud_comp_direction_strip.dds" "HUD compass direction strip"
Assert-BsaEntry $textures2 "textures\interface\hud\hud_comp_direction_strip.dds" "HUD compass strip"
Assert-BsaEntry $textures2 "textures\interface\hud\hud_compass_icon.dds" "HUD compass icon"
Assert-BsaEntry $textures2 "textures\interface\hud\hud_compass_mark.dds" "HUD compass mark"
Assert-BsaEntry $textures2 "textures\pipboy3000\pipboyarm01.dds" "Pip-Boy arm texture"
Assert-BsaEntry $textures2 "textures\pipboy3000\screen.dds" "Pip-Boy screen texture"
Assert-BsaEntry $textures2 "textures\pipboy3000\pipboyscanlines.dds" "Pip-Boy scanlines texture"

$textures = Get-BsaList $texturesBsa
Assert-BsaEntry $textures "textures\dlc04\interface\loading\dlc04loadingscreen01.dds" "DLC loading art sample"

Write-ProofLine ""
if ($StrictNoSilentDrop) {
    Write-ProofLine "FNV data inventory strict proof PASS"
}
else {
    Write-ProofLine "FNV data inventory discovery PASS"
}
Write-ProofLine "ProofDir: $ProofDir"
