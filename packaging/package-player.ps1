# Builds dist/communitygarage-player.zip: what a player extracts into their Azimuth folder.
$root = Split-Path $PSScriptRoot -Parent
$stage = Join-Path $env:TEMP 'ct-player-package'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $stage, (Join-Path $stage 'fh2-mods'), (Join-Path $stage 'communitygarage'), (Join-Path $stage 'licenses') | Out-Null
Copy-Item (Join-Path $root 'build\CommunityGarage.dll') (Join-Path $stage 'fh2-mods') -ErrorAction Stop
# The platform DLL is built as d3dcompiler_47.dll; Azimuth 1.0.1+ loads it as DWrite.dll, so ship it under that name.
Copy-Item (Join-Path $root 'build\platform\d3dcompiler_47.dll') (Join-Path $stage 'DWrite.dll') -ErrorAction Stop
Copy-Item (Join-Path $root 'packaging\templates\player-server.ini') (Join-Path $stage 'communitygarage\server.ini') -ErrorAction Stop
Copy-Item (Join-Path $root 'packaging\templates\PLAYER-README.txt') (Join-Path $stage 'README.txt') -ErrorAction Stop
Copy-Item (Join-Path $root 'docs\TERMS.md') $stage -ErrorAction Stop
Copy-Item (Join-Path $root 'LICENSE') $stage -ErrorAction Stop
Copy-Item (Join-Path $root 'platform\LICENSE') (Join-Path $stage 'licenses\FH2-Mod-Platform-LICENSE.txt') -ErrorAction Stop
$zip = Join-Path $root 'dist\communitygarage-player.zip'
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "wrote $zip"


