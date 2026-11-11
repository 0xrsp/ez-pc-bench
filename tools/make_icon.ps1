# Regenerates the app / site icons from assets/icon.svg:
#   assets/icon.ico         16-256 px, PNG entries (embedded in both exes via gui.rc / app.rc)
#   site/favicon.ico        copy of the above
#   site/favicon.svg        copy of the master
#   site/apple-touch-icon.png  180 px
# Rasterizes with headless Microsoft Edge (installed with Windows), so there are no extra tools.
# Run from anywhere: powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$svg = Join-Path $root 'assets\icon.svg'
$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 180, 256
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
          "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found' }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('ezb_icon_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null
try {
    # One page, every size side by side at known x offsets, transparent background, 1:1 pixels.
    $body = [IO.File]::ReadAllText($svg)
    $x = 0; $html = ''; $pos = @{}
    foreach ($s in $sizes) {
        $pos[$s] = $x
        $html += "<div style='position:absolute;left:${x}px;top:0;width:${s}px;height:${s}px'>" +
                 ($body -replace '<svg ', "<svg width='$s' height='$s' ") + '</div>'
        $x += $s + 8
    }
    $page = Join-Path $tmp 'icons.html'
    [IO.File]::WriteAllText($page, "<!doctype html><html><body style='margin:0;background:transparent'>$html</body></html>")
    $shot = Join-Path $tmp 'shot.png'
    $args = @('--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
              "--user-data-dir=`"$tmp\profile`"", '--default-background-color=00000000', "--window-size=$($x + 8),300",
              "--screenshot=`"$shot`"", ([Uri]$page).AbsoluteUri)
    Start-Process -FilePath $edge -ArgumentList $args -Wait -WindowStyle Hidden
    if (-not (Test-Path $shot)) { throw 'Edge did not write a screenshot' }

    $sheet = [Drawing.Bitmap]::FromFile($shot)
    $png = @{}
    foreach ($s in $sizes) {
        $bmp = $sheet.Clone((New-Object Drawing.Rectangle $pos[$s], 0, $s, $s), [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $ms = New-Object IO.MemoryStream
        $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
        $png[$s] = $ms.ToArray()
        $bmp.Dispose()
    }
    $sheet.Dispose()

    # ICO: 6-byte header, 16-byte directory entry per image, then the PNG data (Vista+ reads PNG entries).
    $ico = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
    $out = New-Object IO.MemoryStream
    $w = New-Object IO.BinaryWriter $out
    $w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$ico.Count)
    $offset = 6 + 16 * $ico.Count
    foreach ($s in $ico) {
        $dim = if ($s -ge 256) { 0 } else { $s }
        $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([UInt16]1); $w.Write([UInt16]32)
        $w.Write([UInt32]$png[$s].Length); $w.Write([UInt32]$offset)
        $offset += $png[$s].Length
    }
    foreach ($s in $ico) { $w.Write($png[$s]) }
    $w.Flush()
    $icoPath = Join-Path $root 'assets\icon.ico'
    [IO.File]::WriteAllBytes($icoPath, $out.ToArray())

    $site = Join-Path $root 'site'
    Copy-Item $icoPath (Join-Path $site 'favicon.ico') -Force
    Copy-Item $svg (Join-Path $site 'favicon.svg') -Force
    [IO.File]::WriteAllBytes((Join-Path $site 'apple-touch-icon.png'), $png[180])
    "icon.ico: $((Get-Item $icoPath).Length) bytes, sizes $($ico -join ', ')"
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
