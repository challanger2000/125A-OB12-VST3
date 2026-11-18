# What each panel key did: new flash strings and Master to Slave bytes per step. Args: run dir, step s, first s.
import collections, re, sys
d, step, first = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
T = collections.defaultdict(list)
M = collections.defaultdict(list)
for line in open('build/%s/host.txt' % d, errors='replace'):
    p = line.split(None, 3)
    t = float(p[0])
    if t < first:
        continue
    i = int((t - first) / step)
    if p[1] == 'S':
        a, text = int(p[2], 16), p[3].rstrip('\n').strip()
        if re.search(r'[A-Za-z]{3}', text) and text not in T[i]:
            T[i].append(text)
    elif p[1] == 'M':
        M[i].append(p[2])
for i in sorted(set(T) | set(M)):
    print('%3d row %X bit %d' % (i, i // 8, i % 8), ' '.join(M[i])[:40], '|', T[i][:8])
