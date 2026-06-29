# Clawd Tank Badge 🦀

A wearable ESP32-C3 badge with a round display showing **Clawd**, a pixel-art crab
that reacts in real time to your **Claude Code** activity — and shows your Claude
plan's usage ring and a live countdown to the next 5-hour window reset.

Touch the screen and Clawd's eyes follow your finger; pet it (stroke left–right)
and it blushes with floating hearts. 💕

> Inspired by [marciogranzotto/clawd-tank](https://github.com/marciogranzotto/clawd-tank).

---

## Features

- **Live activity** — the crab animates per tool: reading, editing, running bash,
  searching, MCP, thinking, happy, error, sleeping.
- **Plan usage ring** — outer arc fills with your current Claude utilization (%),
  color-graded blue → green → yellow → red.
- **Reset countdown** — `H:MM:SS` until the 5-hour window resets. Computed on the
  always-on backend, so it keeps ticking even if the badge has no clock/NTP.
- **Touch interaction** — eyes glide toward your finger; petting triggers a happy
  reaction. Touching also wakes the crab from sleep.
- **Works anywhere** — the badge polls a small relay over HTTPS, so it shows live
  status on any WiFi (home, shop, phone hotspot) with no port-forwarding.

## How it works

```
  ┌─────────────┐   activity (hooks)    ┌──────────────────────────┐
  │ PC: Claude  │ ───────────────────▶  │  Raspberry Pi (always on)│
  │ Code hooks  │   POST /event         │                          │
  └─────────────┘                       │  • clawd_relay.py  :8070 │
                                        │  • usage_sync.py         │
  ┌─────────────┐   usage % + reset     │    (Pi's own Claude      │
  │ claude.ai   │ ◀──────────────────── │     Code OAuth token)    │
  │ usage API   │                       │  • cloudflared tunnel    │
  └─────────────┘                       └────────────┬─────────────┘
                                                     │ HTTPS
                          GET /state every 3s        ▼
                    ┌──────────────────────────────────────┐
                    │  ESP32-C3 badge (any WiFi, anywhere)  │
                    └──────────────────────────────────────┘
```

The PC is optional (only feeds activity). The Pi is the always-on source of truth
for usage % and the reset countdown, using its **own** Claude Code login — so the
badge stays accurate even when your PC is off.

## Hardware

**Waveshare ESP32-C3-Touch-LCD-1.28** — a 1.28" 240×240 round display:

- MCU: ESP32-C3 (2.4 GHz only, **not** WPA3-compatible — set hotspots to WPA2)
- Display: GC9A01 (SPI)
- Touch: CST816S capacitive (I2C)

All wiring is on-board — no soldering needed. Pin map (already set in `platformio.ini`):

| Bus     | Signal | GPIO |
|---------|--------|------|
| Display | MOSI / SCLK / CS / DC / BL | 7 / 6 / 10 / 2 / 3 |
| Touch   | SDA / SCL / RST / INT      | 4 / 5 / 1 / 0 |

> A `cyd` build target is also included for the cheaper ESP32-2432S028 ("CYD")
> 240×320 board (ILI9341, no round display / touch petting).

## Repository layout

| Path | What |
|------|------|
| `src/clawd_tank.cpp` | Badge firmware (display, animation, touch, WiFi, polling) |
| `src/wifi_credentials.example.h` | Copy to `wifi_credentials.h` and fill in your WiFi |
| `platformio.ini` | Build targets: `esp32c3` (badge) and `cyd` |
| `hook_sender.ps1` | Claude Code hook → posts activity to the relay |
| `pi/clawd_relay.py` | Event broker the badge polls (`/event`, `/state`) |
| `pi/usage_sync.py` | Polls claude.ai usage with the Pi's own token; feeds the relay |
| `pi/clawd-usage-sync.service` | systemd unit example |

## Setup

### 1. Flash the badge

```bash
# Install PlatformIO, then:
cp src/wifi_credentials.example.h src/wifi_credentials.h
# edit wifi_credentials.h with your SSID/password(s)

pio run -e esp32c3 -t upload
```

The badge boots, joins WiFi, and starts polling the relay. On its own it will show
the crab and a "READY" state until a relay provides usage data.

### 2. (Optional) Feed activity from your PC

Add `hook_sender.ps1` as a Claude Code hook so tool events animate the crab.
Point `$RELAY_URL` at your relay's public `/event` URL.

### 3. (Recommended) Run the backend on an always-on machine

On a Raspberry Pi (or any Linux box) that's always on:

1. Log in once with Claude Code (`claude`) so the Pi has its own credentials.
2. Run `pi/clawd_relay.py` and `pi/usage_sync.py` as systemd services
   (see `pi/clawd-usage-sync.service`).
3. Expose the relay over HTTPS (e.g. a [Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/))
   and set that URL as `RELAY_URL` in the firmware.

The relay computes `remain_sec` from the Pi's clock, so the countdown is accurate
without the badge needing NTP.

## Touch

| Gesture | Reaction |
|---------|----------|
| Touch & hold | Eyes glide toward your finger |
| Stroke left–right 3×+ | "Petting" — happy eyes, blush, floating hearts (2.5 s) |
| Touch while sleeping | Wakes the crab |

## Credits

- Crab character & concept: [marciogranzotto/clawd-tank](https://github.com/marciogranzotto/clawd-tank)
- Display/touch libs: [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI), [CST816S](https://github.com/fbiego/CST816S)

## License

MIT — see [LICENSE](LICENSE).
