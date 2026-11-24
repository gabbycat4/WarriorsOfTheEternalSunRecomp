#!/usr/bin/env python3
"""glide_replay.py LOG [first last] - replay a WOTES_MOTION_LOG through the same
tracking/glide rules as game/wotes_video.c (step 4.2) at 4 pictures per frame,
and report how far each tracked object moves between consecutive pictures
(screen px), before (real frames only) and after (with in-betweens)."""
import sys, collections
def wrap10(d):
    d &= 0x3FF
    return d - 0x400 if d >= 0x200 else d
fr = []; cur = None
for l in open(sys.argv[1]):
    if l.startswith('F '): cur = {}; fr.append(cur)
    elif l.startswith('V ') and cur is not None:
        p = l.split(); cur['hs'] = int(p[p.index('hs') + 1], 16) & 0x3FF; cur['vs'] = int(p[p.index('vs') + 1], 16) & 0x3FF
    elif l.startswith('S ') and cur is not None:
        cur['spr'] = []
        for t in l.split()[2:]:
            y, sl, at, x = (int(v, 16) for v in t.split(','))
            cur['spr'].append(((x & 0x1FF) - 128, (y & 0x3FF) - 128, at, (((sl >> 10) & 3) + 1) * 8, (((sl >> 8) & 3) + 1) * 8))
a = int(sys.argv[2]) if len(sys.argv) > 2 else 0; b = int(sys.argv[3]) if len(sys.argv) > 3 else len(fr)
objs = []
next_id = [0]
def disp(o, p):
    prog = min(1.0, max(0.0, (o['since'] + p) / o['period']))
    return o['fx'] + (o['wx'] - o['fx']) * prog, o['fy'] + (o['wy'] - o['fy']) * prog
def period(o):
    g = sorted(o['gaps'][:4])
    if len(g) < 2: return 1
    return max(1, min(12, g[len(g) // 2]))
cam = None
steps_after = collections.Counter(); steps_before = collections.Counter()
last_shown = {}; last_real = {}
for fi in range(a, b):
    f = fr[fi]; hs, vs = f['hs'], f['vs']
    prevcam = cam if cam else (hs, vs); cam = (hs, vs)
    for o in objs: o['seen'] = False
    match = [-1] * len(f['spr'])
    for pas in (0, 1):
        for k, (x, y, at, w, h) in enumerate(f['spr']):
            if match[k] >= 0: continue
            key = ((at & 0x7FF) >> 3) | (((at >> 13) & 3) << 12); key2 = (((at >> 13) & 3) << 12) | (w << 6) | h
            wx, wy = x - hs, y + vs; best = -1; bd = 1 << 30
            for i, o in enumerate(objs):
                if o['seen']: continue
                if (o['key'] if pas == 0 else o['key2']) != (key if pas == 0 else key2): continue
                dx, dy = wrap10(wx - o['wx']), wrap10(wy - o['wy']); d = dx * dx + dy * dy
                if d < bd: bd, best = d, i
            if best >= 0 and bd <= 48 * 48: match[k] = best; objs[best]['seen'] = True
    for o in objs: o['seen'] = False
    keep = []
    for k, (x, y, at, w, h) in enumerate(f['spr']):
        key = ((at & 0x7FF) >> 3) | (((at >> 13) & 3) << 12); key2 = (((at >> 13) & 3) << 12) | (w << 6) | h
        wx, wy = x - hs, y + vs
        if match[k] < 0:
            o = {'wx': wx, 'wy': wy, 'fx': wx, 'fy': wy, 'since': 1000, 'period': 1, 'gaps': [], 'id': next_id[0] }
            next_id[0] += 1
            objs.append(o)
        else:
            o = objs[match[k]]
            dx, dy = wrap10(wx - o['wx']), wrap10(wy - o['wy'])
            if abs(dx) > 16 or abs(dy) > 16:
                o['wx'], o['wy'], o['fx'], o['fy'], o['since'] = wx, wy, wx, wy, 1000
            elif dx or dy:
                cx, cy = disp(o, 1.0)
                if o['since'] < 1000: o['gaps'].insert(0, o['since'] + 1)
                o['period'] = period(o)
                o['fx'] = cx + (wx - (o['wx'] + dx)); o['fy'] = cy + (wy - (o['wy'] + dy))
                o['wx'], o['wy'] = wx, wy; o['since'] = 0
            elif o['since'] < 1000: o['since'] += 1
        o['key'], o['key2'], o['seen'] = key, key2, True
    objs = [o for o in objs if o['seen']]
    dhs, dvs = wrap10(hs - prevcam[0]), wrap10(vs - prevcam[1])
    for pi in range(4):
        p = (pi + 1) / 4.0
        ohs = round(dhs * (p - 1)) if abs(dhs) <= 32 else 0; ovs = round(dvs * (p - 1)) if abs(dvs) <= 32 else 0
        ch, cv = hs + ohs, vs + ovs
        for o in objs:
            x, y = disp(o, p); sx, sy = round(x) + ch, round(y) - cv
            if o['id'] in last_shown:
                d = max(abs(sx - last_shown[o['id']][0]), abs(sy - last_shown[o['id']][1]))
                if d < 48: steps_after[d] += 1
            last_shown[o['id']] = (sx, sy)
    for o in objs:
        sx, sy = o['wx'] + hs, o['wy'] - vs
        if o['id'] in last_real:
            d = max(abs(sx - last_real[o['id']][0]), abs(sy - last_real[o['id']][1]))
            if d < 48: steps_before[d] += 1
        last_real[o['id']] = (sx, sy)
print('per real frame, screen px moved by an object (before):', sorted(steps_before.items()))
print('per picture at 240, screen px moved (after):           ', sorted(steps_after.items()))
big = lambda c: sum(n for d, n in c.items() if d >= 4)
print('moves of 4+ px:  before %d (per frame)   after %d (per picture)' % (big(steps_before), big(steps_after)))
