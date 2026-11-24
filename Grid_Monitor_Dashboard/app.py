import streamlit as st
from datetime import datetime
import pandas as pd
from gridmon.serial_client import GridSerial, ports
from gridmon.parser import status, stats, config, dump_samples

st.set_page_config(page_title="Grid Monitor", page_icon="⚡", layout="wide")
st.title("Grid Monitor")
st.caption("STM32 Nucleo-F446RE · Zephyr RTOS · USB UART dashboard")

_defaults = {"client": None, "data": None, "error": None, "updated": None,
             "wave_v": [], "wave_i": [], "wave_fs": 10240.0,
             "wave_kv": 0.33, "wave_ki": 0.007324, "wave_count": 0,
             "wave_error": None}
for _k, _v in _defaults.items():
    if _k not in st.session_state:
        st.session_state[_k] = _v

def close_port():
    c = st.session_state.client
    if c:
        try: c.close()
        except Exception: pass
    st.session_state.client = None
    st.session_state.data = None

def refresh_measurements(quick=False):
    c = st.session_state.client
    if not c or not c.is_open:
        st.session_state.error = "Not connected."
        return
    try:
        if quick and st.session_state.data is not None:
            raw = dict(st.session_state.data.get("raw", {}))
            raw["status"] = c.command("grid status")
            raw["stats"] = c.command("grid stats")
            raw["features"] = c.command("grid feat")
            st.session_state.data["status"] = status(raw["status"])
            st.session_state.data["stats"] = stats(raw["stats"])
            st.session_state.data["raw"] = raw
        else:
            raw = {"status": c.command("grid status"),
                   "stats": c.command("grid stats"),
                   "features": c.command("grid feat"),
                   "config": c.command("grid cfg show")}
            st.session_state.data = {"status": status(raw["status"]),
                                     "stats": stats(raw["stats"]),
                                     "config": config(raw["config"]), "raw": raw}
        st.session_state.updated = datetime.now()
        st.session_state.error = None
    except Exception as e:
        st.session_state.error = str(e)

with st.sidebar:
    st.header("STM32 connection")
    available = ports()
    choices = [p[0] for p in available]
    labels = {p[0]: f"{p[0]} — {p[1]}" for p in available}
    selected = st.selectbox("ST-LINK Virtual COM Port", choices,
                            format_func=lambda x: labels.get(x, x)) if choices else None
    st.caption("115200 baud · 8-N-1 · no flow control")
    connected = st.session_state.client is not None and st.session_state.client.is_open
    if not connected:
        if st.button("Connect", type="primary", disabled=selected is None, use_container_width=True):
            try:
                st.session_state.client = GridSerial(selected)
                st.session_state.wave_v, st.session_state.wave_i = [], []
                st.session_state.wave_count = 0
                st.session_state.error = None
                refresh_measurements()
                st.rerun()
            except Exception as e:
                st.session_state.error = str(e)
    else:
        st.success(f"Connected: {st.session_state.client.ser.port}")
        if st.button("Disconnect", use_container_width=True):
            close_port(); st.rerun()
    auto_refresh = st.toggle("Live telemetry updates (1s)", value=True, disabled=not connected)
    if st.button("Refresh measurements", disabled=not connected, use_container_width=True):
        with st.spinner("Reading STM32 shell..."): refresh_measurements()
    if st.session_state.updated:
        st.caption("Updated " + st.session_state.updated.strftime("%H:%M:%S"))
    st.divider()
    with st.expander("Bench Calibration (0-15V)", expanded=False):
        cal_v = st.number_input("Known bench AC Volts (e.g. 5.0 or 12.0)", min_value=1.0, max_value=250.0, value=5.0, step=0.5)
        if st.button("Calibrate Voltage", disabled=not connected, use_container_width=True):
            try:
                st.session_state.client.command(f"grid cal v {cal_v}")
                st.session_state.client.command("grid cfg save")
                st.success(f"Calibrated to {cal_v}V!")
                refresh_measurements()
                st.rerun()
            except Exception as e:
                st.error(str(e))
        if st.button("Set Low-Voltage Thresholds (0.05W)", disabled=not connected, use_container_width=True):
            try:
                st.session_state.client.command("grid cfg set t_event_w 0.05")
                st.session_state.client.command("grid cfg set t_min_irms 0.005")
                st.session_state.client.command("grid cfg set t_eps_a 0.005")
                st.session_state.client.command("grid cfg save")
                st.success("Configured for Low-Voltage Bench (0.05W event threshold)!")
                refresh_measurements()
                st.rerun()
            except Exception as e:
                st.error(str(e))
    st.info("Bench monitoring active. Live updates enabled.")

if not choices:
    st.warning("No serial ports detected. Connect the Nucleo and refresh the page.")
if st.session_state.error:
    st.error(st.session_state.error)

