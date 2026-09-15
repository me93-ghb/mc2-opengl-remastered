#!/usr/bin/env python3
"""Headless two-instance multiplayer smoke (MP-4, v0: lobby slice).

Starts a host and a client via dev/macos-run.sh (no window), drives them with the
MC2_MP_AUTOHOST / MC2_MP_AUTOJOIN hooks, waits for the [MP] log lines that prove
MP-1 (join, settings applied, ready, a settings change reaching the client), then
kills both. Exit 0 = all checks seen, 1 = a check timed out (named on stderr).

  python3 tests/mp/run_mp_smoke.py [--port 27600] [--timeout 90] [--keep-logs]
"""
import argparse, os, signal, subprocess, sys, time, re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUN = os.path.join(REPO, "run")
LAUNCH = os.path.join(REPO, "dev", "macos-run.sh")

def start(name, env, logdir):
    log = open(os.path.join(logdir, f"mp_{name}.log"), "w")
    e = dict(os.environ); e.update(env)
    e.pop("MC2_MACOS_WINDOW", None)        # headless
    e["MC2_LOG"] = "1"; e["MC2_SKIP_INTRO"] = "1"
    e.setdefault("MC2_WIDTH", "640"); e.setdefault("MC2_HEIGHT", "480")   # headless: tiny framebuffers, this box has 8 GB
    p = subprocess.Popen([LAUNCH], cwd=RUN, env=e, stdout=log, stderr=subprocess.STDOUT,
                         start_new_session=True)
    return p, log.name

