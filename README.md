# Smart Grid Edge Monitor

> **Micro-PMU, Sub-2ms Fast Protection & Non-Intrusive Load Monitoring (NILM)**  
> **Platform:** STM32 Nucleo-F446RE (ARM Cortex-M4) · Zephyr RTOS · Python Streamlit

---

An open-source edge monitoring system that combines high-speed synchronous mains acquisition, hardware circuit protection, and harmonic load classification into a single microcontroller.

---

## Hardware Architecture & Interfacing

<p align="center">
  <img src="hardware_schematic.jpg" alt="Smart Grid Monitor Hardware Interfacing Schematic" width="680">
</p>

### Channel Specifications
* **Voltage Input (PA0 / ADC1 Channel 0):** ZMPT101B module via 4.7 kΩ / 10 kΩ resistive divider (0.68×) and 15 nF filter (fc ≈ 3.2 kHz)
* **Current Input (PA1 / ADC2 Channel 1):** SCT-013-000 current clamp across 220 Ω burden resistor + 1.65 V mid-rail DC bias node (10 kΩ / 10 kΩ with 10 µF capacitor) and 15 nF filter (fc ≈ 3.2 kHz)
* **Relay Protection (PA8 / GPIO D7):** Drives 2N2222 NPN transistor base (1 kΩ series, 10 kΩ pull-down) to switch Songle 5V active-low relay module with fail-safe boot state
* **Host Telemetry:** ST-LINK USB Virtual COM Port (115200 8-N-1) on USART2

---

## Key Specifications

| Parameter | Specification |
| :--- | :--- |
| **Microcontroller** | STM32 Nucleo-F446RE (ARM Cortex-M4 @ 180 MHz) |
| **Operating System** | Zephyr RTOS v4.2.0 (Preemptive Scheduler) |
| **Acquisition Engine** | 10,240 Hz Synchronous Dual-ADC (0.0° Phase Skew) |
| **Fault Protection** | Instantaneous Trip in < 1.56 ms (Sub-2ms Hardware Cutoff) |
| **FFT & Harmonics** | 2048-point CMSIS-DSP Real FFT (h3, h5, h7, THD) |
| **Appliance Classes** | Resistive, SMPS, Motor / Inductive, Standby |

---

## RTOS Thread Architecture

Safety protection and heavy signal processing are decoupled across preemptive priority levels:

1. **Priority 0 — Protection & Cycle Detection (`protect.c`):**  
   Runs directly on 16-sample DMA half-buffer completions (1.5625 ms). Evaluates instantaneous peak limits and ADC rail saturation (`CLIP_LO = 6`, `CLIP_HI = 4089`) for sub-2ms relay cutoff. Tracks zero crossings with hysteresis and integrates discrete cycle metrics.
2. **Priority 2 — DSP & NILM Engine (`dsp_task.c`):**  
   Consumes cycle events over Zephyr `zbus`. Executes 2048-point CMSIS-DSP FFT, harmonic extraction, and 3-cycle temporal confidence debouncing.
3. **Priority 5 — Shell & Telemetry (`grid_shell.c`, `telemetry.c`):**  
   Manages NVS flash fault logging, serial interactive shell, and live telemetry streaming to the host dashboard.

---

## Mathematical Formulation

### 1. Timing Geometry & FFT Bin Alignment
* For 50 Hz mains sampled at fs = 10,240 Hz, each cycle contains exactly 204.8 samples.
* A window of N = 2048 samples covers exactly 10 full cycles:

```
Bin Resolution:        Δf = fs / N = 10,240 Hz / 2048 = 5.0 Hz / bin
Fundamental (50 Hz):   bin = 50 / 5 = 10
3rd Harmonic (150 Hz): bin = 150 / 5 = 30
5th Harmonic (250 Hz): bin = 250 / 5 = 50
```
Exact integer bin alignment completely eliminates spectral leakage.

### 2. Adaptive DC Midpoint Cancellation
An exponential single-pole low-pass filter tracks the 1.65 V DC bias midpoint dynamically without phase lag:

```
midpoint_new = midpoint_old + 0.25 × (cycle_mean - midpoint_old)
```

### 3. Discrete Power & RMS Integration
Calculated in O(1) time per sample using running statistical sums:

