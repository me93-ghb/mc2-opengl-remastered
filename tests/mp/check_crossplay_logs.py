#!/usr/bin/env python3
"""Check host/client logs from an automated cross-platform match.

Usage: python tests/mp/check_crossplay_logs.py host.log client.log
Launch both builds with MC2_LOG=1 and the existing MP smoke-test hooks.
"""
import pathlib
import re
import sys

host, client = (pathlib.Path(p).read_text(errors='replace') for p in sys.argv[1:3])
checks = []
def check(name, ok, detail=''):
    checks.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'} {name}" + (f': {detail}' if detail else ''))
def find(pattern, text):
    return re.findall(pattern, text)

check('host accepted peer', find(r'\[MP\] peer .* joined -> commanderID 1', host))
check('client received identity and settings', '[MP] assigned commanderID 1' in client and '[MP] settings applied map=' in client)
check('chat relayed', 'The game will begin in 5 seconds.' in host and 'The game will begin in 5 seconds.' in client)
check('no rejected packets', '[MP_PROTOCOL]' not in host + client)
hr = find(r'roster hash=(\w+) movers=(\d+) local=\d+ turrets=(\d+) seed=(0x\w+)', host)
cr = find(r'roster hash=(\w+) movers=(\d+) local=\d+ turrets=(\d+) seed=(0x\w+)', client)
check('identical initial roster and seed', hr and cr and hr[0] == cr[0], str((hr[:1], cr[:1])))
check('both entered mission', 'phase=mission_ready' in host and 'phase=mission_ready' in client)
check('host applied client order', find(r'order from commanderID 1: .*applied=[1-9]', host))
def positions(text):
    first, last = {}, {}
    for cid, idx, r, c in find(r'\[MP_POS\] t=\S+ cid=(\d+) idx=(\d+) r=(\d+) c=(\d+)', text):
        key = (cid, idx); value = (int(r), int(c))
        first.setdefault(key, value); last[key] = value
    return first, last
hf, hl = positions(host); cf, cl = positions(client)
for label, first, last in [('host',hf,hl), ('client',cf,cl)]:
    check(f'client mechs moved on {label}', any(first[k] != last[k] for k in first if k[0] == '1'))
common = hl.keys() & cl.keys()
agree = sum(max(abs(hl[k][i]-cl[k][i]) for i in (0,1)) <= 3 for k in common)
check('final positions agree within 3 cells', common and agree / len(common) >= 0.75, f'{agree}/{len(common)}')
check('host relayed weapon fire', find(r'fire chunks sent=[1-9]', host))
check('host sent damage', find(r'weapon hits sent=[1-9]', host))
check('client applied damage', find(r'weapon hits applied=[1-9]', client))
check('host sent world events', find(r'world updates sent=[1-9]', host))
check('client applied world events', find(r'world updates applied=[1-9]', client))
hw = find(r'\[MP\] mission over: winningTeam=(-?\d+)', host)
cw = find(r'\[MP\] mission over: winningTeam=(-?\d+)', client)
check('match ended with same winner', hw and cw and hw[0] == cw[0], str((hw,cw)))
sys.exit(0 if all(checks) else 1)
