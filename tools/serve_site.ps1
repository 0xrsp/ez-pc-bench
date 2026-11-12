# Serves the website (site/) locally at http://localhost:8765 for previewing changes.
# Copies the Inter fonts into site/fonts/ and the run library into site/library/, and builds site/data/library.json
# for the results page (all git-ignored; the Pages workflow does the same on deploy).
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
$lib = Join-Path $root 'site\library'
New-Item -ItemType Directory -Force $lib | Out-Null
Copy-Item (Join-Path $root 'library\*.toon.txt') $lib -Force
& $python.Source (Join-Path $root 'tools\site_library.py') (Join-Path $root 'library') (Join-Path $root 'site\data\library.json')
Write-Host "Serving $root\site at http://localhost:$Port  (Ctrl+C to stop)"
& $python.Source -m http.server $Port --bind 127.0.0.1 --directory (Join-Path $root 'site')