```
V_rms = k_v × sqrt( (Σ v² / N) - (v_mean)² )
I_rms = k_i × sqrt( (Σ i² / N) - (i_mean)² )

Active Power (P)    = k_v × k_i × ( (Σ v·i / N) - (v_mean × i_mean) )
Apparent Power (S)  = V_rms × I_rms
Power Factor (PF)   = P / S
Reactive Power (Q)  = sqrt( max(S² - P², 0) )
```

### 4. Harmonic Distortion & Ratios
Extracted from the 2048-point real FFT spectrum:

```
Harmonic Ratio (hn) = I_n / I_1
Total Harmonic Distortion (THD) = sqrt( Σ I_n² ) / I_1   (for n = 2 to 50)
```

---

## NILM Classification Model

Rather than using computationally heavy black-box neural networks that require impractical training datasets, the edge firmware employs a **Deterministic Physics-Informed State Machine** based on fundamental electrical circuit properties:

* **Resistive (`LOAD_RESISTIVE`):** Electric heaters, kettles, irons.  
  *Characteristics:* THD < 20%, |PF| ≥ 0.90.
* **Switch-Mode Power Supply (`LOAD_SMPS`):** Laptop chargers, phone adapters, LED drivers.  
  *Characteristics:* Bridge rectifier pulse conduction causes THD ≥ 20% and 3rd harmonic ratio h3 ≥ 10%.
* **Motor / Inductive (`LOAD_MOTOR`):** Fans, pumps, refrigerator compressors.  
  *Characteristics:* Magnetic flux lag and startup surge → Inrush ratio ≥ 1.6× or |PF| < 0.90 with clean current.
* **Standby / Disconnected (`LOAD_NONE`):** Current < 15 mA for ≥ 5 consecutive cycles.

### EMI Suppression (Bhattacharya Temporal Filter)
Whenever an appliance turns on or off, mechanical contact arcing causes transient voltage spikes. The classification output is gated by a **3-cycle temporal confidence filter**: appliance state must remain stable for 3 consecutive inference cycles (60 ms) before updating the output, completely rejecting switching glitches.

---

## Scientific Literature & References

1. **George W. Hart (1992)** — *"Nonintrusive Appliance Load Monitoring"*, **Proceedings of the IEEE**, Vol. 80, No. 12, pp. 1870–1891. *(Foundational NILM signature framework).*
2. **Asmit Bhattacharya (2026)** — *"Non-Intrusive Load Monitoring via Edge-Based TinyML on Microcontrollers: A Low-Latency Approach for Residential Energy Efficiency"*, **IJERT**, Vol. 15, Issue 03. *(Temporal confidence filter for edge EMI immunity).*
3. **IEEE Standard 1459-2010** — *"IEEE Standard Definitions for the Measurement of Electric Power Quantities Under Sinusoidal, Nonsinusoidal, Balanced, or Unbalanced Conditions."*
4. **IEC 61000-3-2** — *"Electromagnetic Compatibility (EMC) - Limits for Harmonic Current Emissions."*

---

## Quickstart Guide

### 1. Run Mathematical Tests on Laptop (No Hardware Needed)
Verifies cycle integration, zero-crossing detection, CMSIS FFT stubs, and NILM classification:

```bash
cd grid_monitor/tests/host
make test
```
*Expected result:* `[OK] All 45 host checks passed successfully.`

---

### 2. Run the Live Streamlit Dashboard
Connect the STM32 board via USB, then run:

```bash
cd Grid_Monitor_Dashboard
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
streamlit run app.py
```
Open `http://localhost:8501` to view live waveform plots (V, I) and real-time telemetry gauges.

---

### 3. Build & Flash Firmware
Requires [Zephyr SDK 0.17.0](https://docs.zephyrproject.org/) and `west`:

```bash
west init -l grid_monitor
west update
west build -b nucleo_f446re grid_monitor
west flash
```

---

## Interactive Serial Commands

Open the serial console at `115200 8N1` (e.g., via `screen`, `minicom`, or PuTTY):

```
grid status        # Shows real-time Vrms, Irms, P, S, Q, PF, freq, and load class
grid feat          # Shows FFT fundamental, h3, h5, h7, THD, and phase angle
grid dump 128      # Streams raw ADC counts for oscilloscope reconstruction
grid cal v 230.0   # Calibrates voltage factor against a reference multimeter
grid cal i 1.50    # Calibrates current factor against a reference load
grid on / off      # Manually controls the relay
grid reset         # Clears latched trip condition and re-energizes load
grid fault_log     # Displays flash log of previous over-current fault events
```

---

## License
MIT License. Open-source for academic and industrial research.
