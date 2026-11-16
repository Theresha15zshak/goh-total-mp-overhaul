"""Carries addresses from one build of the game's exe to the next, without the old exe: only its gx index
(functions, call sites, RIP-relative references) is needed.

A function's fingerprint is its size plus the offsets of every call and data reference inside it. A relink
moves functions but leaves that alone, so old and new function lists are aligned on it (unique fingerprints
anchor, order fills the gaps). Code addresses move with their function; data addresses are voted on by
the instructions that reference them.

    python -I port.py <old.pkl> <new.pkl> <hex rva>...      one line per address: old -> new, how sure
    from port import Port; p = Port(old, new); p.map(rva) -> (new_rva or None, kind)

kind: exact (inside an unchanged function), data (voted by N references), near (no reference of its own:
moved with unchanged neighbours on both sides), changed (its function changed: look by hand), lost.
"""
import bisect, collections, difflib, pickle, sys


def _load(path):
    x = pickle.load(open(path, 'rb'))
    funcs = x['funcs']
    fb = [f[0] for f in funcs]

    def fi(r):
        i = bisect.bisect_right(fb, r) - 1
        return i if i >= 0 and funcs[i][0] <= r < funcs[i][1] else -1
    feats = [[] for _ in funcs]
    site_target = {}
    for kind, table in (('c', x['calls']), ('x', x['xref'])):
        for tgt, sites in table.items():
            for s in sites:
                site_target[(kind, s)] = tgt
                i = fi(s)
                if i >= 0:
                    feats[i].append((kind, s - funcs[i][0]))
    sig = [(funcs[i][1] - funcs[i][0], tuple(sorted(feats[i]))) for i in range(len(funcs))]
    return x, funcs, fb, fi, sig, site_target


class Port:
    def __init__(self, old, new):
        self.xo, self.fo, self.fbo, self.fio, self.so, self.sto = _load(old)
        self.xn, self.fn, self.fbn, self.fin, self.sn, self.stn = _load(new)
        self._align()
        self._data = {}

    def _align(self):
        so, sn = self.so, self.sn
        co, cn = collections.Counter(so), collections.Counter(sn)
        pos_n = {s: j for j, s in enumerate(sn) if cn[s] == 1}
        pairs = [(i, pos_n[s]) for i, s in enumerate(so) if co[s] == 1 and s in pos_n]
        # longest increasing run of j: anchors that kept their order
        tails, link, at = [], [None] * len(pairs), []
        for k, (i, j) in enumerate(pairs):
            p = bisect.bisect_left(tails, j)
            if p == len(tails):
                tails.append(j); at.append(k)
            else:
                tails[p] = j; at[p] = k
            link[k] = at[p - 1] if p else None
        keep, k = [], at[-1] if at else None
        while k is not None:
            keep.append(pairs[k]); k = link[k]
        keep.reverse()
        self.match = {}          # old index -> (new index, exact?)
        prev = (-1, -1)
        for a in keep + [(len(so), len(sn))]:
            i0, j0 = prev[0] + 1, prev[1] + 1
            i1, j1 = a
            go, gn = so[i0:i1], sn[j0:j1]
            if go and gn:
                if len(go) == len(gn):
                    for k in range(len(go)):
                        self.match[i0 + k] = (j0 + k, go[k] == gn[k])
                else:
                    sm = difflib.SequenceMatcher(None, go, gn, autojunk=False)
                    for tag, a0, a1, b0, b1 in sm.get_opcodes():
                        if tag == 'equal':
                            for k in range(a1 - a0):
                                self.match[i0 + a0 + k] = (j0 + b0 + k, True)
                        elif tag == 'replace' and a1 - a0 == b1 - b0:
                            for k in range(a1 - a0):
                                self.match[i0 + a0 + k] = (j0 + b0 + k, False)
            if i1 < len(so):
                self.match[i1] = (j1, True)
            prev = a
        self.exact = sum(1 for v in self.match.values() if v[1])

    def code(self, rva):
        i = self.fio(rva)
        if i < 0:
            return None, 'lost'
        m = self.match.get(i)
        if not m or not m[1]:
            # not aligned by order: where do the unchanged callers of this function call now?
            start, how = self.data(self.fo[i][0])
            j = self.fin(start) if start is not None and 'SPLIT' not in how else -1
            if j >= 0 and self.fn[j][0] == start:
                m = (j, self.sn[j] == self.so[i])
        if not m:
            return None, 'lost'
        j, exact = m
        new = self.fn[j][0] + (rva - self.fo[i][0])
        return (new, 'exact') if exact else (self.fn[j][0], 'changed')

    def data(self, rva):
        if rva in self._data:
            return self._data[rva]
        self._data[rva] = (None, 'lost')      # a function that calls itself must not ask about itself
        votes = collections.Counter()
        for kind, table in (('x', self.xo['xref']), ('c', self.xo['calls'])):
            for s in table.get(rva, ()):
                n, how = self.code(s)
                if how == 'exact':
                    t = self.stn.get((kind, n))
                    if t is not None:
                        votes[t] += 1
        if votes:
            (t, c), total = votes.most_common(1)[0], sum(votes.values())
            r = (t, 'data %d/%d' % (c, total)) if c * 10 >= total * 9 else (t, 'data SPLIT %s' % dict(votes.most_common(3)))
        else:
            r = (None, 'lost')
        self._data[rva] = r
        return r

    def near(self, rva, reach=0x400):
        """An address nothing references directly (a field inside a global, a table entry): the shift of the
        referenced addresses around it, if they agree on both sides."""
        if not hasattr(self, '_targets'):
            self._targets = sorted(set(self.xo['xref']) | set(self.xo['calls']))
        k = bisect.bisect_left(self._targets, rva)
        shifts = []
        for step, rng in ((-1, range(k - 1, max(k - 9, -1), -1)), (1, range(k, min(k + 8, len(self._targets))))):
            for q in rng:
                t = self._targets[q]
                if abs(t - rva) > reach:
                    break
                n, how = self.data(t)
                if n is not None and 'SPLIT' not in how:
                    shifts.append((step, n - t)); break
        if len(shifts) == 2 and shifts[0][1] == shifts[1][1]:
            return rva + shifts[0][1], 'near'
        return None, 'lost (neighbours %s)' % [hex(s[1] & 0xffffffff) for s in shifts]

    def map(self, rva):
        text_end = self.fo[-1][1]
        if self.fo[0][0] <= rva < text_end:
            n, how = self.code(rva)
            if n is not None:
                return n, how
        n, how = self.data(rva)
        if n is None:
            n, how = self.near(rva)
        return n, how


if __name__ == '__main__':
    p = Port(sys.argv[1], sys.argv[2])
    print('functions: %d old, %d new, %d carried exactly, %d changed' % (len(p.fo), len(p.fn), p.exact, len(p.match) - p.exact))
    for a in sys.argv[3:]:
        r = int(a, 16)
        n, how = p.map(r)
        print('%8x -> %8s  %s' % (r, '%x' % n if n is not None else '?', how))
