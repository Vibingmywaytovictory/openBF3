"""Level layouts of the mock VU world (DESIGN.md 13.2) and the scripted user's spots on them.

Every layout is invented [mock]: a ground plane y = 0, one wall (20 x 6 m, facing -z) with 26 m of clear floor in
front, a range lane (200 m open, or a backstop at range_len), a flat open area with open sky, and Conquest flags far
from all of them (no bleed or capture near the measurement sites). The level names and game modes are the Session 1
ones (MP_Subway ConquestSmall0, MP_001 ConquestLarge0). origin shifts the whole layout (W1 quantisation test).
"""

SUBWAY = ('Levels/MP_Subway/MP_Subway', 'ConquestSmall0')
BAZAAR = ('Levels/MP_001/MP_001', 'ConquestLarge0')


def _add(a, o):
    return [a[0] + o[0], a[1] + o[1], a[2] + o[2]]


def layout(which='subway', origin=None, range_len=None):
    """Return the level table for vu_world/vu_conquest (lists of 3 floats; offsets applied)."""
    o = list(origin or (0.0, 0.0, 0.0))
    name, mode = SUBWAY if which == 'subway' else BAZAAR
    planes = [{'p': _add([0.0, 0.0, 0.0], o), 'n': [0.0, 1.0, 0.0], 'mat': 'ground'}]
    rects = [{'name': 'wall', 'c': _add([0.0, 3.0, 100.0], o), 'n': [0.0, 0.0, -1.0], 'u': [1.0, 0.0, 0.0],
              'v': [0.0, 1.0, 0.0], 'hu': 10.0, 'hv': 3.0, 'mat': 'wall'}]
    if range_len:
        # backstop across the lane, range_len + 2 m from the range start (the lane ray runs at 1.3 m)
        rects.append({'name': 'backstop', 'c': _add([100.0 + float(range_len) + 2.0, 3.0, -100.0], o),
                      'n': [-1.0, 0.0, 0.0], 'u': [0.0, 0.0, 1.0], 'v': [0.0, 1.0, 0.0], 'hu': 5.0, 'hv': 3.0,
                      'mat': 'wall'})
    if which == 'subway':
        xs = [-60.0, 0.0, 60.0]
        hq = 200.0
        tickets = 400
    else:
        xs = [-120.0, -60.0, 0.0, 60.0, 120.0]
        hq = 260.0
        tickets = 300
    flags = []
    for i, x in enumerate(xs):
        flags.append({'name': 'CP_' + 'ABCDEFG'[i], 'pos': _add([x, 0.0, -300.0], o), 'r': 10.0, 'area': 60.0,
                      'hq': False, 'team': 0})
    flags.append({'name': 'HQ_US', 'pos': _add([-hq, 0.0, -300.0], o), 'r': 15.0, 'area': 0.0, 'hq': True, 'team': 1})
    flags.append({'name': 'HQ_RU', 'pos': _add([hq, 0.0, -300.0], o), 'r': 15.0, 'area': 0.0, 'hq': True, 'team': 2})
    spots = {
        # user stands here and looks at the wall (15 m, square on)
        'wall': {'pos': _add([0.5, 0.0, 85.0], o), 'dir': [0.0, 0.0, 1.0]},
        'range': {'pos': _add([100.0, 0.0, -100.0], o), 'dir': [1.0, 0.0, 0.0]},
        'flat': {'pos': _add([-100.0, 0.0, -100.0], o), 'dir': [-1.0, 0.0, 0.0]},
        # look 10 m ahead and down at the ground
        'ring': {'pos': _add([-100.0, 0.0, 40.0], o), 'dir': [0.0, -0.16, 1.0]},
        'deploy': {'pos': _add([0.5, 0.0, 70.0], o), 'dir': [0.0, 0.0, 1.0]},
    }
    return {'name': name, 'mode': mode, 'tickets': tickets, 'planes': planes, 'rects': rects, 'flags': flags,
            'spots': spots}


def levels(origin=None, range_len=None):
    """The map list in order: MP_Subway ConquestSmall0, then MP_001 ConquestLarge0."""
    return [layout('subway', origin, range_len), layout('bazaar', origin, range_len)]
