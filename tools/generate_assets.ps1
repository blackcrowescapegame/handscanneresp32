param(
    [string]$Ffmpeg = "ffmpeg"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "htmlhandscanner"
$ui = Join-Path $root "data\ui"
$audio = Join-Path $root "data\audio"
$work = Join-Path $PSScriptRoot ".asset-work"

New-Item -ItemType Directory -Force -Path $ui, $audio, $work | Out-Null

function Invoke-Ffmpeg([string[]]$Arguments) {
    & $Ffmpeg -hide_banner -loglevel error -y @Arguments
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE" }
}

$basePng = Join-Path $work "base.png"
$skullPng = Join-Path $work "skull.png"
$hintPng = Join-Path $work "hint.png"

# Reproduce the browser's 1280x800 layout: the background covers the panel and
# the 115%x150% hand layer is offset -60px/-400px with contain sizing.
Invoke-Ffmpeg @(
    "-i", (Join-Path $source "Background.png"),
    "-i", (Join-Path $source "Handprint.png"),
    "-filter_complex", "[0:v]scale=1280:800:flags=lanczos[bg];[1:v]scale=1200:1200:flags=lanczos,colorchannelmixer=aa=0.8[hand];[bg][hand]overlay=76:-400:format=auto,format=rgb24[out]",
    "-map", "[out]", "-frames:v", "1", $basePng
)

# CSS object-fit: cover for the skull and contain on black for the hint.
Invoke-Ffmpeg @(
    "-i", (Join-Path $source "Skull.png"),
    "-vf", "scale=1280:1774:flags=lanczos,crop=1280:800:0:487,format=rgb24",
    "-frames:v", "1", $skullPng
)
Invoke-Ffmpeg @(
    "-i", (Join-Path $source "02_Liquidos_Hint_Belial.png"),
    "-vf", "scale=1035:800:flags=lanczos,pad=1280:800:122:0:black,format=rgb24",
    "-frames:v", "1", $hintPng
)

foreach ($name in @("base", "skull", "hint")) {
    Invoke-Ffmpeg @(
        "-i", (Join-Path $work "$name.png"),
        "-pix_fmt", "rgb565le", "-f", "rawvideo", "-frames:v", "1",
        (Join-Path $ui "$name.rgb565")
    )
}

$clips = @{
    "finger" = "finger_tap.mp3"
    "granted" = "access-granted.mp3"
    "denied" = "access-denied.mp3"
    "riser" = "riser-initial.mp3"
}
foreach ($entry in $clips.GetEnumerator()) {
    Invoke-Ffmpeg @(
        "-i", (Join-Path $source $entry.Value),
        "-ac", "1", "-ar", "16000", "-sample_fmt", "s16",
        "-f", "s16le", (Join-Path $audio "$($entry.Key).pcm")
    )
}

Get-ChildItem $ui, $audio -File | Select-Object FullName, Length

