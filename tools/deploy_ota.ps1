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

$platformioPath = Join-Path $root "platformio.ini"
$platformioSource = Get-Content -LiteralPath $platformioPath -Raw
$versionMatch = [regex]::Match(
    $platformioSource,
    '(?m)^\s*custom_firmware_version\s*=\s*([^\s;]+)'
)
if (-not $versionMatch.Success) {
    throw "custom_firmware_version is missing in $platformioPath"
}
$expectedVersion = $versionMatch.Groups[1].Value

$previousPassword = $env:HANDSCANNER_OTA_PASSWORD
try {
    # Keep the credential process-local. The PlatformIO post script also strips
    # espota's debug flag so the password is never echoed to the console.
    $env:HANDSCANNER_OTA_PASSWORD = $match.Groups[1].Value
    Push-Location $root
    try {
        $arguments = @("run", "-e", "waveshare-esp32-p4-10-1-ota")
        $beforeHealth = $null
        if (-not $BuildOnly) {
            $arguments += @("-t", "upload", "--upload-port", $Target)
            try {
                $beforeHealth = Invoke-RestMethod -Uri "http://$Target/api/health" -TimeoutSec 3
            }
            catch {
                # Deployment can still proceed when the pre-flight health request is unavailable.
            }
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
        if ($exitCode -ne 0 -or $BuildOnly) {
            exit $exitCode
        }

        Write-Host "Verifying firmware $expectedVersion at http://$Target/api/health ..."
        $deadline = (Get-Date).AddSeconds(60)
        $lastHealth = $null
        while ((Get-Date) -lt $deadline) {
            try {
                $lastHealth = Invoke-RestMethod -Uri "http://$Target/api/health" -TimeoutSec 3
                $versionChanged = $null -eq $beforeHealth -or $beforeHealth.version -ne $expectedVersion
                $partitionChanged = $null -ne $beforeHealth -and `
                    $beforeHealth.ota_partition -ne $lastHealth.ota_partition
                if ($lastHealth.version -eq $expectedVersion -and `
                    ($versionChanged -or $partitionChanged)) {
                    Write-Host "OTA verified: firmware $($lastHealth.version), partition $($lastHealth.ota_partition)"
                    exit 0
                }
            }
            catch {
                # A short connection failure is expected while the device reboots.
            }
            Start-Sleep -Seconds 2
        }

        if ($null -ne $lastHealth) {
            throw "OTA was not activated. Expected firmware $expectedVersion but device reports $($lastHealth.version) (OTA state: $($lastHealth.ota_state), Arduino error: $($lastHealth.ota_error), updater error: $($lastHealth.ota_update_error) $($lastHealth.ota_update_error_message))."
        }
        throw "OTA upload finished, but the health API did not return within 60 seconds."
    }
    finally {
        Pop-Location
    }
}
finally {
    $env:HANDSCANNER_OTA_PASSWORD = $previousPassword
}
