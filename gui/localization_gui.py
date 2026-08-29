"""Live floor-map GUI for the three-anchor ESP32-S3 phone localizer.

The master board sends one JSON object per line over USB serial. This program
converts RSSI to distance, solves a three-circle least-squares position and
draws every detected phone on a room map.
"""

from __future__ import annotations

import json
import math
import queue
import random
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # A friendly GUI error is shown when Connect is pressed.
    serial = None
    list_ports = None


APP_DIR = Path(__file__).resolve().parent
CONFIG_PATH = APP_DIR / "config.json"
DEVICE_TIMEOUT_SECONDS = 5.0


def load_config() -> dict:
    with CONFIG_PATH.open("r", encoding="utf-8") as file:
        config = json.load(file)

    anchors = sorted(config["anchors"], key=lambda anchor: anchor["id"])
    if [anchor["id"] for anchor in anchors] != [0, 1, 2]:
        raise ValueError("config.json must contain anchors with IDs 0, 1 and 2")
    config["anchors"] = anchors
    return config


def rssi_to_distance(rssi: float, rssi_at_1m: float, path_loss: float) -> float:
    """Log-distance path-loss model. Values must be calibrated for the room."""
    return 10.0 ** ((rssi_at_1m - rssi) / (10.0 * path_loss))


def solve_position(anchors: list[dict], distances: list[float],
                   previous: tuple[float, float] | None,
                   room_width: float, room_height: float) -> tuple[float, float]:
    """Solve distance residuals using a small two-variable Gauss-Newton loop."""
    if previous is None:
        x = sum(anchor["x"] for anchor in anchors) / len(anchors)
        y = sum(anchor["y"] for anchor in anchors) / len(anchors)
    else:
        x, y = previous

    for _ in range(12):
        j11 = j12 = j22 = 0.0
        g1 = g2 = 0.0

        for anchor, measured_distance in zip(anchors, distances):
            dx = x - float(anchor["x"])
            dy = y - float(anchor["y"])
            predicted = max(math.hypot(dx, dy), 0.001)
            residual = predicted - measured_distance
            jx = dx / predicted
            jy = dy / predicted

            j11 += jx * jx
            j12 += jx * jy
            j22 += jy * jy
            g1 += jx * residual
            g2 += jy * residual

        determinant = j11 * j22 - j12 * j12
        if abs(determinant) < 1e-9:
            break

        step_x = (j22 * g1 - j12 * g2) / determinant
        step_y = (-j12 * g1 + j11 * g2) / determinant
        x -= step_x
        y -= step_y

        x = min(max(x, 0.0), room_width)
        y = min(max(y, 0.0), room_height)
        if math.hypot(step_x, step_y) < 0.001:
            break

    return x, y


class SerialReader(threading.Thread):
    def __init__(self, port: str, baud: int, output_queue: queue.Queue):
        super().__init__(daemon=True)
        self.port = port
        self.baud = baud
        self.output_queue = output_queue
        self.stop_event = threading.Event()
        self.connection = None

    def run(self) -> None:
        try:
            self.connection = serial.Serial(self.port, self.baud, timeout=0.4)
            self.output_queue.put(("status", f"Connected to {self.port}"))
            while not self.stop_event.is_set():
                raw = self.connection.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line.startswith("{"):
                    self.output_queue.put(("log", line))
                    continue
                try:
                    data = json.loads(line)
                    if data.get("type") == "measurement":
                        self.output_queue.put(("measurement", data))
                except json.JSONDecodeError:
                    self.output_queue.put(("log", f"Bad JSON: {line[:100]}"))
        except Exception as error:  # Serial errors must reach the Tk main thread.
            self.output_queue.put(("error", str(error)))
        finally:
            if self.connection and self.connection.is_open:
                self.connection.close()
            self.output_queue.put(("disconnected", None))

    def stop(self) -> None:
        self.stop_event.set()


