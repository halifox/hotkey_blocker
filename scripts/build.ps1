[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$Version = '',

    [switch]$Clean,
    [switch]$Package
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$preset = "$($Architecture.ToLowerInvariant())-$($Configuration.ToLowerInvariant())"
$buildRoot = Join-Path $repoRoot 'out\build'
$binRoot = Join-Path $repoRoot 'out\bin'
$packageRoot = Join-Path $repoRoot 'out\packages'

if ($Version -and $Version -notmatch '^\d+\.\d+\.\d+$') {
    throw 'Version 必须是三段数字版本，例如 1.0.0。'
}

function Get-VsDevCommandPath {
    $vswhereCandidates = @()
    if ($env:ProgramFiles) {
        $vswhereCandidates += Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if (${env:ProgramFiles(x86)}) {
        $vswhereCandidates += Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    }

    $vswhere = $vswhereCandidates |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if (-not $vswhere) {
        throw '找不到 vswhere.exe。请安装带 C++ 工作负载的 Visual Studio，或先进入 Visual Studio Developer PowerShell。'
    }

    $installationPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1
    $installationPath = if ($installationPath) { $installationPath.Trim() } else { '' }
    if (-not $installationPath) {
        throw '找不到包含 MSVC x86/x64 工具链的 Visual Studio 安装。'
    }

    $vsDevCmd = Join-Path $installationPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $vsDevCmd)) {
        throw "找不到 VsDevCmd.bat：$vsDevCmd"
    }
    return $vsDevCmd
}

function Invoke-VsCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TargetArchitecture,

        [Parameter(Mandatory = $true)]
        [string]$Command
    )

    $batchPath = Join-Path ([System.IO.Path]::GetTempPath()) ("hkb-build-{0}.cmd" -f [guid]::NewGuid().ToString('N'))
    $batch = @"
@echo off
call "$vsDevCmd" -arch=$TargetArchitecture -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
$Command
exit /b %errorlevel%
"@

    [System.IO.File]::WriteAllText($batchPath, $batch, [System.Text.Encoding]::ASCII)
    try {
        & cmd.exe /d /s /c "`"$batchPath`""
        if ($LASTEXITCODE -ne 0) {
            throw "构建命令失败，退出码：$LASTEXITCODE"
        }
    } finally {
        Remove-Item -LiteralPath $batchPath -Force -ErrorAction SilentlyContinue
    }
}

function Remove-BuildOutputs {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Presets
    )

    foreach ($name in $Presets) {
        foreach ($root in @($buildRoot, $binRoot)) {
            $path = Join-Path $root $name
            if (Test-Path -LiteralPath $path) {
                Remove-Item -LiteralPath $path -Recurse -Force
            }
        }
    }
}

function Invoke-ConfigureAndBuild {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TargetArchitecture,

        [Parameter(Mandatory = $true)]
        [string]$TargetPreset,

        [switch]$CreatePackage
    )

    $versionArgument = if ($Version) { " -DHKB_PROJECT_VERSION=$Version" } else { '' }
    $command = "cmake --preset `"$TargetPreset`"$versionArgument && cmake --build --preset `"$TargetPreset`" --parallel"
    if ($CreatePackage) {
        $command += " && cmake --build --preset `"$TargetPreset`" --target package"
    }

    Write-Host "==> $TargetArchitecture $Configuration ($TargetPreset)"
    Invoke-VsCommand -TargetArchitecture $TargetArchitecture -Command $command
}

$vsDevCmd = Get-VsDevCommandPath
$presetsToBuild = @($preset)
if ($Package -and $Architecture -eq 'x64') {
    $presetsToBuild = @("x86-$($Configuration.ToLowerInvariant())", $preset)
}

if ($Clean) {
    Remove-BuildOutputs -Presets $presetsToBuild
}

if ($Package) {
    $targetBuildDir = Join-Path $buildRoot $preset
    if (Test-Path -LiteralPath $targetBuildDir) {
        Get-ChildItem -LiteralPath $targetBuildDir -File -Filter 'HotkeyBlocker-*.exe' -ErrorAction SilentlyContinue |
            Remove-Item -Force
    }
    if (Test-Path -LiteralPath $packageRoot) {
        Get-ChildItem -LiteralPath $packageRoot -File -Filter 'HotkeyBlocker-*.exe*' -ErrorAction SilentlyContinue |
            Remove-Item -Force
    }
}

if ($Package -and $Architecture -eq 'x64') {
    Invoke-ConfigureAndBuild -TargetArchitecture 'x86' -TargetPreset "x86-$($Configuration.ToLowerInvariant())"
}
Invoke-ConfigureAndBuild -TargetArchitecture $Architecture -TargetPreset $preset -CreatePackage:$Package

if ($Package) {
    New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
    $targetBuildDir = Join-Path $buildRoot $preset
    $packages = Get-ChildItem -LiteralPath $targetBuildDir -File -Filter 'HotkeyBlocker-*.exe'
    if (-not $packages) {
        throw "CPack 没有在 $targetBuildDir 生成安装包。"
    }
    foreach ($packageFile in $packages) {
        Copy-Item -LiteralPath $packageFile.FullName -Destination $packageRoot -Force
    }

    Get-ChildItem -LiteralPath $packageRoot -File -Filter 'HotkeyBlocker-*.exe' |
        ForEach-Object {
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            "$hash  $($_.Name)" | Set-Content -LiteralPath (Join-Path $packageRoot "$($_.Name).sha256") -Encoding ASCII
        }
    Write-Host "安装包和 SHA-256 校验文件已输出到：$packageRoot"
}
