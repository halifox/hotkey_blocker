[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$Version = '',

    [string]$SignCertificatePath = '',
    [string]$SignCertificatePassword = '',
    [string]$TimestampUrl = 'http://timestamp.digicert.com',

    [switch]$Clean,
    [switch]$Package,
    [switch]$RequireSignature
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$preset = "$($Architecture.ToLowerInvariant())-$($Configuration.ToLowerInvariant())"
$buildRoot = Join-Path $repoRoot 'out\build'
$binRoot = Join-Path $repoRoot 'out\bin'
$packageRoot = Join-Path $repoRoot 'out\packages'

if (-not $SignCertificatePath -and $env:HKB_SIGN_CERTIFICATE_PATH) {
    $SignCertificatePath = $env:HKB_SIGN_CERTIFICATE_PATH
}
if (-not $SignCertificatePassword -and $env:HKB_SIGN_CERTIFICATE_PASSWORD) {
    $SignCertificatePassword = $env:HKB_SIGN_CERTIFICATE_PASSWORD
}
if ($env:HKB_SIGN_TIMESTAMP_URL) {
    $TimestampUrl = $env:HKB_SIGN_TIMESTAMP_URL
}

$signingEnabled = -not [string]::IsNullOrWhiteSpace($SignCertificatePath)
if ($RequireSignature -and -not $signingEnabled) {
    throw '发布构建必须提供签名证书。请设置 HKB_SIGN_CERTIFICATE_PATH 或传入 -SignCertificatePath。'
}
if ($signingEnabled -and -not (Test-Path -LiteralPath $SignCertificatePath -PathType Leaf)) {
    throw "找不到签名证书：$SignCertificatePath"
}

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

function Get-SignToolPath {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $kitsRoot = if (${env:ProgramFiles(x86)}) {
        Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    } else {
        $null
    }
    if ($kitsRoot -and (Test-Path -LiteralPath $kitsRoot)) {
        $candidate = Get-ChildItem -LiteralPath $kitsRoot -Recurse -Filter signtool.exe -File |
            Sort-Object FullName -Descending |
            Select-Object -First 1 -ExpandProperty FullName
        if ($candidate) {
            return $candidate
        }
    }
    throw '找不到 signtool.exe。请安装 Windows SDK。'
}

function Sign-Artifact {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not $signingEnabled) {
        return
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "找不到待签名文件：$Path"
    }

    $arguments = @('sign', '/fd', 'SHA256', '/f', $SignCertificatePath)
    if ($SignCertificatePassword) {
        $arguments += @('/p', $SignCertificatePassword)
    }
    if ($TimestampUrl) {
        $arguments += @('/tr', $TimestampUrl, '/td', 'SHA256')
    }
    $arguments += $Path

    Write-Host "签名：$Path"
    & $signTool @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "签名失败：$Path"
    }

    & $signTool verify /pa /all $Path
    if ($LASTEXITCODE -ne 0) {
        throw "签名验证失败：$Path"
    }
}

function Get-RuntimeArtifacts {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('x64', 'x86')]
        [string]$TargetArchitecture,

        [Parameter(Mandatory = $true)]
        [string]$TargetPreset
    )

    $directory = Join-Path $binRoot $TargetPreset
    $files = @(
        (Join-Path $directory 'HotkeyBlocker.exe'),
        (Join-Path $directory $(if ($TargetArchitecture -eq 'x64') { 'HotkeyHook64.dll' } else { 'HotkeyHook32.dll' }))
    )
    if ($TargetArchitecture -eq 'x86') {
        $files += Join-Path $directory 'HotkeyBlockerInjector32.exe'
    }
    return $files
}

function Sign-RuntimeArtifacts {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$TargetPresets
    )

    foreach ($targetPreset in $TargetPresets) {
        $targetArchitecture = if ($targetPreset.StartsWith('x64-', [System.StringComparison]::OrdinalIgnoreCase)) {
            'x64'
        } else {
            'x86'
        }
        foreach ($artifact in Get-RuntimeArtifacts -TargetArchitecture $targetArchitecture -TargetPreset $targetPreset) {
            Sign-Artifact -Path $artifact
        }
    }
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
        [string]$TargetPreset
    )

    $versionArgument = if ($Version) { " -DHKB_PROJECT_VERSION=$Version" } else { '' }
    $command = "cmake --preset `"$TargetPreset`"$versionArgument && cmake --build --preset `"$TargetPreset`" --parallel"

    Write-Host "==> $TargetArchitecture $Configuration ($TargetPreset)"
    Invoke-VsCommand -TargetArchitecture $TargetArchitecture -Command $command
}

function Invoke-CreatePackage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TargetArchitecture,

        [Parameter(Mandatory = $true)]
        [string]$TargetPreset
    )

    Write-Host "==> package $TargetArchitecture $Configuration ($TargetPreset)"
    Invoke-VsCommand -TargetArchitecture $TargetArchitecture -Command "cmake --build --preset `"$TargetPreset`" --target package"
}

$vsDevCmd = Get-VsDevCommandPath
$signTool = if ($signingEnabled) { Get-SignToolPath } else { $null }
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
Invoke-ConfigureAndBuild -TargetArchitecture $Architecture -TargetPreset $preset

Sign-RuntimeArtifacts -TargetPresets $presetsToBuild

if ($Package) {
    Invoke-CreatePackage -TargetArchitecture $Architecture -TargetPreset $preset
}

if ($Package) {
    New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
    $targetBuildDir = Join-Path $buildRoot $preset
    $packages = Get-ChildItem -LiteralPath $targetBuildDir -File -Filter 'HotkeyBlocker-*.exe'
    if (-not $packages) {
        throw "CPack 没有在 $targetBuildDir 生成安装包。"
    }
    foreach ($packageFile in $packages) {
        $packagePath = Join-Path $packageRoot $packageFile.Name
        Copy-Item -LiteralPath $packageFile.FullName -Destination $packagePath -Force
        Sign-Artifact -Path $packagePath
    }

    Get-ChildItem -LiteralPath $packageRoot -File -Filter 'HotkeyBlocker-*.exe' |
        ForEach-Object {
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            "$hash  $($_.Name)" | Set-Content -LiteralPath (Join-Path $packageRoot "$($_.Name).sha256") -Encoding ASCII
        }
    Write-Host "安装包和 SHA-256 校验文件已输出到：$packageRoot"
}