class LocalizerGui(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("ESP32-S3 Phone Localizer")
        self.geometry("1120x720")
        self.minsize(920, 620)
        self.configure(bg="#0f172a")

        try:
            self.config_data = load_config()
        except Exception as error:
            messagebox.showerror("Configuration error", str(error))
            self.destroy()
            return

        self.messages: queue.Queue = queue.Queue()
        self.reader: SerialReader | None = None
        self.demo_running = False
        self.devices: dict[str, dict] = {}
        self.palette = [
            "#f43f5e", "#22c55e", "#eab308", "#a855f7",
            "#06b6d4", "#f97316", "#ec4899", "#84cc16",
        ]

        self._configure_style()
        self._build_layout()
        self.refresh_ports()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(50, self.process_messages)
        self.after(250, self.redraw)

    def _configure_style(self) -> None:
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TFrame", background="#0f172a")
        style.configure("Panel.TFrame", background="#1e293b")
        style.configure("TLabel", background="#0f172a", foreground="#e2e8f0")
        style.configure("Panel.TLabel", background="#1e293b", foreground="#e2e8f0")
        style.configure("Title.TLabel", background="#0f172a", foreground="#60a5fa",
                        font=("Segoe UI", 18, "bold"))
        style.configure("TButton", padding=7)
        style.configure("Treeview", background="#111827", fieldbackground="#111827",
                        foreground="#e5e7eb", rowheight=27)
        style.configure("Treeview.Heading", background="#334155", foreground="#f8fafc")

    def _build_layout(self) -> None:
        header = ttk.Frame(self)
        header.pack(fill="x", padx=18, pady=(14, 8))
        ttk.Label(header, text="ESP32-S3 Phone Localizer", style="Title.TLabel").pack(side="left")

        controls = ttk.Frame(header)
        controls.pack(side="right")
        self.port_box = ttk.Combobox(controls, width=18, state="readonly")
        self.port_box.pack(side="left", padx=4)
        ttk.Button(controls, text="Refresh", command=self.refresh_ports).pack(side="left", padx=4)
        self.connect_button = ttk.Button(controls, text="Connect", command=self.toggle_connection)
        self.connect_button.pack(side="left", padx=4)
        ttk.Button(controls, text="Demo", command=self.toggle_demo).pack(side="left", padx=4)

        content = ttk.Frame(self)
        content.pack(fill="both", expand=True, padx=18, pady=(0, 12))

        map_panel = ttk.Frame(content, style="Panel.TFrame", padding=10)
        map_panel.pack(side="left", fill="both", expand=True)
        self.canvas = tk.Canvas(map_panel, bg="#f8fafc", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)

        side_panel = ttk.Frame(content, style="Panel.TFrame", width=330, padding=12)
        side_panel.pack(side="right", fill="y", padx=(12, 0))
        side_panel.pack_propagate(False)

        ttk.Label(side_panel, text="Tracked devices", style="Panel.TLabel",
                  font=("Segoe UI", 12, "bold")).pack(anchor="w", pady=(0, 8))
        columns = ("mac", "x", "y", "rssi")
        self.device_table = ttk.Treeview(side_panel, columns=columns, show="headings", height=13)
        for column, width, title in [
            ("mac", 135, "Device MAC"), ("x", 43, "X"),
            ("y", 43, "Y"), ("rssi", 75, "RSSI 0/1/2"),
        ]:
            self.device_table.heading(column, text=title)
            self.device_table.column(column, width=width, anchor="center")
        self.device_table.pack(fill="x")

        ttk.Label(side_panel, text="Status", style="Panel.TLabel",
                  font=("Segoe UI", 11, "bold")).pack(anchor="w", pady=(18, 4))
        self.status_var = tk.StringVar(value="Not connected")
        ttk.Label(side_panel, textvariable=self.status_var, style="Panel.TLabel",
                  wraplength=290).pack(anchor="w")

        room = self.config_data
        anchor_text = "\n".join(
            f"A{a['id']} {a['name']}: ({a['x']:.1f}, {a['y']:.1f}) m"
            for a in room["anchors"]
        )
        details = (
            f"\nRoom: {room['room_width_m']:.1f} × {room['room_height_m']:.1f} m\n"
            f"{anchor_text}\n\n"
            "Edit config.json after measuring your real room and completing RSSI calibration."
        )
        ttk.Label(side_panel, text=details, style="Panel.TLabel",
                  wraplength=290, justify="left").pack(anchor="w", pady=(8, 0))

        self.log_text = tk.Text(side_panel, height=7, bg="#111827", fg="#94a3b8",
                                insertbackground="white", relief="flat", font=("Consolas", 8))
        self.log_text.pack(fill="both", expand=True, pady=(16, 0))

    def refresh_ports(self) -> None:
        if list_ports is None:
            self.port_box["values"] = []
            return
        ports = [port.device for port in list_ports.comports()]
        self.port_box["values"] = ports
        if ports and self.port_box.get() not in ports:
            self.port_box.set(ports[0])

    def toggle_connection(self) -> None:
        if self.reader is not None:
            self.disconnect()
            return
        if serial is None:
            messagebox.showerror(
                "Missing dependency",
                "pyserial is not installed. Run: py -m pip install pyserial",
            )
            return
        port = self.port_box.get()
        if not port:
            messagebox.showwarning("Serial port", "Connect the master board and select its COM port.")
            return

        self.demo_running = False
        self.reader = SerialReader(port, int(self.config_data["serial_baud"]), self.messages)
        self.reader.start()
        self.connect_button.configure(text="Disconnect")
        self.status_var.set(f"Opening {port}...")

    def disconnect(self) -> None:
        if self.reader is not None:
            self.reader.stop()
            self.reader = None
        self.connect_button.configure(text="Connect")
        self.status_var.set("Not connected")

    def toggle_demo(self) -> None:
        self.demo_running = not self.demo_running
        if self.demo_running:
            self.disconnect()
            self.status_var.set("Demo mode: simulated moving phones")
            self._demo_tick()
        else:
            self.status_var.set("Demo stopped")

    def _demo_tick(self) -> None:
        if not self.demo_running:
            return
        now = time.monotonic()
        room_width = float(self.config_data["room_width_m"])
        room_height = float(self.config_data["room_height_m"])
        positions = [
            (room_width * (0.50 + 0.30 * math.sin(now * 0.35)),
             room_height * (0.50 + 0.30 * math.cos(now * 0.42))),
            (room_width * (0.50 + 0.24 * math.sin(now * 0.27 + 2.0)),
             room_height * (0.50 + 0.25 * math.cos(now * 0.31 + 1.0))),
        ]
        for index, (x, y) in enumerate(positions, start=1):
            values = []
            for anchor in self.config_data["anchors"]:
                distance = max(math.hypot(x - anchor["x"], y - anchor["y"]), 0.3)
                rssi = anchor["rssi_at_1m"] - 10.0 * anchor["path_loss"] * math.log10(distance)
                values.append(int(round(rssi + random.gauss(0, 0.7))))
            self.handle_measurement({
                "mac": f"DE:MO:00:00:00:{index:02d}",
                "rssi": values,
                "samples": [20, 20, 20],
            })
        self.after(500, self._demo_tick)

    def process_messages(self) -> None:
        try:
            while True:
                kind, payload = self.messages.get_nowait()
                if kind == "measurement":
                    self.handle_measurement(payload)
                elif kind == "status":
                    self.status_var.set(payload)
                    self.append_log(payload)
                elif kind == "log" and payload:
                    self.append_log(payload)
                elif kind == "error":
                    self.status_var.set(f"Serial error: {payload}")
                    self.append_log(f"ERROR: {payload}")
                elif kind == "disconnected" and self.reader is not None:
                    self.reader = None
                    self.connect_button.configure(text="Connect")
        except queue.Empty:
            pass
        self.after(50, self.process_messages)

    def handle_measurement(self, data: dict) -> None:
        try:
            mac = str(data["mac"])
            rssi_values = [float(value) for value in data["rssi"]]
            if len(rssi_values) != 3 or any(value >= 0 or value < -120 for value in rssi_values):
                return

            anchors = self.config_data["anchors"]
            distances = [
                rssi_to_distance(rssi, anchor["rssi_at_1m"], anchor["path_loss"])
                for rssi, anchor in zip(rssi_values, anchors)
            ]
            previous = None
            if mac in self.devices:
                previous = (self.devices[mac]["raw_x"], self.devices[mac]["raw_y"])
            raw_x, raw_y = solve_position(
                anchors, distances, previous,
                float(self.config_data["room_width_m"]),
                float(self.config_data["room_height_m"]),
            )

            smoothing = float(self.config_data.get("position_smoothing", 0.25))
            if mac in self.devices:
                old = self.devices[mac]
                x = old["x"] + smoothing * (raw_x - old["x"])
                y = old["y"] + smoothing * (raw_y - old["y"])
                color = old["color"]
            else:
                x, y = raw_x, raw_y
                color = self.palette[len(self.devices) % len(self.palette)]

            self.devices[mac] = {
                "x": x, "y": y, "raw_x": raw_x, "raw_y": raw_y,
                "rssi": [int(value) for value in rssi_values],
                "distances": distances,
                "samples": data.get("samples", [0, 0, 0]),
                "updated": time.monotonic(), "color": color,
            }
            self.update_table()
        except (KeyError, TypeError, ValueError, OverflowError) as error:
            self.append_log(f"Ignored measurement: {error}")

    def update_table(self) -> None:
        for row in self.device_table.get_children():
            self.device_table.delete(row)
        for mac, device in sorted(self.devices.items()):
            short_mac = mac[-8:]
            rssi_text = "/".join(str(value) for value in device["rssi"])
            self.device_table.insert("", "end", values=(
                short_mac, f"{device['x']:.2f}", f"{device['y']:.2f}", rssi_text,
            ))

    def append_log(self, text: str) -> None:
        self.log_text.insert("end", text + "\n")
        self.log_text.see("end")
        lines = int(self.log_text.index("end-1c").split(".")[0])
        if lines > 100:
            self.log_text.delete("1.0", "20.0")

    def redraw(self) -> None:
        now = time.monotonic()
        expired = [mac for mac, device in self.devices.items()
                   if now - device["updated"] > DEVICE_TIMEOUT_SECONDS]
        for mac in expired:
            del self.devices[mac]
        if expired:
            self.update_table()

        canvas = self.canvas
        canvas.delete("all")
        width = max(canvas.winfo_width(), 200)
        height = max(canvas.winfo_height(), 200)
        margin = 50
        room_width = float(self.config_data["room_width_m"])
        room_height = float(self.config_data["room_height_m"])
        scale = min((width - 2 * margin) / room_width,
                    (height - 2 * margin) / room_height)
        left = (width - room_width * scale) / 2
        top = (height - room_height * scale) / 2

        def point(x: float, y: float) -> tuple[float, float]:
            return left + x * scale, top + (room_height - y) * scale

        canvas.create_rectangle(left, top, left + room_width * scale,
                                top + room_height * scale,
                                fill="#eef2ff", outline="#334155", width=3)

        for metre in range(1, int(room_width) + 1):
            x, _ = point(metre, 0)
            canvas.create_line(x, top, x, top + room_height * scale,
                               fill="#cbd5e1", dash=(2, 5))
        for metre in range(1, int(room_height) + 1):
            _, y = point(0, metre)
            canvas.create_line(left, y, left + room_width * scale, y,
                               fill="#cbd5e1", dash=(2, 5))

        anchors = self.config_data["anchors"]
        anchor_points = [point(anchor["x"], anchor["y"]) for anchor in anchors]
        canvas.create_polygon(*[coordinate for p in anchor_points for coordinate in p],
                              outline="#60a5fa", fill="", width=2, dash=(7, 5))
        for anchor, (x, y) in zip(anchors, anchor_points):
            canvas.create_oval(x - 11, y - 11, x + 11, y + 11,
                               fill="#1d4ed8", outline="white", width=2)
            canvas.create_text(x, y - 23, text=f"A{anchor['id']} {anchor['name']}",
                               fill="#172554", font=("Segoe UI", 10, "bold"))

        for index, (mac, device) in enumerate(sorted(self.devices.items()), start=1):
            x, y = point(device["x"], device["y"])
            color = device["color"]
            canvas.create_oval(x - 13, y - 13, x + 13, y + 13,
                               fill=color, outline="white", width=3)
            canvas.create_text(x, y, text=str(index), fill="white",
                               font=("Segoe UI", 9, "bold"))
            canvas.create_text(x, y + 25,
                               text=f"{mac[-8:]}  ({device['x']:.2f}, {device['y']:.2f})",
                               fill="#111827", font=("Segoe UI", 9, "bold"))

        canvas.create_text(left, top - 25,
                           text=f"Room {room_width:.1f} m × {room_height:.1f} m",
                           anchor="w", fill="#334155", font=("Segoe UI", 11, "bold"))
        self.after(250, self.redraw)

    def on_close(self) -> None:
        self.demo_running = False
        if self.reader is not None:
            self.reader.stop()
        self.destroy()


if __name__ == "__main__":
    app = LocalizerGui()
    app.mainloop()
