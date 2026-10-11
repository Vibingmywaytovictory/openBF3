"""Summarise ebx-dump weapon closures into one table of key numbers.

usage: python -I summarize.py <out.json> <dump.json> [<dump.json> ...]
Every value is read from the dump; nothing is computed except the follow of references.
"""
import json
import sys


class Closure:
    def __init__(self, path):
        self.doc = json.load(open(path, encoding='utf-8'))
        self.by_part = {}
        for p in self.doc['partitions']:
            self.by_part[p['partition']] = {i['$guid']: i for i in p['instances']}

    def resolve(self, ref, part):
        if not isinstance(ref, dict) or '$ref' not in ref:
            return None, None
        target = part if ref['$ref'] == 'internal' else ref['$ref']
        inst = self.by_part.get(target, {}).get(ref.get('instance'))
        return inst, target


def pick(d, *keys):
    return {k: d.get(k) for k in keys}


def dispersion(dd):
    return None if not dd else pick(dd, 'MinAngle', 'MaxAngle', 'IncreasePerShot', 'DecreasePerSecond')


def recoil(r):
    return None if not r else pick(r, 'RecoilAmplitudeIncPerShot', 'RecoilAmplitudeMax', 'RecoilAmplitudeDecreaseFactor',
                                   'HorizontalRecoilAmplitudeIncPerShotMin', 'HorizontalRecoilAmplitudeIncPerShotMax',
                                   'HorizontalRecoilAmplitudeMax')


def projectile(c, ref, part):
    inst, ipart = c.resolve(ref, part)
    if inst is None:
        return {'unresolved': ref}
    out = {'partition': ipart, 'type': inst['$type']}
    for k in ('StartDamage', 'EndDamage', 'DamageFalloffStartDistance', 'DamageFalloffEndDistance', 'InitialSpeed',
              'Gravity', 'TimeToLive', 'Damage', 'MaxSpeed', 'EngineStrength', 'TimeToArm', 'InstantHit',
              'AmmunitionType', 'Drag', 'TimeToArmExplosion', 'DetonateOnTimeout'):
        if k in inst:
            out[k] = inst[k]
    mp = inst.get('MaterialPair')
    if isinstance(mp, dict):
        out['MaterialPair'] = mp.get('instance')
    ex, epart = c.resolve(inst.get('Explosion'), ipart)
    if ex:
        out['Explosion'] = pick(ex, 'BlastDamage', 'BlastRadius', 'InnerBlastRadius', 'BlastImpulse', 'ShockwaveDamage',
                                'ShockwaveRadius', 'ShockwaveImpulse')
    # GrenadeEntityData keeps its fuse in a different place depending on subtype; keep any *Time* scalars.
    if inst['$type'] == 'GrenadeEntityData':
        out['scalars'] = {k: v for k, v in inst.items() if isinstance(v, (int, float)) and not k.startswith('Is')}
    return out


