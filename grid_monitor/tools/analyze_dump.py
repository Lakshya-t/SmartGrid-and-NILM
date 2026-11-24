#!/usr/bin/env python3
"""
analyze_dump.py - recompute the Grid Monitor features on a PC.

Use it to cross-check the firmware: capture raw samples with
    grid dump 2048
(see grid_console.py), then run
    python analyze_dump.py dump.txt [--plot] [--turns N] [--invert]

--demo resistive|inductive|smps   runs on synthetic data (no board needed).
Needs: numpy (and matplotlib for --plot).
"""
import argparse
import re
import sys
import numpy as np

# Same illustrative thresholds as firmware defaults (src/cfg.c). TUNE THESE.
T = dict(min_irms=0.02, thd_res=0.10, pf_res=0.90, thd_smps=0.30, h3_smps=0.15,
         inrush_motor=2.0)


def parse_dump(path):
    hdr, v, i, inside = {}, [], [], False
    for line in open(path, errors="ignore"):
        line = line.strip()
        if line.startswith("DUMP BEGIN"):
            hdr = {k: float(x) for k, x in re.findall(r"(\w+)=([-\d.eE+]+)", line)}
            inside, v, i = True, [], []
        elif line.startswith("DUMP END"):
            inside = False
        elif inside and re.fullmatch(r"\d+,\d+", line):
            a, b = line.split(",")
            v.append(int(a))
            i.append(int(b))
    if not v:
        sys.exit("no DUMP BEGIN ... DUMP END block found in " + path)
    if len(v) < 1024:
        sys.exit(f"only {len(v)} samples - capture at least 1024 (use: grid dump 2048)")
    return hdr, np.array(v, float), np.array(i, float)


def demo(kind, fs=10240.0, n=2048, f=50.0):
    t = np.arange(n) / fs
    th = 2 * np.pi * f * t
    kv, ki = 230 * np.sqrt(2) / 800.0, 0.007324
    v = 2100 + 800 * np.sin(th)
    pk = lambda a: a * np.sqrt(2) / ki
    if kind == "resistive":
        i = 2048 + pk(0.5) * np.sin(th)
    elif kind == "inductive":
        i = 2048 + pk(0.5) * np.sin(th - np.radians(45))
    else:
        a = np.abs(np.sin(th)) - 0.8
        i = 2048 + np.where(a > 0, a / 0.2, 0) * np.sign(np.sin(th)) * pk(0.8)
    rng = np.random.default_rng(1)
    v, i = np.round(v + rng.normal(0, .7, n)), np.round(i + rng.normal(0, .7, n))
    return dict(fs=fs, n=n, kv=kv, ki=ki), v, i


def whole_cycles(v, hyst=30):
    """Index range of complete mains cycles (rising zero crossings of v)."""
    x = v - v.mean()
    armed, cr = False, []
    for k, s in enumerate(x):
        if s <= -hyst:
            armed = True
        elif armed and s >= 0:
            armed = False
            cr.append(k)
    return (cr[0], cr[-1]) if len(cr) >= 2 else (0, len(v))


def analyze(hdr, v, i, invert=False, hmax=50):
    fs, kv, ki = hdr["fs"], hdr["kv"], hdr["ki"]
    sgn = -1.0 if invert else 1.0
    a, b = whole_cycles(v)
    vv, ii = v[a:b] - v[a:b].mean(), sgn * (i[a:b] - i[a:b].mean())
    vrms, irms = np.sqrt(np.mean(vv ** 2)) * kv, np.sqrt(np.mean(ii ** 2)) * ki
    p = np.mean(vv * ii) * kv * ki
    s = vrms * irms
    r = dict(vrms=vrms, irms=irms, p=p, s=s, pf=p / s if s else 0.0,
             q=np.sqrt(max(s * s - p * p, 0)), ipeak=np.max(np.abs(ii)) * ki)

    n = len(v)
    w = 0.5 * (1 - np.cos(2 * np.pi * np.arange(n) / (n - 1)))
    V = np.fft.rfft((v - v.mean()) * w)
    I = np.fft.rfft(sgn * (i - i.mean()) * w)
    df = fs / n
    k0 = int(round(50.0 / df))
    kf = k0 - 2 + int(np.argmax(np.abs(V[k0 - 2:k0 + 3])))
    amp = lambda X, k: 4 * np.abs(X[k]) / n
    peak = lambda X, k: max((k - 1, k, k + 1), key=lambda q: abs(X[q]))
    i1 = amp(I, peak(I, kf))
    hs = {}
    for h in range(2, hmax + 1):
        k = h * kf
        if k + 1 >= len(I):
            break
        hs[h] = amp(I, peak(I, k))
    thd = np.sqrt(sum(x * x for x in hs.values())) / i1 if i1 else 0.0
    phi = np.angle(V[kf]) - np.angle(I[kf])
    phi = (phi + np.pi) % (2 * np.pi) - np.pi
    crest = np.max(np.abs(ii)) / (np.sqrt(np.mean(ii ** 2)) or 1)
    r.update(i1=i1 / np.sqrt(2) * ki, h3=hs.get(3, 0) / i1, h5=hs.get(5, 0) / i1,
             h7=hs.get(7, 0) / i1, thd=thd, phi1_deg=np.degrees(phi), crest=crest,
             freq_est=kf * df)
    return r


