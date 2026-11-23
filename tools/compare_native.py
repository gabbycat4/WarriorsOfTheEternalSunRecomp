#!/usr/bin/env python3
"""compare_native.py BEFORE_DIR AFTER_DIR - regeneration gate (from RKA's
compare_native_coverage.py): every instruction emitted natively before must
still be emitted after. Exit 1 and list them if any were lost."""
import glob, re, sys
from pathlib import Path
def emitted(d):
    out = set()
    for f in glob.glob(str(Path(d) / '*_part*.c')):
        out.update(int(m, 16) for m in re.findall(r'/\* \$([0-9A-F]{6}) \*/', open(f).read()))
    return out
b, a = emitted(sys.argv[1]), emitted(sys.argv[2])
lost = sorted(b - a)
print('before %d after %d gained %d lost %d' % (len(b), len(a), len(a - b), len(lost)))
for pc in lost[:200]:
    print('lost $%06X' % pc)
sys.exit(1 if lost else 0)