@st.fragment(run_every="1s" if auto_refresh and connected else None)
def telemetry_panel():
    if auto_refresh and connected:
        refresh_measurements(quick=True)
    data = st.session_state.data
    if data is None:
        st.info("Connect to the Nucleo COM port, then click **Refresh measurements**.")
        return
    s, t = data["status"], data["stats"]
    st.subheader("System overview")
    a, b, c, d = st.columns(4)
    a.metric("Relay", s.get("relay", "Unknown"))
    b.metric("Load class", s.get("load_class", "Unknown"))
    c.metric("Sampling rate", f'{t["sampling_hz"]:.1f} Hz' if t.get("sampling_hz") is not None else "—")
    d.metric("Acquisition blocks", f'{t["blocks"]:,}' if t.get("blocks") is not None else "—")
    if "no usable" in s.get("mains_text", "").lower():
        st.warning("Firmware reports no usable voltage signal yet.")
    st.subheader("Electrical measurements")
    cols = st.columns(4)
    for col, (label, key, unit, digits) in zip(cols, [
        ("Voltage RMS", "voltage_v", "V", 2), ("Current RMS", "current_rms_a", "A", 3),
        ("Current peak", "current_peak_a", "A", 3), ("Frequency", "frequency_hz", "Hz", 2)]):
        value = s.get(key); col.metric(label, f"{value:.{digits}f} {unit}" if value is not None else "—")
    cols = st.columns(4)
    for col, (label, key, unit) in zip(cols, [("Active power", "active_w", "W"),
        ("Apparent power", "apparent_va", "VA"), ("Reactive power", "reactive_var", "var"),
        ("Power factor", "pf", "")]):
        value = s.get(key); col.metric(label, f"{value:.3f} {unit}".strip() if value is not None else "—")
    st.subheader("Protection and acquisition")
    cols = st.columns(5)
    for col, (label, key) in zip(cols, [("ADC errors", "adc_errors"), ("DMA errors", "dma_errors"),
        ("Fast trips", "fast_trips"), ("Ring overruns", "ring_overruns"), ("FFT runs", "fft_runs")]):
        col.metric(label, t.get(key) if t.get(key) is not None else "—")
    with st.expander("Firmware configuration (read-only)"):
        st.dataframe([{"Parameter": k, "Value": v} for k, v in data["config"].items()],
                     use_container_width=True, hide_index=True)
    with st.expander("Raw UART responses"):
        for name, raw in data["raw"].items():
            st.markdown(f"**{name}**"); st.code(raw or "(empty)", language="text")

telemetry_panel()

st.divider()
st.header("Live voltage and current waveforms")
st.write("The dashboard repeatedly requests short ADC snapshots using the existing `grid dump` shell command.")
connected = st.session_state.client is not None and st.session_state.client.is_open
live = st.toggle("Enable live waveform updates", value=False, disabled=not connected)
col_b, col_m = st.columns([1, 1])
with col_b:
    batch = st.select_slider("Snapshot window (samples)", options=[128, 256, 512, 1024], value=512)
with col_m:
    disp_mode = st.radio("Display style", ["Continuous Snapshot (Oscilloscope)", "Stitched History"], horizontal=True)

if not connected:
    st.info("Connect to the STM32 to enable waveform capture.")
elif not live:
    st.info("Enable live waveform updates to start capturing samples.")

@st.fragment(run_every="700ms")
def waveform_panel():
    c = st.session_state.client
    if not c or not c.is_open or not live:
        return
    try:
        raw = c.command(f"grid dump {batch}", timeout=8.0)
        fs, kv, ki, samples = dump_samples(raw)
        if samples:
            st.session_state.wave_fs = fs or st.session_state.wave_fs
            st.session_state.wave_kv = kv or st.session_state.wave_kv
            st.session_state.wave_ki = ki or st.session_state.wave_ki
            if disp_mode == "Continuous Snapshot (Oscilloscope)":
                st.session_state.wave_v = [x[0] for x in samples]
                st.session_state.wave_i = [x[1] for x in samples]
            else:
                st.session_state.wave_v.extend(x[0] for x in samples)
                st.session_state.wave_i.extend(x[1] for x in samples)
                st.session_state.wave_v = st.session_state.wave_v[-2048:]
                st.session_state.wave_i = st.session_state.wave_i[-2048:]
            st.session_state.wave_count += len(samples)
            st.session_state.wave_error = None
    except Exception as e:
        st.session_state.wave_error = str(e)
    if st.session_state.wave_error:
        st.error("Waveform capture error: " + st.session_state.wave_error); return
    v, i = st.session_state.wave_v, st.session_state.wave_i
    if not v:
        st.info("Waiting for ADC samples..."); return
    fs = st.session_state.wave_fs
    voff, ioff = sum(v)/len(v), sum(i)/len(i)
    frame = pd.DataFrame({
        "Time (ms)": [1000*n/fs for n in range(len(v))],
        "Voltage ADC counts": v, "Current ADC counts": i,
        "Voltage (V)": [(x-voff)*st.session_state.wave_kv for x in v],
        "Current (A)": [(x-ioff)*st.session_state.wave_ki for x in i],
    }).set_index("Time (ms)")
    st.caption(f"Window: {len(v)} samples ({len(v)*1000/fs:.1f} ms) · {fs:.1f} samples/s")
    left, right = st.columns(2)
    with left:
        st.markdown("**Voltage waveform (V)**"); st.line_chart(frame[["Voltage (V)"]], height=270)
    with right:
        st.markdown("**Current waveform (A)**"); st.line_chart(frame[["Current (A)"]], height=270)
    if st.button("Clear waveform history", key="clear_wave"):
        st.session_state.wave_v, st.session_state.wave_i = [], []
        st.session_state.wave_count = 0
        st.rerun()
        st.session_state.wave_count = 0
        st.rerun()

waveform_panel()
st.caption("This first version polls finite snapshots; it is not gap-free ADC streaming. A dedicated firmware telemetry mode is required for truly continuous waveforms.")
