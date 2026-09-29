# Installs purr for this user, on Windows:
#
#     irm https://raw.githubusercontent.com/BlenMiner/PurrEngine/release/install.ps1 | iex
#
# Nightly versions instead: set $env:PURR_CHANNEL = 'nightly' first.
# purr goes in %LOCALAPPDATA%\Purr, and its bin folder on the user's PATH. No
# admin rights needed. Once installed, `purr upgrade` keeps it up to date.

$ErrorActionPreference = 'Stop'
$repo = 'BlenMiner/PurrEngine'
$package = 'purr-windows-x64.zip'
$channel = if ($env:PURR_CHANNEL -eq 'nightly') { 'nightly' } else { 'stable' }
$root = Join-Path $env:LOCALAPPDATA 'Purr'
$bin = Join-Path $root 'bin'

if (Test-Path (Join-Path $bin 'purr.exe')) {
    Write-Host "purr is already installed in $root; upgrading it."
    & (Join-Path $bin 'purr.exe') upgrade "--$channel"
    return
}

# Semantic versioning's order of two tags: -1, 0 or 1. A pre-release comes
# before its release, and its parts compare as numbers when they are, before words.
$versionPattern = '^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+.*)?$'
function Compare-Version([string]$a, [string]$b) {
    $x = [regex]::Match($a, $versionPattern)
    $y = [regex]::Match($b, $versionPattern)
    for ($i = 1; $i -le 3; $i++) {
        $c = ([bigint]$x.Groups[$i].Value).CompareTo([bigint]$y.Groups[$i].Value)
        if ($c -ne 0) { return [Math]::Sign($c) }
    }
    if (-not $x.Groups[4].Success -or -not $y.Groups[4].Success) {
        return [int]$y.Groups[4].Success - [int]$x.Groups[4].Success
    }
    $xs = $x.Groups[4].Value.Split('.')
    $ys = $y.Groups[4].Value.Split('.')
    for ($i = 0; $i -lt [Math]::Min($xs.Count, $ys.Count); $i++) {
        $xNumber = $xs[$i] -match '^\d+$'
        $yNumber = $ys[$i] -match '^\d+$'
        if ($xNumber -and $yNumber) { $c = ([bigint]$xs[$i]).CompareTo([bigint]$ys[$i]) }
        elseif ($xNumber) { $c = -1 }
        elseif ($yNumber) { $c = 1 }
        else { $c = [string]::CompareOrdinal($xs[$i], $ys[$i]) }
        if ($c -ne 0) { return [Math]::Sign($c) }
    }
    return [Math]::Sign($xs.Count - $ys.Count)
}

# The channel's highest version, not the last one published (as purr upgrade
# picks, compiler/cli/release.c). Nightly takes stable releases too, when
# they're newer. Stable also asks for GitHub's latest release, since nightly
# ones can push it out of the list.
function Select-Release($releases, [string]$channel) {
    $best = $null
    foreach ($r in $releases) {
        if ($r.draft -or ($channel -eq 'stable' -and $r.prerelease) -or $r.tag_name -notmatch $versionPattern) { continue }
        if (-not $best -or (Compare-Version $r.tag_name $best.tag_name) -gt 0) { $best = $r }
    }
    return $best
}

# ForEach-Object unrolls the list: Windows PowerShell returns a JSON array as one object.
$releases = @(Invoke-RestMethod "https://api.github.com/repos/$repo/releases?per_page=100" | ForEach-Object { $_ })
if ($channel -eq 'stable') {
    try { $releases += Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" } catch { }
}
$release = Select-Release $releases $channel
if (-not $release) { throw "There's no $channel release of purr yet." }
$zipUrl = ($release.assets | Where-Object name -eq $package).browser_download_url
$sumsUrl = ($release.assets | Where-Object name -eq 'SHA256SUMS').browser_download_url
if (-not $zipUrl -or -not $sumsUrl) { throw "Release $($release.tag_name) has no $package." }

$work = Join-Path ([IO.Path]::GetTempPath()) "purr-install-$([Guid]::NewGuid())"
New-Item -ItemType Directory -Path $work | Out-Null
try {
    Write-Host "Downloading purr $($release.tag_name.TrimStart('v'))..."
    $zip = Join-Path $work $package
    $sums = Join-Path $work 'SHA256SUMS'
    Invoke-WebRequest $zipUrl -OutFile $zip -UseBasicParsing
    Invoke-WebRequest $sumsUrl -OutFile $sums -UseBasicParsing

    # The download must match the checksum published with it.
    $line = Get-Content $sums | Where-Object { $_ -match [Regex]::Escape($package) } | Select-Object -First 1
    $expected = ($line -split '\s+')[0]
    $actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if (-not $expected -or $actual -ne $expected.ToLowerInvariant()) {
        throw 'The download is damaged (its checksum does not match). Nothing was installed.'
    }

    New-Item -ItemType Directory -Path $root -Force | Out-Null
    Expand-Archive -Path $zip -DestinationPath $root -Force
    Set-Content -Path (Join-Path $root 'channel') -Value $channel -NoNewline
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

# purr's bin folder on the user's PATH, for new terminals.
$path = [Environment]::GetEnvironmentVariable('Path', 'User')
if (-not (($path -split ';') -contains $bin)) {
    [Environment]::SetEnvironmentVariable('Path', ($(if ($path) { "$path;" } else { '' }) + $bin), 'User')
    $env:Path = "$env:Path;$bin"
}

Write-Host "Installed purr in $root."
# PurrLang in VS Code and the editors like it.
& (Join-Path $bin 'purr.exe') editors
Write-Host 'Open a new terminal, go to a folder with .purr files and run: purr run'
