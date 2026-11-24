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
* **Voltage Input (`PA0` / ADC1 Channel 0):** ZMPT101B module via $4.7\text{ k}\Omega / 10\text{ k}\Omega$ resistive divider ($0.68\times$) and $15\text{ nF}$ filter ($f_c \approx 3.2\text{ kHz}$)
* **Current Input (`PA1` / ADC2 Channel 1):** SCT-013-000 current clamp across $220\,\Omega$ burden resistor + $1.65\text{ V}$ mid-rail DC bias node ($10\text{ k}\Omega / 10\text{ k}\Omega$ with $10\,\mu\text{F}$ capacitor) and $15\text{ nF}$ filter ($f_c \approx 3.2\text{ kHz}$)
* **Relay Protection (`PA8` / GPIO D7):** Drives 2N2222 NPN transistor base ($1\text{ k}\Omega$ series, $10\text{ k}\Omega$ pull-down) to switch Songle 5V active-low relay module with fail-safe boot state
* **Host Telemetry:** ST-LINK USB Virtual COM Port (`115200 8-N-1`) on USART2

---

## Quick Reference Specs

| Parameter | Specification |
| :--- | :--- |
| **Microcontroller** | STM32 Nucleo-F446RE (ARM Cortex-M4 @ 180 MHz, 512 KB Flash, 128 KB SRAM) |
| **Operating System** | Zephyr RTOS v4.2.0 (Preemptive Priority-Based Scheduling) |
| **Sampling Rate** | 10,240 Hz (TIM2 TRGO hardware triggered, dual-regular simultaneous) |
| **Sampling Resolution** | 12-bit ADC, $0.0^\circ$ hardware phase skew (Dual DMA Mode 2) |
| **Protection Response** | Instantaneous peak trip in $< 1.56\text{ ms}$ (DMA ISR level) |
| **Mains Metrics** | True RMS $V_{\text{rms}}, I_{\text{rms}}$, Active ($P$), Apparent ($S$), Reactive ($Q$), Power Factor ($\text{PF}$), Frequency ($f$) |
| **Spectral Analysis** | 2048-point real FFT, $5.0\text{ Hz}$ bin resolution, fundamental $I_1$, $h_3, h_5, h_7$, $\text{THD}$ |
| **Appliance Classes** | Resistive, SMPS, Motor / Inductive, Standby / Idle |
| **Inference Latency** | Sub-cycle (< 0.8 ms per cycle in Zephyr DSP thread) |
| **Memory Footprint** | 182 KB Flash (69.5%), 87 KB SRAM (66.6%) on STM32F446RE |

---

## RTOS Thread Architecture

Safety protection and heavy signal processing are decoupled across preemptive priority levels:

1. **Priority 0 — Protection & Cycle Detection (`protect.c`):**  
   Runs directly on 16-sample DMA half-buffer completions ($1.5625\text{ ms}$). Evaluates instantaneous peak limits and ADC rail saturation (`CLIP_LO = 6`, `CLIP_HI = 4089`) for sub-2ms relay cutoff. Tracks zero crossings with hysteresis and integrates discrete cycle metrics.
2. **Priority 2 — DSP & NILM Engine (`dsp_task.c`):**  
   Consumes cycle events over Zephyr `zbus`. Executes 2048-point CMSIS-DSP FFT, harmonic extraction, and 3-cycle temporal confidence debouncing.
3. **Priority 5 — Shell & Telemetry (`grid_shell.c`, `telemetry.c`):**  
   Manages NVS flash fault logging, serial interactive shell, and live telemetry streaming to the host dashboard.

---

## Mathematical Formulation

### 1. Timing Geometry & FFT Bin Alignment
* For $50\text{ Hz}$ mains at $f_s = 10,240\text{ Hz}$, each cycle contains exactly $204.8$ samples.
* A window of $N = 2048$ samples covers exactly $10$ full cycles:
  $$\Delta f = \frac{f_s}{N} = \frac{10,240}{2048} = 5.0\text{ Hz/bin}$$
* Fundamental ($50\text{ Hz}$) maps to **bin 10**, 3rd harmonic ($150\text{ Hz}$) to **bin 30**, and 5th harmonic ($250\text{ Hz}$) to **bin 50**, eliminating spectral leakage.

### 2. Adaptive DC Midpoint Cancellation
An exponential single-pole low-pass filter tracks the $1.65\text{ V}$ bias midpoint dynamically without phase lag:
$$\mu_{\text{new}} = \mu_{\text{old}} + 0.25 \cdot \left(\bar{x}_{\text{cycle}} - \mu_{\text{old}}\right)$$

### 3. Discrete Power & RMS Integration
Using running statistical sums accumulated in $O(1)$ time per sample ($S_v, S_i, S_{vv}, S_{ii}, S_{vi}$):
* **True RMS Voltage & Current:**
  $$V_{\text{rms}} = k_v \sqrt{\frac{S_{vv}}{N} - \left(\frac{S_v}{N}\right)^2}, \qquad I_{\text{rms}} = k_i \sqrt{\frac{S_{ii}}{N} - \left(\frac{S_i}{N}\right)^2}$$
* **Active Power ($P$ in Watts):**
  $$P = k_v \cdot k_i \left( \frac{S_{vi}}{N} - \frac{S_v}{N} \cdot \frac{S_i}{N} \right)$$
* **Apparent Power ($S$), Power Factor ($\text{PF}$), and Non-Active Power ($Q$):**
  $$S = V_{\text{rms}} \cdot I_{\text{rms}}, \qquad \text{PF} = \frac{P}{S}, \qquad Q = \sqrt{S^2 - P^2}$$
* **Total Harmonic Distortion ($\text{THD}$):**
  $$\text{THD} = \frac{\sqrt{\sum_{n=2}^{H} I_n^2}}{I_1}$$

---

## NILM Classification Model

Rather than using computationally heavy black-box neural networks that require impractical training datasets, the edge firmware employs a **Deterministic Physics-Informed State Machine** based on fundamental electrical circuit properties:

* **Resistive (`LOAD_RESISTIVE`):** Electric heaters, kettles, irons.  
  *Characteristics:* $\text{THD} < 20\%$, $|\text{PF}| \ge 0.90$.
* **Switch-Mode Power Supply (`LOAD_SMPS`):** Laptop chargers, phone adapters, LED drivers.  
  *Characteristics:* Bridge rectifier pulse conduction causes $\text{THD} \ge 20\%$ and 3rd harmonic ratio $h_3 \ge 10\%$.
* **Motor / Inductive (`LOAD_MOTOR`):** Fans, pumps, refrigerator compressors.  
  *Characteristics:* Magnetic flux lag and startup surge $\rightarrow$ Inrush ratio $\ge 1.6\times$ or $|\text{PF}| < 0.90$ with clean current.
* **Standby / Disconnected (`LOAD_NONE`):** Current $< 15\text{ mA}$ for $\ge 5$ consecutive cycles.

### EMI Suppression (Bhattacharya Temporal Filter)
Whenever an appliance turns on or off, mechanical contact arcing causes transient voltage spikes. The classification output is gated by a **3-cycle temporal confidence filter**: appliance state must remain stable for $3$ consecutive inference cycles ($60\text{ ms}$) before updating the output, completely rejecting switching glitches.

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
Open `http://localhost:8501` to view live waveform plots ($V, I$) and real-time telemetry gauges.

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
