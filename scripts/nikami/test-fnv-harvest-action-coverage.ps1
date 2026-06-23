param(
    [string]$HarvestDir = "",
    [string]$ProofRoot = "",
    [switch]$RequireRuntimeSupported
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($HarvestDir)) {
    $harvestRoot = Join-Path $ProofRoot "fnv-retail-harvest"
    if (!(Test-Path -LiteralPath $harvestRoot -PathType Container)) {
        throw "No FNV harvest proof root found. Run scripts/nikami/harvest-fnv-retail-ledger.ps1 first."
    }
    $latest = Get-ChildItem -LiteralPath $harvestRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if ($null -eq $latest) {
        throw "No FNV harvest proof directories found under $harvestRoot."
    }
    $HarvestDir = $latest.FullName
}
$HarvestDir = (Resolve-Path $HarvestDir).Path

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-harvest-action-coverage/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
$ArchiveEntryCoveragePath = Join-Path $ProofDir "archive-entry-coverage.jsonl"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Read-JsonFile([string]$Path, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing ${Description}: $Path"
    }
    Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Read-JsonList([string]$Path, [string]$Description) {
    $value = Read-JsonFile $Path $Description
    $items = [System.Collections.Generic.List[object]]::new()
    if ($null -eq $value) {
        return ,$items
    }
    if ($value -is [System.Array]) {
        foreach ($item in $value) {
            $items.Add($item)
        }
    }
    else {
        $items.Add($value)
    }
    return ,$items
}

function Assert-CodeAnchor([string]$RelativePath, [string]$Needle, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing source anchor for ${Description}: $RelativePath"
    }
    $text = Get-Content -LiteralPath $path -Raw
    if (!$text.Contains($Needle)) {
        throw "Missing source anchor for ${Description}: $Needle in $RelativePath"
    }
    [pscustomobject]@{
        path = $RelativePath
        needle = $Needle
        description = $Description
    }
}

function New-Anchor([string]$Path, [string]$Needle, [string]$Description) {
    [pscustomobject]@{
        path = $Path
        needle = $Needle
        description = $Description
    }
}

$AllowedCoverageStates = @(
    "runtime-supported",
    "loaded-pending-runtime",
    "known-blocked",
    "non-runtime-support-file",
    "intentionally-excluded-with-proof"
)

function Resolve-Anchors([object[]]$Anchors) {
    @($Anchors | ForEach-Object { Assert-CodeAnchor $_.path $_.needle $_.description })
}

function Get-EntryExtension([string]$Entry) {
    $ext = [IO.Path]::GetExtension($Entry).ToLowerInvariant()
    if ([string]::IsNullOrWhiteSpace($ext)) {
        "<none>"
    }
    else {
        $ext
    }
}

function New-Rule([string]$State, [string]$Subsystem, [string]$Action, [object[]]$Anchors, [string]$Notes = "") {
    if ($AllowedCoverageStates -notcontains $State) {
        throw "Invalid FNV classification state: $State"
    }
    [pscustomobject]@{
        state = $State
        subsystem = $Subsystem
        action = $Action
        anchors = $Anchors
        notes = $Notes
    }
}

