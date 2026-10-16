"""Law and fault switches of the mock VU runtime (DESIGN.md 13.3). The first value of each tuple is the default.

Each law switch is recovered by one analysis rule; the e2e tests run both (all) variants and require the verdict to
name the variant the mock was set to. Robustness switches model the R section 9 unknowns and Phase 0 failure modes.
A switch whose value list starts with None takes free values (numbers, strings, tables) checked by its own code.
"""

LAWS = {
    # weapons
    'disc': (('area', 'radius'), 'W1'),
    'ra_units': (('deg', 'rad', 'norm'), 'W1 secondary / P0.11'),
    'decay': ((15.0, 8.0), 'W2 (deg/s)'),
    'min_rise': (('immediate', 'ramp'), 'W2'),
    'recovery': (('linear', 'exp'), 'W3'),
    'rec_rate': ((8.0, 18.0, 30.0), 'W3/W4 recovery rate (deg/s or 1/s)'),
    'inc': ((None,), 'spread increase per shot override (deg; None = weapon data)'),
    'hcorr': ((0.0, 0.6), 'W3 (lag-1 correlation of horizontal kicks)'),
    'rearm': (('release', 'recovered', 'spreadmin', 'timer:0.3'), 'W4'),
    'falloff': (('linear', 'smooth'), 'W5'),
    'm98b_end': ((150.0, 120.0), 'W5 (m)'),
    'pass_soldiers': ((False, True), 'W6'),
    'prot_threshold': ((False, True), 'W6'),
    'blast': (('linear', 'quadratic'), 'W7'),
    'rpg_direct': ((225.0, 100.0), 'W7'),
    # soldier
    'regen_delay': ((10.0, 16.0), 'S1 (s)'),
    'regen_rate': ((5.0,), 'S1 (hp/s)'),
    'supp_abort': ((True, False), 'S1'),
    'supp_effect': (('event', 'none'), 'S1 (none: bot bullets never suppress)'),
    'immune_s': ((2.0,), 'S2 (s)'),
    'motor': (('fraction', 'linear', 'instant'), 'S3'),
    'gravity': ((9.81, 15.0), 'S3 (m/s^2)'),
    # modes
    'bleed': (('A', 'B'), 'M1'),
    'capture': (('model', 'linear_n'), 'M2'),
    'flip': (('lose_get', 'direct'), 'M2'),
    # tick
    'hz': ((30, 60), 'T1'),
    'coupled': (('none', 'accel', 'recovery', 'decay'), 'T1'),
}

FAULTS = {
    'bot_damage': ((True, False), 'L14'),
    'gs_cadence': (('tick', 'frame:2', 'frame:3'), 'L11'),
    'gs_server': ((True, False), 'L11'),
    'ef_bullets': ((True, False), 'L12'),
    'hit_noise': ((0.0, 0.02), 'L13 (m)'),
    'idle_advance': ((True, False), 'L23'),
    'rcon_vars': ((True, False), 'L05'),
    'iter_types': ((True, False), 'L15'),
    'is_parent': ((True, False), 'L32 (DataContainer:Is matches the parent data type)'),
    'vm': (('same', 'separate'), 'L27'),
    'init_order': (('shared_first', 'realm_first'), 'L27'),
    'preround': ((False, True), 'L21'),
    'asset_case': (('insensitive', 'exact'), 'L07'),
    'print_limit': ((0, 600), 'L20 (bytes, 0 = none)'),
    'log_drop': ((0.0, 0.01), 'L20 (fraction)'),
    'log_garbage': ((0.0, 0.002), 'L20 (fraction of garbage lines inserted)'),
    'spawn_fails': ((None,), 'stuck path: e.g. "WA:2" makes SpawnSoldierAt a no-op for that config'),
    'chat': ((True, False), 'L06 (False: only the console path works)'),
    'rr_state': (('kept', 'lost'), 'L22 (lost: a round restart reloads the Lua state)'),
    'fire_mode': (('auto', 'single', 'burst', 'stuck'), 'L30 (the mode a weapon spawns in; stuck ignores cycling)'),
    'input_dt': (('ignored', 'required'), 'L08 (required: bot inputs with deltaTime 0 do nothing)'),
    'w1_outliers': ((None,), 'k@f: k impacts per W-A config planted at f times the disc radius'),
    'tcost': ((None,), '{spawn, kill, remove} ticket costs (default 1/0/0)'),
    'member_missing': (('raise', 'nil'), 'reading an absent member raises (DESIGN 3.1) or returns nil'),
    'aim_bias': ((0.0, 30.0), 'L08 (deg added to bot yaw: the P0.9 aim fault)'),
    'origin': ((None,), 'world origin offset {x, y, z} of the level layout (m)'),
    'range_len': ((None,), 'a backstop at this distance down the range lane (m; None = open 200 m)'),
}


def defaults():
    """Return {switch: default value} for every law and fault switch."""
    out = {k: v[0][0] for k, v in LAWS.items()}
    out.update({k: v[0][0] for k, v in FAULTS.items()})
    return out


def make(**overrides):
    """Defaults with overrides; unknown switches or string values outside the listed set raise."""
    table = dict(LAWS)
    table.update(FAULTS)
    out = defaults()
    for k, v in overrides.items():
        if k not in table:
            raise KeyError('unknown mock switch: %s' % k)
        allowed = table[k][0]
        if allowed[0] is not None and isinstance(allowed[0], str) and v not in allowed:
            raise ValueError('%s=%r not in %r' % (k, v, allowed))
        out[k] = v
    return out


def parse(tokens):
    """['disc=radius', 'hz=60', 'bot_damage=false'] -> overrides dict (numbers and booleans converted)."""
    out = {}
    for tok in tokens:
        if '=' not in tok:
            raise ValueError('law %r is not key=value' % tok)
        k, v = tok.split('=', 1)
        if v.lower() in ('true', 'yes', 'on'):
            val = True
        elif v.lower() in ('false', 'no', 'off'):
            val = False
        else:
            try:
                val = int(v)
            except ValueError:
                try:
                    val = float(v)
                except ValueError:
                    val = v
        out[k] = val
    return out
