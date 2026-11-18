# Writes study scripts for OB12_SLAVE_FILE: program bytes and A8 parameters over small values.
import re
d = open('F:/Claude/OB-12/build/c/build/live_master_to_slave.bin', 'rb').read()
m = re.search(rb'\xf0\x7e\x04.*?\xf7', d, re.S).group()
for v in (0, 2, 4, 5):
    lines = []
    for k in range(3, len(m) - 1):
        t = 13 + (k - 3) * .6
        a = bytearray(m)
        a[k] = v
        lines += ['%.2f %s' % (t, a.hex(' ')), '%.2f 94 3c 64' % (t + .1), '%.2f 84 3c 40' % (t + .45), '%.2f %s' % (t + .5, m.hex(' '))]
    open('F:/Claude/OB-12/build/bv%d.txt' % v, 'w').write('\n'.join(lines) + '\n')
lines = ['13.00 94 3c 64']
t = 13.1
for v in (1, 2, 4, 5):
    for n in range(128):
        lines.append('%.2f a8 %02x %02x' % (t, n, v))
        t += .05
open('F:/Claude/OB-12/build/ba8.txt', 'w').write('\n'.join(lines) + '\n')
print('a8 end', t)
