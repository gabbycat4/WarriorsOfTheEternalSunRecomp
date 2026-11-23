#!/usr/bin/env python3
"""wotes_probe.py - snapshot a running WotESRecomp through its debug port.

Run while the game is open (normal or interpreter mode), e.g. while it is stuck:
    python3 tools/wotes_probe.py            # port 4400
Writes build/probe-YYYYmmdd-HHMMSS.json with: frame info, registers, the recent
routine trail, the Z80 (sound CPU) state and RAM, all 64 KB of 68000 RAM read twice
one second apart, and the recent RAM-write history (who wrote what, when).
Read-only: it changes nothing in the game.
"""
import json, socket, sys, time, os

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4400
_id = 0

def cmd(obj):
    global _id
    _id += 1
    obj = dict(obj, id=_id)
    s = socket.create_connection(("127.0.0.1", PORT), timeout=30)
    s.sendall((json.dumps(obj) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(1 << 20)
        if not chunk:
            break
        buf += chunk
    s.close()
    txt = buf.decode(errors="replace").strip()
    try:
        return json.loads(txt)
    except Exception:
        return {"raw": txt}

def ram_dump():
    out = []
    for a in range(0xFF0000, 0x1000000, 4096):
        out.append(cmd({"cmd": "read_ram", "addr": "%06X" % a, "size": 4096}))
    return out

def z80_dump():
    out = []
    for a in range(0, 0x2000, 1024):
        out.append(cmd({"cmd": "read_z80_ram", "addr": a, "len": 1024}))
    return out

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    outdir = os.path.join(here, "..", "build")
    snap = {"port": PORT, "time": time.strftime("%Y-%m-%d %H:%M:%S")}
    print("probing port %d ..." % PORT)
    snap["ping"] = cmd({"cmd": "ping"})
    snap["frame_info_1"] = cmd({"cmd": "frame_info"})
    snap["registers_1"] = cmd({"cmd": "get_registers"})
    snap["crash_trail_1"] = cmd({"cmd": "crash_trail"})
    snap["z80_state_1"] = cmd({"cmd": "z80_state"})
    snap["z80_ram_1"] = z80_dump()
    snap["ram_1"] = ram_dump()
    time.sleep(1.0)
    snap["frame_info_2"] = cmd({"cmd": "frame_info"})
    snap["registers_2"] = cmd({"cmd": "get_registers"})
    snap["crash_trail_2"] = cmd({"cmd": "crash_trail"})
    snap["z80_state_2"] = cmd({"cmd": "z80_state"})
    snap["z80_ram_2"] = z80_dump()
    snap["ram_2"] = ram_dump()
    cnt = cmd({"cmd": "rdb_count"})
    snap["rdb_count"] = cnt
    total = 0
    for k in ("total", "count"):
        if isinstance(cnt, dict) and isinstance(cnt.get(k), int):
            total = cnt[k]
    start = max(0, total - 30000)
    snap["rdb_dump"] = cmd({"cmd": "rdb_dump", "start": start, "count": 30000})
    name = os.path.join(outdir, time.strftime("probe-%Y%m%d-%H%M%S.json"))
    with open(name, "w") as f:
        json.dump(snap, f)
    print("wrote", os.path.normpath(name))

if __name__ == "__main__":
    main()
