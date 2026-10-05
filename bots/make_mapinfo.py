#!/usr/bin/env python3
"""STEP 9 (2026-09-24): derive the bot team-play points of every installed map from its navmesh and entities.

No hand-made per-map tables: for each maps/<map>.bsp that has a maps/<map>.nav this reads
  - the .nav (version 16): areas, their place names ("BombsiteA", "LongA", ...), connections, hiding spots;
  - the .bsp entity lump: func_bomb_target (bounds from the brush model), T / CT spawns, hostages, hostage rescue zones;
and writes csgo/addons/family_maps/<map>.txt for the family_party plugin's bot brain (plugin/family_brain.cpp):
  kind bomb|hostage            site <L> x y z              (bomb site centre, or hostage group centre)
  tspawn / ctspawn x y z       entry <L> x y z  choke <L>  (where the T route enters the site / just before it)
  stack <L> x y z              (T gathering point on the way to site L)
  hold <L> x y z   (1-4 per site, CT holding spots with cover looking at the entry, best first)
  post <L> x y z   (1-5 per site, T post-plant / hostage-guard spots)
  lurk <L> x y z   (lurker spot while the main group hits L = the choke of another site)
  retake <L> x y z (CT gathering point before a retake of L)   mid x y z (between the sites)
  rescue x y z     (hostage maps)                               radius <L> r (site region radius)
  rot <from> <L> x y z  (STEP 9b: CT rotation waypoints, in order, from site <from> or 'mid' to site L)
  occgrid / occ T|CT    (STEP 10: earliest-occupy time grid for the quiet walk, see occ_grid)
  spot x y z flags      (STEP 10b: every nav hiding spot + flags, for B5 cover, see hide_spots)
  appr / apwp / apstk / apchk / apent   (STEP 12J: up to 3 T approaches per bomb site, see t_approaches)
  midarea <place> x0 y0 x1 y1 z          (STEP 12J: the nav areas of the *mid* places, see mid_areas)
Run: python3 make_mapinfo.py <csgo dir> <out dir>   (assemble.sh runs it on every assemble; a map without a
.nav or without objectives is skipped and the brain leaves its bots to the stock AI).
"""
import heapq
import lzma
import math
import os
import re
import struct
import sys


# ---------------------------------------------------------------- nav
class Area:
    __slots__ = ('id', 'nw', 'se', 'conn', 'hide', 'place', 'c', 'occ')


def parse_nav(path):
    d = open(path, 'rb').read()
    o = 0

    def u(fmt):
        nonlocal o
        v = struct.unpack_from('<' + fmt, d, o)
        o += struct.calcsize('<' + fmt)
        return v if len(v) > 1 else v[0]
    if u('I') != 0xFEEDFACE:
        raise ValueError('not a nav file')
    ver = u('I')
    if ver != 16:
        raise ValueError('nav version %d' % ver)
    u('I'); u('I'); u('B')
    places = []
    for _ in range(u('H')):
        n = u('H')
        places.append(d[o:o + n].rstrip(b'\0').decode('latin-1'))
        o += n
    u('B')
    areas = []
    for _ in range(u('I')):
        a = Area()
        a.id = u('I'); u('I')
        a.nw, a.se = u('3f'), u('3f')
        u('2f')
        a.conn = []
        for _d in range(4):
            n = u('I')
            a.conn += list(struct.unpack_from('<%dI' % n, d, o))
            o += 4 * n
        a.hide = []
        for _h in range(u('B')):
            _id, x, y, z, fl = u('I'), *u('3f'), u('B')
            a.hide.append((x, y, z, fl))
        for _e in range(u('I')):
            u('I'); u('B'); u('I'); u('B')
            ns = u('B')
            o += 5 * ns
        p = u('H')
        a.place = places[p - 1] if 0 < p <= len(places) else ''
        for _l in range(2):
            n = u('I')
            o += 4 * n
        a.occ = u('2f')     # earliest occupy time: [0] T, [1] CT
        u('4f')
        n = u('I')
        o += 5 * n
        u('I')
        n = u('B')
        o += 14 * n          # CS:GO approach areas
        a.c = ((a.nw[0] + a.se[0]) / 2, (a.nw[1] + a.se[1]) / 2, (a.nw[2] + a.se[2]) / 2)
        areas.append(a)
    return areas


