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

# Build directly for the panel's native 800x1280 portrait orientation. The hand
# keeps its aspect ratio, fits within the short edge, and is centered vertically.
Invoke-Ffmpeg @(
    "-i", (Join-Path $source "Background.png"),
    "-i", (Join-Path $source "Handprint.png"),
    "-filter_complex", "[0:v]scale=1312:880:flags=lanczos,crop=1280:800:16:23,transpose=1[bg];[1:v]scale=820:820:flags=lanczos,colorchannelmixer=aa=0.8[hand];[bg][hand]overlay=-10:230:format=auto,format=rgb24[out]",
    "-map", "[out]", "-frames:v", "1", $basePng
)

# CSS object-fit: cover for the skull and contain on black for the hint.
# The source skull is already portrait, so crop it directly to the panel's
# portrait frame. Rotating a landscape crop here made the skull appear sideways.
Invoke-Ffmpeg @(
    "-i", (Join-Path $source "Skull.png"),
    "-vf", "scale=800:1280:force_original_aspect_ratio=increase:flags=lanczos,crop=800:1280,format=rgb24",
    "-frames:v", "1", $skullPng
)
Invoke-Ffmpeg @(
    "-i", (Join-Path $source "02_Liquidos_Hint_Belial.png"),
    "-vf", "scale=1035:800:flags=lanczos,pad=1280:800:122:0:black,transpose=1,format=rgb24",
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
