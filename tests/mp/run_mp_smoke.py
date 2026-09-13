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
    ap.add_argument("--play", type=int, default=15, help="seconds to keep both in-mission after start")
    a = ap.parse_args()
    logdir = os.path.join(REPO, "tests", "mp", "logs"); os.makedirs(logdir, exist_ok=True)

    henv = {"MC2_MP_AUTOHOST": "1", "MC2_MP_AUTOTEST": "1", "MC2_MP_PORT": str(a.port), "MC2_MP_SESSION": "smoke"}
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
        client, clog = start("client", {"MC2_MP_AUTOJOIN": f"127.0.0.1:{a.port}"}, logdir)
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
                hh = re.search(r"roster hash=([0-9a-f]+) movers=(\d+) local=(\d+) seed=(0x[0-9a-f]+)", open(hlog, errors="replace").read())
                ch = re.search(r"roster hash=([0-9a-f]+) movers=(\d+) local=(\d+) seed=(0x[0-9a-f]+)", open(clog, errors="replace").read())
                same = hh and ch and hh.group(1) == ch.group(1) and hh.group(4) == ch.group(4)
                checks.append(("roster+seed identical", bool(same), f"host={hh.groups() if hh else None} client={ch.groups() if ch else None}"))
                print(f"[mp-smoke] {'PASS' if same else 'FAIL'} roster+seed identical: host={hh.groups() if hh else None} client={ch.groups() if ch else None}", flush=True)
                ok = bool(same)
            if ok:
                time.sleep(a.play)
                alive = all(p.poll() is None for p in procs)
                print(f"[mp-smoke] {'PASS' if alive else 'FAIL'} both alive after {a.play}s in mission", flush=True)
                ended = [l.strip() for p in (hlog, clog) for l in open(p, errors="replace") if "[MP] mission over" in l]
                print(f"[mp-smoke] {'FAIL' if ended else 'PASS'} no premature mission end: {ended[:2]}", flush=True)
                ok = alive and not ended
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
