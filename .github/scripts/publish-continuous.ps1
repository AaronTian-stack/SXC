[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Repository,

    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string] $CommitSha,

    [ValidateRange(1, 100)]
    [int] $Keep = 5,

    [string] $DistDirectory = 'dist',

    [string] $ReleaseTag = 'continuous'
)

$ErrorActionPreference = 'Stop'

function Invoke-GitHub {
    param(
        [Parameter(Mandatory)]
        [string[]] $Arguments
    )

    $output = & gh @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "gh $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
    return ($output -join [Environment]::NewLine)
}

$distPath = (Resolve-Path -LiteralPath $DistDirectory).Path
$windowsName = "sxc-sdk-windows-release-$CommitSha.zip"
$linuxName = "sxc-sdk-linux-release-$CommitSha.tar.gz"
$windowsPath = Join-Path $distPath $windowsName
$linuxPath = Join-Path $distPath $linuxName

foreach ($archive in @($windowsPath, $linuxPath)) {
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        throw "Missing continuous-build archive: $archive"
    }
}

& gh release view $ReleaseTag --repo $Repository *> $null
if ($LASTEXITCODE -ne 0) {
    Invoke-GitHub @(
        'release', 'create', $ReleaseTag,
        '--repo', $Repository,
        '--target', $CommitSha,
        '--prerelease',
        '--title', 'Continuous builds',
        '--notes', 'Prebuilt SDKs from recent successful pushes to main. These are development snapshots, not stable releases.'
    ) | Out-Null
}

Invoke-GitHub @(
    'release', 'upload', $ReleaseTag,
    $windowsPath, $linuxPath,
    '--repo', $Repository,
    '--clobber'
) | Out-Null

$release = Invoke-GitHub @('api', "repos/$Repository/releases/tags/$ReleaseTag") | ConvertFrom-Json
$assetPattern = '^sxc-sdk-(windows|linux)-release-([0-9a-f]{40})(\.zip|\.tar\.gz)$'
$buildAssets = @($release.assets | Where-Object { $_.name -match $assetPattern })
$assetsBySha = @{}

foreach ($asset in $buildAssets) {
    if ($asset.name -notmatch $assetPattern) {
        continue
    }
    $sha = $Matches[2]
    if (-not $assetsBySha.ContainsKey($sha)) {
        $assetsBySha[$sha] = @()
    }
    $assetsBySha[$sha] += $asset
}

$commits = Invoke-GitHub @('api', "repos/$Repository/commits?sha=main&per_page=100") | ConvertFrom-Json
$retainedShas = [System.Collections.Generic.List[string]]::new()
foreach ($commit in $commits) {
    if ($assetsBySha.ContainsKey($commit.sha)) {
        $retainedShas.Add($commit.sha)
        if ($retainedShas.Count -eq $Keep) {
            break
        }
    }
}

if ($retainedShas.Count -lt $Keep) {
    $fallbackShas = $assetsBySha.Keys |
        Where-Object { -not $retainedShas.Contains($_) } |
        Sort-Object {
            ($assetsBySha[$_] | Measure-Object -Property created_at -Maximum).Maximum
        } -Descending
    foreach ($sha in $fallbackShas) {
        $retainedShas.Add($sha)
        if ($retainedShas.Count -eq $Keep) {
            break
        }
    }
}

foreach ($sha in @($assetsBySha.Keys)) {
    if ($retainedShas.Contains($sha)) {
        continue
    }
    foreach ($asset in $assetsBySha[$sha]) {
        Invoke-GitHub @('api', '--method', 'DELETE', "repos/$Repository/releases/assets/$($asset.id)") | Out-Null
    }
}

$release = Invoke-GitHub @('api', "repos/$Repository/releases/tags/$ReleaseTag") | ConvertFrom-Json
$remainingAssets = @($release.assets)
$builds = foreach ($sha in $retainedShas) {
    $windowsAsset = $remainingAssets | Where-Object { $_.name -eq "sxc-sdk-windows-release-$sha.zip" } | Select-Object -First 1
    $linuxAsset = $remainingAssets | Where-Object { $_.name -eq "sxc-sdk-linux-release-$sha.tar.gz" } | Select-Object -First 1
    $createdAt = @($windowsAsset.created_at, $linuxAsset.created_at) |
        Where-Object { $_ } |
        Sort-Object |
        Select-Object -First 1

    [ordered]@{
        sha       = $sha
        createdAt = $createdAt
        assets    = [ordered]@{
            windows = $windowsAsset.browser_download_url
            linux   = $linuxAsset.browser_download_url
        }
    }
}

$index = [ordered]@{
    schemaVersion = 1
    channel       = 'continuous'
    latest        = $retainedShas[0]
    generatedAt   = [DateTimeOffset]::UtcNow.ToString('o')
    builds        = @($builds)
}
$indexPath = Join-Path $distPath 'index.json'
$index | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $indexPath -Encoding utf8

Invoke-GitHub @(
    'release', 'upload', $ReleaseTag,
    $indexPath,
    '--repo', $Repository,
    '--clobber'
) | Out-Null

if ($retainedShas[0] -eq $CommitSha) {
    $windowsTotPath = Join-Path $distPath 'sxc-sdk-windows-release-tot.zip'
    $linuxTotPath = Join-Path $distPath 'sxc-sdk-linux-release-tot.tar.gz'
    Copy-Item -LiteralPath $windowsPath -Destination $windowsTotPath -Force
    Copy-Item -LiteralPath $linuxPath -Destination $linuxTotPath -Force

    Invoke-GitHub @(
        'release', 'upload', $ReleaseTag,
        $windowsTotPath, $linuxTotPath,
        '--repo', $Repository,
        '--clobber'
    ) | Out-Null
}

Write-Host "Published $CommitSha and retained $($retainedShas.Count) continuous build(s)."
