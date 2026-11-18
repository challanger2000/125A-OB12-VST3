# Print flash strings and Master bytes after a key sequence, per step. Args: run dir, step seconds, first.
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
        s = p[3].rstrip('\n').strip()
        if re.search(r'[A-Za-z]{4}|[A-Z][a-z]+ |[A-Z]+[ -][A-Z]+', s) and not re.match(r'^M?L', s) and s not in T[i]:
            T[i].append(s)
    elif p[1] == 'M':
        M[i].append(p[2])
for i in sorted(set(T) | set(M)):
    print(i, ' '.join(M[i])[:60], '|', T[i][:12])
