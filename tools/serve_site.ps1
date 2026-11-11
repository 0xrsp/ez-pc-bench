# Serves the website (site/) locally at http://localhost:8765 for previewing changes.
# Copies the Inter fonts into site/fonts/ first (git-ignored; the Pages workflow copies them on deploy).
# Needs Python 3 on PATH. Stop with Ctrl+C.
# Usage: powershell -ExecutionPolicy Bypass -File tools\serve_site.ps1 [-Port 8765]
param([int]$Port = 8765)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$fonts = Join-Path $root 'site\fonts'
New-Item -ItemType Directory -Force $fonts | Out-Null
foreach ($f in 'Inter-Regular.ttf', 'Inter-Medium.ttf') {
    Copy-Item (Join-Path $root "third_party\inter\$f") $fonts -Force
}
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
if (-not $python) { throw 'Python 3 not found on PATH (needed for its built-in web server).' }
Write-Host "Serving $root\site at http://localhost:$Port  (Ctrl+C to stop)"
& $python.Source -m http.server $Port --bind 127.0.0.1 --directory (Join-Path $root 'site')
