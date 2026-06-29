param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string]$RuntimeTag = "",
    [string]$ActorTarget = "GSEasyPete",
    [ValidateSet("npc", "creature", "auto")]
    [string]$ActorKind = "npc",
    [int]$RunSeconds = 3600,
    [string]$ScreenshotFrames = "",
    [int]$ActorDumpFrame = 360,
    [string]$BootstrapCell = "FormId:0x10daeb9",
    [double]$BootstrapX = -67480,
    [double]$BootstrapY = 1500,
    [double]$BootstrapZ = 8425,
    [double]$BootstrapRotX = 0,
    [double]$BootstrapRotY = 0,
    [double]$BootstrapRotZ = 1.5708,
    [double]$BootstrapHour = 10,
    [double]$ActorStageX = -67480,
    [double]$ActorStageY = 1500,
    [double]$ActorStageZ = 8425,
    [double]$ActorStageRotX = 0,
    [double]$ActorStageRotY = 0,
    [double]$ActorStageRotZ = 1.5708,
    [double]$ActorViewOffsetZ = 108,
    [double]$ActorViewTargetZ = 108,
    [string]$NeutralActorPreviewProfile = "face",
    [string]$CharacterBuilderPhase = "t-pose",
    [string]$FnvRotationMode = "bindCoreBindLowerRawUpper",
    [string]$FnvSkinningMatrixAudit = "arms,rightHand,leftHand,HeadOld,HeadHuman",
    [switch]$NoSound,
    [switch]$OpenStudio,
    [int]$ServePort = 0,
    [switch]$SkipStudioCatalog
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if ([string]::IsNullOrWhiteSpace($RuntimeTag)) {
    $RuntimeTag = "fnv-live-authoring-$Stamp"
}
$RunDir = Join-Path $ProofRoot "fnv-live-character-authoring/$Stamp"
$StudioDir = Join-Path $RunDir "studio"
$LiveAuthoringFile = Join-Path $RunDir "live-authoring.json"
$LiveRuntimeCommandFile = Join-Path $RunDir "live-runtime-command.json"
$ManifestPath = Join-Path $RunDir "live-authoring-run.json"
New-Item -ItemType Directory -Force -Path $RunDir, $StudioDir | Out-Null
$InitialActorKitAnimationSource = if ($ActorKind -ine "creature") { "hands-at-side" } else { "" }
$InitialFaceFamilyParts = @("body-skin", "head-skin", "face-organs", "hair-beard")
$InitialActorKitParts = if ($ActorKind -ine "creature") { $InitialFaceFamilyParts -join "," } else { "" }

function Quote-ProcessArgument([string]$Value) {
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Add-Arg([System.Collections.Generic.List[string]]$List, [string]$Name, [object]$Value) {
    if ($null -eq $Value) { return }
    if ($Value -is [double] -and [double]::IsNaN($Value)) { return }
    if ($Value -is [string] -and [string]::IsNullOrWhiteSpace($Value)) { return }
    $List.Add($Name)
    $List.Add([string]$Value)
}

function Get-FreeLoopbackPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    try {
        $listener.Start()
        return [int]$listener.LocalEndpoint.Port
    }
    finally {
        $listener.Stop()
    }
}

function Resolve-FnvDataFromLatestHarvest([string]$ProofRootPath) {
    $harvestRoot = Join-Path $ProofRootPath "fnv-retail-harvest"
    if (!(Test-Path -LiteralPath $harvestRoot -PathType Container)) { return "" }
    $manifests = Get-ChildItem -LiteralPath $harvestRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "manifest.json" } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
    foreach ($manifestPath in $manifests) {
        try {
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            $candidate = [string]$manifest.fnvData
            if (![string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate -PathType Container)) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }
        catch {
        }
    }
    return ""
}

function Resolve-VcpkgRootFromKnownPaths([string]$RepoRootPath) {
    $candidates = @(
        $env:NIKAMI_VCPKG_ROOT,
        "D:\code\c\FMODS\vcpkg",
        (Join-Path $RepoRootPath "vcpkg"),
        (Join-Path (Split-Path $RepoRootPath -Parent) "vcpkg")
    )
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $toolchain = Join-Path $candidate "scripts/buildsystems/vcpkg.cmake"
        if (Test-Path -LiteralPath $toolchain -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return ""
}

if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Resolve-FnvDataFromLatestHarvest $ProofRoot
}
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = Resolve-VcpkgRootFromKnownPaths $RepoRoot
}

