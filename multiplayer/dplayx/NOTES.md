# NOTES - first proving run, 2026-10-05

- 22:44 built dplayx.dll (234 KB, exports ord1/ord2) first try, no code fixes needed.
- Booted lotr2h-dp1/dp2 (views 6084/6085, net lotr2mp), test_up.sh pushed DLL, game relaunched with dplayx=n.
- Lobby: only deviation = TCP/IP row at y=153 (our list is shorter than Wine's). Both reached setup screen first try.
- Start -> both in game, 0x553240 = 1 on both. 4 End-turn rounds -> 2,3,4,5 on both, in lockstep. "too late" count 0 on both.
- Host log: Open(create), CreatePlayer id 0x1000, enum answered, joiner conn, player 0x1001, SetSessionDesc lock.
  ~2900 MSG lines host, ~1970 joiner. No UNSUPPORTED call logged by either peer.
- Version 99 hello over a raw socket: REJECT 0x887700FA, host logged "version mismatch".
- Killed the joiner game: host logged SYS type=5 id=0x1001, "conn 0 dropped", game received it (MSG RECV from=0 size=44 b4=05).
- Trap found: docker cp cannot copy from tmpfs; logs were lost on rm. Evidence = console excerpts + screenshots in
  .workers/out/container-harness/dp/ (a1-a6 host lobby, b1-b3 joiner, c1/c2 in-game, d1/d2 after turn 5).
