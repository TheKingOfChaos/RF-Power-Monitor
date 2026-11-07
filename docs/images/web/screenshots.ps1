# Retake the web page screenshots in docs/images/web from a running device.
#
#   $env:RFPOWER_WEB_PASSWORD = "<the device's web password>"
#   powershell -File docs/images/web/screenshots.ps1 -Ip 10.132.0.77
#
# Needs Docker Desktop running (the Playwright image, as for docs/diagrams). The password is passed
# to the container in the environment and never printed. Real addresses, the MAC, contact, location
# and community are replaced by documentation values (screenshots.mjs). Check the images before
# committing.
param([Parameter(Mandatory = $true)][string]$Ip)
$ErrorActionPreference = "Stop"
if (-not $env:RFPOWER_WEB_PASSWORD) { throw "set RFPOWER_WEB_PASSWORD first" }

$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$work = Join-Path ([IO.Path]::GetTempPath()) "rfpm-web-shots"
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item (Join-Path $PSScriptRoot "screenshots.mjs") $work -Force

docker run --rm -e RFPOWER_WEB_PASSWORD -v "${work}:/out" -w /tmp/shots mcr.microsoft.com/playwright:v1.48.2-noble `
    sh -c "npm init -y >/dev/null && npm install --silent playwright@1.48.2 >/dev/null && cp /out/screenshots.mjs . && node screenshots.mjs $Ip"
if ($LASTEXITCODE -ne 0) { throw "screenshots failed" }

foreach ($n in "web-login", "web-status") {
    Copy-Item (Join-Path $work "$n.png") (Join-Path $repo "docs\images\web\$n.png") -Force
}
Remove-Item -Recurse -Force $work
Write-Host "Screenshots updated in docs/images/web"