$initialControls = [ordered]@{}
foreach ($prefix in @("OPENMW_FNV_HEADGEAR", "OPENMW_FNV_HAIR", "OPENMW_FNV_BROW", "OPENMW_FNV_EYE", "OPENMW_FNV_BEARD", "OPENMW_FNV_MOUTH")) {
    $defaultX = if ($prefix -eq "OPENMW_FNV_HAIR") { 90.0 } else { 0.0 }
    $defaultY = if ($prefix -eq "OPENMW_FNV_HAIR") { 90.0 } else { 0.0 }
    $defaultZ = if ($prefix -in @("OPENMW_FNV_BROW", "OPENMW_FNV_EYE", "OPENMW_FNV_BEARD", "OPENMW_FNV_MOUTH")) { -90.0 } else { 0.0 }
    $initialControls["${prefix}_OFFSET_X"] = 0.0
    $initialControls["${prefix}_OFFSET_Y"] = 0.0
    $initialControls["${prefix}_OFFSET_Z"] = 0.0
    $initialControls["${prefix}_ROTATION_X"] = $defaultX
    $initialControls["${prefix}_ROTATION_Y"] = $defaultY
    $initialControls["${prefix}_ROTATION_Z"] = $defaultZ
    $initialControls["${prefix}_PIVOT_MODE"] = $false
}
foreach ($entry in @(
    @{ Prefix = "OPENMW_FNV_HAIR_MESH_HAIRMESSY01"; Rotation = @(0.0, 0.0, -90.0) },
    @{ Prefix = "OPENMW_FNV_HAIR_MESH_HAIRBUN"; Rotation = @(90.0, 90.0, 0.0) }
)) {
    $prefix = $entry.Prefix
    $rotation = $entry.Rotation
    $initialControls["${prefix}_OFFSET_X"] = 0.0
    $initialControls["${prefix}_OFFSET_Y"] = 0.0
    $initialControls["${prefix}_OFFSET_Z"] = 0.0
    $initialControls["${prefix}_ROTATION_X"] = $rotation[0]
    $initialControls["${prefix}_ROTATION_Y"] = $rotation[1]
    $initialControls["${prefix}_ROTATION_Z"] = $rotation[2]
    $initialControls["${prefix}_PIVOT_MODE"] = $false
}
foreach ($prefix in @(
    "OPENMW_FNV_BONE_BIP01_R_HAND",
    "OPENMW_FNV_BONE_BIP01_L_HAND",
    "OPENMW_FNV_BONE_BIP01_R_THUMB1",
    "OPENMW_FNV_BONE_BIP01_R_THUMB2",
    "OPENMW_FNV_BONE_BIP01_R_FINGER11",
    "OPENMW_FNV_BONE_BIP01_R_FINGER12",
    "OPENMW_FNV_BONE_BIP01_R_FINGER21",
    "OPENMW_FNV_BONE_BIP01_R_FINGER22",
    "OPENMW_FNV_BONE_BIP01_R_FINGER31",
    "OPENMW_FNV_BONE_BIP01_R_FINGER32",
    "OPENMW_FNV_BONE_BIP01_R_FINGER41",
    "OPENMW_FNV_BONE_BIP01_R_FINGER42",
    "OPENMW_FNV_BONE_BIP01_L_THUMB1",
    "OPENMW_FNV_BONE_BIP01_L_THUMB2",
    "OPENMW_FNV_BONE_BIP01_L_FINGER11",
    "OPENMW_FNV_BONE_BIP01_L_FINGER12",
    "OPENMW_FNV_BONE_BIP01_L_FINGER21",
    "OPENMW_FNV_BONE_BIP01_L_FINGER22",
    "OPENMW_FNV_BONE_BIP01_L_FINGER31",
    "OPENMW_FNV_BONE_BIP01_L_FINGER32",
    "OPENMW_FNV_BONE_BIP01_L_FINGER41",
    "OPENMW_FNV_BONE_BIP01_L_FINGER42"
)) {
    $initialControls["${prefix}_OFFSET_X"] = 0.0
    $initialControls["${prefix}_OFFSET_Y"] = 0.0
    $initialControls["${prefix}_OFFSET_Z"] = 0.0
    $initialControls["${prefix}_ROTATION_X"] = 0.0
    $initialControls["${prefix}_ROTATION_Y"] = 0.0
    $initialControls["${prefix}_ROTATION_Z"] = 0.0
}

