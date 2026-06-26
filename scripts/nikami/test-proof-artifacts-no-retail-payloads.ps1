param(
    [string]$ProofRoot = "",
    [string]$RepoRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
}
else {
    $RepoRoot = (Resolve-Path $RepoRoot).Path
}

if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
$ProofRoot = (Resolve-Path $ProofRoot).Path

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "proof-artifact-payload-safety/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

$PayloadExtensions = @(
    ".bsa", ".esm", ".esp", ".ini",
    ".egt", ".egm", ".tri", ".lip", ".ogg", ".wav", ".mp3", ".dds", ".nif", ".kf",
    ".psd", ".tai", ".ctl", ".dat", ".spt", ".dlodsettings", ".psa"
)

$RawByteTextPatterns = @(
    '"hex32"\s*:',
    '"rawBytes"\s*:',
    '"sampleBytes"\s*:',
    '"firstBytes"\s*:',
    '"base64"\s*:',
    '"contentBase64"\s*:',
    '"bytesBase64"\s*:'
)

$ContentLedgerTextPayloadPatterns = @(
    '"source"\s*:',
    '"questName"\s*:',
    '"topicName"\s*:',
    '"response"\s*:',
    '"notes"\s*:',
    '"edits"\s*:',
    '"text"\s*:',
    '"fullName"\s*:',
    '"value"\s*:\s*"'
)

function Find-FirstPatternMatch([string]$Path, [string[]]$Patterns) {
    $regexes = @($Patterns | ForEach-Object {
        [System.Text.RegularExpressions.Regex]::new(
            $_,
            [System.Text.RegularExpressions.RegexOptions]::Compiled -bor
                [System.Text.RegularExpressions.RegexOptions]::CultureInvariant
        )
    })
    $stream = [System.IO.FileStream]::new(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
    )
    $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8, $true, 1048576)
    $buffer = New-Object char[] 1048576
    $tail = ""
    $charOffset = 0L
    try {
        while (($count = $reader.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $chunk = $tail + [string]::new($buffer, 0, $count)
            for ($i = 0; $i -lt $regexes.Count; ++$i) {
                if ($regexes[$i].IsMatch($chunk)) {
                    return "pattern=$($Patterns[$i]) charOffset~$charOffset"
                }
            }
            if ($chunk.Length -gt 512) {
                $tail = $chunk.Substring($chunk.Length - 512)
            }
            else {
                $tail = $chunk
            }
            $charOffset += $count
        }
        return $null
    }
    finally {
        $reader.Dispose()
    }
}

function Invoke-RipgrepPatternScan([string]$Root, [string[]]$Patterns, [string[]]$Globs) {
    $rg = Get-Command "rg" -ErrorAction SilentlyContinue
    if ($null -eq $rg) {
        return $null
    }

    $matches = @()
    foreach ($pattern in $Patterns) {
        $literal = $pattern
        $match = [regex]::Match($pattern, '"[^"]+"')
        if ($match.Success) {
            $literal = $match.Value
        }

        $args = @(
            "--no-heading",
            "--line-number",
            "--hidden",
            "--no-ignore",
            "--color", "never",
            "--fixed-strings"
        )
        foreach ($glob in $Globs) {
            $args += "--glob"
            $args += $glob
        }
        $args += $literal
        $args += $Root

        $output = & $rg.Source @args 2>&1
        $exit = $LASTEXITCODE
        if ($exit -eq 0) {
            $matches += @($output | ForEach-Object { $_.ToString() })
            continue
        }
        if ($exit -eq 1) {
            continue
        }

        $outputText = $output -join [Environment]::NewLine
        if ($outputText -match "os error 32|being used by another process") {
            return $null
        }

        throw "rg proof artifact scan failed with exit code $exit`: $outputText"
    }

    return @($matches)
}

function Write-FailureLines([string]$Prefix, [object[]]$Items, [int]$Limit = 200) {
    $count = 0
    foreach ($item in $Items) {
        if ($count -ge $Limit) { break }
        Write-ProofLine "$Prefix$item"
        ++$count
    }
    if ($Items.Count -gt $Limit) {
        Write-ProofLine "$Prefix... truncated $($Items.Count - $Limit) additional match(es)"
    }
}

Write-ProofLine "Proof artifact payload safety $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofRoot: $ProofRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

$allFiles = @(Get-ChildItem -LiteralPath $ProofRoot -Recurse -File -Force)
$allDirectories = @(Get-ChildItem -LiteralPath $ProofRoot -Recurse -Directory -Force)

$payloadFiles = @()
foreach ($file in $allFiles) {
    if ($PayloadExtensions -contains $file.Extension.ToLowerInvariant()) {
        $payloadFiles += $file.FullName
    }
}

$tempExtractDirs = @($allDirectories |
    Where-Object { $_.Name -ieq "temp-extract" } |
    Select-Object -ExpandProperty FullName)

$TextGlobs = @("*.json", "*.jsonl", "*.txt", "*.log", "*.csv", "*.md")
$rawByteMatches = @(Invoke-RipgrepPatternScan $ProofRoot $RawByteTextPatterns $TextGlobs)
if ($null -eq $rawByteMatches) {
    $rawByteMatches = @()
    $textFiles = $allFiles |
        Where-Object { @(".json", ".jsonl", ".txt", ".log", ".csv", ".md") -contains $_.Extension.ToLowerInvariant() }
    foreach ($file in $textFiles) {
        $match = Find-FirstPatternMatch $file.FullName $RawByteTextPatterns
        if ($null -ne $match) {
            $rawByteMatches += "$($file.FullName) $match"
        }
    }
}

$contentLedgerTextMatches = @()
$contentLedgerRoot = Join-Path $ProofRoot "fnv-content-ledger"
if (Test-Path -LiteralPath $contentLedgerRoot -PathType Container) {
    $contentLedgerFiles = $allFiles |
        Where-Object { $_.FullName.StartsWith($contentLedgerRoot, [System.StringComparison]::OrdinalIgnoreCase) -and $_.Extension.ToLowerInvariant() -eq ".json" }
    foreach ($file in $contentLedgerFiles) {
        $match = Find-FirstPatternMatch $file.FullName $ContentLedgerTextPayloadPatterns
        if ($null -ne $match) {
            $contentLedgerTextMatches += "$($file.FullName) $match"
        }
    }
}

Write-ProofLine "Payload extension files: $($payloadFiles.Count)"
Write-ProofLine "Temp extract dirs: $($tempExtractDirs.Count)"
Write-ProofLine "Raw byte text matches: $($rawByteMatches.Count)"
Write-ProofLine "Content ledger text payload matches: $($contentLedgerTextMatches.Count)"

if ($payloadFiles.Count -gt 0) {
    Write-FailureLines "FAIL payload file: " @($payloadFiles)
}
if ($tempExtractDirs.Count -gt 0) {
    Write-FailureLines "FAIL temp extract dir: " @($tempExtractDirs)
}
if ($rawByteMatches.Count -gt 0) {
    Write-FailureLines "FAIL raw byte text: " @($rawByteMatches)
}
if ($contentLedgerTextMatches.Count -gt 0) {
    Write-FailureLines "FAIL content ledger text payload: " @($contentLedgerTextMatches)
}

if ($payloadFiles.Count -gt 0 -or $tempExtractDirs.Count -gt 0 -or $rawByteMatches.Count -gt 0 -or $contentLedgerTextMatches.Count -gt 0) {
    throw "Proof artifact payload safety failed. See $SummaryFile."
}

Write-ProofLine ""
Write-ProofLine "Proof artifact payload safety PASS"
Write-ProofLine "ProofDir: $ProofDir"