# ---------------------------------------------------------------- bsp entities
def parse_bsp(path):
    with open(path, 'rb') as f:
        h = f.read(8 + 64 * 16)
        if h[:4] != b'VBSP':
            raise ValueError('not a bsp')

        def lump(i):
            ofs, ln, _v, _cc = struct.unpack_from('<iiii', h, 8 + 16 * i)
            f.seek(ofs)
            b = f.read(ln)
            if b[:4] == b'LZMA':     # Valve lzma lump: id, actual size, lzma size, 5 property bytes, raw lzma1
                size, _lsize = struct.unpack_from('<II', b, 4)
                props = b[12:17]
                pb = props[0] // 45; lp = (props[0] % 45) // 9; lc = props[0] % 9
                dict_size = struct.unpack_from('<I', props, 1)[0]
                dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': dict_size,
                                                                     'lc': lc, 'lp': lp, 'pb': pb}])
                b = dec.decompress(b[17:], size)
            return b
        ents_txt = lump(0).decode('latin-1', 'replace')
        models_raw = lump(14)
    models = [struct.unpack_from('<9f', models_raw, i) for i in range(0, len(models_raw) - 47, 48)]
    ents = []
    for block in re.findall(r'\{([^{}]*)\}', ents_txt):
        ents.append(dict(re.findall(r'"([^"]*)"\s+"([^"]*)"', block)))
    return ents, models


def vec(s):
    try:
        v = [float(x) for x in s.split()]
        return tuple(v) if len(v) == 3 else None
    except ValueError:
        return None


def brush_bounds(ent, models):
    m = ent.get('model', '')
    if not m.startswith('*'):
        return None
    i = int(m[1:])
    if i >= len(models):
        return None
    mn, mx = models[i][0:3], models[i][3:6]
    org = vec(ent.get('origin', '0 0 0')) or (0, 0, 0)
    return tuple(a + b for a, b in zip(mn, org)), tuple(a + b for a, b in zip(mx, org))


# ---------------------------------------------------------------- geometry / graph
def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


class Map:
    def __init__(self, areas):
        self.areas = areas
        self.byid = {a.id: a for a in areas}

    def nearest(self, p):
        best, bd = None, 1e18
        for a in self.areas:
            inside = min(a.nw[0], a.se[0]) - 1 <= p[0] <= max(a.nw[0], a.se[0]) + 1 and \
                min(a.nw[1], a.se[1]) - 1 <= p[1] <= max(a.nw[1], a.se[1]) + 1
            dz = abs(a.c[2] - p[2])
            d = dz if inside else dist(a.c, p) + 1000
            if d < bd:
                best, bd = a, d
        return best

    def dijkstra(self, src, side=None, goal=None, goal_r=0, contest=0.0, extra=None):
        """side 0 = T / 1 = CT: areas the OTHER team reaches well before this one cost 4x more (enemy ground), except
        inside the goal region - so a T route takes the T approaches (long, tunnels) and not the CT spawn.
        contest > 0 (STEP 9b rotations): also areas the other team reaches less than `contest` s after this one (cache
        mid) - a rotation keeps to the ground its own team holds.
        extra (STEP 12J): area id -> cost multiplier for entering it (the next T approach avoids the earlier ones)."""
        dd = {src.id: 0.0}
        par = {src.id: None}
        pq = [(0.0, src.id)]
        while pq:
            d0, i = heapq.heappop(pq)
            if d0 > dd.get(i, 1e18):
                continue
            a = self.byid[i]
            for j in a.conn:
                b = self.byid.get(j)
                if not b:
                    continue
                w = dist(a.c, b.c)
                if side is not None and (b.occ[1 - side] < b.occ[side] + contest if contest else b.occ[1 - side] + 3 < b.occ[side])                         and not (goal and dist(b.c, goal) <= goal_r):
                    w *= 4
                if extra and j in extra:
                    w *= extra[j]
                nd = d0 + w
                if nd < dd.get(j, 1e18):
                    dd[j] = nd
                    par[j] = i
                    heapq.heappush(pq, (nd, j))
        return dd, par

    def path(self, par, dst):
        out = []
        i = dst.id
        if i not in par:
            return []
        while i is not None:
            out.append(self.byid[i])
            i = par[i]
        return out[::-1]


