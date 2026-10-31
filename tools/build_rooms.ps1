# Builds the wider rooms from the 2x paintings, after build_screens.ps1: <game>\lords2hd\wide\<name>.bin
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
Add-Type -Path (Join-Path $PSScriptRoot "Room.cs")
[Room]::Armoury((Join-Path $game "lords2hd\screens\armoury.bin"), (Join-Path $game "lords2hd\wide\armoury.bin"))
Get-ChildItem (Join-Path $game "lords2hd\wide")
