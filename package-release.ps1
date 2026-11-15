param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$LoaderPath
)

# Layout (both archives): scripts\SWTFURawMouseFix.asi and scripts\swtfu-raw-mouse-fix.ini.
# The convenience archive adds Ultimate ASI Loader as winmm.dll with winmm.ini
# (loadfromscriptsonly=1): the game imports winmm.dll, and dinput8.dll stays free for other
# mods (the SWTFU Advanced Configurator ships its own dinput8.dll loader, which does not keep
# other plugins loaded).

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$repositoryRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$buildDirectory = Join-Path $repositoryRoot 'build'
$distDirectory = Join-Path $repositoryRoot 'dist'
$stagingRoot = Join-Path $buildDirectory 'release-staging'
$plugin = Join-Path $buildDirectory 'SWTFURawMouseFix.asi'
$configuration = Join-Path $repositoryRoot 'swtfu-raw-mouse-fix.ini'
$loaderConfiguration = Join-Path $repositoryRoot 'winmm.ini'
$readmeTemplate = Join-Path $repositoryRoot 'release-readme.txt'
$projectLicense = Join-Path $repositoryRoot 'LICENSE'
$thirdPartyNotice = Join-Path $repositoryRoot 'THIRD-PARTY-NOTICES.txt'
$loaderVersion = '9.7.4'
$expectedLoaderHash = 'D5A059AA467A7A7127C8F6169F79FA63FF0F55986EE9EB2FD9A281BEBF2AA2E6'
$archivePrefix = 'StarWarsTheForceUnleashedUltimateSithEdition-RawInputMouseFix'

foreach ($requiredFile in @($plugin, $configuration, $loaderConfiguration, $readmeTemplate,
        $projectLicense, $thirdPartyNotice)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required release input is missing: $requiredFile"
    }
}
if ((Get-Content -LiteralPath $readmeTemplate -Raw) -match 'TODO') {
    throw "release-readme.txt still has TODO sections; finish it before packaging."
}

$pluginMachine = [System.IO.File]::ReadAllBytes($plugin)
$peOffset = [System.BitConverter]::ToInt32($pluginMachine, 0x3C)
if ([System.BitConverter]::ToUInt16($pluginMachine, $peOffset + 4) -ne 0x14C) {
    throw "The plugin is not an x86 module: $plugin"
}
$pluginText = [System.Text.Encoding]::ASCII.GetString($pluginMachine)
# A development build carries the mouse-reader tracer; releases are built without it.
if ($pluginText.Contains('trace armed')) {
    throw "The plugin is a development build (build-msvc.cmd dev); build it without 'dev' first: $plugin"
}
if (-not $pluginText.Contains("$Version`0")) {
    throw "The plugin's version string is not $Version (SWTFU_RAW_MOUSE_VERSION in swtfu_raw_mouse_fix.cpp): $plugin"
}

function Confirm-ChildPath {
    param([string]$Path, [string]$Parent)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($fullParent,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside $fullParent : $fullPath"
    }
    return $fullPath
}