def along(path, idx, back):
    """the point `back` path-units before path[idx] (towards path[0])"""
    left = back
    i = idx
    while i > 0 and left > 0:
        left -= dist(path[i].c, path[i - 1].c)
        i -= 1
    return path[i].c


def spaced(cands, n, gap):
    out = []
    for p in cands:
        if all(dist(p, q) >= gap for q in out):
            out.append(p)
        if len(out) >= n:
            break
    return out


def fmt(p):
    return '%.0f %.0f %.0f' % p


def build(mapname, csgo_dir):
    nav = os.path.join(csgo_dir, 'maps', mapname + '.nav')
    bsp = os.path.join(csgo_dir, 'maps', mapname + '.bsp')
    areas = parse_nav(nav)
    ents, models = parse_bsp(bsp)
    M = Map(areas)
    tsp = [vec(e['origin']) for e in ents if e.get('classname') == 'info_player_terrorist' and vec(e.get('origin', ''))]
    ctsp = [vec(e['origin']) for e in ents if e.get('classname') == 'info_player_counterterrorist' and vec(e.get('origin', ''))]
    if not tsp or not ctsp:
        return None, 'no spawns'
    mean = lambda ps: tuple(sum(p[k] for p in ps) / len(ps) for k in range(3))
    tspawn = M.nearest(mean(tsp)).c
    ctspawn = M.nearest(mean(ctsp)).c
    kind, sites = None, []          # sites: (label, centre, radius)
    rescue = None
    targets = [brush_bounds(e, models) for e in ents if e.get('classname') == 'func_bomb_target']
    targets = [t for t in targets if t]
    targets += [(vec(e['origin']), vec(e['origin'])) for e in ents
                if e.get('classname') == 'info_bomb_target' and vec(e.get('origin', ''))]
    hostages = [vec(e['origin']) for e in ents
                if e.get('classname') in ('hostage_entity', 'info_hostage_spawn') and vec(e.get('origin', ''))]
    if targets:
        kind = 'bomb'
        for mn, mx in targets:
            ctr = tuple((a + b) / 2 for a, b in zip(mn, mx))
            inside = [a for a in areas if mn[0] - 64 <= a.c[0] <= mx[0] + 64 and mn[1] - 64 <= a.c[1] <= mx[1] + 64
                      and mn[2] - 96 <= a.c[2] <= mx[2] + 160]
            if not inside:
                inside = [M.nearest(ctr)]
            c = mean([a.c for a in inside])
            c = M.nearest(c).c
            half = 0.5 * math.hypot(mx[0] - mn[0], mx[1] - mn[1])
            places = {}
            for a in inside:
                places[a.place] = places.get(a.place, 0) + 1
            place = max(places, key=places.get) if places else ''
            m = re.search(r'([A-Z])$', place) if 'bomb' in place.lower() or 'site' in place.lower() else None
            sites.append([m.group(1) if m else '', c, max(450.0, min(half + 150.0, 1100.0))])
        # labels: from the place name, else A/B by distance from the T spawn order
        used = {s[0] for s in sites if s[0]}
        free = [x for x in 'ABCDEF' if x not in used]
        for s in sites:
            if not s[0] or [t[0] for t in sites].count(s[0]) > 1:
                s[0] = free.pop(0)
    elif hostages:
        kind = 'hostage'
        groups = []
        for h in hostages:
            for g in groups:
                if dist(mean(g), h) < 900:
                    g.append(h)
                    break
            else:
                groups.append([h])
        groups.sort(key=len, reverse=True)
        for i, g in enumerate(groups[:2]):
            sites.append(['AB'[i], M.nearest(mean(g)).c, 600.0])
        rz = [brush_bounds(e, models) for e in ents if e.get('classname') == 'func_hostage_rescue']
        rz = [r for r in rz if r]
        rescue = M.nearest(tuple((a + b) / 2 for a, b in zip(*rz[0]))).c if rz else ctspawn
    else:
        return None, 'no bomb targets or hostages'
    if len(sites) > 4:
        sites = sites[:4]
    ta, ca = M.nearest(tspawn), M.nearest(ctspawn)
    lines = ['kind ' + kind, 'tspawn ' + fmt(tspawn), 'ctspawn ' + fmt(ctspawn)]
    if rescue:
        lines.append('rescue ' + fmt(rescue))
    chokes = {}
    for lab, c, r in sites:
        lines.append('site %s %s' % (lab, fmt(c)))
        lines.append('radius %s %.0f' % (lab, r))
        # the side that attacks: T for bomb sites, CT for hostages
        side, aspawn = (0, ta) if kind == 'bomb' else (1, ca)
        _d, apar = M.dijkstra(aspawn, side, c, r)
        p = M.path(apar, M.nearest(c))
        if len(p) < 2:
            p = [aspawn, M.nearest(c)]
        idx = next((i for i, a in enumerate(p) if dist(a.c, c) <= r), len(p) - 1)
        entry = p[idx].c
        choke = along(p, idx, 250)
        stack = along(p, idx, 900)
        if dist(stack, aspawn.c) < 400:
            stack = along(p, idx, 500)
        chokes[lab] = choke
        lines += ['entry %s %s' % (lab, fmt(entry)), 'choke %s %s' % (lab, fmt(choke)), 'stack %s %s' % (lab, fmt(stack))]
        spots = []
        for a in areas:
            if dist(a.c, c) > r + 350:
                continue
            for hx, hy, hz, fl in a.hide:
                spots.append(((hx, hy, hz), fl))
        # defenders' holds (CT at a bomb site): cover, 250-900 from the entry, inside the site region
        holds = []
        for pnt, fl in spots:
            de = dist(pnt, entry)
            if dist(pnt, c) > r + 150 or de < 200:
                continue
            score = abs(de - 550) - (250 if fl & 1 else 0) + (300 if fl & 8 else 0)
            holds.append((score, pnt))
        holds.sort()
        hl = spaced([p for _s, p in holds], 4, 220)
        if not hl:
            hl = [c]
        if kind == 'bomb':
            lines += ['hold %s %s' % (lab, fmt(p)) for p in hl]
        # post-plant / hostage guard spots: cover, 250-900 from the centre
        posts = sorted(((abs(dist(pnt, c) - 550) - (250 if fl & 1 else 0), pnt) for pnt, fl in spots
                        if 200 <= dist(pnt, c) <= 950), key=lambda t: t[0])
        pl = spaced([p for _s, p in posts], 5, 200) or [c]
        lines += ['post %s %s' % (lab, fmt(p)) for p in pl]
        # defenders gather before a retake (bomb: CT side) / hostage maps: the T guard's forward hold
        dside, dspawn = (1, ca) if kind == 'bomb' else (0, ta)
        _d, dpar = M.dijkstra(dspawn, dside, c, r)
        dp = M.path(dpar, M.nearest(c))
        if len(dp) >= 2:
            didx = next((i for i, a in enumerate(dp) if dist(a.c, c) <= r), len(dp) - 1)
            lines.append('retake %s %s' % (lab, fmt(along(dp, didx, 700))))
        else:
            lines.append('retake %s %s' % (lab, fmt(c)))
    for lab, c, r in sites:
        others = [s for s in sites if s[0] != lab]
        if others:
            o = min(others, key=lambda s: dist(s[1], c))
            lines.append('lurk %s %s' % (lab, fmt(chokes[o[0]])))
    mid = None
    if len(sites) >= 2:
        a0 = M.nearest(sites[0][1])
        _d, par = M.dijkstra(a0)
        p = M.path(par, M.nearest(sites[1][1]))
        if len(p) >= 2:
            tot = sum(dist(p[i].c, p[i + 1].c) for i in range(len(p) - 1))
            mid = along(p, len(p) - 1, tot / 2)
            lines.append('mid ' + fmt(mid))
    # STEP 9b: defenders' rotation routes (bomb maps): from each site (and mid) to each other site over the CT side of
    # the map (T-early ground costs 4x, as for the T routes), as waypoints ~500 apart. The bots' own path search
    # otherwise takes e.g. dust2's B tunnels -> mid -> short into the Ts.
    if kind == 'bomb' and len(sites) >= 2:
        starts = [(lab, c) for lab, c, _r in sites] + ([('mid', mid)] if mid else [])
        for src, sc in starts:
            _d, par = M.dijkstra(M.nearest(sc), 1, contest=5.0)
            for lab, c, r in sites:
                if lab == src:
                    continue
                p = M.path(par, M.nearest(c))
                if len(p) < 2:
                    continue
                wps, run = [], 0.0
                for i in range(1, len(p)):
                    run += dist(p[i - 1].c, p[i].c)
                    if dist(p[i].c, c) <= r:
                        break                    # the hold / retake goal takes it from the site edge
                    if run >= 500:
                        wps.append(p[i].c)
                        run = 0.0
                lines += ['rot %s %s %s' % (src, lab, fmt(w)) for w in wps]
    if kind == 'bomb':
        for lab, c, r in sites:
            lines += t_approaches(M, ta, lab, c, r)
        lines += mid_areas(areas)
    lines += occ_grid(areas)
    lines += hide_spots(areas)
    return lines, None


