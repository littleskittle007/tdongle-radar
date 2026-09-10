# T-Dongle-S3 Radio Radar (NETWATCH)

A passive WiFi + Bluetooth scanner drawn as a sweeping radar. The dongle
**transmits nothing** — it only reads the beacons and advertising packets
that nearby devices already broadcast into the air. It's the radio
equivalent of noting which shops have their signs lit: legal, passive,
and safe to walk around with.

![preview](preview.png)

## What it shows

- **WiFi access points** — every router announces itself several times a
  second so your phone can list networks. The radar reads each one's
  name, signal strength, channel, and whether it's encrypted.
- **Bluetooth LE devices** — earbuds, watches and fitness bands advertise
  themselves so they can be found. The radar reads address, signal
  strength, and any advertised name.

Signal strength sets a blip's **distance** from the centre — strong
signals plot near the middle, weak ones near the rim. As you physically
walk toward a device, its blip drifts inward. Bearing is fixed per device
(hashed from its identity) so contacts hold still instead of jumping; it
is **not** a real direction, since one antenna can't tell which way a
signal came from. This is a proximity visualiser, not a tracker.

Cyan blips are WiFi, amber are BLE. A contact that hasn't been heard in
12 seconds fades and drops off.

## Controls

| Input | On radar | In contact list |
|---|---|---|
| Tap (< 0.5s) | cycle WIFI / BLE / BOTH | next contact |
| Hold (>= 0.5s) | open contact list | exit list |

The contact list shows every device in range with a signal-bar readout,
and for WiFi the channel and OPEN/LOCKED status.

## Files

```
tdongle-radar/
  platformio.ini
  src/
    main.cpp       radar display + input
    scanners.h     the passive WiFi and BLE scanners
    display.h      raw ST7735 driver (+ line/circle/disc)
    font5x7.h      bitmap font
```

## Building

Open the folder in VS Code with PlatformIO, hold BOOT, plug in, click the
**→** (Upload) arrow.

**Important:** this uses a `huge_app.csv` partition (4MB app slot) because
WiFi + BLE together make a large binary that won't fit the default
~1.3MB partition. That line is already in `platformio.ini`; if the build
complains the image is too big, that's the setting to check.

The first build pulls in the BLE stack and is slow. Later builds are
quick.

## Notes and limits

- **BLE API version.** This targets ESP32 Arduino core **3.x**, where
  `BLEScan::start()` returns a pointer (`res->getCount()`). On the older
  2.x core it returned by value (`res.getCount()`) — if you get a compile
  error on that line, that's why. PlatformIO's current espressif32
  platform installs a 3.x core, so this should just work.
- **BLE scanning blocks ~2 seconds.** While a BLE sweep runs the radar
  sweep pauses briefly. In WIFI mode it's perfectly smooth; in BLE or
  BOTH you'll see the sweep hitch every few seconds. That's the radio,
  not a bug.
- **RSSI is noisy.** Signal strength jumps around by nature, so blips
  breathe in and out a little even when nothing's moving. Real, not a
  glitch.
- **Power banks** may cut out — the dongle draws little. Use a computer
  USB port.

## Is this legal?

Yes. It only listens to what devices openly broadcast, the same
information your phone's WiFi and Bluetooth pickers already show. It does
not connect to, probe, deauthenticate, or interfere with anything. Keep
it that way — the ESP32 *can* be told to transmit disruptive frames, and
doing that on networks you don't own is illegal in most places.
