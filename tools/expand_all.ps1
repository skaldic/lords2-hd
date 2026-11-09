# Paints the sides of every widened screen with tools\expand_sides.ps1 (FLUX.1 Fill), -Count variants
# each, from -Seed on. About 10 minutes to load the model per screen plus 4 minutes per variant.
#   .\tools\expand_all.ps1 -Only merchant,custom -Seed 2 -Count 2
[CmdletBinding(PositionalBinding = $false)]
param([string]$Only = "", [int]$Seed = 1, [int]$Count = 2)
$style = "1990s pre-rendered 3D computer game art."
$prompts = [ordered]@{
    merchant = "A medieval village market yard in daylight, $style The scene continues naturally and simply: to the left open packed-earth ground and a green meadow with distant hills under a blue sky with clouds; to the right the timber-framed house wall and the wooden cart continue. Uncluttered, no extra people or animals, no fences."
    custom   = "A game menu background made of flat weathered beige sandstone slabs with thin dark grooves between them. The same plain smooth sandstone surface continues to the left and right edges, evenly lit, no objects, no carvings, no veins."
    skirmish = "A game menu background of blue-grey slate stone slabs with fine pale scratches and thin grooves between the slabs. The same plain blue-grey slate surface continues to the left and right edges, evenly lit, no objects, no text."
    skircust = "A game menu background of blue-grey slate stone slabs with fine pale scratches and thin grooves between the slabs. The same plain blue-grey slate surface continues to the left and right edges, evenly lit, no objects, no text."
    cas_back = "A wall of dark grey rough-hewn stone bricks laid in even horizontal courses, a game menu background. The same dark brick wall continues to the left and right edges with bricks of the same size and colour, evenly lit, no objects, no windows."
    score1   = "A torn yellow parchment sheet lying on a grey-green weathered stone wall carved with faint bas-relief medieval figures and towers. The same soft low-contrast grey-green carved stone continues to the left and right edges, no text."
    score2   = "A pale teal-grey weathered plaster wall with faint ghostly engravings of medieval soldiers and weapons. The same plain pale teal-grey rough plaster surface continues to the left and right edges, soft, low contrast, no text, no frames."
    start    = "A very dark charcoal rough rock surface, almost black, with a faint rust-brown patch in the middle. The same very dark charcoal rock continues to the left and right edges, no text, no logos, no objects."
    gateway  = "Medieval weapons and armour lying on a table in a dark room under a single spotlight, $style The table surface continues to the left and right and falls away into black shadow. Dark, no new bright objects, no text."
    armoury  = "A dark medieval castle armoury, $style A wall of grey stone blocks lit by warm torchlight, round wooden shields with crossed swords hanging under a long wooden beam, a dark ceiling above, a pale stone slab bench along the bottom. The stone wall, the beam and the stone bench continue to the left and right."
}
$names = if ($Only) { $Only -split ',' } else { $prompts.Keys }
foreach ($n in $names) {
    try { & (Join-Path $PSScriptRoot "expand_sides.ps1") -Name $n -Prompt $prompts[$n] -Seed $Seed -Count $Count }
    catch { "$n failed: $_" }
}
