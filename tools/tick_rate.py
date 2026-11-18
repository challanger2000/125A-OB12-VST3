# Slave delays against DSP ramp end times. Arguments: run directories with cells.bin of D010, D042.
import re, sys
import numpy as np
COLS = ['D010', 'D042', 'C051', 'C052', 'C054']
for d in sys.argv[1:]:
    delays = {}
    idx = pos = None
    for line in open('build/%s/r.txt' % d, errors='replace'):
        m = re.match(r'slave watch (\S+) idx (\S+) pc (\S+) W (\S+) (\S+)', line)
        if not m:
            continue
        pc, addr, val = m.group(3), int(m.group(4), 16), int(m.group(5), 16)
        if pc == 'FC441C':
            pos, idx, slot = addr - 0xEF7A9E, int(m.group(2), 16), val
        elif pc == 'FC442A':
            delays.setdefault(slot, []).append(val * 256 + pos - idx)
    x = np.fromfile('build/%s/build/cells.bin' % d, dtype='<i4').reshape(-1, len(COLS))
    out = []
    for c, name in enumerate(COLS):
        v = x[:, c].astype(np.int64)
        ch = np.nonzero(np.diff(v))[0]
        if len(ch) == 0:
            out.append(name + ' still')
            continue
        out.append('%s ends %.4f s' % (name, (ch[-1] - ch[0]) / 48000))
    print(d, {k: v[-3:] for k, v in sorted(delays.items())}, out)
