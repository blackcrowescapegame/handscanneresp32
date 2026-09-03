param(
    [Parameter(Position = 0)]
    [string]$Target = "192.168.40.57",

    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$secretsPath = Join-Path $root "include\secrets.h"
$source = Get-Content -LiteralPath $secretsPath -Raw
$match = [regex]::Match(
    $source,
    '(?m)^\s*#define\s+HANDSCANNER_OTA_PASSWORD\s+"([^"]+)"'
)

if (-not $match.Success) {
    throw "HANDSCANNER_OTA_PASSWORD is missing or empty in $secretsPath"
}

$previousPassword = $env:HANDSCANNER_OTA_PASSWORD
try {
    # Keep the credential process-local. The PlatformIO post script also strips
    # espota's debug flag so the password is never echoed to the console.
    $env:HANDSCANNER_OTA_PASSWORD = $match.Groups[1].Value
    Push-Location $root
    try {
        $arguments = @("run", "-e", "waveshare-esp32-p4-10-1-ota")
        if (-not $BuildOnly) {
            $arguments += @("-t", "upload", "--upload-port", $Target)
        }
        $previousErrorActionPreference = $ErrorActionPreference
        try {
            # Windows PowerShell 5 wraps any native stderr line (including
            # compiler warnings) in a NativeCommandError when the global
            # preference is Stop. Let PlatformIO stream directly to the console
            # so espota's carriage-return progress animation remains visible.
            $ErrorActionPreference = "Continue"
            & pio @arguments
            $exitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        exit $exitCode
    }
    finally {
        Pop-Location
    }
}
finally {
    $env:HANDSCANNER_OTA_PASSWORD = $previousPassword
}
