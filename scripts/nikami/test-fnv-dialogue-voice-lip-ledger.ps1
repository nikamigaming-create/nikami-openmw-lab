param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$HarvestDir = "",
    [string]$ProofRoot = "",
    [string[]]$Content = @(
        "FalloutNV.esm",
        "DeadMoney.esm",
        "HonestHearts.esm",
        "OldWorldBlues.esm",
        "LonesomeRoad.esm",
        "GunRunnersArsenal.esm",
        "CaravanPack.esm",
        "ClassicPack.esm",
        "MercenaryPack.esm",
        "TribalPack.esm"
    ),
    [switch]$RequireAnyLipSidecar,
    [switch]$RequireAllResolvedVoiceLips
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
$ProofDir = Join-Path $ProofRoot "fnv-dialogue-voice-lip-ledger/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Get-U16([byte[]]$Data, [int64]$Offset) {
    [BitConverter]::ToUInt16($Data, [int]$Offset)
}

function Get-U32([byte[]]$Data, [int64]$Offset) {
    [BitConverter]::ToUInt32($Data, [int]$Offset)
}

function Get-FourCC([byte[]]$Data, [int64]$Offset) {
    [Text.Encoding]::ASCII.GetString($Data, [int]$Offset, 4)
}

function Get-HexForm([uint32]$Value) {
    "0x{0:x8}" -f $Value
}

function Get-ZString([byte[]]$Data) {
    if ($null -eq $Data -or $Data.Length -eq 0) {
        return ""
    }
    $end = [Array]::IndexOf($Data, [byte]0)
    if ($end -lt 0) {
        $end = $Data.Length
    }
    if ($end -eq 0) {
        return ""
    }
    [Text.Encoding]::GetEncoding(1252).GetString($Data, 0, $end)
}

function Copy-Bytes([byte[]]$Data, [int64]$Offset, [int64]$Count) {
    $bytes = [byte[]]::new([int]$Count)
    [Array]::Copy($Data, [int]$Offset, $bytes, 0, [int]$Count)
    $bytes
}

function Read-Subrecords([byte[]]$Payload) {
    $items = [System.Collections.Generic.List[object]]::new()
    $offset = 0
    $extendedSize = $null
    while ($offset + 6 -le $Payload.Length) {
        $type = Get-FourCC $Payload $offset
        $size = [int](Get-U16 $Payload ($offset + 4))
        $offset += 6

        if ($type -eq "XXXX") {
            if ($offset + 4 -gt $Payload.Length) {
                break
            }
            $extendedSize = [int](Get-U32 $Payload $offset)
            $offset += $size
            continue
        }

        if ($null -ne $extendedSize) {
            $size = $extendedSize
            $extendedSize = $null
        }
        if ($offset + $size -gt $Payload.Length) {
            break
        }
        $items.Add([pscustomobject]@{
            type = $type
            data = Copy-Bytes $Payload $offset $size
        })
        $offset += $size
    }
    $items
}

function Get-FirstSubrecord([object[]]$Subrecords, [string]$Type) {
    $Subrecords | Where-Object { $_.type -eq $Type } | Select-Object -First 1
}

function Get-FirstZString([object[]]$Subrecords, [string]$Type) {
    $subrecord = Get-FirstSubrecord $Subrecords $Type
    if ($null -eq $subrecord) {
        return ""
    }
    Get-ZString $subrecord.data
}

function Normalize-ArchivePath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }
    $Path.Replace("/", "\").TrimStart("\").ToLowerInvariant()
}

function Normalize-SoundArchivePath([string]$Path) {
    $normalized = Normalize-ArchivePath $Path
    if ([string]::IsNullOrWhiteSpace($normalized)) {
        return ""
    }
    if ($normalized.StartsWith("sound\")) {
        return $normalized
    }
    "sound\$normalized"
}

function Set-ArchivePathExtension([string]$Path, [string]$Extension) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }
    if ($Path -match "\.[^\\\.]+$") {
        return [Regex]::Replace($Path, "\.[^\\\.]+$", $Extension)
    }
    "$Path$Extension"
}

function Get-AudioPathCandidates([string]$Path) {
    $normalized = Normalize-SoundArchivePath $Path
    $candidates = [System.Collections.Generic.List[string]]::new()
    if (![string]::IsNullOrWhiteSpace($normalized)) {
        $candidates.Add($normalized)
        foreach ($ext in @(".ogg", ".wav", ".mp3")) {
            $candidate = Set-ArchivePathExtension $normalized $ext
            if (!$candidates.Contains($candidate)) {
                $candidates.Add($candidate)
            }
        }
    }
    $candidates
}

