# teensy/mirror_relay/build.ps1 - compile (and with -Upload, flash) the mirror relay. Windows, Arduino IDE 2's arduino-cli.
# The relay's headers come from pi/engine (relay_proto.h, mirror_desc.h): one protocol file for both sides.
param([switch]$Upload)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$eng = (Resolve-Path "$here\..\..\pi\engine").Path
$cli = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$bp = Join-Path $env:TEMP "mirror_relay-build"
# the build stamp the relay logs at start (git short hash, + when the tree has changes)
$sha = (git -C $here rev-parse --short HEAD).Trim()
if (git -C $here status --porcelain -- . ../../pi/engine/relay_proto.h ../../pi/engine/mirror_desc.h) { $sha += '+' }
& $cli compile --fqbn teensy:avr:teensy41:usb=serial,opt=o2std --build-path $bp `
	--build-property "build.flags.cpp=-std=gnu++17 -fno-exceptions -fpermissive -fno-rtti -fno-threadsafe-statics -felide-constructors -Wno-error=narrowing -Wno-psabi -Wno-maybe-uninitialized -Wall -I$eng -DRELAY_BUILD=\`"$sha\`"" $here
if ($LASTEXITCODE) { exit $LASTEXITCODE }
(Get-FileHash "$bp\mirror_relay.ino.hex" -Algorithm SHA256).Hash
if ($Upload) { & $cli upload --fqbn teensy:avr:teensy41:usb=serial --input-dir $bp -p usb:0 $here }
