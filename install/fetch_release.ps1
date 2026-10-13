# Downloads the newest DUGco AlphaGenSAM release (including betas) from GitHub, unzips it
# to a temp folder and prints the folder that contains AlphaGenSAM.ofx.bundle.
# Used by install.bat when it is run from a clone of the repository (which has no built
# plug-in) instead of from an unzipped release.
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$repo = 'dughogan/nuke-alphagen-ofx'
$headers = @{ 'User-Agent' = 'dugco-alphagensam-installer' }
$releases = Invoke-RestMethod "https://api.github.com/repos/$repo/releases" -Headers $headers
$asset = $null
foreach ($r in $releases) {
    $asset = $r.assets | Where-Object { $_.name -like '*_win64.zip' } | Select-Object -First 1
    if ($asset) { Write-Host "Downloading $($r.name): $($asset.name) ..."; break }
}
if (-not $asset) { throw "No release download found at https://github.com/$repo/releases" }
$work = Join-Path $env:TEMP ('dugco_alphagensam_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
$zip = Join-Path $work $asset.name
Invoke-WebRequest $asset.browser_download_url -OutFile $zip -Headers $headers -UseBasicParsing
Expand-Archive -Path $zip -DestinationPath (Join-Path $work 'release') -Force
$bundle = Get-ChildItem (Join-Path $work 'release') -Recurse -Directory -Filter 'AlphaGenSAM.ofx.bundle' | Select-Object -First 1
if (-not $bundle) { throw "The download didn't contain AlphaGenSAM.ofx.bundle" }
# last line = the folder holding the bundle (read by install.bat)
Write-Output $bundle.Parent.FullName
