"""Confirm schema/api_inventory.json entries against a local VU-Docs clone, read-only (DESIGN.md section 12.3).

VU-Docs is an API reference only: this module reads its markdown pages at check time and returns booleans and small
integers (present, read-only flag, enum value). It never stores or prints page text, so nothing from VU-Docs enters
the repository. Page paths are relative to <root>/content/vext/ref unless they start with 'content/'.
"""

import os
import re

_MAX_PAGE_BYTES = 512 * 1024


def _page_path(root, doc):
    if doc.startswith('content/'):
        return os.path.join(root, *doc.split('/'))
    return os.path.join(root, 'content', 'vext', 'ref', *doc.split('/'))


def _read(path):
    with open(path, 'rb') as f:
        data = f.read(_MAX_PAGE_BYTES + 1)
    if len(data) > _MAX_PAGE_BYTES:
        raise ValueError('page too large')
    return data.decode('utf-8', errors='replace')


def _md_name(name):
    # Markdown escapes underscores inside bold names (CharacterPoseType\_Stand).
    return re.escape(name).replace('_', r'\\?_')


def confirm(entry, root):
    """Return a dict {found: bool, ro: bool|None, value: int|None, reason: str} for one inventory entry."""
    path = _page_path(root, entry['doc'])
    if not os.path.isfile(path):
        return {'found': False, 'ro': None, 'value': None, 'reason': 'page missing'}
    try:
        text = _read(path)
    except (OSError, ValueError) as exc:
        return {'found': False, 'ro': None, 'value': None, 'reason': 'unreadable: %s' % type(exc).__name__}
    kind = entry['kind']
    sym = entry['sym']
    if kind == 'prop':
        name = sym.split('.', 1)[1]
        m = re.search(r'\{\{<\s*prop\s+"' + re.escape(name) + r'"(\s+true)?\s*>\}\}', text)
        if not m:
            return {'found': False, 'ro': None, 'value': None, 'reason': 'property not on page'}
        return {'found': True, 'ro': bool(m.group(1)), 'value': None, 'reason': ''}
    if kind in ('method', 'event', 'hook', 'ctor'):
        name = sym.split(':', 1)[1] if (kind == 'method' and ':' in sym) else sym
        if kind == 'method' and '.' in name:
            name = name.split('.', 1)[1]
        found = re.search(r'^>\s*\*\*' + re.escape(name) + r'\*\*\(', text, re.M) is not None
        return {'found': found, 'ro': None, 'value': None, 'reason': '' if found else 'signature not on page'}
    if kind == 'enum':
        name = sym.split('.', 1)[1]
        m = re.search(r'^\|\s*\*\*' + _md_name(name) + r'\*\*\s*\|\s*Value:\s*\*\*(-?\d+)\*\*', text, re.M)
        if not m:
            return {'found': False, 'ro': None, 'value': None, 'reason': 'enum value not on page'}
        value = int(m.group(1))
        ok = entry.get('value') is None or entry.get('value') == value
        return {'found': ok, 'ro': None, 'value': value, 'reason': '' if ok else 'enum value differs'}
    if kind == 'global':
        found = re.search(r'\b' + re.escape(sym) + r'\b', text) is not None
        return {'found': found, 'ro': None, 'value': None, 'reason': '' if found else 'name not on page'}
    if kind == 'dtype':
        # A data container type name used with entityData:Is(); the page must exist and, when the entry names a
        # parent, its 'Inherits from' line must name that parent.
        parent = entry.get('parent')
        if parent is None:
            return {'found': True, 'ro': None, 'value': None, 'reason': ''}
        found = re.search(r'^Inherits from \[' + re.escape(parent) + r'\]', text, re.M) is not None
        return {'found': found, 'ro': None, 'value': None, 'reason': '' if found else 'parent differs'}
    if kind == 'etype':
        # An entity type name (entity:Is, EntityManager:GetIterator): a '## <name>' heading on the entity listing.
        found = re.search(r'^##\s+' + re.escape(sym) + r'\s*$', text, re.M) is not None
        return {'found': found, 'ro': None, 'value': None, 'reason': '' if found else 'entity type not listed'}
    return {'found': False, 'ro': None, 'value': None, 'reason': 'unknown kind'}


def confirm_all(entries, root):
    """Confirm every entry; returns (n_confirmed, n_total, failures[list of (sym, doc, reason)], ro{sym: bool})."""
    failures = []
    ro = {}
    n_ok = 0
    for e in entries:
        r = confirm(e, root)
        if r['found']:
            n_ok += 1
            if r['ro'] is not None:
                ro[e['sym']] = r['ro']
        else:
            failures.append((e['sym'], e['doc'], r['reason']))
    return n_ok, len(entries), failures, ro
