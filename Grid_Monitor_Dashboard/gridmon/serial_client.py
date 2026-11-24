import re, time, threading
import serial
from serial.tools import list_ports
PROMPT = re.compile(r"uart:~\s*\$", re.I)
ANSI = re.compile(r"\x1B(?:[@-Z\-_]|\[[0-?]*[ -/]*[@-~])")
def ports():
    return [(p.device, p.description or p.device) for p in list_ports.comports()]
class GridSerial:
    def __init__(self, port, baud=115200):
        self.lock = threading.Lock()
        self.ser = serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1,
                                 timeout=0.15, write_timeout=1, xonxoff=False,
                                 rtscts=False, dsrdtr=False)
        time.sleep(1.0); self.ser.reset_input_buffer()
    @property
    def is_open(self): return self.ser.is_open
    def close(self):
        with self.lock:
            if self.ser.is_open: self.ser.close()
    def command(self, command, timeout=4.0):
        with self.lock:
            if not self.ser.is_open: raise RuntimeError("Serial port is closed.")
            if "\n" in command or "\r" in command: raise ValueError("Send one shell command at a time.")
            self.ser.reset_input_buffer(); self.ser.write((command.strip()+"\r\n").encode("ascii")); self.ser.flush()
            buf = bytearray(); deadline = time.monotonic()+timeout
            while time.monotonic() < deadline:
                buf.extend(self.ser.read(max(1, self.ser.in_waiting)))
                if PROMPT.search(buf.decode("utf-8", "replace")): break
            raw = ANSI.sub("", buf.decode("utf-8", "replace")).replace("\r", "")
            if not PROMPT.search(raw):
                raise TimeoutError(f"No uart:~$ prompt after {command!r}. Check COM port, baud rate, firmware shell, and whether PuTTY is connected.")
            lines=[]
            for line in raw.splitlines():
                line=line.strip()
                if line and line != command.strip() and not PROMPT.fullmatch(line): lines.append(line)
            return "\n".join(lines)
