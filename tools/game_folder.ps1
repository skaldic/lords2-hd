# Finds the game folder (the one with Lords2.exe) for the build and test scripts. Dot-source it:
#   . "$PSScriptRoot\game_folder.ps1"; $game = Get-GameFolder $Game
# The first of these that holds Lords2.exe wins:
#   1. the folder passed in (a script's -Game)
#   2. the L2HD_GAME environment variable
#   3. the first line of .game-folder in the repo's root (git-ignored: each machine writes its own)
#   4. the repo's parent folder (the repo used to live in <game>\decomp)
function Get-GameFolder([string]$Given = "") {
    $repo = Split-Path $PSScriptRoot -Parent
    $file = Join-Path $repo ".game-folder"
    $tried = @()
    $candidates = @(
        @{ From = "-Game"; Path = $Given },
        @{ From = "L2HD_GAME"; Path = $env:L2HD_GAME },
        @{ From = ".game-folder"; Path = $(if (Test-Path $file) { (Get-Content $file -TotalCount 1).Trim() } else { "" }) },
        @{ From = "the repo's parent"; Path = (Split-Path $repo -Parent) }
    )
    foreach ($c in $candidates) {
        if (-not $c.Path) { continue }
        if (Test-Path (Join-Path $c.Path "Lords2.exe")) { return (Resolve-Path $c.Path).Path }
        $tried += "$($c.From): $($c.Path)"
        if ($c.From -ne "the repo's parent") { break }   # a folder given on purpose that is wrong is an error, not a reason to guess
    }
    throw ("No Lords2.exe found (tried " + ($tried -join "; ") + "). Pass -Game <the game folder>, set L2HD_GAME, " +
        "or write the game folder's path in $file")
}