function Get-AdjustedFormId([uint32]$Raw, [string[]]$Masters, [string]$Plugin, [hashtable]$ContentIndex) {
    if ($Raw -eq 0) {
        return 0
    }

    $localFile = [int](($Raw -shr 24) -band 0xff)
    $index = $Raw -band 0x00ffffff
    $source = $Plugin
    if ($localFile -lt $Masters.Count) {
        $source = $Masters[$localFile]
    }

    $globalFile = $localFile
    $key = $source.ToLowerInvariant()
    if ($ContentIndex.ContainsKey($key)) {
        $globalFile = [int]$ContentIndex[$key]
    }

    [uint32](($globalFile -shl 24) -bor $index)
}

function Read-PluginVoiceData([string]$Path, [string]$Plugin, [hashtable]$ContentIndex) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $masters = [System.Collections.Generic.List[string]]::new()
    $sounds = [System.Collections.Generic.List[object]]::new()
    $soundRefs = [System.Collections.Generic.List[object]]::new()
    $infos = [System.Collections.Generic.List[object]]::new()

    function Read-Range([int64]$Offset, [int64]$End) {
        while ($Offset + 24 -le $End) {
            $recordStart = $Offset
            $type = Get-FourCC $bytes $Offset
            $size = [int64](Get-U32 $bytes ($Offset + 4))
            [void](Get-U32 $bytes ($Offset + 8))
            $rawFormId = Get-U32 $bytes ($Offset + 12)
            $Offset += 24

            if ($type -eq "GRUP") {
                $groupEnd = $recordStart + $size
                if ($groupEnd -le $recordStart -or $groupEnd -gt $End) {
                    throw "Invalid GRUP range in ${Path}: start=$recordStart size=$size end=$groupEnd limit=$End"
                }
                $Offset = Read-Range $Offset $groupEnd
                continue
            }

            $nextOffset = $Offset + $size
            if ($nextOffset -lt $Offset -or $nextOffset -gt $End) {
                throw "Invalid ESM4 record range in ${Path}: type=$type start=$recordStart size=$size next=$nextOffset limit=$End"
            }

            if ($type -in @("TES4", "SOUN", "SNDR", "INFO")) {
                $payload = Copy-Bytes $bytes $Offset $size
                $subrecords = @(Read-Subrecords $payload)
                if ($type -eq "TES4") {
                    foreach ($subrecord in $subrecords) {
                        if ($subrecord.type -eq "MAST") {
                            $master = Get-ZString $subrecord.data
                            if (![string]::IsNullOrWhiteSpace($master)) {
                                $masters.Add($master)
                            }
                        }
                    }
                }
                elseif ($type -eq "SOUN") {
                    $sounds.Add([pscustomobject]@{
                        plugin = $Plugin
                        formId = Get-HexForm (Get-AdjustedFormId $rawFormId $masters.ToArray() $Plugin $ContentIndex)
                        editorId = Get-FirstZString $subrecords "EDID"
                        soundFile = Get-FirstZString $subrecords "FNAM"
                    })
                }
                elseif ($type -eq "SNDR") {
                    $soundId = 0
                    $soundSubrecord = Get-FirstSubrecord $subrecords "SNAM"
                    if ($null -ne $soundSubrecord -and $soundSubrecord.data.Length -ge 4) {
                        $soundId = Get-AdjustedFormId (Get-U32 $soundSubrecord.data 0) $masters.ToArray() $Plugin $ContentIndex
                    }
                    $soundRefs.Add([pscustomobject]@{
                        plugin = $Plugin
                        formId = Get-HexForm (Get-AdjustedFormId $rawFormId $masters.ToArray() $Plugin $ContentIndex)
                        editorId = Get-FirstZString $subrecords "EDID"
                        soundId = if ($soundId -eq 0) { "" } else { Get-HexForm $soundId }
                        soundFile = Get-FirstZString $subrecords "ANAM"
                    })
                }
                elseif ($type -eq "INFO") {
                    $sound = 0
                    $soundSource = ""
                    $quest = 0
                    $responseLength = 0
                    foreach ($subrecord in $subrecords) {
                        if ($subrecord.type -eq "QSTI" -and $subrecord.data.Length -ge 4) {
                            $quest = Get-AdjustedFormId (Get-U32 $subrecord.data 0) $masters.ToArray() $Plugin $ContentIndex
                        }
                        elseif ($subrecord.type -eq "SNDD" -and $subrecord.data.Length -ge 4) {
                            $sound = Get-AdjustedFormId (Get-U32 $subrecord.data 0) $masters.ToArray() $Plugin $ContentIndex
                            $soundSource = "SNDD"
                        }
                        elseif ($subrecord.type -eq "TRDT" -and $subrecord.data.Length -ge 20) {
                            $trdtSound = Get-U32 $subrecord.data 16
                            if ($trdtSound -ne 0) {
                                $sound = Get-AdjustedFormId $trdtSound $masters.ToArray() $Plugin $ContentIndex
                                $soundSource = "TRDT"
                            }
                        }
                        elseif ($subrecord.type -eq "SNAM" -and $subrecord.data.Length -ge 4 -and $sound -eq 0) {
                            $sound = Get-AdjustedFormId (Get-U32 $subrecord.data 0) $masters.ToArray() $Plugin $ContentIndex
                            $soundSource = "SNAM"
                        }
                        elseif ($subrecord.type -eq "NAM1") {
                            $responseLength = (Get-ZString $subrecord.data).Length
                        }
                    }

                    $infos.Add([pscustomobject]@{
                        plugin = $Plugin
                        formId = Get-HexForm (Get-AdjustedFormId $rawFormId $masters.ToArray() $Plugin $ContentIndex)
                        rawFormId = Get-HexForm $rawFormId
                        quest = if ($quest -eq 0) { "" } else { Get-HexForm $quest }
                        sound = if ($sound -eq 0) { "" } else { Get-HexForm $sound }
                        soundSource = $soundSource
                        responseLength = $responseLength
                    })
                }
            }

            $Offset = $nextOffset
        }
        return $Offset
    }

    [void](Read-Range 0 $bytes.Length)
    [pscustomobject]@{
        plugin = $Plugin
        masters = @($masters)
        sounds = @($sounds)
        soundRefs = @($soundRefs)
        infos = @($infos)
    }
}