# STEP 12J (Job 5, 2026-09-26): more than one way into each bomb site. The brain's round plans (part D) pick a site AND
# the way in; his playtests: "Ts all ran down Mirage mid" (09-25), "not straight down mid doors two out of three rounds"
# (de_dust2, 09-26). Approach 0 is step 9's T path (the same stack / choke / entry as the old lines); each next one is
# searched again with the areas of the earlier ones 8x dearer (not near the T spawn nor on the site) and kept when it
# enters the site > 350 u from every earlier entry or its middle runs > 700 u from every earlier path, at most 1.8x the
# first one's length. Up to 3 a site. Named by the most common nav place along its middle part; flag 1 = that part
# crosses a place named *mid* (TopofMid, Middle, MidDoors, ...).
#   appr <L> <k> <name> <flags> <length>    apwp <L> <k> x y z   (waypoints ~500 u apart, from the T spawn to its stack)
#   apstk / apchk / apent <L> <k> x y z     (its stack 900 u and choke 250 u before the site edge, its entry)
AP_MAX, AP_DEAR, AP_RATIO, AP_ENTRY_GAP, AP_MID_GAP = 3, 8.0, 1.8, 350.0, 700.0
MID_RE = re.compile(r'mid', re.I)


def path_len(p):
    return sum(dist(p[i].c, p[i + 1].c) for i in range(len(p) - 1))


