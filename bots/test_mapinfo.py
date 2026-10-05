"""STEP 12J unit test of make_mapinfo.py's T approaches on a synthetic nav (no game files needed).
python3 test_mapinfo.py -> ALL PASS

The map: T spawn on the left (x=0), one bomb site on the right (x=3000). Three corridors lead there - north
("LongA", y=+1000), straight through the middle ("Middle", y=0, the shortest) and south ("Tunnels", y=-1000) - separated
by walls; the corridors meet a spawn column (x<=200) and a site column (x>=2800)."""
import sys

import make_mapinfo as mm

FAIL = []


def check(ok, what):
    print('%s %s' % ('PASS' if ok else 'FAIL', what))
    if not ok: FAIL.append(what)


def synthetic():
    cell = 100.0
    areas, grid = [], {}
    nid = 1
    for ix in range(0, 31):
        for iy in range(-12, 13):
            x, y = ix * cell, iy * cell
            if 2 < ix < 28 and iy not in (-10, -9, 0, 1, 9, 10):
                continue   # walls between the three corridors
            a = mm.Area()
            a.id = nid
            nid += 1
            a.nw, a.se = (x - 50, y - 50, 0.0), (x + 50, y + 50, 0.0)
            a.c = (x, y, 0.0)
            a.hide = []
            a.conn = []
            a.occ = (x / 250.0, 1000.0)   # the Ts get everywhere first (no enemy ground)
            if ix <= 2: a.place = 'TSpawn'
            elif ix >= 28: a.place = 'BombsiteA'
            elif iy >= 9: a.place = 'LongA'
            elif iy <= -9: a.place = 'Tunnels'
            else: a.place = 'Middle'
            grid[(ix, iy)] = a
            areas.append(a)
    for (ix, iy), a in grid.items():
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            b = grid.get((ix + dx, iy + dy))
            if b: a.conn.append(b.id)
    return areas, grid


def main():
    areas, grid = synthetic()
    M = mm.Map(areas)
    ta = grid[(0, 0)]
    out = mm.t_approaches(M, ta, 'A', (3000.0, 0.0, 0.0), 450.0)
    appr = [l.split() for l in out if l.startswith('appr ')]
    print('\n'.join(l for l in out if not l.startswith('apwp')))
    names = [a[3] for a in appr]
    check(len(appr) == 3, 'three approaches found (%d)' % len(appr))
    check(appr and appr[0][3] == 'Middle' and appr[0][4] == '1', 'approach 0 = the shortest, through the middle, flagged mid')
    check('LongA' in names and 'Tunnels' in names, 'the other two are the north and south corridors: %s' % names)
    check(all(a[4] == '0' for a in appr if a[3] != 'Middle'), 'only the middle one is flagged mid')
    for k in range(len(appr)):
        for key in ('apstk', 'apchk', 'apent'):
            check(sum(1 for l in out if l.startswith('%s A %d ' % (key, k))) == 1, '%s line for approach %d' % (key, k))
        check(sum(1 for l in out if l.startswith('apwp A %d ' % k)) >= 2, 'waypoints for approach %d' % k)
    # an old plugin reads these files: its parser (sscanf "%*s %31s %f %f %f") must not take any new line as a known key
    known = ('site', 'entry', 'choke', 'stack', 'retake', 'lurk', 'hold', 'post', 'radius', 'rot', 'kind', 'tspawn', 'ctspawn', 'mid',
             'rescue', 'occgrid', 'occ', 'spot')
    check(all(l.split()[0] not in known for l in out), 'no new line reuses an old key')
    mids = mm.mid_areas(areas)
    check(mids and all(l.split()[1] == 'Middle' for l in mids), 'midarea lines = the Middle areas (%d)' % len(mids))
    # a map with only one way in keeps one approach
    one = [a for a in areas if a.place != 'LongA' and a.place != 'Tunnels']
    for a in one:
        a.conn = [j for j in a.conn if any(b.id == j for b in one)]
    M1 = mm.Map(one)
    out1 = mm.t_approaches(M1, ta, 'A', (3000.0, 0.0, 0.0), 450.0)
    check(sum(1 for l in out1 if l.startswith('appr ')) == 1, 'one corridor -> one approach')
    print('ALL PASS' if not FAIL else 'SOME FAILED (%d)' % len(FAIL))
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