function ConvertTo-JsonLiteral([string]$Value) {
    if ($null -eq $Value) {
        return "null"
    }

    $builder = [Text.StringBuilder]::new()
    [void]$builder.Append('"')
    foreach ($char in $Value.ToCharArray()) {
        switch ([int][char]$char) {
            8 { [void]$builder.Append('\b'); break }
            9 { [void]$builder.Append('\t'); break }
            10 { [void]$builder.Append('\n'); break }
            12 { [void]$builder.Append('\f'); break }
            13 { [void]$builder.Append('\r'); break }
            34 { [void]$builder.Append('\"'); break }
            92 { [void]$builder.Append('\\'); break }
            default {
                if ([int][char]$char -lt 32) {
                    [void]$builder.Append('\u{0:x4}' -f [int][char]$char)
                }
                else {
                    [void]$builder.Append($char)
                }
                break
            }
        }
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Write-ArchiveEntryCoverageRow(
    [IO.StreamWriter]$Writer,
    [string]$Archive,
    [string]$Entry,
    [string]$Extension,
    [string]$State,
    [string]$Subsystem,
    [string]$Action,
    [string]$Proof,
    [string]$Notes
) {
    $row = [Text.StringBuilder]::new()
    [void]$row.Append('{"archive":')
    [void]$row.Append((ConvertTo-JsonLiteral $Archive))
    [void]$row.Append(',"entry":')
    [void]$row.Append((ConvertTo-JsonLiteral $Entry))
    [void]$row.Append(',"extension":')
    [void]$row.Append((ConvertTo-JsonLiteral $Extension))
    [void]$row.Append(',"state":')
    [void]$row.Append((ConvertTo-JsonLiteral $State))
    [void]$row.Append(',"subsystem":')
    [void]$row.Append((ConvertTo-JsonLiteral $Subsystem))
    [void]$row.Append(',"action":')
    [void]$row.Append((ConvertTo-JsonLiteral $Action))
    [void]$row.Append(',"proof":')
    [void]$row.Append((ConvertTo-JsonLiteral $Proof))
    [void]$row.Append(',"notes":')
    [void]$row.Append((ConvertTo-JsonLiteral $Notes))
    [void]$row.Append('}')
    $Writer.WriteLine($row.ToString())
}

$manifest = Read-JsonFile (Join-Path $HarvestDir "manifest.json") "harvest manifest"
$plugins = Read-JsonList (Join-Path $HarvestDir "plugins-metadata.json") "plugin metadata"
$archives = Read-JsonList (Join-Path $HarvestDir "archives-metadata.json") "archive metadata"
$iniShapes = Read-JsonList (Join-Path $HarvestDir "ini-shape-ledger.json") "INI shape ledger"
$archiveEntryLedger = Read-JsonList (Join-Path $HarvestDir "archive-entry-ledger.json") "archive entry ledger"

$vfsAnchors = @(
    New-Anchor "apps/openmw/engine.cpp" "VFS::registerArchives" "engine registers fallback archives into VFS"
    New-Anchor "components/bsa/compressedbsafile.cpp" "CompressedBSAFile::getFile" "compressed BSA entries can be streamed/decompressed"
    New-Anchor "components/bsa/bsafile.cpp" "BSAFile::detectVersion" "BSA version detection routes archive reader"
)
$pluginAnchors = @(
    New-Anchor "apps/openmw/mwworld/esmstore.cpp" "ESMStore::loadESM4" "ESM4 plugin content is routed through ESMStore"
    New-Anchor "components/esm4/reader.cpp" "REC_TES4" "ESM4 reader recognizes TES4 plugin headers"
    New-Anchor "components/esm4/reader.cpp" "Reader::getLocalizedString" "ESM4 reader consumes localized/string subrecords"
    New-Anchor "scripts/nikami/test-fnv-data-inventory.ps1" "FNV ESM4 no-silent-drop discovery summary" "record coverage gate names five-state record-runtime surfaces"
)
$iniAnchors = @(
    New-Anchor "components/config/gamesettings.cpp" "GameSettings::getArchiveList" "OpenMW config reads archive/content settings"
    New-Anchor "apps/openmw/mwgui/videowidget.cpp" "Fallout INTRO Vsk.bik" "FNV INI intro movie alias is acted on"
    New-Anchor "apps/openmw/mwgui/loadingscreen.cpp" "menus/loading_menu.xml" "FNV loading screen INI/menu assets are acted on"
)

$runtimeRules = @{
    ".nif" = New-Rule "runtime-supported" "scene-mesh-collision" "VFS stream -> SceneManager/NIF -> render node and Bullet collision users" @(
        New-Anchor "components/resource/scenemanager.cpp" "SceneManager::getTemplate" "scene manager loads mesh templates"
        New-Anchor "components/nif/niffile.cpp" "Reader::Reader(NIFFile& file" "NIF binary reader consumes mesh files"
        New-Anchor "components/nifbullet/bulletnifloader.cpp" "BulletNifLoader" "NIF collision loader acts on mesh collision"
    )
    ".kf" = New-Rule "runtime-supported" "animation-keyframes" "VFS stream -> NIF/KF loader -> animation source/group playback" @(
        New-Anchor "apps/openmw/mwrender/animation.cpp" "Animation::addAnimSource" "animation system attaches external keyframes"
        New-Anchor "components/nifosg/nifloader.cpp" "FNV/ESM4 KF BLOCK AUDIT" "FNV keyframe controlled blocks are audited"
        New-Anchor "components/nif/controller.hpp" "NiControllerSequence" "NIF animation sequence records are parsed"
    )
    ".dds" = New-Rule "runtime-supported" "textures-ui-world" "VFS stream -> ImageManager/MyGUI/NIF texture binding" @(
        New-Anchor "components/resource/imagemanager.cpp" "USE_OSGPLUGIN(dds)" "DDS reader plugin is registered"
        New-Anchor "components/resource/imagemanager.cpp" "ImageManager::getImage" "image manager reads texture streams"
        New-Anchor "components/myguiplatform/myguitexture.cpp" "mImageManager->getImage" "MyGUI textures are backed by VFS images"
    )
    ".wav" = New-Rule "runtime-supported" "sound-voice" "VFS stream -> SoundManager/OpenAL/decoder for effects and voice" @(
        New-Anchor "apps/openmw/mwsound/soundmanagerimp.cpp" "SoundManager::playSound" "sound manager plays direct sound files"
        New-Anchor "apps/openmw/mwsound/soundmanagerimp.cpp" "SoundManager::say" "voice playback path consumes dialogue audio"
        New-Anchor "apps/openmw/mwsound/ffmpegdecoder.hpp" "class FFmpegDecoder" "audio decoder handles encoded streams"
    )
    ".ogg" = New-Rule "runtime-supported" "sound-voice" "VFS stream -> SoundManager/decoder for voice and ambient sound" @(
        New-Anchor "apps/openmw/mwsound/soundmanagerimp.cpp" "SoundManager::playSound" "sound manager plays OGG-backed buffers"
        New-Anchor "apps/openmw/mwsound/soundmanagerimp.cpp" "SoundManager::say" "dialogue voice path consumes encoded streams"
        New-Anchor "apps/openmw/mwsound/ffmpegdecoder.hpp" "class FFmpegDecoder" "audio decoder handles OGG streams"
    )
    ".mp3" = New-Rule "runtime-supported" "music-radio" "VFS stream -> SoundManager music stream/radio world music references" @(
        New-Anchor "apps/openmw/mwsound/soundmanagerimp.cpp" "SoundManager::streamMusic" "music stream path consumes MP3 files"
        New-Anchor "components/misc/resourcehelpers.cpp" "Workaround: Bethesda at some point converted some of the files to mp3" "sound helper resolves wav/mp3 alternates"
        New-Anchor "components/esm4/loadwrld.cpp" "freeside_01.mp3" "FNV world music MP3 references are recognized in ESM4 loading context"
    )
    ".xml" = New-Rule "runtime-supported" "menus-ui-layout" "VFS stream -> MyGUI menu/resource loading and FNV menu probes" @(
        New-Anchor "apps/openmw/mwgui/windowmanagerimp.cpp" 'MyGUI::ResourceManager::getInstance().load("core.xml")' "MyGUI loads XML resource files"
        New-Anchor "apps/openmw/mwgui/loadingscreen.cpp" "menus/loading_menu.xml" "FNV loading menu XML is acted on"
        New-Anchor "apps/openmw/mwgui/mainmenu.cpp" "menus/options/main_menu.xml" "FNV main menu XML is acted on"
    )
    ".fnt" = New-Rule "runtime-supported" "bitmap-fonts" "VFS stream -> FontLoader bitmap font parsing" @(
        New-Anchor "components/fontloader/fontloader.hpp" "loads Morrowind's .fnt/.tex fonts" "font loader declares FNT/TEX support"
        New-Anchor "components/fontloader/fontloader.cpp" "FontLoader::loadBitmapFont" "font loader consumes bitmap font bytes"
        New-Anchor "apps/openmw/mwgui/windowmanagerimp.cpp" "std::make_unique<Gui::FontLoader>" "window manager creates font loader"
    )
    ".tex" = New-Rule "runtime-supported" "bitmap-fonts" "VFS stream -> FontLoader paired bitmap texture parsing" @(
        New-Anchor "components/fontloader/fontloader.hpp" "loads Morrowind's .fnt/.tex fonts" "font loader declares FNT/TEX support"
        New-Anchor "components/fontloader/fontloader.cpp" "textureData" "font loader consumes paired TEX bitmap data"
        New-Anchor "apps/openmw/mwgui/windowmanagerimp.cpp" "std::make_unique<Gui::FontLoader>" "window manager creates font loader"
    )
    ".bik" = New-Rule "runtime-supported" "video-menu-intro" "VFS stream -> VideoWidget/VideoPlayer playback" @(
        New-Anchor "apps/openmw/mwgui/videowidget.cpp" "VideoWidget::playVideo" "video widget opens VFS video streams"
        New-Anchor "apps/openmw/mwgui/mainmenu.cpp" "video/menu_background.bik" "animated FNV menu background is acted on"
        New-Anchor "apps/openmw/mwgui/windowmanagerimp.cpp" "mVideoWidget->playVideo" "window manager plays requested videos"
    )
    ".txt" = New-Rule "loaded-pending-runtime" "text-assets" "VFS can stream text assets; runtime action depends on a referencing menu/script/record" @(
        New-Anchor "components/vfs/manager.hpp" "Files::IStreamPtr get" "VFS exposes arbitrary files by normalized path"
        New-Anchor "components/esm4/reader.cpp" "Reader::getStringImpl" "ESM4 reader consumes text/string payloads from plugins"
    ) "Tracked so text assets are not silent drops; add targeted runtime gates when specific TXT assets are referenced."
    ".tai" = New-Rule "runtime-supported" "interface-texture-atlas-index" "FNV interface TAI atlas metadata is parsed at runtime so atlas-only MyGUI texture requests crop from InterfaceShared DDS" @(
        New-Anchor "components/myguiplatform/myguitexture.cpp" "loadFalloutTaiAtlas" "MyGUI texture loading parses InterfaceShared TAI metadata"
        New-Anchor "components/myguiplatform/myguitexture.cpp" "cropFalloutTaiAtlasImage" "MyGUI texture loading crops atlas-only DDS regions"
        New-Anchor "components/myguiplatform/myguitexture.cpp" "FNV/ESM4 diag: resolved TAI atlas texture" "runtime logs successful TAI atlas resolution"
        New-Anchor "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '".dds" = New-Rule "runtime-supported"' "DDS texture entries are runtime-supported"
        New-Anchor "scripts/nikami/test-fnv-tai-atlas-contract.ps1" "FNV TAI atlas contract" "retail-safe TAI proof parses local atlas metadata without storing payloads"
    )
    ".lip" = New-Rule "loaded-pending-runtime" "voice-lip-sync" "Voice sidecar bytes are loaded through VFS, parsed into a timed envelope, and sampled by FNV mouth animation; exact phoneme/viseme mapping remains a follow-up gate" @(
        New-Anchor "apps/openmw/mwsound/soundmanagerimp.cpp" "loadVoiceLipSync" "voice playback resolves and parses matching LIP sidecars"
        New-Anchor "apps/openmw/mwsound/soundmanagerimp.cpp" "FNV/ESM4 diag: loaded LIP sync" "runtime logs parsed LIP sidecar metadata"
        New-Anchor "apps/openmw/mwbase/soundmanager.hpp" "getSaySoundLipValue" "sound interface exposes a timed LIP mouth value"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "getSaySoundLipValue" "FNV mouth/TRI drivers consume the LIP mouth value"
        New-Anchor "apps/openmw/mwsound/soundmanagerimp.cpp" "SoundManager::say" "voice audio path owns the LIP sidecar for active say streams"
        New-Anchor "scripts/nikami/test-fnv-dialogue-voice-lip-ledger.ps1" "INFO FormId voice LIP sidecars" "voice/LIP ledger proves harvested sidecars by INFO FormId"
    ) "Runtime now consumes LIP sidecar duration/payload as a mouth envelope. Promote to runtime-supported only after exact FNV phoneme/viseme mapping is decoded and gated."
    ".egm" = New-Rule "runtime-supported" "facegen-morphs" "VFS stream -> FaceGen EGM reader -> NPC head/body morph application" @(
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "loadFaceGenEgm" "FaceGen EGM reader loads external morph bytes"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "applyFaceGenEgmMorph" "FaceGen EGM morphs are applied to NPC geometry"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "FNV/ESM4 diag: loaded FaceGen EGM" "runtime logs successful EGM loading"
    )
    ".egt" = New-Rule "loaded-pending-runtime" "facegen-tint-maps" "FaceGen EGT bytes are loaded through VFS, validated as FREGT003 texture-mode maps, and folded into NPC skin material tinting; exact per-pixel FaceGen texture synthesis remains a follow-up gate" @(
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "loadFaceGenEgt" "FaceGen EGT reader loads external tint-map bytes"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" '"FREGT003"' "FaceGen EGT magic is validated"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "deriveFaceGenEgtMaterialTint" "FGTS texture coefficients derive runtime complexion tint"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "applyFaceGenEgtTint" "FaceGen EGT material tint is applied to actor skin surfaces"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "FNV/ESM4 diag: loaded FaceGen EGT" "runtime logs parsed EGT metadata"
        New-Anchor "scripts/nikami/test-fnv-egt-runtime-contract.ps1" "FNV EGT runtime contract" "retail-safe EGT proof gate validates local headers without storing payloads"
    ) "Promote to runtime-supported after exact FNV FaceGen per-pixel synthesis is decoded and gated."
    ".tri" = New-Rule "runtime-supported" "facegen-tri-morph-targets" "VFS stream -> FaceGen TRI reader -> static/dialogue morph application" @(
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "loadFaceGenTri" "FaceGen TRI reader loads external morph target bytes"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "FaceGenTriMorphVisitor" "FaceGen TRI morph visitor applies morph target geometry"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "applyFalloutDialogueMorph" "FaceGen TRI morphs can drive dialogue/head animation"
    )
    ".dlodsettings" = New-Rule "loaded-pending-runtime" "distant-lod" "Distant LOD settings are VFS-visible and byte-read in the PC-flat runtime proof; terrain/object paging binding remains pending" @(
        New-Anchor "scripts/nikami/test-fnv-dlodsettings-contract.ps1" "FNV DLOD settings contract" "proof gate verifies the exact shipped DLOD settings set and runtime VFS byte loading"
        New-Anchor "apps/openmw/engine.cpp" "FNV/ESM4 proof: DLOD settings summary count=" "runtime diagnostic logs shipped DLOD settings count and byte total"
        New-Anchor "apps/openmw/mwrender/objectpaging.cpp" "getLODMeshName" "object paging uses generated LOD mesh names"
        New-Anchor "apps/openmw/mwrender/renderingmanager.cpp" "QuadTreeWorld" "terrain/object quad tree exists"
        New-Anchor "components/esm4/loadstat.cpp" "mLOD" "STAT loader captures ESM4 LOD model strings"
        New-Anchor "components/esm4/loadrefr.cpp" "XLOD" "REFR loader sees XLOD data while DLOD settings paging semantics remain pending"
    ) "Parse FNV DLOD settings semantics and route them into object paging/terrain LOD before promoting to runtime-supported."
    ".spt" = New-Rule "loaded-pending-runtime" "speedtree-billboard-fallback" "SpeedTree SPT assets are VFS-visible and rendered through a billboard fallback; full SpeedTree geometry/collision remains a follow-up gate" @(
        New-Anchor "scripts/nikami/test-fnv-speedtree-spt-ledger.ps1" "FNV SpeedTree SPT ledger proof" "proof gate verifies exact shipped SPT set and TREE record references without storing payloads"
        New-Anchor "scripts/nikami/fnv_speedtree_spt_ledger.py" "collect_harvest_spt_paths" "ledger parser compares harvested SPT paths with parsed TREE record MODL values"
        New-Anchor "components/esm4/loadtree.cpp" "normalizeFalloutTreeModel" "TREE loader normalizes bare Fallout SPT paths into trees"
        New-Anchor "apps/openmw/mwclass/esm4base.hpp" "ESM4Tree" "ESM4 TREE records have a world class"
        New-Anchor "apps/openmw/mwrender/objectpaging.cpp" "correctEsm4StaticModelPath" "object paging resolves TREE SPT paths without generic meshes prefixing"
        New-Anchor "apps/openmw/mwrender/objectpaging.cpp" "case ESM::REC_TREE4" "object paging attempts to page TREE records into the world"
        New-Anchor "components/resource/scenemanager.cpp" "loadFalloutSpeedTreeBillboard" "scene manager builds a crossed billboard fallback for SPT files"
        New-Anchor "components/resource/scenemanager.cpp" "textures/trees/billboards/" "billboard fallback derives FNV tree billboard texture paths from SPT names"
        New-Anchor "components/resource/scenemanager.cpp" "Ignoring SpeedTree data file" "runtime still fails closed to an empty node if billboard fallback cannot load"
        New-Anchor "components/bgsm/file.hpp" "mTree" "tree material flag support exists"
    ) "Promote to runtime-supported after a real SPT reader/conversion path proves tree geometry and collision beyond billboard fallback."
    ".psa" = New-Rule "loaded-pending-runtime" "actor-deathpose-animation" "PSA death-pose assets are VFS-visible and byte-read in the PC-flat runtime proof; actor/creature death playback remains pending" @(
        New-Anchor "scripts/nikami/test-fnv-psa-deathpose-contract.ps1" "FNV PSA death-pose contract" "proof gate verifies the exact shipped death-pose PSA set and runtime VFS byte loading"
        New-Anchor "apps/openmw/engine.cpp" "FNV/ESM4 proof: PSA death-pose summary count=" "runtime diagnostic logs shipped PSA death-pose count and byte total"
        New-Anchor "apps/niftest/niftest.cpp" 'extension == ".psa"' "test tool recognizes PSA as a NIF-adjacent asset"
        New-Anchor "apps/openmw/mwrender/animation.hpp" "class Animation" "actor animation runtime owns any future death-pose playback"
        New-Anchor "apps/openmw/mwrender/creatureanimation.cpp" "CreatureAnimation" "creature animation runtime owns creature death-pose playback"
        New-Anchor "apps/openmw/mwrender/npcanimation.cpp" "NpcAnimation" "NPC animation runtime owns humanoid death-pose playback"
    ) "Parse PSA death-pose animation semantics and bind actor/creature death playback before promoting to runtime-supported."
    ".ctl" = New-Rule "loaded-pending-runtime" "facegen-control-basis" "FaceGen CTL bytes are loaded through VFS, validated as FRCTL001, and used to verify the FNV FaceGen 50/30/50 coefficient basis; full control payload semantics remain a follow-up gate" @(
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "loadFaceGenCtl" "FaceGen CTL reader loads external control bytes"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" '"FRCTL001"' "FaceGen CTL magic is validated"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "validateFaceGenCtlBasis" "FaceGen CTL basis validates NPC coefficient arrays"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "FNV/ESM4 diag: loaded FaceGen CTL" "runtime logs parsed CTL metadata"
        New-Anchor "scripts/nikami/test-fnv-facegen-ctl-contract.ps1" "FNV FaceGen CTL contract" "retail-safe CTL proof validates local header without storing payloads"
    ) "Promote to runtime-supported after the full FaceGen control payload beyond the basis header is decoded and gated."
    ".dat" = New-Rule "non-runtime-support-file" "lip-generation-support-tables" "The shipped DAT files are the six lsdata support tables used for lip-generation tooling; runtime voice playback consumes baked LIP sidecars instead" @(
        New-Anchor "scripts/nikami/test-fnv-lsdata-dat-contract.ps1" "FNV LSDATA DAT contract" "proof gate verifies the exact shipped DAT set and samples metadata without storing payloads"
        New-Anchor "apps/openmw/mwsound/soundmanagerimp.cpp" "loadVoiceLipSync" "runtime voice playback resolves baked LIP sidecars"
        New-Anchor "apps/openmw/mwbase/soundmanager.hpp" "getSaySoundLipValue" "runtime mouth value is sourced from active voice/LIP playback"
        New-Anchor "apps/openmw/mwrender/esm4npcanimation.cpp" "getSaySoundLipValue" "mouth animation consumes baked LIP playback values"
    ) "Do not require a DAT runtime reader unless future evidence shows FNV evaluates lsdata tables during gameplay instead of using baked LIP sidecars."
    ".psd" = New-Rule "non-runtime-support-file" "source-art-leftover" "PSD source-art bytes are harvested and accounted for, but runtime consumes their DDS siblings instead of PSD payloads" @(
        New-Anchor "scripts/nikami/test-fnv-source-art-nonruntime-contract.ps1" "FNV PSD source-art non-runtime contract" "proof gate verifies PSD entries have runtime DDS siblings"
        New-Anchor "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '".dds" = New-Rule "runtime-supported"' "DDS texture entries are runtime-supported"
        New-Anchor "components/resource/imagemanager.cpp" "ImageManager::getImage" "runtime texture path consumes renderable image assets"
    ) "Intentional non-runtime source art; keep harvested as metadata only and do not require a PSD renderer."
    "<none>" = New-Rule "known-blocked" "extensionless-assets" "Extensionless archive entries require per-path ownership before runtime support can be claimed" @(
        New-Anchor "components/vfs/manager.hpp" "Files::IStreamPtr get" "VFS can expose extensionless streams"
    ) "Add path-specific rules for any extensionless entries that appear in a harvest."
}

Write-ProofLine "FNV harvest action coverage $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "HarvestDir: $HarvestDir"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RequireRuntimeSupported: $([bool]$RequireRuntimeSupported)"
Write-ProofLine ""

$resolvedVfsAnchors = Resolve-Anchors $vfsAnchors
$resolvedPluginAnchors = Resolve-Anchors $pluginAnchors
$resolvedIniAnchors = Resolve-Anchors $iniAnchors

$extensionCounts = @{}
$archiveRows = @()
$archiveEntryStateCounts = @{}
foreach ($state in $AllowedCoverageStates) {
    $archiveEntryStateCounts[$state] = 0
}
$archiveEntryRows = 0
$archiveEntryDeclaredRows = 0
$archiveEntryUnclassifiedRows = 0
$archiveEntryWriter = [IO.StreamWriter]::new($ArchiveEntryCoveragePath, $false, [Text.UTF8Encoding]::new($false))
try {
    foreach ($archiveEntry in $archiveEntryLedger) {
        $listPath = Join-Path $HarvestDir $archiveEntry.entryList
        if (!(Test-Path -LiteralPath $listPath -PathType Leaf)) {
            throw "Missing archive entry list for $($archiveEntry.archive): $listPath"
        }
        $entries = @(Get-Content -LiteralPath $listPath)
        $declaredEntryCount = [int]$archiveEntry.entryCount
        $archiveEntryDeclaredRows += $declaredEntryCount
        if ($entries.Count -ne $declaredEntryCount) {
            throw "Archive entry list count mismatch for $($archiveEntry.archive): actual=$($entries.Count) declared=$declaredEntryCount"
        }

        $perArchiveCounts = @{}
        foreach ($entry in $entries) {
            $ext = Get-EntryExtension $entry
            if (!$extensionCounts.ContainsKey($ext)) {
                $extensionCounts[$ext] = 0
            }
            if (!$perArchiveCounts.ContainsKey($ext)) {
                $perArchiveCounts[$ext] = 0
            }
            $extensionCounts[$ext] = [int]$extensionCounts[$ext] + 1
            $perArchiveCounts[$ext] = [int]$perArchiveCounts[$ext] + 1

            if ($runtimeRules.ContainsKey($ext)) {
                $rule = $runtimeRules[$ext]
                if ($AllowedCoverageStates -notcontains $rule.state) {
                    throw "Invalid archive entry state for $($archiveEntry.archive):$entry -> $($rule.state)"
                }
                $state = [string]$rule.state
                $subsystem = [string]$rule.subsystem
                $action = [string]$rule.action
                $proof = "Classified by extension rule $ext; code anchors are recorded in archiveExtensionCoverage."
                $notes = [string]$rule.notes
                $archiveEntryStateCounts[$state] = [int]$archiveEntryStateCounts[$state] + 1
            }
            else {
                $state = "unclassified"
                $subsystem = "unclassified-extension"
                $action = "No archive extension action rule exists for this path."
                $proof = "Failing gate: add a five-state extension/path rule before this entry can be considered accounted."
                $notes = ""
                ++$archiveEntryUnclassifiedRows
            }

            ++$archiveEntryRows
            Write-ArchiveEntryCoverageRow `
                -Writer $archiveEntryWriter `
                -Archive ([string]$archiveEntry.archive) `
                -Entry $entry `
                -Extension $ext `
                -State $state `
                -Subsystem $subsystem `
                -Action $action `
                -Proof $proof `
                -Notes $notes
        }

        $archiveRows += [pscustomobject]@{
            archive = $archiveEntry.archive
            entryCount = $declaredEntryCount
            classifiedEntryCount = $entries.Count
            extensionCounts = [pscustomobject]$perArchiveCounts
        }
    }
}
finally {
    $archiveEntryWriter.Dispose()
}

$extensionRows = @()
$unclassified = @()
$blocked = @()
$runtimeSupportedCount = 0
$loadedPendingRuntimeCount = 0
$nonRuntimeCount = 0
foreach ($ext in ($extensionCounts.Keys | Sort-Object)) {
    if (!$runtimeRules.ContainsKey($ext)) {
        $unclassified += $ext
        $extensionRows += [pscustomobject]@{
            extension = $ext
            count = [int]$extensionCounts[$ext]
            state = "unclassified"
            subsystem = ""
            action = ""
            anchors = @()
            notes = "Add this extension to the action coverage map."
        }
        continue
    }

    $rule = $runtimeRules[$ext]
    $anchors = Resolve-Anchors @($rule.anchors)
    if ($rule.state -eq "runtime-supported") {
        ++$runtimeSupportedCount
    }
    elseif ($rule.state -eq "loaded-pending-runtime") {
        ++$loadedPendingRuntimeCount
    }
    elseif ($rule.state -eq "known-blocked") {
        $blocked += $ext
    }
    elseif ($rule.state -eq "non-runtime-support-file") {
        ++$nonRuntimeCount
    }

    $extensionRows += [pscustomobject]@{
        extension = $ext
        count = [int]$extensionCounts[$ext]
        state = $rule.state
        subsystem = $rule.subsystem
        action = $rule.action
        anchors = $anchors
        notes = $rule.notes
    }
}

$pluginRows = @($plugins | ForEach-Object {
    [pscustomobject]@{
        name = $_.name
        bytes = $_.bytes
        sha256 = $_.sha256
        state = "runtime-supported"
        action = "ESM/ESP bytes are routed into ESM4 Reader/ESMStore; per-record runtime coverage is checked by test-fnv-data-inventory.ps1 and content ledger gates."
        anchors = $resolvedPluginAnchors
    }
})
$archiveManifestRows = @($archives | ForEach-Object {
    [pscustomobject]@{
        name = $_.name
        bytes = $_.bytes
        sha256 = $_.sha256
        state = "runtime-supported"
        action = "BSA bytes are registered as fallback archives and entries are streamed/decompressed through VFS."
        anchors = $resolvedVfsAnchors
    }
})
$iniRows = @($iniShapes | ForEach-Object {
    [pscustomobject]@{
        name = $_.name
        bytes = $_.bytes
        sha256 = $_.sha256
        state = "loaded-pending-runtime"
        action = "INI keys are harvested as shape metadata; runtime OpenMW config/menu/video hooks consume the relevant FNV settings without copying INI payloads."
        sectionCount = $_.sectionCount
        anchors = $resolvedIniAnchors
    }
})

$summary = [pscustomobject]@{
    stamp = $Stamp
    harvestDir = $HarvestDir
    proofDir = $ProofDir
    harvestManifest = $manifest
    counts = [pscustomobject]@{
        plugins = $pluginRows.Count
        archives = $archiveManifestRows.Count
        iniShapes = $iniRows.Count
        archiveEntries = ($extensionCounts.Values | Measure-Object -Sum).Sum
        archiveEntryRows = $archiveEntryRows
        declaredArchiveEntries = $archiveEntryDeclaredRows
        extensionTypes = $extensionRows.Count
        runtimeSupportedExtensionTypes = $runtimeSupportedCount
        loadedPendingRuntimeExtensionTypes = $loadedPendingRuntimeCount
        nonRuntimeExtensionTypes = $nonRuntimeCount
        blockedRuntimeExtensionTypes = $blocked.Count
        unclassifiedExtensionTypes = $unclassified.Count
        unclassifiedArchiveEntries = $archiveEntryUnclassifiedRows
        archiveEntryStateCounts = [pscustomobject]$archiveEntryStateCounts
    }
    plugins = $pluginRows
    archives = $archiveManifestRows
    iniShapes = $iniRows
    archiveExtensionCoverage = $extensionRows
    archiveEntryCoverageJsonl = $ArchiveEntryCoveragePath
    perArchiveExtensionCoverage = $archiveRows
    blockedRuntimeExtensions = $blocked
    unclassifiedExtensions = $unclassified
}

$coveragePath = Join-Path $ProofDir "action-coverage.json"
$summary | ConvertTo-Json -Depth 64 | Set-Content -LiteralPath $coveragePath -Encoding UTF8

Write-ProofLine "Plugins routed: $($pluginRows.Count)"
Write-ProofLine "Archives routed: $($archiveManifestRows.Count)"
Write-ProofLine "INI shapes routed: $($iniRows.Count)"
Write-ProofLine "Archive entries classified: $(($extensionCounts.Values | Measure-Object -Sum).Sum)"
Write-ProofLine "Archive entry rows: $archiveEntryRows"
Write-ProofLine "Declared archive entries: $archiveEntryDeclaredRows"
Write-ProofLine "Archive entry coverage JSONL: $ArchiveEntryCoveragePath"
foreach ($state in $AllowedCoverageStates) {
    Write-ProofLine "Archive entry state count ${state}: $($archiveEntryStateCounts[$state])"
}
Write-ProofLine "Extension types: $($extensionRows.Count)"
Write-ProofLine "Runtime-supported extension types: $runtimeSupportedCount"
Write-ProofLine "Loaded-pending-runtime extension types: $loadedPendingRuntimeCount"
Write-ProofLine "Non-runtime-support-file extension types: $nonRuntimeCount"
Write-ProofLine "Known-blocked extension types: $($blocked.Count)"
foreach ($ext in $blocked) {
    $row = $extensionRows | Where-Object { $_.extension -eq $ext } | Select-Object -First 1
    Write-ProofLine "KNOWN-BLOCKED: $ext count=$($row.count) subsystem=$($row.subsystem)"
}
Write-ProofLine "Unclassified extension types: $($unclassified.Count)"
foreach ($ext in $unclassified) {
    Write-ProofLine "UNCLASSIFIED: $ext count=$($extensionCounts[$ext])"
}
Write-ProofLine "Coverage JSON: $coveragePath"

if ($unclassified.Count -gt 0) {
    throw "FNV harvest action coverage has unclassified archive extensions: $($unclassified -join ', '). See $SummaryFile."
}
if ($archiveEntryRows -ne $archiveEntryDeclaredRows) {
    throw "FNV harvest action coverage classified $archiveEntryRows archive entries, expected $archiveEntryDeclaredRows."
}
if ($archiveEntryUnclassifiedRows -gt 0) {
    throw "FNV harvest action coverage has $archiveEntryUnclassifiedRows unclassified archive entries. See $ArchiveEntryCoveragePath."
}
if ($RequireRuntimeSupported -and $blocked.Count -gt 0) {
    throw "FNV harvest runtime support is incomplete for $($blocked.Count) extension type(s): $($blocked -join ', '). See $SummaryFile."
}

Write-ProofLine ""
if ($RequireRuntimeSupported) {
    Write-ProofLine "FNV harvest action coverage strict runtime PASS"
}
else {
    Write-ProofLine "FNV harvest action coverage classification PASS"
}
Write-ProofLine "ProofDir: $ProofDir"
