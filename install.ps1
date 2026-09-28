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

# The newest release of the channel. Nightly takes stable releases too, when they're newer.
$releases = Invoke-RestMethod "https://api.github.com/repos/$repo/releases?per_page=30"
$release = $releases | Where-Object { -not $_.draft -and ($channel -eq 'nightly' -or -not $_.prerelease) } |
    Select-Object -First 1
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
