#!/usr/bin/env python3
"""Rank writable globals in UnityPlayer.dll by RIP-relative reference count.

The GameObjectManager singleton is a pointer in a writable section read by many
engine functions, so it should rank high. Candidates are evidence for dynamic
validation, not proof.
"""
import struct
import sys
from collections import Counter
from pathlib import Path

def main():
    path = Path(sys.argv[1] if len(sys.argv) > 1 else r"F:\Game Dev\Rob Run\UnityPlayer.dll")
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    sec_at = pe + 24 + opt
    sections = []
    for i in range(nsec):
        at = sec_at + i * 40
        name = data[at:at + 8].rstrip(b"\0").decode("latin1")
        vsize, rva, rawsize, raw = struct.unpack_from("<IIII", data, at + 8)
        chars = struct.unpack_from("<I", data, at + 36)[0]
        sections.append({"name": name, "rva": rva, "vsize": vsize,
                         "raw": raw, "rawsize": rawsize, "chars": chars})

    def file_off(rva):
        for s in sections:
            if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["rawsize"]):
                return s["raw"] + (rva - s["rva"])
        return None

    def section_of(rva):
        for s in sections:
            if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["rawsize"]):
                return s
        return None

    writable = [s for s in sections if s["chars"] & 0x80000000 and not s["chars"] & 0x20000000]
    votes = Counter()
    execs = [s for s in sections if s["chars"] & 0x20000000]
    for s in execs:
        start = s["raw"]
        end = min(len(data), s["raw"] + s["rawsize"])
        rva = s["rva"]
        i = start
        while i + 7 <= end:
            b0 = data[i]
            if b0 in (0x48, 0x4C) and data[i + 1] in (0x8B, 0x8D, 0x8A) and (data[i + 2] & 0xC7) == 0x05:
                disp = struct.unpack_from("<i", data, i + 3)[0]
                instr_rva = rva + (i - start)
                target = instr_rva + 7 + disp
                sec = section_of(target)
                if sec is not None and (sec["chars"] & 0x80000000) and not (sec["chars"] & 0x20000000):
                    votes[target] += 1
                i += 7
                continue
            i += 1

    print(f"image base 0x{base:X}; executable sections: {[s['name'] for s in execs]}")
    print(f"writable sections: {[s['name'] for s in writable]}")
    out_rvas = []
    for target, n in votes.most_common(300):
        sec = section_of(target)
        out_rvas.append(target)
        print(f"rva=0x{target:08X} votes={n:5d} section={sec['name']}")
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else path.parent / "gomcands.txt"
    out.write_text("\n".join(f"0x{r:X}" for r in out_rvas) + "\n", encoding="ascii")
    print(f"wrote {len(out_rvas)} candidates to {out}")

    # Reference-site dump for a specific global, to recover the container layout.
    if len(sys.argv) > 3:
        want = int(sys.argv[3], 0)
        refs = []
        for s in execs:
            start = s["raw"]
            end = min(len(data), s["raw"] + s["rawsize"])
            rva = s["rva"]
            i = start
            while i + 7 <= end:
                b0 = data[i]
                if b0 in (0x48, 0x4C) and data[i + 1] in (0x8B, 0x8D, 0x8A) and (data[i + 2] & 0xC7) == 0x05:
                    disp = struct.unpack_from("<i", data, i + 3)[0]
                    instr_rva = rva + (i - start)
                    target = instr_rva + 7 + disp
                    if target == want:
                        nxt = data[i + 7:i + 7 + 24]
                        refs.append((instr_rva, nxt))
                    i += 7
                    continue
                i += 1
        print(f"references to 0x{want:X}: {len(refs)}")
        for r, nxt in refs[:40]:
            print(f"  at 0x{r:08X}: " + " ".join(f"{b:02X}" for b in nxt))

if __name__ == "__main__":
    main()
