# mpwindow - a multiplayer game never stalls on one window (multiplayer module)

Lockstep multiplayer waits for the slowest peer. In a session (lobby or game) this module keeps the game running
while unfocused or minimised, whatever `[display] background` says, and moves / sizes the window itself while its
title bar or border is dragged (Windows' own move loop stops the game loop: 0 frames in 3 s, so every peer
stalled). Outside a session the normal drag (Aero snap) stays.

`mpwindow.c` only. No exe patch: window messages, and a read of 0x553030 (session flag) or our dplayx.dll's
LF_NetSessionOpen export. Needs: core.
