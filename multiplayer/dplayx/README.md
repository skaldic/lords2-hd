# dplayx.dll - drop-in DirectPlay for Lords of the Realm II (original Lords2.exe)

Our own implementation of the 2 exports and the IDirectPlay2 subset the game uses, over plain TCP.
Put `dplayx.dll` next to `Lords2.exe`. Host: pick "TCP/IP" -> Create. Joiner: set `LOTR2NET_HOST=<host ip/name>` or
a `lotr2net.ini` beside the exe containing `host=<ip>`, pick "TCP/IP" -> Connect. Host needs TCP 47700 reachable.
Under Wine: `WINEDLLOVERRIDES=dplayx=n`. Build: `bash build.sh`. Details, traps, test recipe: `AGENTS.md`.
Ships no Sierra bytes. Log: `dplayx.log` beside the exe.
Exe timing fixes: `mpfix` is ON by default since v5 (`MPFIX_DEFAULT` in exepatch.c); `mpfix=0` in `lotr2net.ini` (or env `LOTR2NET_MPFIX=0`) turns it off, on BOTH peers.
Peers must run the same build, the same exe, the same `LOTR2NET_SLOWLINK` and the same mpfix setting (the handshake PROTO number encodes all four).
Datagrams are limited to 260 bytes (the game's receive buffer). Sends time out after 5 s and keepalive drops dead peers in about 10 s.
