# F-stop darkroom timer: relay firmware and tablet web app

The tablet shows the interface. The relay unit (an ESP32-S3 board) times every exposure by itself and switches the enlarger and safelight, so a tablet that sleeps, crashes or loses Bluetooth cannot change an exposure in progress.

```
web/                  the tablet app (open index.html over https)
firmware/relay/       relay firmware: relay.ino, relay_core.h (timing logic), config.h (pins)
firmware/tests/       unit tests and a real-time test bridge for the timing logic
test/integration.js   runs the real web app against the real timing logic
docs/PROTOCOL.md      the Bluetooth messages
simulator/            the interactive simulator, for trying the interface with no hardware
```

## What has and has not been tested

Tested on a PC:

* The timing logic in `relay_core.h`: 94 unit checks (exposure length, strip test gaps, pause, cancel, pedal, focus timeout, lost link, plan validation, latency offset, safelight, one-hour lamp cap).
* The web app against that same logic: 58 integration checks. The only fake part is the Bluetooth radio. Frames pass byte for byte in both directions.

**Not tested, because it needs hardware:** `relay.ino` (Bluetooth service, pins, buzzer, saved settings), Web Bluetooth in Chrome on your tablet, and the mains switching. It was written against the NimBLE-Arduino 2.x and Arduino-ESP32 3.x documentation and type-checked, but expect to fix small things on first run. The bring-up steps below are designed so you find them safely.

## Parts

* ESP32-S3-WROOM-1 N16R8 development board with the expansion adapter.
* A 2-channel 5 V relay module: the common blue board with two Songle SRD-05VDC-SL-C relays and optocoupler inputs.
* A foot pedal (a normally open switch), and a passive piezo buzzer.
* A time-lag 2 A fuse and holder, a master switch, an enclosure, cable glands, mains cable suitable for your country, and a separate certified USB charger for the board.
* Optional: a 275 V AC varistor across the relay contacts, and an inrush limiter (NTC thermistor), see below.

## Choosing the relay for a transformer-fed halogen enlarger

The enlarger's 100 W halogen lamp runs from a transformer, so the relay switches the mains side of a transformer: about 0.5 A steady, but a much larger surge when it is energised. Transformer inrush can be 10 to 15 times the rated current for several cycles, and up to about 60 times for a toroidal one.

* **Zero-cross solid state relays, such as the G3MB-202P, are the wrong tool.** Switching on at zero voltage is the worst moment to energise a transformer, and manufacturers of solid state relays say zero-cross starting should be avoided for this load. A generic random turn-on solid state relay is not a proven answer either. Solid state relays also usually fail shorted.
* **A mechanical relay is a good match.** The contacts of the SRD-05VDC-SL-C are rated 10 A, about twenty times the steady current, so the surge is comfortable. They leak nothing when off. Like any relay, the contacts can weld shut after a bad surge, so keep the master switch.
* Mechanical relays take about 10 ms to switch. Use the latency offset in Settings if you want to trim it.

Suggested split: relay 1 for the enlarger, relay 2 for the safelight, both on the one module.

## Wiring the 2-channel relay module

Its pin header is labelled VCC, IN2, IN1, GND, and a second header GND, VCC, JD-VCC with a blue jumper.

| Module | Goes to | Notes |
|---|---|---|
| **Remove the blue jumper** | | The jumper joins the coil supply and the logic supply. Take it off so each has its own. |
| VCC | board 3V3 | powers the optocoupler inputs at 3.3 V |
| JD-VCC | board 5V | powers the relay coils (about 70 mA each) |
| GND | board GND | |
| IN1 | GPIO 4 | enlarger, `PIN_ENLARGER` |
| IN2 | GPIO 5 | safelight, `PIN_SAFELIGHT` |

The inputs are **active low**: the relay is on when the pin is low. `config.h` is already set that way. An unconnected input leaves the relay off, so the relay stays off while the board boots. Power the board from a USB charger that can supply at least 1 A, because two coils and the board together draw a few hundred milliamps.

Other connections:

| Board pin | Goes to |
|---|---|
| GPIO 6 | foot pedal, other leg to GND |
| GPIO 7 | piezo buzzer, other leg to GND |