function Reset-StagingDirectory {
    param([string]$Path)

    $safePath = Confirm-ChildPath -Path $Path -Parent $buildDirectory
    if (Test-Path -LiteralPath $safePath) {
        Remove-Item -LiteralPath $safePath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $safePath | Out-Null
    return $safePath
}

# Archive-relative paths with forward slashes, sorted (the order of SHA256SUMS.txt and the zip).
function Get-PayloadFiles {
    param([string]$Directory)

    $root = $Directory.TrimEnd('\') + '\'
    Get-ChildItem -LiteralPath $Directory -File -Recurse |
        ForEach-Object {
            [pscustomobject]@{
                Path     = $_.FullName
                Relative = $_.FullName.Substring($root.Length).Replace('\', '/')
            }
        } |
        Sort-Object Relative
}

function Write-PayloadHashes {
    param([string]$Directory)

    $lines = Get-PayloadFiles -Directory $Directory |
        Where-Object Relative -ne 'SHA256SUMS.txt' |
        ForEach-Object {
            $hash = (Get-FileHash -LiteralPath $_.Path -Algorithm SHA256).Hash
            '{0}  {1}' -f $hash.ToLowerInvariant(), $_.Relative
        }
    Set-Content -LiteralPath (Join-Path $Directory 'SHA256SUMS.txt') `
        -Value $lines -Encoding Ascii
}

function New-ZipFromDirectory {
    param([string]$Directory, [string]$Archive)

    $stream = [System.IO.File]::Open($Archive, [System.IO.FileMode]::CreateNew)
    try {
        $zip = New-Object System.IO.Compression.ZipArchive($stream,
            [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($file in Get-PayloadFiles -Directory $Directory) {
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,
                    $file.Path, $file.Relative, [System.IO.Compression.CompressionLevel]::Optimal)
            }
        } finally { $zip.Dispose() }
    } finally { $stream.Dispose() }
}

function New-ReleaseArchive {
    param(
        [string]$Name,
        [string]$Stage,
        [bool]$IncludeLoader,
        [string]$ResolvedLoader
    )

    $stagePath = Reset-StagingDirectory -Path $Stage
    $scripts = Join-Path $stagePath 'scripts'
    New-Item -ItemType Directory -Path $scripts | Out-Null
    Copy-Item -LiteralPath $plugin -Destination $scripts
    Copy-Item -LiteralPath $configuration -Destination $scripts
    Copy-Item -LiteralPath $projectLicense `
        -Destination (Join-Path $stagePath 'LICENSE.txt')

    $releaseReadme = (Get-Content -LiteralPath $readmeTemplate -Raw).Replace(
        '@VERSION@', $Version)
    Set-Content -LiteralPath (Join-Path $stagePath 'README.txt') `
        -Value $releaseReadme -Encoding UTF8

    if ($IncludeLoader) {
        Copy-Item -LiteralPath $ResolvedLoader `
            -Destination (Join-Path $stagePath 'winmm.dll')
        Copy-Item -LiteralPath $loaderConfiguration -Destination $stagePath
        Copy-Item -LiteralPath $thirdPartyNotice `
            -Destination (Join-Path $stagePath 'THIRD-PARTY-NOTICES.txt')
    }

    Write-PayloadHashes -Directory $stagePath

    $archive = Join-Path $distDirectory ($Name + '.zip')
    $archiveHash = $archive + '.sha256'
    if ((Test-Path -LiteralPath $archive) -or
        (Test-Path -LiteralPath $archiveHash)) {
        throw "Release output already exists; refusing to overwrite: $archive"
    }

    New-ZipFromDirectory -Directory $stagePath -Archive $archive
    $zipHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    $zipHashLine = '{0}  {1}' -f $zipHash.ToLowerInvariant(),
        [System.IO.Path]::GetFileName($archive)
    Set-Content -LiteralPath $archiveHash -Value $zipHashLine -Encoding Ascii
    Write-Host "wrote $archive"
}

New-Item -ItemType Directory -Force -Path $distDirectory | Out-Null

New-ReleaseArchive -Name "$archivePrefix-v$Version-plugin-only" `
    -Stage (Join-Path $stagingRoot 'plugin-only') `
    -IncludeLoader $false -ResolvedLoader ''

if ($LoaderPath) {
    $resolvedLoader = (Resolve-Path -LiteralPath $LoaderPath).Path
    $actualLoaderHash =
        (Get-FileHash -LiteralPath $resolvedLoader -Algorithm SHA256).Hash
    if ($actualLoaderHash -ne $expectedLoaderHash) {
        throw "Loader hash mismatch (expected the x86 Ultimate ASI Loader v$loaderVersion): $actualLoaderHash"
    }

    New-ReleaseArchive -Name "$archivePrefix-v$Version-with-Ultimate-ASI-Loader-v$loaderVersion" `
        -Stage (Join-Path $stagingRoot 'with-loader') `
        -IncludeLoader $true -ResolvedLoader $resolvedLoader
}
