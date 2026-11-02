# Retake the LibreNMS screenshots in docs/images/librenms from the LibreNMS in librenms/test.
#
#   powershell -File librenms/test/screenshots.ps1 [-Device 1]
#
# Needs Docker Desktop running (the Playwright image, as for docs/diagrams) and the LibreNMS login
# in secret/librenms.md (gitignored; line 1 "username: ...", line 2 the password). The page's
# contact address is replaced by ops@example.com in the pictures. Check the images before committing.
param([string]$Device = "1")
$ErrorActionPreference = "Stop"

$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$secret = Join-Path $repo "secret"
$work = Join-Path ([IO.Path]::GetTempPath()) "rfpm-librenms-shots"
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item (Join-Path $PSScriptRoot "screenshots.mjs") $work -Force

docker run --rm --add-host=host.docker.internal:host-gateway -v "${work}:/out" -v "${secret}:/secret:ro" `
    -w /tmp/shots mcr.microsoft.com/playwright:v1.48.2-noble `
    sh -c "npm init -y >/dev/null && npm install --silent playwright@1.48.2 >/dev/null && cp /out/screenshots.mjs . && node screenshots.mjs $Device"
if ($LASTEXITCODE -ne 0) { throw "screenshots failed" }

foreach ($n in "device-overview", "device-eventlog", "device-neighbours", "device-inventory") {
    Copy-Item (Join-Path $work "$n.png") (Join-Path $repo "docs\images\librenms\$n.png") -Force
}
Remove-Item -Recurse -Force $work
Write-Host "Screenshots updated in docs/images/librenms"
