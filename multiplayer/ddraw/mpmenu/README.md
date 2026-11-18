# mpmenu - Multiplayer inside the title menu (multiplayer module)

The title's Multiplayer button opens our screens instead of Windows dialogs: Create a game, Search local games
(LAN / Hamachi / VPN broadcast), Connect by address (Host / Port typed in place), all drawn with the game's own
panel, font and buttons. Also the in-game "Exit the game?" screen when Escape is pressed on the title (escape
module).

`mpmenu.c` only. Patches (prologue jumps, bytes checked first): 0x41eaa3 (title buttons renderer) and 0x41ea14
(title panel renderer). Input through the core's frame hook and the window's keys; answers the game's own
Multiplayer dialogs 0x6c / 0x74 in DialogBoxParamA. The Multiplayer screens need our dplayx.dll (its LF_NetMenu /
LF_NetFind exports); without it the stock dialogs run and only the Exit screen is ours. Needs: core.

The technique (redraw the title buttons through the game's own routines and take their clicks) was learned from
"Jen With a B"'s Lords II Mod Menu v1.1 - credit to Jen. Its title hooks are the same two functions Jen's Mod
Menu hooks, so the two cannot both be active.
