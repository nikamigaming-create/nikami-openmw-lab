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
    [pscustomobject]@{
        state = $State
        subsystem = $Subsystem
        action = $Action
        anchors = $Anchors
        notes = $Notes
    }
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
    New-Anchor "scripts/nikami/test-fnv-data-inventory.ps1" "FNV ESM4 no-silent-drop discovery summary" "record coverage gate names unsupported/source-only/store/runtime surfaces"
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
    ".txt" = New-Rule "vfs-readable-runtime-conditional" "text-assets" "VFS can stream text assets; runtime action depends on a referencing menu/script/record" @(
        New-Anchor "components/vfs/manager.hpp" "Files::IStreamPtr get" "VFS exposes arbitrary files by normalized path"
        New-Anchor "components/esm4/reader.cpp" "Reader::getStringImpl" "ESM4 reader consumes text/string payloads from plugins"
    ) "Tracked so text assets are not silent drops; add targeted runtime gates when specific TXT assets are referenced."
    ".tai" = New-Rule "blocked-runtime-support" "interface-texture-atlas-index" "FNV interface TAI atlas/index bytes are harvested but no runtime consumer is identified" @(
        New-Anchor "components/vfs/manager.hpp" "Files::IStreamPtr get" "VFS can expose TAI streams"
        New-Anchor "components/myguiplatform/myguitexture.cpp" "mImageManager->getImage" "MyGUI texture loading path exists for paired interface textures"
    ) "Identify the TAI format for textures/interface/interfaceshared.tai and connect it to the FNV menu renderer if required."
    ".lip" = New-Rule "vfs-readable-runtime-conditional" "voice-lip-sync" "Voice sidecar bytes are loaded through VFS, parsed into a timed envelope, and sampled by FNV mouth animation; exact phoneme/viseme mapping remains a follow-up gate" @(
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
    ".egt" = New-Rule "vfs-readable-runtime-conditional" "facegen-tint-maps" "FaceGen EGT bytes are loaded through VFS, validated as FREGT003 texture-mode maps, and folded into NPC skin material tinting; exact per-pixel FaceGen texture synthesis remains a follow-up gate" @(
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
    ".dlodsettings" = New-Rule "blocked-runtime-support" "distant-lod" "Distant LOD settings are harvested but not yet consumed by terrain/object paging" @(
        New-Anchor "apps/openmw/mwrender/objectpaging.cpp" "ObjectPaging" "object paging subsystem exists"
        New-Anchor "apps/openmw/mwrender/renderingmanager.cpp" "QuadTreeWorld" "terrain/object quad tree exists"
    ) "Add FNV DLOD settings parser and route to object paging/terrain LOD."
    ".spt" = New-Rule "blocked-runtime-support" "speedtree-tree-assets" "SpeedTree SPT assets are harvested but not yet parsed/rendered" @(
        New-Anchor "apps/openmw/mwclass/esm4base.hpp" "ESM4Tree" "ESM4 TREE records have a world class"
        New-Anchor "components/bgsm/file.hpp" "mTree" "tree material flag support exists"
    ) "Add SPT reader or conversion path for FNV tree assets."
    ".psa" = New-Rule "blocked-runtime-support" "procedural/animation-support" "PSA support assets are harvested but no runtime reader is wired" @(
        New-Anchor "apps/niftest/niftest.cpp" 'extension == ".psa"' "test tool recognizes PSA as a NIF-adjacent asset"
    ) "Identify PSA consumer in Gamebryo/FNV pipeline, then add reader or explicit runtime bypass."
    ".ctl" = New-Rule "blocked-runtime-support" "misc-control-data" "CTL misc-control bytes are VFS-visible but no runtime consumer is identified" @(
        New-Anchor "components/bsa/compressedbsafile.hpp" "FileFlag_MISC = 0x100" "archive reader classifies CTL and similar misc files"
    ) "Identify CTL owner format and add a parser or explicit ignore rule with proof."
    ".dat" = New-Rule "blocked-runtime-support" "misc-data" "DAT bytes are VFS-visible but no runtime consumer is identified" @(
        New-Anchor "components/vfs/manager.hpp" "Files::IStreamPtr get" "VFS can expose DAT streams"
    ) "Identify DAT owner format and add a parser or explicit ignore rule with proof."
    ".psd" = New-Rule "blocked-runtime-support" "source-art-leftover" "PSD source-art bytes are harvested; no runtime engine action is expected yet" @(
        New-Anchor "components/vfs/manager.hpp" "Files::IStreamPtr get" "VFS can expose PSD streams"
    ) "Verify whether PSD files are accidental shipped source art; if so, document as intentionally non-runtime."
    "<none>" = New-Rule "blocked-runtime-support" "extensionless-assets" "Extensionless archive entries require per-path ownership before runtime support can be claimed" @(
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
foreach ($archiveEntry in $archiveEntryLedger) {
    $listPath = Join-Path $HarvestDir $archiveEntry.entryList
    if (!(Test-Path -LiteralPath $listPath -PathType Leaf)) {
        throw "Missing archive entry list for $($archiveEntry.archive): $listPath"
    }
    $perArchiveCounts = @{}
    foreach ($entry in Get-Content -LiteralPath $listPath) {
        $ext = Get-EntryExtension $entry
        if (!$extensionCounts.ContainsKey($ext)) {
            $extensionCounts[$ext] = 0
        }
        if (!$perArchiveCounts.ContainsKey($ext)) {
            $perArchiveCounts[$ext] = 0
        }
        $extensionCounts[$ext] = [int]$extensionCounts[$ext] + 1
        $perArchiveCounts[$ext] = [int]$perArchiveCounts[$ext] + 1
    }
    $archiveRows += [pscustomobject]@{
        archive = $archiveEntry.archive
        entryCount = $archiveEntry.entryCount
        extensionCounts = [pscustomobject]$perArchiveCounts
    }
}

$extensionRows = @()
$unclassified = @()
$blocked = @()
$runtimeSupportedCount = 0
$runtimeConditionalCount = 0
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
    elseif ($rule.state -eq "vfs-readable-runtime-conditional") {
        ++$runtimeConditionalCount
    }
    elseif ($rule.state -eq "blocked-runtime-support") {
        $blocked += $ext
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
        state = "runtime-reader-routed"
        action = "ESM/ESP bytes are routed into ESM4 Reader/ESMStore; per-record runtime coverage is checked by test-fnv-data-inventory.ps1 and content ledger gates."
        anchors = $resolvedPluginAnchors
    }
})
$archiveManifestRows = @($archives | ForEach-Object {
    [pscustomobject]@{
        name = $_.name
        bytes = $_.bytes
        sha256 = $_.sha256
        state = "vfs-archive-routed"
        action = "BSA bytes are registered as fallback archives and entries are streamed/decompressed through VFS."
        anchors = $resolvedVfsAnchors
    }
})
$iniRows = @($iniShapes | ForEach-Object {
    [pscustomobject]@{
        name = $_.name
        bytes = $_.bytes
        sha256 = $_.sha256
        state = "shape-harvested-config-routed"
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
        extensionTypes = $extensionRows.Count
        runtimeSupportedExtensionTypes = $runtimeSupportedCount
        runtimeConditionalExtensionTypes = $runtimeConditionalCount
        blockedRuntimeExtensionTypes = $blocked.Count
        unclassifiedExtensionTypes = $unclassified.Count
    }
    plugins = $pluginRows
    archives = $archiveManifestRows
    iniShapes = $iniRows
    archiveExtensionCoverage = $extensionRows
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
Write-ProofLine "Extension types: $($extensionRows.Count)"
Write-ProofLine "Runtime-supported extension types: $runtimeSupportedCount"
Write-ProofLine "Runtime-conditional extension types: $runtimeConditionalCount"
Write-ProofLine "Blocked runtime extension types: $($blocked.Count)"
foreach ($ext in $blocked) {
    $row = $extensionRows | Where-Object { $_.extension -eq $ext } | Select-Object -First 1
    Write-ProofLine "BLOCKED runtime: $ext count=$($row.count) subsystem=$($row.subsystem)"
}
Write-ProofLine "Unclassified extension types: $($unclassified.Count)"
foreach ($ext in $unclassified) {
    Write-ProofLine "UNCLASSIFIED: $ext count=$($extensionCounts[$ext])"
}
Write-ProofLine "Coverage JSON: $coveragePath"

if ($unclassified.Count -gt 0) {
    throw "FNV harvest action coverage has unclassified archive extensions: $($unclassified -join ', '). See $SummaryFile."
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