def back_index(path, idx, back):
    """the index of the area `back` path-units before path[idx]"""
    left, i = back, idx
    while i > 0 and left > 0:
        left -= dist(path[i].c, path[i - 1].c)
        i -= 1
    return i


def middle(path, idx):
    """the areas of path[:idx+1] between 20% and 90% of its length (the part that makes a route a route)"""
    tot = sum(dist(path[i].c, path[i + 1].c) for i in range(idx))
    out, run = [], 0.0
    for i in range(idx + 1):
        if i:
            run += dist(path[i - 1].c, path[i].c)
        if 0.2 * tot <= run <= 0.9 * tot:
            out.append(path[i])
    return out or path[:idx + 1]


def route_name(mid_part, k):
    counts = {}
    for a in mid_part:
        pl = a.place or ''
        if not pl or 'spawn' in pl.lower() or 'bombsite' in pl.lower():
            continue
        counts[pl] = counts.get(pl, 0) + 1
    return max(counts, key=counts.get) if counts else 'route%d' % k


def t_approaches(M, ta, lab, c, r, side=0):
    found = []          # (path, idx)
    extra = {}
    for k in range(AP_MAX * 2):
        if len(found) >= AP_MAX:
            break
        _d, par = M.dijkstra(ta, side, c, r, extra=extra)
        p = M.path(par, M.nearest(c))
        if len(p) < 2:
            break
        idx = next((i for i, a in enumerate(p) if dist(a.c, c) <= r), len(p) - 1)
        if found:
            base = path_len(found[0][0][:found[0][1] + 1])
            if path_len(p[:idx + 1]) > AP_RATIO * max(1.0, base):
                break
            entry_new = all(dist(p[idx].c, q[j].c) > AP_ENTRY_GAP for q, j in found)
            mid_new = max((min(dist(a.c, b.c) for q, _j in found for b in q) for a in middle(p, idx)), default=0.0) > AP_MID_GAP
            ok = entry_new or mid_new
        else:
            ok = True
        if ok:
            found.append((p, idx))
        # the next search avoids this path (not near the T spawn, not on the site)
        for a in p:
            if dist(a.c, ta.c) > 700 and dist(a.c, c) > r + 300:
                extra[a.id] = extra.get(a.id, 1.0) * AP_DEAR
    out = []
    for k, (p, idx) in enumerate(found):
        mp = middle(p, idx)
        flags = 1 if any(MID_RE.search(a.place or '') for a in mp) else 0
        out.append('appr %s %d %s %d %.0f' % (lab, k, re.sub(r'\s+', '_', route_name(mp, k)), flags, path_len(p[:idx + 1])))
        si = back_index(p, idx, 900)
        if dist(p[si].c, ta.c) < 400:
            si = back_index(p, idx, 500)
        run = 0.0
        for i in range(1, si):
            run += dist(p[i - 1].c, p[i].c)
            if run >= 500:
                out.append('apwp %s %d %s' % (lab, k, fmt(p[i].c)))
                run = 0.0
        out.append('apstk %s %d %s' % (lab, k, fmt(p[si].c)))
        out.append('apchk %s %d %s' % (lab, k, fmt(along(p, idx, 250))))
        out.append('apent %s %d %s' % (lab, k, fmt(p[idx].c)))
    return out


