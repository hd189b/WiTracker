# ESP32-S3 Phone Localizer

Three ESP32-S3 development boards estimate the 2D position of phones that have
voluntarily connected to the system's Wi-Fi network. The master is also Anchor
0. Two passive anchors measure the RSSI of phone-to-master Wi-Fi packets and
send windowed measurements to the master with encrypted ESP-NOW. The master
streams combined JSON measurements to a Python desktop GUI over USB.

> RSSI localization is approximate. It cannot produce an exact indoor position.
> Calibrated performance may be useful for room/zone tracking, but reflections,
> people, phone orientation and different phone radios can cause metre-scale
> errors. UWB is the appropriate upgrade when reliable sub-metre accuracy is a
> requirement.

## Project files

```text
esp32_phone_localizer/
├── firmware/
│   ├── master/master.ino
│   ├── node1/node1.ino
│   └── node2/node2.ino
├── gui/
│   ├── localization_gui.py
│   ├── config.json
│   ├── requirements.txt
│   ├── run_gui.bat
│   └── run_gui.sh
└── docs/
    └── esp32_placement_reference.png
```

## Physical placement

Use a wide triangle around the perimeter of the tracking area:

- Master/Anchor 0: lower centre, connected to the PC by USB.
- Anchor Node 1: upper-left corner.
- Anchor Node 2: upper-right corner.
- Keep all boards approximately 1.2–1.5 m above the floor.
- Your boards use built-in PCB antennas. Keep the PCB antenna end clear of
  metal, wiring, hands and walls, and use the same board orientation at all
  three positions.
- Do not hide a PCB antenna behind a computer, metal case or power bank.
- Measure the real anchor coordinates from one chosen room origin.

Open `docs/esp32_placement_reference.png` for the illustrated arrangement.

## Network design

- All phones connect only to the master's `ESP32_LOCALIZER` SoftAP.
- All three boards remain on 2.4 GHz Wi-Fi channel 6.
- Phones open `http://192.168.4.1/` and keep that page visible. It sends a small
  request every 250 ms so the anchors have predictable packets to measure.
- Node 1 and Node 2 use promiscuous reception only to read the Wi-Fi header and
  RSSI of packets addressed to the localizer master.
- Measurements are identified by the phone's Wi-Fi MAC used for this network.
  Modern phones may use a randomized per-network MAC; that is acceptable for a
  session but the identifier can change after forgetting/rejoining the network.
- Only devices whose owners intentionally join this Wi-Fi network should be
  tracked.

## 1. Arduino IDE setup

1. Install Arduino IDE 2.x.
2. Install **esp32 by Espressif Systems** from Boards Manager. The sketches
   target Arduino-ESP32 3.x.
3. Select **Tools > Board > ESP32 Arduino > ESP32S3 Dev Module**.
4. Use these typical settings:
   - USB CDC On Boot: Enabled when using the native USB connector.
   - CPU Frequency: 240 MHz.
   - Flash Mode: QIO.
   - Partition Scheme: Default.
5. If your board has separate UART and USB/OTG connectors, use the connector
   that normally works for Arduino upload and Serial Monitor.

No third-party Arduino library is required. WiFi, WebServer, DNSServer,
ESP-NOW and the low-level Wi-Fi APIs come from the Espressif board package.

## 2. Flash the three boards

Upload each sketch to the correct board:

| Board | Sketch |
| --- | --- |
| Master / Anchor 0 | `firmware/master/master.ino` |
| Anchor Node 1 | `firmware/node1/node1.ino` |
| Anchor Node 2 | `firmware/node2/node2.ino` |

The sketches assign fixed locally administered interface MAC addresses:

| Interface | MAC |
| --- | --- |
| Master AP | `02:4C:4F:43:00:01` |
| Node 1 STA | `02:4C:4F:43:01:01` |
| Node 2 STA | `02:4C:4F:43:02:01` |

These addresses make filtering and encrypted peer configuration predictable.
If you change one, update every sketch that references it.

The included ESP-NOW PMK and LMK are demonstration keys. Before a real
deployment, replace both 16-byte keys with different random values and make
the same change in all three sketches. Also change the SoftAP password.

