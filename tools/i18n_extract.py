#!/usr/bin/env python3
"""Lists the English UI strings of the viewer (keys of src/core/i18n_vi.inc) that have no
Vietnamese translation yet. Usage: python3 tools/i18n_extract.py [--all]"""
import re, sys, pathlib
root = pathlib.Path(__file__).resolve().parent.parent
LIT = r'"(?:[^"\\]|\\.)*"'
def joined(group):  # adjacent literals "a" "b" -> a b
    return ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', group))
keys = []
for f in sorted((root / 'src' / 'app').glob('*.cpp')):
    s = f.read_text(encoding='utf-8')
    for m in re.finditer(r'\btr(?:Id)?\(\s*(' + LIT + r'(?:\s*' + LIT + r')*)\s*\)', s):
        keys.append(joined(m.group(1)))
    # tables whose entries are translated at display time
    for table in ('kTools', 'kShortcuts', 'kSteps', 'kTitles', 'kHistoryLabels', 'kSelectionNames'):
        m = re.search(table + r'\[\]\s*=\s*\{(.*?)\};', s, re.S)
        if m:
            for lit in re.finditer(LIT + r'(?:\s*' + LIT + r')*', m.group(1)):
                keys.append(joined(lit.group(0)))
seen, ordered = set(), []
for k in keys:
    if k and k not in seen and re.search('[A-Za-z]', k):
        seen.add(k); ordered.append(k)
inc = (root / 'src' / 'core' / 'i18n_vi.inc').read_text(encoding='utf-8')
have = set(joined(m.group(1)) for m in re.finditer(r'\{\s*(' + LIT + r'(?:\s*' + LIT + r')*)\s*,', inc))
todo = ordered if '--all' in sys.argv else [k for k in ordered if k not in have]
for k in todo: print(k)
print(f'# {len(ordered)} keys, {len(ordered) - len([k for k in ordered if k not in have])} translated', file=sys.stderr)