def summarize(path):
    c = Closure(path)
    root = c.doc['root']
    insts = c.by_part[root]
    swd = next(i for i in insts.values() if i['$type'] == 'SoldierWeaponData')
    wf, _ = c.resolve(swd.get('WeaponFiring'), root)
    out = {'root': root, 'DamageGiverName': swd.get('DamageGiverName'), 'AnimBaseSet': swd.get('AnimBaseSet'),
           'RenderFov': swd.get('RenderFov'), 'ZoomRenderFov': swd.get('ZoomRenderFov')}
    if wf is None:
        return out
    out['DeployTime'] = wf.get('DeployTime')
    out['AltDeployTime'] = wf.get('AltDeployTime')
    ffd, _ = c.resolve(wf.get('PrimaryFire'), root)
    if ffd:
        fl = ffd['FireLogic']
        out['FireLogic'] = pick(fl, 'FireLogicType', 'FireLogicTypeArray', 'RateOfFire', 'RateOfFireForBurst',
                                'ClientFireRateMultiplier', 'ReloadType', 'ReloadTime', 'ReloadTimeBulletsLeft',
                                'ReloadThreshold', 'ReloadDelay', 'PreFireDelay', 'AutomaticDelay', 'TriggerPullWeight')
        out['FireLogic']['BoltAction'] = pick(fl['BoltAction'], 'BoltActionDelay', 'BoltActionTime')
        out['Ammo'] = pick(ffd['Ammo'], 'MagazineCapacity', 'NumberOfMagazines', 'TraceFrequency')
        shot = ffd['Shot']
        out['Shot'] = {'InitialSpeed': shot['InitialSpeed'], 'NumberOfBulletsPerShell': shot['NumberOfBulletsPerShell'],
                       'NumberOfBulletsPerShot': shot['NumberOfBulletsPerShot'],
                       'NumberOfBulletsPerBurst': shot['NumberOfBulletsPerBurst']}
        out['Projectile'] = projectile(c, shot.get('ProjectileData'), root)
        out['OverHeat'] = pick(ffd['OverHeat'], 'HeatPerBullet', 'HeatDropPerSecond', 'OverHeatThreshold')
        disp = ffd.get('Dispersion') or []
        out['ShotDispersion'] = [pick(x, 'MinAngle', 'MaxAngle', 'IncreasePerShot', 'DecreasePerSecond') for x in disp]
    sway, spart = c.resolve(wf.get('WeaponSway'), root)
    if sway:
        st = sway['Stand']
        out['GunSway'] = {
            'partition': spart,
            'FirstShotRecoilMultiplier': sway.get('FirstShotRecoilMultiplier'),
            'ShootingRecoilDecreaseScale': sway.get('ShootingRecoilDecreaseScale'),
            'Stand.NoZoom.BaseValue': dispersion(st['NoZoom'].get('BaseValue')),
            'Stand.NoZoom.Moving': dispersion(st['NoZoom'].get('Moving')),
            'Stand.NoZoom.Recoil': recoil(st['NoZoom'].get('Recoil')),
            'Stand.Zoom.BaseValue': dispersion(st['Zoom'].get('BaseValue')),
            'Stand.Zoom.Moving': dispersion(st['Zoom'].get('Moving')),
            'Stand.Zoom.Recoil': recoil(st['Zoom'].get('Recoil')),
            'Crouch.Zoom.BaseValue': dispersion(sway['Crouch']['Zoom'].get('BaseValue')),
            'Prone.Zoom.BaseValue': dispersion(sway['Prone']['Zoom'].get('BaseValue')),
        }
    return out


def main():
    rows = [summarize(p) for p in sys.argv[2:]]
    json.dump(rows, open(sys.argv[1], 'w', encoding='utf-8'), indent=1)
    for r in rows:
        fl = r.get('FireLogic', {})
        pj = r.get('Projectile', {})
        sw = r.get('GunSway', {})
        print(r['root'], '|', fl.get('FireLogicType'), fl.get('RateOfFire'), '| mag', r.get('Ammo', {}).get('MagazineCapacity'),
              'x', r.get('Ammo', {}).get('NumberOfMagazines'), '| reload', fl.get('ReloadTimeBulletsLeft'), fl.get('ReloadTime'),
              '| v', r.get('Shot', {}).get('InitialSpeed', {}).get('z'), '| dmg', pj.get('StartDamage'), pj.get('EndDamage'),
              pj.get('DamageFalloffStartDistance'), pj.get('DamageFalloffEndDistance'), '| g', pj.get('Gravity'),
              '| pellets', r.get('Shot', {}).get('NumberOfBulletsPerShell'), '| expl', pj.get('Explosion'),
              '| FSRM', sw.get('FirstShotRecoilMultiplier'), '| hip', sw.get('Stand.NoZoom.BaseValue'),
              '| ads', sw.get('Stand.Zoom.BaseValue'), '| rec', sw.get('Stand.Zoom.Recoil'))


main()