# STEP 12J: every nav area of a place named *mid* - the brain counts how many Ts go through mid each round
MID_MAX = 600


def mid_areas(areas):
    out = []
    for a in areas:
        if a.place and MID_RE.search(a.place) and len(out) < MID_MAX:
            x0, x1 = sorted((a.nw[0], a.se[0]))
            y0, y1 = sorted((a.nw[1], a.se[1]))
            out.append('midarea %s %.0f %.0f %.0f %.0f %.0f' % (re.sub(r'\s+', '_', a.place), x0, y0, x1, y1, a.c[2]))
    return out


# STEP 10b (2026-09-25): every nav hiding spot with its flags (1 IN_COVER, 2 GOOD_SNIPER, 4 IDEAL_SNIPER, 8 EXPOSED).
# The brain's B5 (footwork dial): a bot that picked an EXPOSED spot to hide at is moved to the nearest IN_COVER one.
#   spot x y z flags
def hide_spots(areas):
    return ['spot %.0f %.0f %.0f %d' % (x, y, z, fl) for a in areas for x, y, z, fl in a.hide]


# STEP 10 (2026-09-24): where each team can already be. The nav's earliest-occupy time (seconds after the freeze, at
# run speed) of every area, spread over a 200-unit grid: a cell holds the lowest T / CT time of any area within 800
# units (2D). The brain's quiet walk (footwork dial) walks where the ENEMY time is already past the round clock.
#   occgrid <x0> <y0> <cell> <nx> <ny>        occ <T|CT> <row> <nx times in 1/10 s, 1200 = never>
OCC_CELL, OCC_REACH = 200.0, 800.0


