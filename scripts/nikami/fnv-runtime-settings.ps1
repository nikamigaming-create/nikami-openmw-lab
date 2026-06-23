$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

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