## 3. First hardware test

1. Power all three boards.
2. Open Node 1 and Node 2 Serial Monitor at 115200 baud. Each should print its
   node ID, fixed MAC and channel 6.
3. Connect one phone to:
   - SSID: `ESP32_LOCALIZER`
   - Password: `LocateMe2026`
4. The phone may warn that the network has no Internet. Choose **Stay
   connected** or the equivalent option.
5. Open `http://192.168.4.1/` and keep the page open with the screen awake.
6. Anchor serial status should eventually show queued reports increasing.
7. Connect the master to the PC. The GUI uses this USB serial connection, so
   close Arduino Serial Monitor before opening the GUI.

## 4. PC GUI setup

### Windows

1. Install Python 3.11 or newer and enable **Add Python to PATH**.
2. Double-click `gui/run_gui.bat`.
3. Select the master's COM port and click **Connect**.

Manual commands:

```powershell
cd gui
py -m pip install -r requirements.txt
py localization_gui.py
```

### Linux/macOS

```bash
cd gui
python3 -m pip install -r requirements.txt
python3 localization_gui.py
```

The **Demo** button displays two simulated moving phones without hardware. Use
it first to verify that Tkinter and the drawing interface work.

## 5. Enter the actual room geometry

Edit `gui/config.json` and replace the example 8 m × 6 m room dimensions and
anchor coordinates. Coordinates are in metres. The included reference layout
uses:

```text
Master: (4.0, 0.5)
Node 1: (0.5, 5.5)
Node 2: (7.5, 5.5)
```

The three anchors must not be placed along one straight line. Spread them near
the outer edge of the area being measured.

## 6. RSSI calibration — required

The example calibration values are placeholders. Calibrate each anchor in the
real room before judging position accuracy.

For each anchor:

1. Use one reference phone with the tracking page running.
2. Put the phone exactly 1.0 m from the anchor with normal phone orientation.
3. Collect RSSI for at least 30 seconds and record the median. Put that value in
   `rssi_at_1m` for the anchor.
4. Repeat at known distances such as 2 m, 3 m and 4 m.
5. Estimate the path-loss value for each measurement:

   `n = (RSSI_at_1m - measured_RSSI) / (10 × log10(distance_metres))`

6. Average the estimates and put the result in `path_loss` for that anchor.
   Indoor starting values are often around 2.0–3.5, but the measured value is
   more important than a generic value.
7. Repeat calibration when anchor position, height, enclosure or room layout
   changes.

Different phone models transmit differently. For a research-grade evaluation,
record error separately for every test phone model and orientation.

## Serial data format

The master outputs one complete three-anchor measurement per JSON line:

```json
{"type":"measurement","mac":"92:31:45:72:10:C4","rssi":[-51,-63,-58],"samples":[18,16,17],"age_ms":[12,35,28]}
```

The GUI ignores normal startup/status text and processes only JSON lines whose
`type` is `measurement`.

## Troubleshooting

### Phone connects but no device appears

- Open `http://192.168.4.1/` and keep the page in the foreground.
- Confirm that the phone stayed connected despite the no-Internet warning.
- Confirm all sketches use channel 6 and the same master AP MAC.
- Verify Node 1 and Node 2 report counters increase.
- Move the phone near the centre of the triangle for the first test.

### Node report count remains zero

- Confirm the correct Node 1/Node 2 sketch was uploaded.
- Confirm the master is powered and its AP is visible.
- Check the ESP-NOW PMK/LMK and fixed MAC constants match in all sketches.
- Keep the built-in PCB antenna areas unobstructed and similarly oriented.

### GUI has no COM ports

- Install the USB-UART driver required by the board, if applicable.
- Try another data-capable USB cable.
- Close Arduino Serial Monitor before connecting from Python.
- Click **Refresh** after reconnecting the board.

### Position jumps or sticks to an edge

- Calibrate all `rssi_at_1m` and `path_loss` values.
- Increase `position_smoothing` in `config.json`, for example from 0.25 to 0.15
  for slower but steadier movement.
- Keep anchors well separated and at equal height.
- Add a fourth anchor or use location fingerprinting in a later version.
