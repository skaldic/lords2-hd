# dplayx.dll - drop-in DirectPlay for Lords of the Realm II (original Lords2.exe)

Our own implementation of the 2 exports and the IDirectPlay2 subset the game uses, over plain TCP.
Put `dplayx.dll` next to `Lords2.exe`. Host: pick "TCP/IP" -> Create. Joiner: set `LOTR2NET_HOST=<host ip/name>` or
a `lotr2net.ini` beside the exe containing `host=<ip>`, pick "TCP/IP" -> Connect. Host needs TCP 47700 reachable.
Under Wine: `WINEDLLOVERRIDES=dplayx=n`. Build: `bash build.sh`. Details, traps, test recipe: `AGENTS.md`.
Ships no Sierra bytes. Log: `dplayx.log` beside the exe.
Exe timing fixes: `mpfix` is ON by default since v5 (`MPFIX_DEFAULT` in exepatch.c); `mpfix=0` in `lotr2net.ini` (or env `LOTR2NET_MPFIX=0`) turns it off, on BOTH peers.
Peers must run the same build, the same exe, the same `LOTR2NET_SLOWLINK` and the same mpfix setting (the handshake PROTO number encodes all four).
Diagnostics (v12, `diag.c`): one `DIAG` line per second and one `DIAG PEER` line per peer in `dplayx.log`: sim ticks, game-loop timing, time held at the hard stop, queue delay and a measured round trip. The pings run between the DLLs and never reach the game. `diag=0` in `lotr2net.ini` (or env `LOTR2NET_DIAG=0`) turns them off; `log_heartbeats=1` (or env `LOTR2NET_LOG_HEARTBEATS=1`) brings back the per-message heartbeat lines, off since v12. The fields are explained at the top of `diag.c`.
Session logs (v12, `sesslog.c`): once a second player is there, `dplayx.log` and `desync.txt` go to `mp_logs\<direct|gameranger>_<date>_<time>\` beside the exe, and `status.txt`, `lotr2fix-ddraw.log` and the Mod Menu's logs for that run are copied in. `mp_logs=0` in `lotr2net.ini` (or env `LOTR2NET_MP_LOGS=0`) keeps everything beside the exe. The 10 newest folders are kept. The HD ddraw asks for the current folder (export `LF_NetLogDir`) and copies `dplayx.log` and `desync.txt` from it into its own `lords2hd-mp\` folder.
Datagrams are limited to 260 bytes (the game's receive buffer). Sends time out after 5 s and keepalive drops dead peers in about 10 s.