def occ_grid(areas):
    if not areas:
        return []
    x0 = min(min(a.nw[0], a.se[0]) for a in areas) - OCC_REACH
    y0 = min(min(a.nw[1], a.se[1]) for a in areas) - OCC_REACH
    x1 = max(max(a.nw[0], a.se[0]) for a in areas) + OCC_REACH
    y1 = max(max(a.nw[1], a.se[1]) for a in areas) + OCC_REACH
    nx, ny = int((x1 - x0) / OCC_CELL) + 1, int((y1 - y0) / OCC_CELL) + 1
    best = [[[1200] * nx for _ in range(ny)] for _t in range(2)]
    for a in areas:
        ax0, ax1 = sorted((a.nw[0], a.se[0]))
        ay0, ay1 = sorted((a.nw[1], a.se[1]))
        occ = [min(1200, int(round(min(120.0, max(0.0, t)) * 10))) for t in a.occ]
        for iy in range(max(0, int((ay0 - OCC_REACH - y0) / OCC_CELL)), min(ny, int((ay1 + OCC_REACH - y0) / OCC_CELL) + 1)):
            cy = y0 + (iy + 0.5) * OCC_CELL
            dy = max(ay0 - cy, 0.0, cy - ay1)
            for ix in range(max(0, int((ax0 - OCC_REACH - x0) / OCC_CELL)), min(nx, int((ax1 + OCC_REACH - x0) / OCC_CELL) + 1)):
                cx = x0 + (ix + 0.5) * OCC_CELL
                dx = max(ax0 - cx, 0.0, cx - ax1)
                if dx * dx + dy * dy > OCC_REACH * OCC_REACH:
                    continue
                for t in range(2):
                    if occ[t] < best[t][iy][ix]:
                        best[t][iy][ix] = occ[t]
    out = ['occgrid %.0f %.0f %.0f %d %d' % (x0, y0, OCC_CELL, nx, ny)]
    for t, lab in ((0, 'T'), (1, 'CT')):
        out += ['occ %s %d %s' % (lab, iy, ' '.join(str(v) for v in best[t][iy])) for iy in range(ny)]
    return out


def main():
    csgo = sys.argv[1] if len(sys.argv) > 1 else '/opt/csgo/server/csgo'
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(csgo, 'addons', 'family_maps')
    os.makedirs(out, exist_ok=True)
    ok = skipped = 0
    for fn in sorted(os.listdir(os.path.join(csgo, 'maps'))):
        if not fn.endswith('.bsp'):
            continue
        name = fn[:-4]
        if not os.path.exists(os.path.join(csgo, 'maps', name + '.nav')):
            print('[mapinfo] %-20s skipped: no .nav' % name)
            skipped += 1
            continue
        try:
            lines, why = build(name, csgo)
        except Exception as ex:          # a broken file never stops the others
            lines, why = None, 'error: %s' % ex
        if not lines:
            print('[mapinfo] %-20s skipped: %s' % (name, why))
            skipped += 1
            continue
        with open(os.path.join(out, name + '.txt'), 'w', newline='\n') as f:
            f.write('// generated by servers/csgo/bots/make_mapinfo.py from maps/%s.nav + .bsp - do not edit\n' % name)
            f.write('\n'.join(lines) + '\n')
        ok += 1
        print('[mapinfo] %-20s %s, %d sites' % (name, lines[0].split()[1], sum(1 for l in lines if l.startswith('site '))))
    print('[mapinfo] %d maps written to %s, %d skipped' % (ok, out, skipped))


if __name__ == '__main__':
    main()
