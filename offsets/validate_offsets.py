#!/usr/bin/env python3
"""Standalone offset validator for the RRTool public (NoKey) distribution.

`tools/sdk_offsets.py --check` regenerates the offsets from the IL2CPP/CppSDK
dump and compares. That dump is intentionally absent from the NoKey package, so
--check cannot run here (it fails on a missing DumpManifest.sha256). This script
needs no dump: it cross-checks the generated C headers against their committed
JSON reports and flags any divergence. Use it as the pre-commit / CI gate for
the public package.

Checks (exit 1 on any failure, 0 if all pass):
  * robrun_offsets.h      <-> robrun_offsets.json        (field offsets equal)
  * robrun_sdk_offsets.h  <-> robrun_sdk_offsets.json    (field offsets equal)
  * RR_SF_* static-field RVAs <-> robrun_offsets.json static_field_globals
  * no duplicate #define in a header
  * robrun_buildinfo_extra.json parses and does not redefine a header macro
    with a different value

Layout assumed (override with --root):
  <root>/offsets/*.json
  <root>/2_src_port/*.h
"""
import argparse
import json
import re
import sys
from pathlib import Path

DEFINE = re.compile(r'^\s*#define\s+(RR_\w+)\s+(0x[0-9A-Fa-f]+)u?\b')


def parse_header(path):
    """Return ({macro: int}, [duplicate macros])."""
    out, dups = {}, []
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        m = DEFINE.match(line)
        if not m:
            continue
        name, val = m.group(1), int(m.group(2), 16)
        if name in out:
            dups.append(name)
        out[name] = val
    return out, dups


def load_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def json_field_offsets(data):
    """Return {macro: int} from a report's 'fields' ('offset' or 'value')."""
    out = {}
    for macro, entry in data.get('fields', {}).items():
        if isinstance(entry, dict):
            if 'offset' in entry:
                out[macro] = int(entry['offset'])
            elif 'value' in entry:
                out[macro] = int(entry['value'])
    return out


def check_pair(name, header, jpath, errors):
    if not header.exists():
        errors.append(f'{name}: missing header {header}')
        return {}
    if not jpath.exists():
        errors.append(f'{name}: missing json {jpath}')
        return parse_header(header)[0]
    hmap, dups = parse_header(header)
    for d in sorted(set(dups)):
        errors.append(f'{name}: duplicate #define {d} in {header.name}')
    jmap = json_field_offsets(load_json(jpath))
    for macro, jo in jmap.items():
        if macro not in hmap:
            errors.append(f'{name}: {macro} present in {jpath.name} but not in {header.name}')
        elif hmap[macro] != jo:
            errors.append(f'{name}: {macro} MISMATCH {header.name}=0x{hmap[macro]:X} '
                          f'{jpath.name}=0x{jo:X}')
    for macro in hmap:
        if macro.startswith('RR_OFF_') and macro not in jmap:
            errors.append(f'{name}: {macro} present in {header.name} but not in {jpath.name}')
    print(f'  [ok] {name}: {len(jmap)} field offsets match ({header.name} <-> {jpath.name})')
    return hmap


def check_static_fields(header_map, off_json, errors):
    data = load_json(off_json) if off_json.exists() else {}
    globals_ = data.get('static_field_globals', {})
    by_upper = {k.upper(): v for k, v in globals_.items()}
    checked = 0
    for macro, rva in header_map.items():
        if not macro.startswith('RR_SF_'):
            continue
        key = macro[len('RR_SF_'):]
        entry = by_upper.get(key)
        if entry is None:
            print(f'  [warn] {macro}: no static_field_globals entry to confirm (probe live)')
            continue
        want = int(entry.get('rva', -1))
        if want != rva:
            errors.append(f'static-field {macro} MISMATCH header=0x{rva:X} json_rva=0x{want:X}')
        else:
            checked += 1
    if checked:
        print(f'  [ok] static-field globals: {checked} RR_SF_* RVAs match static_field_globals')


def check_extra(extra_path, all_header_macros, errors):
    if not extra_path.exists():
        print('  [skip] robrun_buildinfo_extra.json not present')
        return
    try:
        data = load_json(extra_path)
    except (OSError, ValueError) as exc:
        errors.append(f'robrun_buildinfo_extra.json: invalid JSON ({exc})')
        return
    extra = {}
    for sect in ('static_globals', 'new_fields'):
        for macro, e in data.get(sect, {}).items():
            if isinstance(e, dict) and 'offset' in e:
                extra[macro] = int(e['offset'])
    for macro, e in data.get('function_rvas', {}).items():
        if isinstance(e, dict) and 'rva' in e:
            extra[macro] = int(e['rva'])
    collide = 0
    for macro, val in extra.items():
        if macro in all_header_macros and all_header_macros[macro] != val:
            errors.append(f'extra: {macro} redefines a header macro with a different value '
                          f'(header=0x{all_header_macros[macro]:X} extra=0x{val:X})')
            collide += 1
    print(f'  [ok] robrun_buildinfo_extra.json: {len(extra)} entries, {collide} harmful collisions')


def main():
    here = Path(__file__).resolve()
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', type=Path, default=here.parent.parent,
                    help='RRTool package root (default: two levels up from this script)')
    args = ap.parse_args()
    root = args.root.resolve()
    off = root / 'offsets'
    src = root / '2_src_port'
    print(f'validate_offsets: root={root}')

    errors = []
    h_off = check_pair('offsets', src / 'robrun_offsets.h', off / 'robrun_offsets.json', errors)
    h_sdk = check_pair('sdk', src / 'robrun_sdk_offsets.h', off / 'robrun_sdk_offsets.json', errors)
    check_static_fields(h_off, off / 'robrun_offsets.json', errors)
    merged = dict(h_off)
    merged.update(h_sdk)
    check_extra(off / 'robrun_buildinfo_extra.json', merged, errors)

    if errors:
        print(f'\nFAIL: {len(errors)} problem(s):', file=sys.stderr)
        for e in errors:
            print(f'  - {e}', file=sys.stderr)
        return 1
    print('\nOK: headers and JSON reports are consistent.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
