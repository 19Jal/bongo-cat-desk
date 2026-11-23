"""
send_key.py
Bongo Cat Companion Bot - Windows side 

Sends one byte to the ESP32-S3 over USB Serial for every key press

Protocol:
'L' = left paw tap
'R' = right paw tap
Paws alternate on each new key press
"""

import queue    
import sys
import threading
import time

import serial
import serial.tools.list_ports
from pynput import keyboard

BAUD = 115200
RECONNECT_DELAY_S = 2.0
QUEUE_MAX = 64  # if the board falls behind, drop events instead of lagging

# USB-UART bridge chips commonly found on ESP32 dev boards (VID:PID)
KNOWN_BRIDGES = {
    (0x10C4, 0xEA60),  # Silicon Labs CP210x
    (0x1A86, 0x7523),  # WCH CH340
    (0x1A86, 0x55D4),  # WCH CH9102
    (0x0403, 0x6001),  # FTDI FT232
    (0x303A, 0x1001),  # Espressif native USB (S2/S3/C3/C6)
}

events: "queue.Queue[bytes]" = queue.Queue(maxsize=QUEUE_MAX)
held_keys: set = set()
next_paw = b"L"
stop = threading.Event()


def find_port() -> str | None:
    """Return the first COM port that looks like an ESP32 board."""
    for p in serial.tools.list_ports.comports():
        if (p.vid, p.pid) in KNOWN_BRIDGES:
            return p.device
    return None


# ---------- keyboard hook (keep this FAST: no I/O here) ----------

def on_press(key):
    global next_paw
    if key in held_keys:          # auto-repeat while holding: ignore
        return
    held_keys.add(key)
    try:
        events.put_nowait(next_paw)
    except queue.Full:
        pass                      # board is behind; drop rather than stall typing
    next_paw = b"R" if next_paw == b"L" else b"L"


def on_release(key):
    held_keys.discard(key)


# ---------- serial writer thread ----------

def writer(port_arg: str | None):
    ser = None
    while not stop.is_set():
        if ser is None:
            port = port_arg or find_port()
            if port is None:
                print("No ESP32 found, retrying...")
                time.sleep(RECONNECT_DELAY_S)
                continue
            try:
                ser = serial.Serial(port, BAUD, timeout=0, write_timeout=0.1)
                print(f"Connected to {port}")
                time.sleep(1.5)   # opening the port may reset the board
                while not events.empty():   # discard taps queued while booting
                    events.get_nowait()
            except serial.SerialException as e:
                print(f"Can't open {port}: {e}")
                ser = None
                time.sleep(RECONNECT_DELAY_S)
                continue

        try:
            b = events.get(timeout=0.5)
        except queue.Empty:
            continue

        try:
            ser.write(b)
        except (serial.SerialException, serial.SerialTimeoutException) as e:
            print(f"Lost connection: {e}")
            ser.close()
            ser = None

    if ser:
        ser.close()


def main():
    port_arg = sys.argv[1] if len(sys.argv) > 1 else None
    t = threading.Thread(target=writer, args=(port_arg,), daemon=True)
    t.start()

    print("Bongo Cat running. Press Ctrl+C in this window to quit.")
    with keyboard.Listener(on_press=on_press, on_release=on_release) as listener:
        try:
            listener.join()
        except KeyboardInterrupt:
            pass
    stop.set()
    t.join(timeout=1)


if __name__ == "__main__":
    main()