[pscustomobject][ordered]@{
    schema = "nikami-fnv-live-authoring-v1"
    schemaMarkers = @("runtime-live-authoring-v1", "head-surface-transform-controls-v1", "bone-transform-controls-v1", "generated-control-file-only-v1")
    path = $LiveAuthoringFile
    updatedAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    controls = $initialControls
    policy = [pscustomobject][ordered]@{
        generatedProofOutputsOnly = $true
        noRetailPayloadBytes = $true
        numericRuntimeControlsOnly = $true
    }
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $LiveAuthoringFile -Encoding UTF8

[pscustomobject][ordered]@{
    schema = "nikami-fnv-live-runtime-command-v1"
    schemaMarkers = @("runtime-live-target-switch-v1", "generated-command-file-only-v1")
    path = $LiveRuntimeCommandFile
    updatedAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    actorTarget = $ActorTarget
    runtimeTarget = $ActorTarget
    actorKind = $ActorKind
    entryId = ""
    command = "update-actor-kit"
    actorKitParts = $InitialActorKitParts
    actorKitPartModels = ""
    actorKitAnimationSource = $InitialActorKitAnimationSource
    selectors = [pscustomobject][ordered]@{
        parts = if ($ActorKind -ine "creature") { $InitialFaceFamilyParts } else { @() }
        partModels = @()
        animationSource = $InitialActorKitAnimationSource
    }
    policy = [pscustomobject][ordered]@{
        generatedProofOutputsOnly = $true
        noRetailPayloadBytes = $true
        activeCellActorSwitchOnly = $true
        baseNpcPreviewWhenInactive = $true
        baseCreaturePreviewWhenInactive = $true
        baseActorPreviewWhenInactive = $true
    }
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $LiveRuntimeCommandFile -Encoding UTF8

$StudioRunner = Join-Path $PSScriptRoot "run-fnv-character-studio-catalog.ps1"
$FlatProof = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
$LiveServer = Join-Path $PSScriptRoot "fnv_character_viewer_live_server.py"
$ViewerRunner = Join-Path $PSScriptRoot "run-fnv-character-viewer.ps1"
if (!(Test-Path -LiteralPath $StudioRunner -PathType Leaf)) { throw "Missing studio catalog runner: $StudioRunner" }
if (!(Test-Path -LiteralPath $FlatProof -PathType Leaf)) { throw "Missing flat proof runner: $FlatProof" }
if (!(Test-Path -LiteralPath $LiveServer -PathType Leaf)) { throw "Missing live studio server: $LiveServer" }
if (!(Test-Path -LiteralPath $ViewerRunner -PathType Leaf)) { throw "Missing FNV character viewer runner: $ViewerRunner" }

$ResolvedServePort = if ($ServePort -gt 0) { $ServePort } else { Get-FreeLoopbackPort }
$StudioServerJson = Join-Path $StudioDir "studio-live-server.json"
$StudioUrlFile = Join-Path $StudioDir "studio-url.txt"
$StudioUrl = "http://127.0.0.1:$ResolvedServePort/fnv-live-character-authoring/$Stamp/studio/character-studio.html"

$runtimeArgs = [System.Collections.Generic.List[string]]::new()
$runtimeArgs.Add("-NoProfile")
$runtimeArgs.Add("-ExecutionPolicy")
$runtimeArgs.Add("Bypass")
$runtimeArgs.Add("-File")
$runtimeArgs.Add($FlatProof)
Add-Arg $runtimeArgs "-BuildDir" $BuildDir
Add-Arg $runtimeArgs "-Configuration" $Configuration
Add-Arg $runtimeArgs "-FnvData" $FnvData
Add-Arg $runtimeArgs "-FnvConfigData" $FnvConfigData
Add-Arg $runtimeArgs "-VcpkgRoot" $VcpkgRoot
Add-Arg $runtimeArgs "-ExtraOsgPluginDir" $ExtraOsgPluginDir
Add-Arg $runtimeArgs "-Triplet" $Triplet
Add-Arg $runtimeArgs "-ProofRoot" $ProofRoot
Add-Arg $runtimeArgs "-RuntimeTag" $RuntimeTag
Add-Arg $runtimeArgs "-RunSeconds" $RunSeconds
Add-Arg $runtimeArgs "-ScreenshotFrames" $ScreenshotFrames
Add-Arg $runtimeArgs "-BootstrapCell" $BootstrapCell
Add-Arg $runtimeArgs "-BootstrapX" $BootstrapX
Add-Arg $runtimeArgs "-BootstrapY" $BootstrapY
Add-Arg $runtimeArgs "-BootstrapZ" $BootstrapZ
Add-Arg $runtimeArgs "-BootstrapRotX" $BootstrapRotX
Add-Arg $runtimeArgs "-BootstrapRotY" $BootstrapRotY
Add-Arg $runtimeArgs "-BootstrapRotZ" $BootstrapRotZ
Add-Arg $runtimeArgs "-BootstrapHour" $BootstrapHour
Add-Arg $runtimeArgs "-ActorTarget" $ActorTarget
Add-Arg $runtimeArgs "-ActorKind" $ActorKind
Add-Arg $runtimeArgs "-ActorDumpFrame" $ActorDumpFrame
$runtimeArgs.Add("-StageActor")
$runtimeArgs.Add("-NeutralActorPreview")
if ($ActorKind -ine "creature") {
    $runtimeArgs.Add("-NeutralActorPreviewStandingIdle")
    Add-Arg $runtimeArgs "-ActorKitAnimationSource" $InitialActorKitAnimationSource
}
Add-Arg $runtimeArgs "-NeutralActorPreviewProfile" $NeutralActorPreviewProfile
Add-Arg $runtimeArgs "-ActorStageX" $ActorStageX
Add-Arg $runtimeArgs "-ActorStageY" $ActorStageY
Add-Arg $runtimeArgs "-ActorStageZ" $ActorStageZ
Add-Arg $runtimeArgs "-ActorStageRotX" $ActorStageRotX
Add-Arg $runtimeArgs "-ActorStageRotY" $ActorStageRotY
Add-Arg $runtimeArgs "-ActorStageRotZ" $ActorStageRotZ
Add-Arg $runtimeArgs "-ActorViewOffsetZ" $ActorViewOffsetZ
Add-Arg $runtimeArgs "-ActorViewTargetZ" $ActorViewTargetZ
$runtimeArgs.Add("-ActorViewLocalOffset")
$runtimeArgs.Add("-FnvPartMatrixAudit")
Add-Arg $runtimeArgs "-FnvSkinningMatrixAudit" $FnvSkinningMatrixAudit
    Add-Arg $runtimeArgs "-FnvRotationMode" $FnvRotationMode
Add-Arg $runtimeArgs "-CharacterBuilderPhase" $CharacterBuilderPhase
Add-Arg $runtimeArgs "-LiveAuthoringFile" $LiveAuthoringFile
Add-Arg $runtimeArgs "-LiveRuntimeCommandFile" $LiveRuntimeCommandFile
if ($NoSound) { $runtimeArgs.Add("-NoSound") }

$RuntimeStdout = Join-Path $RunDir "runtime-proof.stdout.log"
$RuntimeStderr = Join-Path $RunDir "runtime-proof.stderr.log"
$RuntimeCommand = "powershell " + (($runtimeArgs.ToArray() | ForEach-Object { Quote-ProcessArgument $_ }) -join " ")
$runtimeProcess = Start-Process -FilePath "powershell" `
    -ArgumentList ($runtimeArgs.ToArray() | ForEach-Object { Quote-ProcessArgument $_ }) `
    -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $RuntimeStdout `
    -RedirectStandardError $RuntimeStderr

$StudioProcess = $null
$StudioCommand = ""
$StudioStdout = Join-Path $RunDir "live-server.stdout.log"
$StudioStderr = Join-Path $RunDir "live-server.stderr.log"
if (!$SkipStudioCatalog) {
    $StudioCatalogJson = Join-Path $StudioDir "character-studio-catalog.json"
    $StudioHtml = Join-Path $StudioDir "character-studio.html"
    [pscustomobject][ordered]@{
        schema = "nikami-fnv-live-minimal-catalog-v1"
        entries = @()
        policy = [pscustomobject][ordered]@{
            generatedProofOutputsOnly = $true
            noRetailPayloadBytes = $true
        }
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $StudioCatalogJson -Encoding UTF8
    @'
<!doctype html>
<meta charset="utf-8">
<title>FNV Live Runtime</title>
<style>
body{font:14px system-ui;margin:24px;max-width:1100px}
label{display:inline-flex;align-items:center;gap:6px;margin:4px 10px 4px 0}
pre{white-space:pre-wrap;border:1px solid #bbb;padding:12px;min-height:240px}
button,input{font:inherit;margin:4px}
button{min-width:84px}
.panel{border:1px solid #bbb;padding:12px;margin:12px 0}
.snaprow{display:flex;flex-wrap:wrap;align-items:center;gap:6px;margin:6px 0}
.snaprow b{min-width:70px}
.snaprow input[type=text]{width:360px;max-width:100%}
.pill{display:inline-block;border:1px solid #aaa;border-radius:999px;padding:3px 8px;margin:2px}
</style>
<h1>FNV Live Runtime</h1>
<p><a href="/nikami/health">health</a> <a href="/nikami/runtime-status">status</a> <a href="/nikami/runtime-audit">audit</a> <a href="/nikami/live-authoring">authoring</a> <a href="/nikami/live-runtime">runtime</a></p>
<p><label>actor <input id="actor" value="GSSunnySmiles"></label><button onclick="setActor()">set actor</button><button onclick="refresh()">refresh</button></p>
<p><label>yaw <input id="yaw" type="number" step="0.01" value="1.5708"></label><label>rate <input id="rate" type="number" step="0.05" value="1.40"></label><button onclick="applyYaw()">apply yaw</button><button onclick="startSpin()">spin</button><button onclick="stopSpin()">stop</button></p>
<div class="panel">
  <div class="snaprow"><b>part</b><input id="surfacePrefix" type="text" value="OPENMW_FNV_HAIR_MESH_HAIRRAIDERMID"><button onclick="loadAuthoring()">load</button><button onclick="setPrefix('OPENMW_FNV_HAIR_MESH_HAIRRAIDERMID')">raider mid</button><button onclick="setPrefix('OPENMW_FNV_HAIR_MESH_HAIRBUN')">hair bun</button><label><input id="drawAxes" type="checkbox" onchange="setAxes()"> axes</label></div>
  <div class="snaprow"><b>origin</b><button onclick="setOrigin('head')">head origin</button><button onclick="setOrigin('part')">part pivot</button><button onclick="resetOrigin()">reset origin</button><span id="originState" class="pill">origin pending</span></div>
  <div class="snaprow" id="snapX"><b>snap X</b></div>
  <div class="snaprow" id="snapY"><b>snap Y</b></div>
  <div class="snaprow" id="snapZ"><b>snap Z</b></div>
  <div class="snaprow"><b>nudge X</b><button onclick="nudgeOrigin('X',-1)">-1</button><button onclick="nudgeOrigin('X',-0.25)">-.25</button><button onclick="nudgeOrigin('X',0.25)">+.25</button><button onclick="nudgeOrigin('X',1)">+1</button></div>
  <div class="snaprow"><b>nudge Y</b><button onclick="nudgeOrigin('Y',-1)">-1</button><button onclick="nudgeOrigin('Y',-0.25)">-.25</button><button onclick="nudgeOrigin('Y',0.25)">+.25</button><button onclick="nudgeOrigin('Y',1)">+1</button></div>
  <div class="snaprow"><b>nudge Z</b><button onclick="nudgeOrigin('Z',-1)">-1</button><button onclick="nudgeOrigin('Z',-0.25)">-.25</button><button onclick="nudgeOrigin('Z',0.25)">+.25</button><button onclick="nudgeOrigin('Z',1)">+1</button></div>
  <div class="snaprow"><b>snap state</b><span id="snapState" class="pill">not loaded</span></div>
</div>
<pre id="out"></pre>
<script>
let spinTimer=null,spinStart=0,spinBase=1.5708;
let authoring=null;
async function j(u,o){let r=await fetch(u,o);return await r.json()}
    const faceFamilyParts=["body-skin","head-skin","face-organs","hair-beard"];
    function actorPayload(extra){return Object.assign({actorTarget:actor.value,runtimeTarget:actor.value,actorKind:"npc",command:"update-actor-kit",actorKitParts:faceFamilyParts.join(","),actorKitPartModels:"",selectors:{parts:faceFamilyParts,partModels:[],animationSource:"hands-at-side"}},extra||{})}
async function postRuntime(extra){let doc=await j("/nikami/live-runtime",{method:"POST",headers:{"content-type":"application/json"},body:JSON.stringify(actorPayload(extra))});out.textContent=JSON.stringify(doc,null,2);return doc}
async function refresh(){out.textContent=JSON.stringify({health:await j("/nikami/health"),status:await j("/nikami/runtime-status"),audit:await j("/nikami/runtime-audit"),runtime:await j("/nikami/live-runtime")},null,2)}
async function setActor(){await postRuntime({command:"set-actor-target"});setTimeout(refresh,500)}
async function applyYaw(){spinBase=parseFloat(yaw.value)||1.5708;await postRuntime({actorStageRotZ:spinBase});setTimeout(refresh,250)}
function startSpin(){stopSpin();spinBase=parseFloat(yaw.value)||1.5708;spinStart=performance.now();spinTimer=setInterval(async()=>{let z=spinBase+((performance.now()-spinStart)/1000)*(parseFloat(rate.value)||1.4);yaw.value=z.toFixed(4);await postRuntime({actorStageRotZ:z})},120)}
function stopSpin(){if(spinTimer){clearInterval(spinTimer);spinTimer=null}}
function key(suffix){return surfacePrefix.value+"_"+suffix}
function num(suffix){let v=Number(authoring?.controls?.[key(suffix)]);return Number.isFinite(v)?v:0}
function bool(suffix){return authoring?.controls?.[key(suffix)]===true}
function setPrefix(value){surfacePrefix.value=value;loadAuthoring()}
async function loadAuthoring(){authoring=await j("/nikami/live-authoring");drawAxes.checked=authoring.controls?.OPENMW_FNV_DRAW_PART_AXES===true;renderSnapState();out.textContent=JSON.stringify(authoring,null,2)}
async function postAuthoring(controls){authoring=await j("/nikami/live-authoring",{method:"POST",headers:{"content-type":"application/json"},body:JSON.stringify({controls})});renderSnapState();out.textContent=JSON.stringify(authoring.lastApplied||controls,null,2);return authoring}
function renderSnapButtons(axis){let node=document.getElementById("snap"+axis);node.innerHTML="<b>snap "+axis+"</b>"+[-180,-90,0,90,180].map(v=>`<button onclick="snap('${axis}',${v})">${v}</button>`).join("")}
function renderSnapState(){snapState.textContent=`dx ${num("PIVOT_DELTA_ROTATION_X")} / dy ${num("PIVOT_DELTA_ROTATION_Y")} / dz ${num("PIVOT_DELTA_ROTATION_Z")}`;originState.textContent=`${bool("PIVOT_MODE")?"part/custom pivot":"head origin"} ox ${num("PIVOT_OFFSET_X").toFixed(2)} oy ${num("PIVOT_OFFSET_Y").toFixed(2)} oz ${num("PIVOT_OFFSET_Z").toFixed(2)}`}
async function snap(axis,value){let c={};c[key("PIVOT_DELTA_ROTATION_X")]=axis==="X"?value:0;c[key("PIVOT_DELTA_ROTATION_Y")]=axis==="Y"?value:0;c[key("PIVOT_DELTA_ROTATION_Z")]=axis==="Z"?value:0;await postAuthoring(c)}
async function setOrigin(mode){let c={};c[key("PIVOT_MODE")]=mode==="part";c[key("PIVOT_OFFSET_X")]=0;c[key("PIVOT_OFFSET_Y")]=0;c[key("PIVOT_OFFSET_Z")]=0;await postAuthoring(c)}
async function resetOrigin(){await setOrigin("head")}
async function nudgeOrigin(axis,delta){let suffix="PIVOT_OFFSET_"+axis;let c={};c[key("PIVOT_MODE")]=true;c[key(suffix)]=num(suffix)+delta;await postAuthoring(c)}
async function setAxes(){await postAuthoring({OPENMW_FNV_DRAW_PART_AXES:drawAxes.checked})}
renderSnapButtons("X");renderSnapButtons("Y");renderSnapButtons("Z");
refresh();loadAuthoring();
    </script>
'@ |
        Set-Content -LiteralPath $StudioHtml -Encoding UTF8

    $studioArgs = @(
        $LiveServer,
        "--root", $ProofRoot,
        "--repo-root", $RepoRoot,
        "--run-dir", $RunDir,
        "--runner", $ViewerRunner,
        "--catalog-path", $StudioCatalogJson,
        "--live-authoring-path", $LiveAuthoringFile,
        "--live-runtime-path", $LiveRuntimeCommandFile,
        "--host", "127.0.0.1",
        "--port", [string]$ResolvedServePort
    ) | ForEach-Object { Quote-ProcessArgument $_ }
    $StudioCommand = "python " + ($studioArgs -join " ")
    $StudioProcess = Start-Process -FilePath "python" `
        -ArgumentList $studioArgs `
        -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $StudioStdout `
        -RedirectStandardError $StudioStderr
    [pscustomobject][ordered]@{
        url = $StudioUrl
        health = "http://127.0.0.1:$ResolvedServePort/nikami/health"
        processId = $StudioProcess.Id
        stdout = $StudioStdout
        stderr = $StudioStderr
        liveAuthoringFile = $LiveAuthoringFile
        liveRuntimeCommandFile = $LiveRuntimeCommandFile
        policy = [pscustomobject][ordered]@{
            loopbackOnly = $true
            generatedProofOutputsOnly = $true
            noRetailAssetsCommitted = $true
        }
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $StudioServerJson -Encoding UTF8
    $StudioUrl | Set-Content -LiteralPath $StudioUrlFile -Encoding UTF8
}

$manifest = [pscustomobject][ordered]@{
    schema = "nikami-fnv-live-character-authoring-run-v1"
    createdAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    runDir = $RunDir
    studioUrl = $StudioUrl
    studioServerJson = $StudioServerJson
    studioProcessId = if ($null -ne $StudioProcess) { $StudioProcess.Id } else { 0 }
    studioStdout = $StudioStdout
    studioStderr = $StudioStderr
    studioCommand = $StudioCommand
    liveAuthoringFile = $LiveAuthoringFile
    liveRuntimeCommandFile = $LiveRuntimeCommandFile
    runtimeProcessId = $runtimeProcess.Id
    runtimeStdout = $RuntimeStdout
    runtimeStderr = $RuntimeStderr
    runtimeCommand = $RuntimeCommand
    runtimeTag = $RuntimeTag
    screenshotFrames = $ScreenshotFrames
    actorKitAnimationSource = $InitialActorKitAnimationSource
    target = [pscustomobject][ordered]@{
        actorTarget = $ActorTarget
        actorKind = $ActorKind
    }
    policy = [pscustomobject][ordered]@{
        generatedProofOutputsOnly = $true
        noRetailAssetsCommitted = $true
        isolatedRuntimeConfig = $true
        liveNumericControlsOnly = $true
        activeCellActorSwitchOnly = $true
        baseNpcPreviewWhenInactive = $true
        baseCreaturePreviewWhenInactive = $true
        baseActorPreviewWhenInactive = $true
        pcFlatFirst = $true
    }
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8

Write-Host "Live character authoring run: $RunDir"
Write-Host "Studio URL: $StudioUrl"
if ($null -ne $StudioProcess) { Write-Host "Studio startup PID: $($StudioProcess.Id)" }
Write-Host "Live authoring file: $LiveAuthoringFile"
Write-Host "Live runtime command file: $LiveRuntimeCommandFile"
Write-Host "Runtime proof PID: $($runtimeProcess.Id)"
Write-Host "Run manifest: $ManifestPath"
Write-Host "Policy: generated proof/control output only; no retail assets are committed"

if ($OpenStudio -and ![string]::IsNullOrWhiteSpace($StudioUrl)) {
    Start-Process -FilePath $StudioUrl | Out-Null
}
