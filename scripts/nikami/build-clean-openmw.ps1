param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$Triplet = "x64-windows",
    [int]$Parallel = 8,
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
$LuaJitLibrary = Join-Path $VcpkgRoot "installed/$Triplet/debug/lib/lua51.lib"

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

if (!$SkipConfigure) {
    $ConfigureArgs = @(
        "-S", $RepoRoot,
        "-B", $BuildPath,
        "-G", "Visual Studio 17 2022",
        "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
        "-DVCPKG_TARGET_TRIPLET=$Triplet",
        "-DLuaJit_INCLUDE_DIR=$LuaJitInclude",
        "-DLuaJit_LIBRARY=$LuaJitLibrary",
        "-DBUILD_OPENMW=ON",
        "-DBUILD_OPENMW_VR=OFF",
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

$BuildArgs = @("--build", $BuildPath, "--config", $Configuration, "--target", "openmw", "--parallel", "$Parallel")
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

$FallbackOsgPluginDir = Join-Path $RepoRoot "build/$Configuration/osgPlugins-3.6.5"
$OutputOsgPluginDir = Join-Path $OutputDir "osgPlugins-3.6.5"
if (Test-Path -LiteralPath $FallbackOsgPluginDir) {
    New-Item -ItemType Directory -Force -Path $OutputOsgPluginDir | Out-Null
    try {
        Copy-Item -Path (Join-Path $FallbackOsgPluginDir "*") -Destination $OutputOsgPluginDir -Recurse -Force
    } catch {
        Write-Warning "Could not overwrite one or more OSG plugins, likely because OpenMW is running. Existing copies will be used."
    }
}
