param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$VsDevCmd = $env:NIKAMI_VSDEVCMD,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [int]$Parallel = 8,
    [switch]$BuildOpenMwVr,
    [switch]$SkipConfigure
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$BuildPath = Join-Path $RepoRoot $BuildDir

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    throw "Set NIKAMI_VCPKG_ROOT or pass -VcpkgRoot."
}

$Toolchain = Join-Path $VcpkgRoot "scripts/buildsystems/vcpkg.cmake"
$LuaJitInclude = Join-Path $VcpkgRoot "installed/$Triplet/include/luajit"
$LuaJitLibrary = if ($Configuration -eq "Debug") {
    Join-Path $VcpkgRoot "installed/$Triplet/debug/lib/lua51.lib"
} else {
    Join-Path $VcpkgRoot "installed/$Triplet/lib/lua51.lib"
}

function Invoke-CheckedNative {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

function Import-VisualStudioEnvironment {
    if (Get-Command "cl.exe" -ErrorAction SilentlyContinue) {
        return
    }

    $Candidates = @()
    if (![string]::IsNullOrWhiteSpace($VsDevCmd)) {
        $Candidates += $VsDevCmd
    }

    $Candidates += @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat"
    )

    $Script:ResolvedVsDevCmd = $Candidates | Where-Object {
        ![string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_)
    } | Select-Object -First 1

    if ([string]::IsNullOrWhiteSpace($Script:ResolvedVsDevCmd)) {
        throw "Could not find cl.exe or VsDevCmd.bat. Pass -VsDevCmd or set NIKAMI_VSDEVCMD."
    }

    $EnvironmentLines = cmd /d /s /c "`"$Script:ResolvedVsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "VsDevCmd failed with exit code $LASTEXITCODE"
    }

    foreach ($Line in $EnvironmentLines) {
        $Equals = $Line.IndexOf("=")
        if ($Equals -gt 0) {
            $Name = $Line.Substring(0, $Equals)
            $Value = $Line.Substring($Equals + 1)
            Set-Item -Path "Env:$Name" -Value $Value
        }
    }
}

function Copy-RequiredItem {
    param(
        [string]$Source,
        [string]$Destination,
        [switch]$Recurse
    )

    try {
        if ($Recurse) {
            Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
        } else {
            Copy-Item -LiteralPath $Source -Destination $Destination -Force
        }
    } catch {
        $Existing = Join-Path $Destination (Split-Path $Source -Leaf)
        if (Test-Path -LiteralPath $Existing) {
            Write-Warning "Could not overwrite $Existing, likely because OpenMW is running. Existing copy will be used."
        } else {
            throw
        }
    }
}

if (!(Test-Path -LiteralPath $Toolchain)) {
    throw "Missing vcpkg toolchain: $Toolchain"
}

if (!(Test-Path -LiteralPath $LuaJitInclude)) {
    throw "Missing LuaJit include directory: $LuaJitInclude"
}

if (!(Test-Path -LiteralPath $LuaJitLibrary)) {
    throw "Missing LuaJit library: $LuaJitLibrary"
}

Import-VisualStudioEnvironment
$PathValue = $env:Path
Remove-Item Env:PATH -ErrorAction SilentlyContinue
$env:Path = $PathValue

if (!$SkipConfigure) {
    $ConfigureArgs = @(
        "-S", $RepoRoot,
        "-B", $BuildPath,
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
        "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        "-DVCPKG_TARGET_TRIPLET=$Triplet",
        "-DLuaJit_INCLUDE_DIR=$LuaJitInclude",
        "-DLuaJit_LIBRARY=$LuaJitLibrary",
        "-DBUILD_OPENMW=ON",
        "-DBUILD_OPENMW_VR=$($BuildOpenMwVr.IsPresent)",
        "-DBUILD_LAUNCHER=OFF",
        "-DBUILD_WIZARD=OFF",
        "-DBUILD_OPENCS=OFF",
        "-DBUILD_BSATOOL=OFF",
        "-DBUILD_ESMTOOL=OFF",
        "-DBUILD_NIFTEST=OFF",
        "-DBUILD_OPENMW_TESTS=OFF"
    )

    Invoke-CheckedNative -Command "cmake" -Arguments $ConfigureArgs
}

$BuildTarget = if ($BuildOpenMwVr) { "openmw_vr" } else { "openmw" }
$BuildArgs = @("--build", $BuildPath, "--config", $Configuration, "--target", $BuildTarget, "--parallel", "$Parallel")
Invoke-CheckedNative -Command "cmake" -Arguments $BuildArgs

$OutputDir = Join-Path $BuildPath $Configuration
if ($Configuration -eq "Debug") {
    $MyGuiDll = Join-Path $VcpkgRoot "installed/$Triplet/debug/bin/Debug/MyGUIEngine_d.dll"
} else {
    $MyGuiDll = Join-Path $VcpkgRoot "installed/$Triplet/bin/Release/MyGUIEngine.dll"
}

if (Test-Path -LiteralPath $MyGuiDll) {
    Copy-RequiredItem -Source $MyGuiDll -Destination $OutputDir
} else {
    Write-Warning "MyGUI runtime DLL was not found at $MyGuiDll"
}

if ($Configuration -eq "Debug") {
    $OsgPluginDir = Join-Path $VcpkgRoot "installed/$Triplet/debug/plugins/osgPlugins-3.6.5"
} else {
    $OsgPluginDir = Join-Path $VcpkgRoot "installed/$Triplet/plugins/osgPlugins-3.6.5"
}

if (Test-Path -LiteralPath $OsgPluginDir) {
    Copy-RequiredItem -Source $OsgPluginDir -Destination $OutputDir -Recurse
} else {
    Write-Warning "OSG plugin directory was not found at $OsgPluginDir"
}

$OutputOsgPluginDir = Join-Path $OutputDir "osgPlugins-3.6.5"
if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) {
    if (!(Test-Path -LiteralPath $ExtraOsgPluginDir)) {
        throw "Extra OSG plugin directory was not found: $ExtraOsgPluginDir"
    }

    New-Item -ItemType Directory -Force -Path $OutputOsgPluginDir | Out-Null
    try {
        Copy-Item -Path (Join-Path $ExtraOsgPluginDir "*") -Destination $OutputOsgPluginDir -Recurse -Force
    } catch {
        Write-Warning "Could not overwrite one or more extra OSG plugins, likely because OpenMW is running. Existing copies will be used."
    }
}

$FallbackOsgPluginDir = Join-Path $RepoRoot "build/$Configuration/osgPlugins-3.6.5"
if (Test-Path -LiteralPath $FallbackOsgPluginDir) {
    New-Item -ItemType Directory -Force -Path $OutputOsgPluginDir | Out-Null
    try {
        Copy-Item -Path (Join-Path $FallbackOsgPluginDir "*") -Destination $OutputOsgPluginDir -Recurse -Force
    } catch {
        Write-Warning "Could not overwrite one or more OSG plugins, likely because OpenMW is running. Existing copies will be used."
    }
}
