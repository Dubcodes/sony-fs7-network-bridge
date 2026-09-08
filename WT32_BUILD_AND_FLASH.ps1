param(
    [string]$Port = "",
    [switch]$Upload,
    [switch]$Monitor
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

$script:PioExe = $null
$script:PioViaPython = $false

if (Get-Command pio -ErrorAction SilentlyContinue) {
    $script:PioExe = (Get-Command pio).Source
} else {
    $DefaultPio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"
    if (Test-Path $DefaultPio) {
        $script:PioExe = $DefaultPio
    } else {
        python -m platformio --version *> $null
        if ($LASTEXITCODE -eq 0) {
            $script:PioViaPython = $true
        } else {
            throw "PlatformIO not found. Expected pio on PATH, $DefaultPio, or python -m platformio."
        }
    }
}

function Invoke-Pio {
    param([string[]]$ArgsList)
    if ($script:PioViaPython) {
        & python -m platformio @ArgsList
    } else {
        & $script:PioExe @ArgsList
    }
    if ($LASTEXITCODE -ne 0) { throw "PlatformIO failed: $($ArgsList -join ' ')" }
}

Write-Host "=== Static validation ==="
python tools\validate_project.py
if ($LASTEXITCODE -ne 0) { throw "Project validator failed" }
python tools\validate_wt32_ota.py
if ($LASTEXITCODE -ne 0) { throw "WT32/OTA validator failed" }

Write-Host "=== Host tests ==="
python -m unittest discover -s tests -v
if ($LASTEXITCODE -ne 0) { throw "Host tests failed" }

Write-Host "=== WT32 build ==="
Invoke-Pio -ArgsList @("run", "-e", "wt32-eth01")

$Firmware = Join-Path $Root ".pio\build\wt32-eth01\firmware.bin"
if (!(Test-Path $Firmware)) { throw "Build succeeded but firmware.bin was not found" }
$Size = (Get-Item $Firmware).Length
Write-Host "firmware.bin: $Firmware"
Write-Host "firmware.bin bytes: $Size"

if ($Upload) {
    if ([string]::IsNullOrWhiteSpace($Port)) {
        throw "Supply -Port COMx when using -Upload. Put GPIO0 to GND and reset/power the WT32 into its bootloader first."
    }
    Write-Host "=== Serial upload to $Port ==="
    Write-Host "GPIO0 must be LOW during reset to enter the ROM bootloader."
    Invoke-Pio -ArgsList @("run", "-e", "wt32-eth01", "-t", "upload", "--upload-port", $Port)
    Write-Host "Upload complete. Power off, REMOVE GPIO0 from GND, then power on normally."
}

if ($Monitor) {
    if ([string]::IsNullOrWhiteSpace($Port)) { throw "Supply -Port COMx when using -Monitor" }
    Write-Host "=== Serial monitor $Port @ 115200 ==="
    Invoke-Pio -ArgsList @("device", "monitor", "--port", $Port, "--baud", "115200")
}
