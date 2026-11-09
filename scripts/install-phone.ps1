# scripts/install-phone.ps1 — install the built APK on the phone and copy your game data to it, from Windows.
#
#   powershell -ExecutionPolicy Bypass -File scripts\install-phone.ps1 -Steam "<your Black Ops II folder>" [-NoData]
#
# The Windows counterpart of `build-android.sh --push-data` once the APK is built (android/app/build/outputs/apk/debug).
# Needs USB debugging on the phone (Settings > About phone > Software information: tap Build number 7 times, then
# Developer options > USB debugging) and the phone plugged in; accept the "Allow USB debugging?" prompt on it.
# Copies everything but Steam's redistributables and your *.unpacked.exe (lift inputs, they stay on this PC), plus
# android/build/game/pe_sections.bin from the lift. adb push restarts a stopped file from scratch; rerunning is safe.
param(
    [Parameter(Mandatory = $true)][string]$Steam,
    [switch]$NoData
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$pkg = 'io.github.lessunderrated.bo2'
$apk = Join-Path $repo 'android\app\build\outputs\apk\debug\app-debug.apk'
$pe = Join-Path $repo 'android\build\game\pe_sections.bin'

$sdk = $env:ANDROID_HOME
if (-not $sdk) {
    $lp = Join-Path $repo 'android\local.properties'
    if (Test-Path $lp) { $sdk = ((Get-Content $lp | Where-Object { $_ -like 'sdk.dir=*' }) -replace '^sdk.dir=', '' -replace '\\:', ':') }
}
$adb = Join-Path $sdk 'platform-tools\adb.exe'
if (-not (Test-Path $adb)) { throw "no adb at $adb (set ANDROID_HOME to your Android SDK)" }
if (-not (Test-Path $apk)) { throw "no APK at ${apk}: build it first (README.md, Build)" }
if (-not (Test-Path (Join-Path $Steam 'zone'))) { throw "$Steam has no zone folder: is it the Black Ops II install folder?" }

$devices = @(& $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\tdevice$' })
if ($devices.Count -eq 0) {
    $all = (& $adb devices | Select-Object -Skip 1 | Where-Object { $_.Trim() }) -join '; '
    throw "no phone ready on adb ($all). Plug it in, enable USB debugging, and accept the prompt on the phone"
}
if ($devices.Count -gt 1 -and -not $env:ANDROID_SERIAL) { throw "several devices: set ANDROID_SERIAL to one of: $($devices -join ', ')" }

Write-Host "installing $apk ($([math]::Round((Get-Item $apk).Length / 1MB)) MB)"
& $adb install -r $apk
if ($LASTEXITCODE -ne 0) { throw "adb install failed" }

if (-not $NoData) {
    $dest = "/sdcard/Android/data/$pkg/files/game"
    & $adb shell mkdir -p $dest
    # The mark says "whole import": it goes first and comes back only once every push succeeded.
    & $adb shell rm -f "$dest/.bo2-import-complete"
    foreach ($entry in Get-ChildItem $Steam) {
        if ($entry.Name -in 'redist', '__CommonRedist', '_CommonRedist') { continue }
        if ($entry.Name -like '*.unpacked.exe') { Write-Host "skipping $($entry.Name) (lift input, stays on this PC)"; continue }
        Write-Host "pushing $($entry.Name)"
        & $adb push $entry.FullName "$dest/" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "adb push $($entry.Name) failed" }
    }
    if (Test-Path $pe) {
        Write-Host "pushing pe_sections.bin"
        & $adb push $pe "$dest/" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "adb push pe_sections.bin failed" }
    } else {
        Write-Warning "no ${pe}: the APK has no game core to run (build-android.sh --lift)"
    }
    # adb creates the folders as `shell` with mode 2770, which the app's own user cannot enter (Android 17 on the S26).
    & $adb shell chmod -R a+rwX $dest
    & $adb shell touch "$dest/.bo2-import-complete"
    & $adb shell chmod a+rw "$dest/.bo2-import-complete"
    Write-Host "game data pushed to $dest"
}
& $adb shell am start -n "$pkg/.SetupActivity" | Out-Null
Write-Host "started. Logs: `"$adb`" logcat -s BO2"