function Resolve-SoundFile([string]$FormId, [hashtable]$Sounds, [hashtable]$SoundRefs, [int]$Depth = 0) {
    if ([string]::IsNullOrWhiteSpace($FormId) -or $Depth -ge 8) {
        return $null
    }
    if ($SoundRefs.ContainsKey($FormId)) {
        $ref = $SoundRefs[$FormId]
        if (![string]::IsNullOrWhiteSpace($ref.soundFile)) {
            return [pscustomobject]@{
                file = $ref.soundFile
                sourceRecordType = "SNDR"
                sourceFormId = $FormId
            }
        }
        if (![string]::IsNullOrWhiteSpace($ref.soundId)) {
            return Resolve-SoundFile $ref.soundId $Sounds $SoundRefs ($Depth + 1)
        }
    }
    if ($Sounds.ContainsKey($FormId)) {
        $sound = $Sounds[$FormId]
        if (![string]::IsNullOrWhiteSpace($sound.soundFile)) {
            return [pscustomobject]@{
                file = $sound.soundFile
                sourceRecordType = "SOUN"
                sourceFormId = $FormId
            }
        }
    }
    return $null
}

function Read-ArchiveEntrySet([string]$HarvestDir) {
    $ledgerPath = Join-Path $HarvestDir "archive-entry-ledger.json"
    if (!(Test-Path -LiteralPath $ledgerPath -PathType Leaf)) {
        throw "Missing archive entry ledger: $ledgerPath"
    }
    $entries = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $ledger = Get-Content -LiteralPath $ledgerPath -Raw | ConvertFrom-Json
    foreach ($archive in $ledger) {
        $listPath = Join-Path $HarvestDir $archive.entryList
        if (!(Test-Path -LiteralPath $listPath -PathType Leaf)) {
            throw "Missing BSA entry list: $listPath"
        }
        foreach ($entry in Get-Content -LiteralPath $listPath) {
            [void]$entries.Add((Normalize-ArchivePath $entry))
        }
    }
    $entries
}

function Add-IndexedPath([hashtable]$Index, [string]$Key, [string]$Path) {
    if (!$Index.ContainsKey($Key)) {
        $Index[$Key] = [System.Collections.Generic.List[string]]::new()
    }
    $Index[$Key].Add($Path)
}