def classify(r):
    if r["irms"] < T["min_irms"]:
        return "No load"
    if r["thd"] >= T["thd_smps"] and r["h3"] >= T["h3_smps"]:
        return "Switch-mode supply"
    if r["thd"] < T["thd_res"] and abs(r["pf"]) >= T["pf_res"]:
        return "Resistive"
    if r["thd"] < T["thd_smps"] and abs(r["pf"]) < T["pf_res"]:
        return "Motor/inductive"
    return "Unknown"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?")
    ap.add_argument("--demo", choices=["resistive", "inductive", "smps"])
    ap.add_argument("--invert", action="store_true", help="clamp fitted backwards")
    ap.add_argument("--turns", type=int, default=None,
                    help="override turns: scales amps (header ki assumes the firmware setting)")
    ap.add_argument("--plot", action="store_true")
    a = ap.parse_args()
    if a.demo:
        hdr, v, i = demo(a.demo)
    elif a.file:
        hdr, v, i = parse_dump(a.file)
    else:
        ap.error("give a dump file or --demo")
    if a.turns:
        hdr["ki"] = hdr["ki"] / a.turns
    r = analyze(hdr, v, i, a.invert)
    print(f"samples {len(v)}   fs {hdr['fs']:.3f} Hz   kv {hdr['kv']:.5f}   ki {hdr['ki']:.6f}")
    print(f"Vrms {r['vrms']:.1f} V   Irms {r['irms']:.3f} A   Ipeak {r['ipeak']:.3f} A")
    print(f"P {r['p']:.1f} W   S {r['s']:.1f} VA   Q(non-active) {r['q']:.1f} var   PF {r['pf']:.3f}")
    print(f"I1 {r['i1']:.3f} A   h3 {r['h3']:.2f}  h5 {r['h5']:.2f}  h7 {r['h7']:.2f}   "
          f"THD {r['thd'] * 100:.1f} %   phi1 {r['phi1_deg']:.1f} deg   crest {r['crest']:.2f}")
    print("class:", classify(r))
    if a.plot:
        import matplotlib.pyplot as plt
        fs = hdr["fs"]
        t = np.arange(len(v)) / fs * 1000
        fig, ax = plt.subplots(2, 1, figsize=(9, 6))
        m = min(len(v), 600)
        ax[0].plot(t[:m], (v[:m] - v.mean()) * hdr["kv"], label="voltage (V)")
        ax[0].plot(t[:m], (i[:m] - i.mean()) * hdr["ki"] * 100, label="current x100 (A)")
        ax[0].set_xlabel("ms"); ax[0].legend(); ax[0].grid(True)
        w = np.hanning(len(i))
        sp = 4 * np.abs(np.fft.rfft((i - i.mean()) * w)) / len(i) / np.sqrt(2) * hdr["ki"]
        f = np.fft.rfftfreq(len(i), 1 / fs)
        ax[1].stem(f[:520], sp[:520]); ax[1].set_xlabel("Hz"); ax[1].set_ylabel("A rms"); ax[1].grid(True)
        plt.tight_layout(); plt.show()


if __name__ == "__main__":
    main()
