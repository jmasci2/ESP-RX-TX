param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("remote", "receiver")]
    [string]$Sketch
)

$ErrorActionPreference = "Stop"

$cliCommand = Get-Command arduino-cli -ErrorAction SilentlyContinue
if ($cliCommand) {
    $cliPath = $cliCommand.Source
} else {
    $candidates = @(
        (Join-Path $env:ProgramFiles "Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe")
    )
    $cliPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

if (-not $cliPath) {
    throw "arduino-cli was not found. Install Arduino IDE or add arduino-cli to PATH."
}

$workspaceRoot = Split-Path -Parent $PSScriptRoot
$sketchPath = switch ($Sketch) {
    "remote" {
        Join-Path $workspaceRoot "ESP-TX\remote"
    }
    "receiver" {
        Join-Path $workspaceRoot "ESP-RX\receiver"
    }
}

if ($Sketch -eq "remote") {
    $fqbn = "esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=huge_app,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc"
} else {
    $fqbn = "esp32:esp32:esp32"
}

$arguments = @("compile", "--fqbn", $fqbn, $sketchPath)
& $cliPath @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
