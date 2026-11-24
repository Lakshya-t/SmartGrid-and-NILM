import re

def num(pattern, text, cast=float):
    m = re.search(pattern, text, re.I)
    return cast(m.group(1)) if m else None

def status(text):
    d = {"raw": text}
    for line in text.splitlines():
        lo = line.lower()
        if lo.startswith("relay"):
            d["relay"] = line.split(":", 1)[-1].strip()
            d["tripped"] = "tripped" in lo
        elif lo.startswith("mains"):
            d["mains_text"] = line.split(":", 1)[-1].strip()
            d["voltage_v"] = num(r"([-+\d.]+)\s*v\s*rms", line)
            d["frequency_hz"] = num(r"([-+\d.]+)\s*hz", line)
        elif lo.startswith("current"):
            d["current_rms_a"] = num(r"([-+\d.]+)\s*a\s*rms", line)
            d["current_peak_a"] = num(r"([-+\d.]+)\s*a\s*peak", line)
        elif lo.startswith("power"):
            d["active_w"] = num(r"\bp\s+([-+\d.]+)\s*w", line)
            d["apparent_va"] = num(r"\bs\s+([-+\d.]+)\s*va", line)
            d["reactive_var"] = num(r"\bq(?:\(non-active\))?\s+([-+\d.]+)\s*var", line)
            d["pf"] = num(r"\bpf\s+([-+\d.]+)", line)
        elif lo.startswith("load class"):
            d["load_class"] = line.split(":", 1)[-1].strip()
    return d

def stats(text):
    patterns = {
        "timer_clock_hz": (r"timer clock\s+(\d+)\s*hz", int),
        "arr": (r"\barr\s+(\d+)", int),
        "sampling_hz": (r"sampling rate\s+([\d.]+)\s*hz", float),
        "blocks": (r"\bblocks\s+(\d+)", int),
        "adc_errors": (r"\badc_err\s+(\d+)", int),
        "dma_errors": (r"\bdma_err\s+(\d+)", int),
        "fast_trips": (r"\bfast_trips\s+(\d+)", int),
        "cycles": (r"\bcycles\s+(\d+)", int),
        "no_signal": (r"\bno-signal\s+(\d+)", int),
        "ring_overruns": (r"\bring overruns\s+(\d+)", int),
        "fft_runs": (r"\bfft runs\s+(\d+)", int),
    }
    return {k: num(p, text, c) for k, (p, c) in patterns.items()} | {"raw": text}

def config(text):
    out = {}
    for line in text.splitlines():
        m = re.match(r"\s*([\w]+)\s*=\s*(.+)", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out

def dump_samples(text):
    """Parse `grid dump`: return sample rate, calibration factors and ADC pairs."""
    h = re.search(r"DUMP BEGIN\s+fs=([\d.]+)\s+n=(\d+)\s+kv=([\d.eE+-]+)\s+ki=([\d.eE+-]+)", text, re.I)
    if not h:
        raise ValueError("Could not parse grid dump header: " + text[:250])
    fs, kv, ki = map(float, (h.group(1), h.group(3), h.group(4)))
    samples, active = [], False
    for line in text.splitlines():
        line = line.strip()
        if line.upper().startswith("DUMP BEGIN"):
            active = True
        elif line.upper().startswith("DUMP END"):
            break
        elif active:
            m = re.fullmatch(r"(\d+)\s*,\s*(\d+)", line)
            if m:
                samples.append((int(m.group(1)), int(m.group(2))))
    return fs, kv, ki, samples
