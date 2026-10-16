"""Law and fault switches of the mock VU runtime (DESIGN.md 13.3). The first value of each tuple is the default.

Each law switch is recovered by one analysis rule; the e2e tests run both (all) variants and require the verdict to
name the variant the mock was set to. Robustness switches model the R section 9 unknowns and Phase 0 failure modes.
"""

LAWS = {
    # weapons
    'disc': (('area', 'radius'), 'W1'),
    'ra_units': (('deg', 'rad', 'norm'), 'W1 secondary / P0.11'),
    'decay': ((15.0, 8.0), 'W2 (deg/s)'),
    'min_rise': (('immediate', 'ramp'), 'W2'),
    'recovery': (('linear', 'exp'), 'W3'),
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
    'immune_s': ((2.0,), 'S2 (s)'),
    'motor': (('fraction', 'linear'), 'S3'),
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
    'vm': (('same', 'separate'), 'L27'),
    'init_order': (('shared_first', 'realm_first'), 'L27'),
    'preround': ((False, True), 'L21'),
    'asset_case': (('insensitive', 'exact'), 'L07'),
    'print_limit': ((0, 600), 'L20 (bytes, 0 = none)'),
    'log_drop': ((0.0, 0.01), 'L20 (fraction)'),
    'spawn_fails': ((None,), 'stuck path: e.g. "WA:2" makes SpawnSoldierAt a no-op for that config'),
    'chat': ((True, False), 'L06 (False: only the console path works)'),
    'level_change': (('kept', 'reloaded'), 'L22'),
}


def defaults():
    """Return {switch: default value} for every law and fault switch."""
    out = {k: v[0][0] for k, v in LAWS.items()}
    out.update({k: v[0][0] for k, v in FAULTS.items()})
    return out


def make(**overrides):
    """Defaults with overrides; unknown switches or values outside the listed set (except free numbers) raise."""
    table = dict(LAWS)
    table.update(FAULTS)
    out = defaults()
    for k, v in overrides.items():
        if k not in table:
            raise KeyError('unknown mock switch: %s' % k)
        allowed = table[k][0]
        if isinstance(allowed[0], str) and v not in allowed and k != 'spawn_fails':
            raise ValueError('%s=%r not in %r' % (k, v, allowed))
        out[k] = v
    return out