function New-VoiceSidecarIndexes([System.Collections.Generic.HashSet[string]]$ArchiveEntries) {
    $lipByInfo = @{}
    $audioByInfo = @{}

    foreach ($entry in $ArchiveEntries) {
        if ($entry -notmatch '^sound\\voice\\([^\\]+)\\.*_([0-9a-f]{8})_[0-9]+\.(lip|ogg|wav|mp3)$') {
            continue
        }

        $plugin = $Matches[1].ToLowerInvariant()
        $form = "0x$($Matches[2].ToLowerInvariant())"
        $extension = $Matches[3].ToLowerInvariant()
        $key = "$plugin|$form"

        if ($extension -eq "lip") {
            Add-IndexedPath $lipByInfo $key $entry
        }
        else {
            Add-IndexedPath $audioByInfo $key $entry
        }
    }

    [pscustomobject]@{
        lipByInfo = $lipByInfo
        audioByInfo = $audioByInfo
    }
}

Write-ProofLine "FNV dialogue voice/LIP ledger $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "HarvestDir: $HarvestDir"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

$contentIndex = @{}
for ($i = 0; $i -lt $Content.Count; ++$i) {
    $contentIndex[$Content[$i].ToLowerInvariant()] = $i
}

$pluginData = [System.Collections.Generic.List[object]]::new()
foreach ($contentName in $Content) {
    $path = Join-Path $FnvData $contentName
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing content file: $path"
    }
    Write-ProofLine "Parsing dialogue voice metadata: $contentName"
    $pluginData.Add((Read-PluginVoiceData $path $contentName $contentIndex))
}

$sounds = @{}
$soundRefs = @{}
$infos = [System.Collections.Generic.List[object]]::new()
foreach ($plugin in $pluginData) {
    foreach ($sound in $plugin.sounds) {
        $sounds[$sound.formId] = $sound
    }
    foreach ($soundRef in $plugin.soundRefs) {
        $soundRefs[$soundRef.formId] = $soundRef
    }
    foreach ($info in $plugin.infos) {
        $infos.Add($info)
    }
}

Write-ProofLine "Loading harvested BSA entry names"
$archiveEntries = Read-ArchiveEntrySet $HarvestDir
$voiceSidecarIndexes = New-VoiceSidecarIndexes $archiveEntries