Avoid GPIO 0, 3, 45, 46 (boot), 19, 20 (USB), 26 to 32 (flash), 33 to 37 (octal flash and PSRAM on N16R8). Check your own board's labels.

**Mains side.** Use COM and NO (normally open) on each relay, so an unpowered relay means lamp off. Switch the live wire only:

```
mains live --[T2A fuse]--[master switch]-- relay 1 COM ...NO -- enlarger live
mains live --------------------------------- relay 2 COM ...NO -- safelight live
neutral and earth pass straight through to both, never through the relay
```

If the fuse blows, the relay contacts weld shut, or the lamp dips when it switches on, the transformer's surge is too large. Fit an inrush limiter (an NTC thermistor in series with the enlarger's mains lead, sized for the transformer) or use a relay with heavier contacts.

## Mains safety

The enlarger and safelight are mains loads. Use an enclosed box with the mains wiring separated from the board, a time-lag fuse (2 A), a proper earth connection, strain relief on every cable, and an RCD-protected supply, which matters in a wet room. Add a master switch. Have someone qualified check the mains side before you use it.

## First power-up, with nothing on mains

0. Wire the low-voltage side only (module VCC, JD-VCC, GND, IN1, IN2, pedal, buzzer). Leave the mains terminals empty.
1. Flash the firmware (below). Open the serial monitor at 115200 baud. You should see `relay ready: FStopRelay`.
2. Install a Bluetooth scanner app (for example nRF Connect) on the tablet or a phone. You should see `FStopRelay` and the service `6f9c0001-...`.
3. Open the web app (below) and tap Connect. The Relay panel (tap "Relay" in the status bar) should show `connected`, `protocol 1`, and status updates arriving.
4. Test each output without mains: Focus light on should light the module's IN1 LED and click relay 1, the Safelight button relay 2, and a start should switch relay 1 for the shown time. Check with a multimeter on continuity between COM and NO. Both relays must be off when the board is unplugged or resetting.
5. Try the foot pedal (short press starts, pauses, resumes; hold one second to cancel) and the buzzer.
6. Only then connect the mains wiring, inside the enclosure.

## Flashing the relay

1. Install the Arduino IDE. In Boards Manager install "esp32 by Espressif Systems" version 3.x. In Library Manager install "NimBLE-Arduino" by h2zero, version 2.x.
2. Open `firmware/relay/relay.ino`. Edit pins and polarity in `config.h` if needed.
3. Board: "ESP32S3 Dev Module". Set Flash Size to 16 MB. For PSRAM choose "OPI PSRAM" (or Disabled). Menu names can differ slightly between versions.
4. Connect the USB port labelled UART or COM (not the native USB one), pick the port, and upload.

## Hosting and installing the web app

Web Bluetooth only works on a secure page.

1. Put the `web` folder on any HTTPS static host (GitHub Pages, Netlify and similar are free).
2. On the tablet open the address in **Chrome**. Web Bluetooth also works in Samsung Internet, not in Safari or on iOS.
3. Use the browser menu, "Install app" or "Add to Home screen". It then opens fullscreen and works without a network.
4. Tap Connect and choose `FStopRelay`. After a drop the app reconnects by itself. After the page is reloaded you may need to tap Connect again, and the pairing list is a system dialog you cannot recolour, so do this with the room lights on.

For development without a host: on a computer run `python3 -m http.server 8080` in `web`, connect the tablet by USB with debugging on, run `adb reverse tcp:8080 tcp:8080`, and open `http://localhost:8080` in Chrome on the tablet. Localhost counts as secure.

When you change any web file, change `CACHE` in `web/sw.js` so tablets pick up the new version.

## Tablet setup checklist

Red safelight filter film over the screen, fullscreen (installed app), Do Not Disturb on, auto-rotate off, on its charger with the screen kept awake, and paper fog test passed. A web page cannot change the tablet's backlight, so the "dim while exposing" setting only darkens the page's own pixels.

## Running the tests

```
firmware/tests/run_tests.sh      # unit tests, and builds /tmp/relay_cli
node test/integration.js         # needs Playwright (npm i playwright)
```

## Changing the protocol

Edit `docs/PROTOCOL.md`, then `relay_core.h`, then the `encode` and `decode` functions in `web/index.html`, then run both tests. If you change a UUID, change it in `relay.ino` and `web/index.html`.
