$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$script:NikamiRuntimeSettingsRoot = Split-Path -Parent $PSCommandPath

function Get-NikamiFnvRootFromData {
    param([Parameter(Mandatory = $true)][string]$FnvData)

    $path = $FnvData
    $resolved = Resolve-Path -LiteralPath $FnvData -ErrorAction SilentlyContinue
    if ($null -ne $resolved) {
        $path = $resolved.Path
    }

    if ((Split-Path -Leaf $path) -ieq "Data") {
        return (Split-Path -Parent $path)
    }

    return $path
}

function Get-NikamiFnvIniNumericSetting {
    param(
        [Parameter(Mandatory = $true)][string]$FnvRoot,
        [Parameter(Mandatory = $true)][string]$SettingName,
        [string[]]$IniFiles = @("Fallout_default.ini", "VeryHigh.ini", "high.ini", "medium.ini", "low.ini")
    )

    $escapedName = [Regex]::Escape($SettingName)
    $pattern = "^\s*$escapedName\s*=\s*([+-]?\d+(?:\.\d+)?)\s*$"
    foreach ($iniName in $IniFiles) {
        $iniPath = Join-Path $FnvRoot $iniName
        if (!(Test-Path -LiteralPath $iniPath -PathType Leaf)) {
            continue
        }

        $match = Select-String -LiteralPath $iniPath -Pattern $pattern -CaseSensitive:$false | Select-Object -First 1
        if ($null -eq $match) {
            continue
        }

        $value = [double]::Parse(
            $match.Matches[0].Groups[1].Value,
            [System.Globalization.CultureInfo]::InvariantCulture)
        return [pscustomobject]@{
            value = $value
            source = $iniPath
            setting = $SettingName
        }
    }

    throw "Missing FNV INI setting '$SettingName' under $FnvRoot"
}

function Get-NikamiFnvViewingDistance {
    param([Parameter(Mandatory = $true)][string]$FnvData)

    $fnvRoot = Get-NikamiFnvRootFromData -FnvData $FnvData
    $setting = Get-NikamiFnvIniNumericSetting -FnvRoot $fnvRoot -SettingName "fBlockLoadDistance"
    if ($setting.value -le 0) {
        throw "Invalid FNV fBlockLoadDistance from $($setting.source): $($setting.value)"
    }

    return [int][Math]::Round($setting.value)
}

function Get-NikamiFnvWeatherFallbacks {
    param(
        [Parameter(Mandatory = $true)][string]$FnvData,
        [Parameter(Mandatory = $true)][string]$ProofRoot,
        [string]$Stamp = ""
    )

    if ([string]::IsNullOrWhiteSpace($Stamp)) {
        $Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    }

    $generator = Join-Path $script:NikamiRuntimeSettingsRoot "fnv_weather_fallbacks.py"
    if (!(Test-Path -LiteralPath $generator -PathType Leaf)) {
        throw "Missing FNV weather fallback generator: $generator"
    }

    $proofDir = Join-Path $ProofRoot "fnv-weather-fallbacks/$Stamp"
    New-Item -ItemType Directory -Force -Path $proofDir | Out-Null
    $linesPath = Join-Path $proofDir "fallbacks.cfg"
    $jsonPath = Join-Path $proofDir "fnv-weather-fallbacks.json"

    & python $generator --fnv-data $FnvData --output-lines $linesPath --output-json $jsonPath | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "FNV weather fallback generator failed with exit code $LASTEXITCODE."
    }
    if (!(Test-Path -LiteralPath $linesPath -PathType Leaf)) {
        throw "FNV weather fallback generator did not produce $linesPath"
    }
    if (!(Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
        throw "FNV weather fallback generator did not produce $jsonPath"
    }

    $lines = @(Get-Content -LiteralPath $linesPath | Where-Object { ![string]::IsNullOrWhiteSpace($_) })
    if ($lines.Count -lt 100) {
        throw "FNV weather fallback generator produced too few fallback lines: $($lines.Count)"
    }

    return [pscustomobject]@{
        Lines = $lines
        LinesPath = $linesPath
        JsonPath = $jsonPath
        ProofDir = $proofDir
    }
}