$voiceRows = [System.Collections.Generic.List[object]]::new()
$voiceSidecarRows = [System.Collections.Generic.List[object]]::new()
$infoWithSound = 0
$infoWithResponse = 0
$resolvedInfoSounds = 0
$resolvedVoiceInfos = 0
$audioArchiveMatches = 0
$lipArchiveMatches = 0
foreach ($info in $infos) {
    if ($info.responseLength -gt 0) {
        ++$infoWithResponse
        $key = "$($info.plugin.ToLowerInvariant())|$($info.rawFormId.ToLowerInvariant())"
        if ($voiceSidecarIndexes.lipByInfo.ContainsKey($key)) {
            $audioPaths = @()
            if ($voiceSidecarIndexes.audioByInfo.ContainsKey($key)) {
                $audioPaths = @($voiceSidecarIndexes.audioByInfo[$key])
            }
            $voiceSidecarRows.Add([pscustomobject]@{
                plugin = $info.plugin
                infoFormId = $info.formId
                rawInfoFormId = $info.rawFormId
                quest = $info.quest
                responseLength = $info.responseLength
                audioPaths = $audioPaths
                lipPaths = @($voiceSidecarIndexes.lipByInfo[$key])
            })
        }
    }

    if ([string]::IsNullOrWhiteSpace($info.sound)) {
        continue
    }
    ++$infoWithSound
    $resolved = Resolve-SoundFile $info.sound $sounds $soundRefs
    if ($null -eq $resolved) {
        continue
    }
    ++$resolvedInfoSounds

    $audioCandidates = @(Get-AudioPathCandidates $resolved.file)
    $audioMatches = @($audioCandidates | Where-Object { $archiveEntries.Contains($_) })
    $audioPath = if ($audioMatches.Count -gt 0) { $audioMatches[0] } else { $audioCandidates[0] }
    $lipPath = Set-ArchivePathExtension $audioPath ".lip"
    $isVoice = $audioPath.StartsWith("sound\voice\")
    if ($isVoice) {
        ++$resolvedVoiceInfos
    }
    $audioExists = $audioMatches.Count -gt 0
    $lipExists = $archiveEntries.Contains($lipPath)
    if ($audioExists) {
        ++$audioArchiveMatches
    }
    if ($lipExists) {
        ++$lipArchiveMatches
    }

    if ($isVoice) {
        $voiceRows.Add([pscustomobject]@{
            plugin = $info.plugin
            infoFormId = $info.formId
            quest = $info.quest
            soundFormId = $info.sound
            soundSource = $info.soundSource
            soundRecordType = $resolved.sourceRecordType
            rawSoundFile = $resolved.file
            audioPath = $audioPath
            audioArchiveExists = $audioExists
            expectedLipPath = $lipPath
            lipArchiveExists = $lipExists
            responseLength = $info.responseLength
        })
    }
}

$missingLipRows = @($voiceRows | Where-Object { !$_.lipArchiveExists })
$missingAudioRows = @($voiceRows | Where-Object { !$_.audioArchiveExists })
$voiceSidecarAudioMatches = @($voiceSidecarRows | Where-Object { $_.audioPaths.Count -gt 0 }).Count
$missingResponseLipSidecars = $infoWithResponse - $voiceSidecarRows.Count

$summary = [pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    harvestDir = $HarvestDir
    content = $Content
    counts = [pscustomobject]@{
        sounds = $sounds.Count
        soundReferences = $soundRefs.Count
        infos = $infos.Count
        infosWithResponse = $infoWithResponse
        infosWithSound = $infoWithSound
        resolvedInfoSounds = $resolvedInfoSounds
        resolvedVoiceInfos = $resolvedVoiceInfos
        voiceAudioArchiveMatches = $audioArchiveMatches
        voiceLipArchiveMatches = $lipArchiveMatches
        infoFormIdVoiceAudioSidecars = $voiceSidecarAudioMatches
        infoFormIdVoiceLipSidecars = $voiceSidecarRows.Count
        infosWithResponseMissingLipSidecars = $missingResponseLipSidecars
        missingVoiceAudio = $missingAudioRows.Count
        missingVoiceLipSidecars = $missingLipRows.Count
    }
    voiceLipRows = $voiceRows
    infoFormIdVoiceSidecars = $voiceSidecarRows
    missingVoiceAudio = $missingAudioRows
    missingVoiceLipSidecars = $missingLipRows
}

$ledgerPath = Join-Path $ProofDir "dialogue-voice-lip-ledger.json"
$summary | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $ledgerPath -Encoding UTF8

Write-ProofLine "SOUN records: $($sounds.Count)"
Write-ProofLine "SNDR records: $($soundRefs.Count)"
Write-ProofLine "INFO records: $($infos.Count)"
Write-ProofLine "INFO responses with text: $infoWithResponse"
Write-ProofLine "INFO sound refs: $infoWithSound"
Write-ProofLine "Resolved INFO sounds: $resolvedInfoSounds"
Write-ProofLine "Resolved voice INFO sounds: $resolvedVoiceInfos"
Write-ProofLine "Voice audio archive matches: $audioArchiveMatches"
Write-ProofLine "Voice LIP sidecar matches: $lipArchiveMatches"
Write-ProofLine "INFO FormId voice audio sidecars: $voiceSidecarAudioMatches"
Write-ProofLine "INFO FormId voice LIP sidecars: $($voiceSidecarRows.Count)"
Write-ProofLine "INFO responses missing FormId LIP sidecars: $missingResponseLipSidecars"
Write-ProofLine "Missing voice audio: $($missingAudioRows.Count)"
Write-ProofLine "Missing voice LIP sidecars: $($missingLipRows.Count)"
Write-ProofLine "Ledger JSON: $ledgerPath"

if ($RequireAnyLipSidecar -and $voiceSidecarRows.Count -le 0) {
    throw "No resolved dialogue voice had a matching .lip sidecar. See $SummaryFile."
}
if ($RequireAllResolvedVoiceLips -and $missingLipRows.Count -gt 0) {
    throw "Resolved dialogue voices are missing $($missingLipRows.Count) .lip sidecar(s). See $SummaryFile."
}

Write-ProofLine ""
Write-ProofLine "FNV dialogue voice/LIP ledger PASS"
Write-ProofLine "ProofDir: $ProofDir"