def wait_for(path, pattern, deadline, procs):
    rx = re.compile(pattern)
    while time.time() < deadline:
        for p in procs:
            if p.poll() is not None:
                return f"process exited early (code {p.returncode})"
        with open(path, errors="replace") as f:
            for line in f:
                if rx.search(line):
                    return line.strip()
        time.sleep(0.5)
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=27600)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--keep-logs", action="store_true")
    ap.add_argument("--no-launch", action="store_true", help="stop after the lobby checks")
    ap.add_argument("--play", type=int, default=150, help="seconds to keep both in-mission after start")
    ap.add_argument("--orders", default="1", help="MC2_MP_SCRIPT_ORDERS for the client: 1=attack, move=move-only, capture=capture nearest building, move when none left, vtol=buy a minelayer, lay mines, call artillery, recover=eject a pilot and buy a Karnov recovery")
    ap.add_argument("--drop", choices=["client", "host"], help="kill that side 40 s into the mission and check the other copes")
    ap.add_argument("--rematch", action="store_true", help="after the match ends, expect both to re-enter the lobby, re-ready, relaunch and load a second mission")
    a = ap.parse_args()
    logdir = os.path.join(REPO, "tests", "mp", "logs"); os.makedirs(logdir, exist_ok=True)

    henv = {"MC2_MP_AUTOHOST": "1", "MC2_MP_AUTOTEST": "1", "MC2_MP_PORT": str(a.port), "MC2_MP_SESSION": "smoke"}
    if a.rematch or a.drop:
        henv["MC2_MP_AUTORESULTS"] = "1"; os.environ["MC2_MP_AUTORESULTS"] = "1"
    if a.orders in ("vtol", "recover"):
        henv["MC2_MP_RP"] = "10000"
    if not a.no_launch:
        henv["MC2_MP_AUTOLAUNCH"] = "2"
    host, hlog = start("host", henv, logdir)
    procs = [host]
    t0 = time.time(); deadline = t0 + a.timeout
    checks = []
    def check(name, path, pattern):
        r = wait_for(path, pattern, deadline, procs)
        ok = r is not None and not str(r).startswith("process exited")
        checks.append((name, ok, r))
        print(f"[mp-smoke] {'PASS' if ok else 'FAIL'} {name}: {r}", flush=True)
        return ok

    ok = check("host: hosting", hlog, r"\[MP\] hosting ")
    if ok:
        client, clog = start("client", {"MC2_MP_AUTOJOIN": f"127.0.0.1:{a.port}", "MC2_MP_SCRIPT_ORDERS": a.orders}, logdir)
        procs.append(client)
        ok = (check("host: peer joined", hlog, r"\[MP\] peer .* joined -> commanderID 1")
              and check("client: assigned cid", clog, r"\[MP\] assigned commanderID 1")
              and check("client: settings applied", clog, r"\[MP\] settings applied map=")
              and check("host: client ready", hlog, r"\[MP\] player 1 ready=1")
              and check("host: autotest toggle", hlog, r"\[MP\] autotest toggled airStrike=")
              and check("client: change applied", clog, r"\[MP\] settings applied .*"))
        if ok:  # the second 'settings applied' must carry the toggled value: compare last lines
            hv = re.search(r"airStrike=(\d)", open(hlog).read()).group(1)
            last = [l for l in open(clog, errors="replace") if "[MP] settings applied" in l][-1]
            # airStrike is not in the client line; accept any second application after the toggle
            n_applied = sum(1 for l in open(clog, errors="replace") if "[MP] settings applied" in l)
            ok = n_applied >= 2
            checks.append(("client: >=2 settings applications", ok, f"count={n_applied} hostAirStrike={hv}"))
            print(f"[mp-smoke] {'PASS' if ok else 'FAIL'} client: >=2 settings applications (count={n_applied})", flush=True)
        if ok and not a.no_launch:
            deadline = time.time() + 150   # two headless mission loads
            ok = (check("host: autolaunch", hlog, r"\[MP\] autolaunch")
                  and check("client: zones received", clog, r"\[MP\] mission setup: zones=")
                  and check("host: mission_ready", hlog, r"phase=mission_ready")
                  and check("client: mission_ready", clog, r"phase=mission_ready")
                  and check("host: roster hash", hlog, r"\[MP\] roster hash=")
                  and check("client: roster hash", clog, r"\[MP\] roster hash=")
                  and check("host: all started", hlog, r"\[MP\] mission setup sent subType=5")
                  and check("client: all started", clog, r"\[MP\] wait all started|\[MP\] mission setup sent subType=4"))
            if ok:
                hh = re.search(r"roster hash=([0-9a-f]+) movers=(\d+) local=(\d+)(?: turrets=\d+)? seed=(0x[0-9a-f]+)", open(hlog, errors="replace").read())
                ch = re.search(r"roster hash=([0-9a-f]+) movers=(\d+) local=(\d+)(?: turrets=\d+)? seed=(0x[0-9a-f]+)", open(clog, errors="replace").read())
                same = hh and ch and hh.group(1) == ch.group(1) and hh.group(4) == ch.group(4)
                checks.append(("roster+seed identical", bool(same), f"host={hh.groups() if hh else None} client={ch.groups() if ch else None}"))
                print(f"[mp-smoke] {'PASS' if same else 'FAIL'} roster+seed identical: host={hh.groups() if hh else None} client={ch.groups() if ch else None}", flush=True)
                ok = bool(same)
            if ok and a.drop:
                # Drop-out test: kill one side hard 40 s in, then judge the survivor from its log.
                time.sleep(40)
                victim, survivor, slog = (client, host, hlog) if a.drop == "client" else (host, client, clog)
                os.killpg(os.getpgid(victim.pid), signal.SIGKILL); victim.wait(timeout=10); procs.remove(victim)
                t_kill = time.time()
                print(f"[mp-smoke] killed {a.drop} at mission t+40s", flush=True)
                def within(name, path, pat, secs):
                    r = wait_for(path, pat, t_kill + secs, procs)
                    good = r is not None and not str(r).startswith("process exited")
                    print(f"[mp-smoke] {'PASS' if good else 'FAIL'} {name} (within {secs}s): {r}", flush=True)
                    return good
                if a.drop == "client":
                    ok = within("host: noticed the drop", hlog, r"\[MP\] peer .* left", 20)
                    time.sleep(45)
                    alive = host.poll() is None
                    pos = [float(re.search(r"t=([0-9.]+)", l).group(1)) for l in open(hlog, errors="replace") if "[MP_POS]" in l]
                    ticking = alive and pos and pos[-1] > 80.0
                    print(f"[mp-smoke] {'PASS' if ticking else 'FAIL'} host: still running the match 45 s after the drop (alive={alive}, last t={pos[-1] if pos else None})", flush=True)
                    over = [l.strip() for l in open(hlog, errors="replace") if "[MP] mission over" in l]
                    print(f"[mp-smoke] INFO host mission over: {over[:1]}", flush=True)
                    ok = ok and ticking
                else:
                    ok = (within("client: noticed the drop", clog, r"\[MP\] host connection lost", 30)
                          and within("client: ended the mission", clog, r"\[MP\] host gone: ending mission", 35)
                          and within("client: back at the main menu", clog, r"\[MP\] host gone: back to main menu", 90))
                    alive = client.poll() is None
                    print(f"[mp-smoke] {'PASS' if alive else 'FAIL'} client: still running", flush=True)
                    ok = ok and alive
            elif ok:
                # Play until both sides report the match over (Elimination), or --play seconds.
                t_play = time.time(); over_h = over_c = None
                while time.time() - t_play < a.play:
                    over_h = over_h or next((l for l in open(hlog, errors="replace") if "[MP] mission over" in l), None)
                    over_c = over_c or next((l for l in open(clog, errors="replace") if "[MP] mission over" in l), None)
                    if (over_h and over_c) or any(p.poll() is not None for p in procs):
                        break
                    time.sleep(2)
                time.sleep(3)
                alive = all(p.poll() is None for p in procs)
                print(f"[mp-smoke] {'PASS' if alive else 'FAIL'} both alive after {time.time()-t_play:.0f}s in mission", flush=True)
                # MP-3 slice 1: a client order reaches the host and moves that mech on the host
                deadline = time.time() + 5   # post-play checks read logs already written
                got = check("host: client order applied", hlog, r"\[MP\] order from commanderID 1: .*applied=[1-9]")
                moved = False
                if got:
                    first, last = {}, {}
                    for l in open(hlog, errors="replace"):
                        mm = re.search(r"\[MP_POS\] t=\S+ cid=1 idx=(\d+) r=(\d+) c=(\d+)", l)
                        if mm:
                            k = mm.group(1); v = (mm.group(2), mm.group(3))
                            first.setdefault(k, v); last[k] = v
                    moved = any(first[k] != last[k] for k in first)
                    checks.append(("host: client mechs moved", moved, f"tracked={len(first)}"))
                    nm = "INFO" if a.orders == "recover" else "FAIL"   # the recover run keeps the lance still on purpose
                    print(f"[mp-smoke] {'PASS' if moved else nm} host: client mechs moved (tracked={len(first)})", flush=True)
                ok = ok and got and (moved or a.orders == "recover")
                # MP-3 slice 2: the client's own mechs move on the CLIENT, and host/client cells agree
                def samples(path):
                    first, last = {}, {}
                    for l in open(path, errors="replace"):
                        mm = re.search(r"\[MP_POS\] t=\S+ cid=(\d+) idx=(\d+) r=(\d+) c=(\d+)", l)
                        if mm:
                            k = (mm.group(1), mm.group(2)); v = (int(mm.group(3)), int(mm.group(4)))
                            first.setdefault(k, v); last[k] = v
                    return first, last
                hf, hl = samples(hlog); cf, cl = samples(clog)
                cmoved = any(cf[k] != cl[k] for k in cf if k[0] == "1") or a.orders == "recover"
                print(f"[mp-smoke] {'PASS' if cmoved else 'FAIL'} client: own mechs moved on client (tracked={sum(1 for k in cf if k[0]=='1')})", flush=True)
                common = [k for k in hl if k in cl]
                agree = sum(1 for k in common if abs(hl[k][0]-cl[k][0]) <= 3 and abs(hl[k][1]-cl[k][1]) <= 3)
                pct = (100.0 * agree / len(common)) if common else 0.0
                agreed = pct >= 75.0
                print(f"[mp-smoke] {'PASS' if agreed else 'FAIL'} host/client final cells within 3: {agree}/{len(common)} ({pct:.0f}%)", flush=True)
                checks.append(("client mechs moved", cmoved, "")); checks.append(("cells agree", agreed, f"{pct:.0f}%"))
                ok = ok and cmoved and agreed
                # MP-3 slice 3: combat traffic flowed and damage landed on the client
                fired = any("[MP] fire chunks sent" in l for l in open(hlog, errors="replace"))
                hitsent = any("[MP] weapon hits sent" in l for l in open(hlog, errors="replace"))
                hitapplied = [l for l in open(clog, errors="replace") if "[MP] weapon hits applied=" in l and "applied=0" not in l]
                nf = "INFO" if a.orders in ("capture", "vtol", "recover") else "FAIL"   # combat is not required in capture runs
                print(f"[mp-smoke] {'PASS' if fired else nf} host: weapon fire relayed", flush=True)
                print(f"[mp-smoke] {'PASS' if hitsent else nf} host: weapon hits sent", flush=True)
                print(f"[mp-smoke] {'PASS' if hitapplied else nf} client: weapon hits applied ({len(hitapplied)} batches)", flush=True)
                checks += [("fire relayed", fired, ""), ("hits sent", hitsent, ""), ("hits applied", bool(hitapplied), "")]
                if a.orders not in ("capture", "vtol", "recover"):   # those runs need not bring the lances together
                    ok = ok and fired and hitsent and bool(hitapplied)
                # MP-3 slice 4: world events (kills/losses at least) reach the client
                wsent = any("[MP] world updates sent=" in l for l in open(hlog, errors="replace"))
                if wsent:  # relay latency: give the client a moment to log the application
                    wait_for(clog, r"\[MP\] world updates applied=[1-9]", time.time() + 15, procs)
                wapplied = [l for l in open(clog, errors="replace") if "[MP] world updates applied=" in l and "applied=0" not in l]
                print(f"[mp-smoke] {'PASS' if wsent else 'FAIL'} host: world updates sent", flush=True)
                print(f"[mp-smoke] {'PASS' if wapplied else 'FAIL'} client: world updates applied ({len(wapplied)} batches)", flush=True)
                checks += [("world sent", wsent, ""), ("world applied", bool(wapplied), "")]
                ok = ok and wsent and bool(wapplied)
                ended = [l.strip() for p in (hlog, clog) for l in open(p, errors="replace") if "[MP] mission over" in l]
                # A mission end is fine (that's a won match) as long as both sides agree on the
                # winner and it did not happen in the first 20 s.
                winners = set(re.search(r"winningTeam=(-?\d+)", l).group(1) for l in ended)
                times = [float(re.search(r" t=([0-9.]+)", l).group(1)) for l in ended]
                end_ok = (not ended) or (len(ended) == 2 and len(winners) == 1 and min(times) > 20.0)
                print(f"[mp-smoke] {'PASS' if end_ok else 'FAIL'} mission end consistent: {ended[:2]}", flush=True)
                ok = ok and alive and end_ok
        if a.drop:
            pass    # drop runs are judged above
        elif a.orders == "recover":
            # Karnov recovery: the client ejects a pilot, asks for a recovery, every machine flies the
            # Karnov and re-crews the same mech; the mech's pilot is alive again on both sides.
            def grab(path, pat): return [re.search(pat, l).groups() for l in open(path, errors="replace") if re.search(pat, l)]
            wait_for(clog, r"\[MP_KARNOV\] recovered", time.time() + 30, procs)
            ejected = any("[MP_KARNOV] eject sent" in l for l in open(clog, errors="replace"))
            asked = any("[MP_KARNOV] request sent" in l for l in open(clog, errors="replace"))
            hreq, creq = grab(hlog, r"\[MP_KARNOV\] request cid=(\d+) idx=(\d+)"), grab(clog, r"\[MP_KARNOV\] request cid=(\d+) idx=(\d+)")
            hrec, crec = grab(hlog, r"\[MP_KARNOV\] recovered cid=(\d+) idx=(\d+) pilot=(\S+) alive=(\d)"), grab(clog, r"\[MP_KARNOV\] recovered cid=(\d+) idx=(\d+) pilot=(\S+) alive=(\d)")
            rec_ok = ejected and asked and hreq and hreq == creq and hrec and hrec == crec and all(x[3] == "1" for x in hrec)
            print(f"[mp-smoke] {'PASS' if rec_ok else 'FAIL'} karnov recovery: ejected={ejected} asked={asked} request host={hreq} client={creq} recovered host={hrec} client={crec}", flush=True)
            def rp(path):
                tot = {}
                for l in open(path, errors="replace"):
                    mm = re.search(r"\[MP_RP\] cid=(\d+) delta=(-?\d+) total=(-?\d+)", l)
                    if mm: tot[mm.group(1)] = mm.group(3)
                return tot
            hr, cr = rp(hlog), rp(clog)
            rp_ok = bool(hr) and hr == cr
            print(f"[mp-smoke] {'PASS' if rp_ok else 'FAIL'} resource points agree: host={hr} client={cr}", flush=True)
            ok = ok and rec_ok and rp_ok
        elif a.orders == "vtol":
            # Reinforcement: client buys a minelayer, host assigns a slot, both create it in that slot;
            # the minelayer lays mines that reach the client; one artillery strike relays; RP totals agree.
            def grab(path, pat): return [re.search(pat, l).groups() for l in open(path, errors="replace") if re.search(pat, l)]
            wait_for(clog, r"\[MP_VTOL\] landed", time.time() + 15, procs)
            bought = any("[MP_VTOL] purchase" in l for l in open(clog, errors="replace"))
            hreq, creq = grab(hlog, r"\[MP_VTOL\] request cid=(\d+) vehicle=(\d+) idx=(\d+)"), grab(clog, r"\[MP_VTOL\] request cid=(\d+) vehicle=(\d+) idx=(\d+)")
            hland, cland = grab(hlog, r"\[MP_VTOL\] landed cid=(\d+) vehicle=(\d+) idx=(\d+) wid=(-?\d+)"), grab(clog, r"\[MP_VTOL\] landed cid=(\d+) vehicle=(\d+) idx=(\d+) wid=(-?\d+)")
            vt_ok = bought and hreq and hreq == creq and hland and cland and [x[:3] for x in hland] == [x[:3] for x in cland] and all(x[3] != "-1" for x in hland + cland)
            print(f"[mp-smoke] {'PASS' if vt_ok else 'FAIL'} reinforcement drop: bought={bought} request host={hreq} client={creq} landed host={hland} client={cland}", flush=True)
            wait_for(clog, r"\[MP_MINE\]", time.time() + 10, procs)
            hm, cm = grab(hlog, r"\[MP_MINE\] r=(\d+) c=(\d+) state=(\d+)"), grab(clog, r"\[MP_MINE\] r=(\d+) c=(\d+) state=(\d+)")
            mine_ok = bool(hm) and all(x in cm for x in hm)
            print(f"[mp-smoke] {'PASS' if mine_ok else 'FAIL'} mines relayed: host={len(hm)} client={len(cm)} missing={[x for x in hm if x not in cm][:3]}", flush=True)
            wait_for(clog, r"\[MP_ART\] cid=", time.time() + 10, procs)
            ha, ca = grab(hlog, r"\[MP_ART\] cid=(\d+) type=(\d+)"), grab(clog, r"\[MP_ART\] cid=(\d+) type=(\d+)")
            art_ok = bool(ha) and ha == ca
            print(f"[mp-smoke] {'PASS' if art_ok else 'FAIL'} artillery relayed: host={ha} client={ca}", flush=True)
            def rp(path):
                tot = {}
                for l in open(path, errors="replace"):
                    mm = re.search(r"\[MP_RP\] cid=(\d+) delta=(-?\d+) total=(-?\d+)", l)
                    if mm: tot[mm.group(1)] = mm.group(3)
                return tot
            hr, cr = rp(hlog), rp(clog)
            rp_ok = bool(hr) and hr == cr
            print(f"[mp-smoke] {'PASS' if rp_ok else 'FAIL'} resource points agree: host={hr} client={cr}", flush=True)
            ok = ok and vt_ok and mine_ok and art_ok and rp_ok
        elif a.orders == "capture":
            # Captures: every [MP_CAP] the host queued must be applied on the client, and the
            # resource-point totals per commander must end up identical on both sides.
            def caps(path): return [l.strip().split("] ",1)[1] for l in open(path, errors="replace") if "[MP_CAP]" in l]
            wait_for(clog, r"\[MP_CAP\]", time.time() + 15, procs)
            hc, cc = caps(hlog), caps(clog)
            cap_ok = bool(hc) and all(x in cc for x in hc)
            print(f"[mp-smoke] {'PASS' if cap_ok else 'FAIL'} captures relayed: host={len(hc)} client={len(cc)} missing={[x for x in hc if x not in cc][:3]}", flush=True)
            def rp(path):
                tot = {}
                for l in open(path, errors="replace"):
                    mm = re.search(r"\[MP_RP\] cid=(\d+) delta=(-?\d+) total=(-?\d+)", l)
                    if mm: tot[mm.group(1)] = mm.group(3)
                return tot
            hr, cr = rp(hlog), rp(clog)
            rp_ok = bool(hr) and hr == cr
            print(f"[mp-smoke] {'PASS' if rp_ok else 'FAIL'} resource points agree: host={hr} client={cr}", flush=True)
            ok = ok and cap_ok and rp_ok
        if ok and a.rematch:
            # Rematch: results dismissed by MC2_MP_AUTORESULTS, both back in the lobby ([2][1]),
            # client re-readies, host relaunches, both load a second mission with identical rosters.
            def count(path, pat):
                rx = re.compile(pat); return sum(1 for l in open(path, errors="replace") if rx.search(l))
            def wait_count(name, path, pat, n, deadline):
                while time.time() < deadline:
                    if any(p.poll() is not None for p in procs):
                        print(f"[mp-smoke] FAIL {name}: process exited early", flush=True); return False
                    if count(path, pat) >= n:
                        print(f"[mp-smoke] PASS {name}", flush=True); return True
                    time.sleep(1)
                print(f"[mp-smoke] FAIL {name}: timeout (count={count(path, pat)} < {n})", flush=True); return False
            dl = time.time() + 300
            ok = (wait_count("host: mission over", hlog, r"\[MP\] mission over", 1, dl)
                  and wait_count("client: mission over", clog, r"\[MP\] mission over", 1, dl)
                  and wait_count("host: results dismissed", hlog, r"\[MP\] autoresults", 1, dl)
                  and wait_count("client: results dismissed", clog, r"\[MP\] autoresults", 1, dl)
                  and wait_count("host: back in lobby", hlog, r"\[MP\] screen \[2\]\[1\]", 2, dl)
                  and wait_count("client: back in lobby", clog, r"\[MP\] screen \[2\]\[1\]", 2, dl)
                  and wait_count("host: second autolaunch", hlog, r"\[MP\] autolaunch", 2, dl)
                  and wait_count("host: second mission_ready", hlog, r"phase=mission_ready", 2, dl)
                  and wait_count("client: second mission_ready", clog, r"phase=mission_ready", 2, dl)
                  and wait_count("host: second roster", hlog, r"\[MP\] roster hash=", 2, dl)
                  and wait_count("client: second roster", clog, r"\[MP\] roster hash=", 2, dl))
            if ok:
                rx = re.compile(r"roster hash=([0-9a-f]+) .*seed=(0x[0-9a-f]+)")
                hh = [rx.search(l).groups() for l in open(hlog, errors="replace") if rx.search(l)]
                ch = [rx.search(l).groups() for l in open(clog, errors="replace") if rx.search(l)]
                ok = len(hh) >= 2 and len(ch) >= 2 and hh[1] == ch[1]
                print(f"[mp-smoke] {'PASS' if ok else 'FAIL'} rematch roster+seed identical: host={hh[1:2]} client={ch[1:2]}", flush=True)
        unhandled = [l.strip() for p in (hlog, clog) for l in open(p, errors="replace") if "unhandled msg type" in l]
        if unhandled:
            ok = False; print("[mp-smoke] FAIL unhandled messages:", unhandled[:5], flush=True)
    for p in procs:
        try: os.killpg(os.getpgid(p.pid), signal.SIGTERM)
        except ProcessLookupError: pass
    for p in procs:
        try: p.wait(timeout=10)
        except subprocess.TimeoutExpired: os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    verdict = "PASS" if ok else "FAIL"
    print(f"[mp-smoke] result={verdict} elapsed={time.time()-t0:.1f}s logs={logdir}", flush=True)
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
