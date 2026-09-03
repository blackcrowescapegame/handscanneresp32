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
        $reportedFailure = $false
        & pio @arguments 2>&1 | ForEach-Object {
            $line = $_.ToString()
            if ($line -match 'Could Not Activate|OTA: failed|Unexpected response') {
                $reportedFailure = $true
            }
            Write-Output $line
        }
        $exitCode = $LASTEXITCODE
        if ($reportedFailure -and $exitCode -eq 0) {
            $exitCode = 1
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
