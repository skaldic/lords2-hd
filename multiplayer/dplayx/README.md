# dplayx.dll - drop-in DirectPlay for Lords of the Realm II (original Lords2.exe)

Our own implementation of the 2 exports and the IDirectPlay2 subset the game uses, over plain TCP.
Put `dplayx.dll` next to `Lords2.exe`. Host: pick "TCP/IP" -> Create. Joiner: pick "TCP/IP" -> Connect and type the
host's address (saved as `host=` in `lords2hd.ini [Multiplayer]`; env `LOTR2NET_HOST` overrides). Host needs TCP 47700 reachable.
Settings: `[Multiplayer]` in `lords2hd.ini` beside the exe (shared with the HD ddraw; env `LOTR2NET_*` overrides). A
`lotr2net.ini` from before v12 is moved in at the first start and renamed `lotr2net.ini.old`.
Under Wine: `WINEDLLOVERRIDES=dplayx=n`. Build: `bash build.sh`. Details, traps, test recipe: `AGENTS.md`.
Ships no Sierra bytes. Log: `dplayx.log` beside the exe.
Exe timing fixes: `mpfix` is ON by default since v5 (`MPFIX_DEFAULT` in exepatch.c); `mpfix=0` in `lords2hd.ini` (or env `LOTR2NET_MPFIX=0`) turns it off, on BOTH peers.
Peers must run the same build, the same exe, the same `LOTR2NET_SLOWLINK` and the same mpfix setting (the handshake PROTO number encodes all four).
Diagnostics (v12, `diag.c`): one `DIAG` line per second and one `DIAG PEER` line per peer in `dplayx.log`: sim ticks, game-loop timing, time held at the hard stop, queue delay and a measured round trip. The pings run between the DLLs and never reach the game. `diag=0` in `lords2hd.ini` (or env `LOTR2NET_DIAG=0`) turns them off; `log_heartbeats=1` (or env `LOTR2NET_LOG_HEARTBEATS=1`) brings back the per-message heartbeat lines, off since v12. The fields are explained at the top of `diag.c`.
Session logs (v12, `sesslog.c`): one folder per game, `lords2hd-mp\<date>_<time>\` beside the exe, shared with the HD ddraw. Once a second player is there, `dplayx.log` and `desync.txt` go into it, and `status.txt` and `lords2hd.log` are copied in; the HD ddraw adds `session.txt` and `sync.txt` (export `LF_NetLogSession`, which also makes the folder for a host with no joiner). The 10 newest folders are kept. `MpLogs=0` in `lords2hd.ini` (or env `LOTR2NET_MP_LOGS=0`) keeps everything beside the exe.
Datagrams are limited to 260 bytes (the game's receive buffer). Sends time out after 5 s and keepalive drops dead peers in about 10 s.
