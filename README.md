# Stock Dashboard v1.21

**Clingman Ave Labs** — E-ink stock ticker for the Heltec Vision Master e290

A battery-powered, Wi-Fi-connected stock dashboard that displays real-time price charts on a 2.9" e-ink display. Supports up to 10 ticker symbols (stocks, crypto, forex) with 8 chart time ranges, all powered by Yahoo Finance — no API key required.

---

## Features

- Up to 10 ticker symbols with drag-to-reorder configuration
- 8 chart views: 1 Day, 5 Day, 1 Month, 6 Month, YTD, 1 Year, 5 Years, All
- Individually toggle which chart views are available
- Configurable auto-refresh interval (5–120 minutes)
- Smart weekend/off-hours sleep to conserve battery
- Crypto and forex 24/7 support with dynamic trading hours
- Y-axis scaling from penny stocks ($0.002) to BTC ($100k+)
- Battery percentage indicator on the display
- Captive portal for easy phone-based configuration
- Live ticker symbol validation before saving
- Factory reset option via the configuration portal

---

## Table of Contents

1. [Installation](#installation)
2. [First-Time Setup](#first-time-setup)
3. [Navigating Tickers and Chart Views](#navigating-tickers-and-chart-views)
4. [Returning to Configuration Mode](#returning-to-configuration-mode)
5. [Configuration Portal](#configuration-portal)
6. [Factory Reset](#factory-reset)
7. [Troubleshooting](#troubleshooting)

---

## Installation

### Prerequisites

- **Hardware:** Heltec Vision Master e290 (ESP32-S3 with 2.9" e-ink display)
- **Software:** Arduino IDE 2.x (or 1.8.19+)

### Step 1 — Install the Arduino IDE

Download and install the Arduino IDE from [https://www.arduino.cc/en/software](https://www.arduino.cc/en/software).

### Step 2 — Add the ESP32-S3 Board Support

1. Open Arduino IDE and go to **File > Preferences**.
2. In the **Additional Boards Manager URLs** field, add:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. Go to **Tools > Board > Boards Manager**, search for **esp32**, and install the **esp32 by Espressif Systems** package.

### Step 3 — Install Required Libraries

Install the following libraries via **Sketch > Include Library > Manage Libraries**:

| Library | Purpose |
|---|---|
| **ArduinoJson** (by Benoit Blanchon) | JSON parsing for Yahoo Finance API responses |
| **heltec-eink-modules** (by Todd Herbert) | E-ink display driver for the Vision Master e290 |

The remaining includes (`WiFi`, `WebServer`, `DNSServer`, `HTTPClient`, `WiFiClientSecure`, `Preferences`, `time`) are built into the ESP32 Arduino core and do not need separate installation.

### Step 4 — Configure Board Settings

1. Go to **Tools > Board** and select **Heltec Vision Master E290** (under the ESP32S3 section).
2. Set the following under the **Tools** menu:
   - **USB CDC On Boot:** Enabled
   - **PSRAM:** OPI PSRAM (if available on your board)
   - **Upload Speed:** 921600
   - **Port:** Select the COM/serial port for your connected device

### Step 5 — Upload the Sketch

1. Open `StockDashboard_v1_21/StockDashboard_v1_21.ino` in the Arduino IDE.
2. Connect the Vision Master e290 to your computer via USB-C.
3. Click **Upload** (right-arrow button) or press **Ctrl+U**.
4. Wait for the upload to complete. The device will restart automatically.

---

## First-Time Setup

On the very first boot (or after a factory reset), the device will automatically enter the setup portal:

1. The e-ink display will show **"WELCOME - FIRST TIME SETUP"** with instructions.
2. On your phone or computer, connect to the Wi-Fi network named **`StockDash-Setup`** (no password).
3. A captive portal page should open automatically. If it doesn't, open a browser and go to **`192.168.4.1`**.
4. **WiFi Tab:** Enter your home Wi-Fi network name (SSID) and password, then tap **Save WiFi**.
5. The device will restart and reconnect. Connect back to **`StockDash-Setup`** to continue.
6. **Dashboard Tab:** Add your desired ticker symbols (e.g., `AAPL`, `BTC-USD`, `EURUSD=X`), choose which chart views to enable, set the refresh interval, and tap **Save Dashboard**.
7. The device will restart and begin displaying your stock data.

---

## Navigating Tickers and Chart Views

The Vision Master e290 has a single physical button (GPIO 21) used for all navigation. The device uses a deep-sleep architecture — it wakes on each button press, updates the display, and goes back to sleep.

### Short Press — Cycle Chart Views

Press and quickly release the button to cycle through the enabled chart views for the current ticker:

**1 Day → 5 Day → 1 Month → 6 Month → YTD → 1 Year → 5 Years → All → (wraps back)**

Only views you enabled in the Dashboard configuration will appear. Disabled views are automatically skipped.

### Long Press (3+ seconds) — Switch Ticker

Press and hold the button for at least **3 seconds** to switch to the next ticker symbol in your list. The display will reset to the first enabled chart view for the new ticker.

This only applies if you have more than one ticker configured. With a single ticker, a long press has no effect.

### Summary

| Action | Duration | Result |
|---|---|---|
| **Short press** | Quick tap (< 3 sec) | Next chart view |
| **Long press** | Hold 3+ seconds | Next ticker symbol (resets to first chart view) |

---

## Returning to Configuration Mode

To re-enter the configuration portal at any time (change Wi-Fi, add/remove tickers, adjust settings):

1. **Press the reset button** on the Vision Master e290 (the small hardware reset button on the board, not the main user button).
2. **Immediately press and hold** the main user button (GPIO 21) while the device is restarting.
3. Keep holding for about **1 second** until the display shows **"CONFIGURATION MODE"**.
4. Connect your phone to the **`StockDash-Setup`** Wi-Fi network.
5. The configuration portal will open with three tabs: **WiFi**, **Dashboard**, and **Reset**.
6. Make your changes and save. The device restarts automatically after saving.

The portal has a **5-minute timeout** — if no changes are saved within 5 minutes, the device returns to normal operation.

---

## Configuration Portal

The portal is a mobile-friendly web interface with three tabs:

### WiFi Tab
- Enter your Wi-Fi SSID and password.
- Use "Show password" to verify your entry.

### Dashboard Tab
- **Tickers:** Add up to 10 Yahoo Finance symbols. Drag to reorder. Symbols are validated live against Yahoo Finance before saving.
  - Stocks: `AAPL`, `MSFT`, `TSLA`
  - Crypto: `BTC-USD`, `ETH-USD`
  - Forex: `EURUSD=X`, `GBPUSD=X`
  - Indices: `^GSPC`, `^DJI`
- **Chart Views:** Toggle which of the 8 time ranges are available when cycling with the button.
- **Refresh Interval:** Set how often the device wakes to fetch new data (5–120 minutes).

### Reset Tab
- **Factory Reset:** Erases all saved configuration (Wi-Fi, tickers, preferences) and returns the device to first-boot state.

---

## Factory Reset

There are two ways to factory reset:

1. **Via the portal:** Enter configuration mode (see above), go to the **Reset** tab, and tap **Factory Reset**.
2. **On-device:** The factory reset confirmation requires using the web portal for safety.

After a factory reset, the device will enter the first-time setup flow on the next boot.

---

## Troubleshooting

| Issue | Solution |
|---|---|
| Display shows nothing after upload | Press the reset button; ensure USB-C cable supports data (not charge-only) |
| Can't find `StockDash-Setup` Wi-Fi | Reset the device while holding the user button to force configuration mode |
| "WiFi" error on display | Check that your SSID and password are correct; re-enter in the config portal |
| Ticker shows no data | Verify the symbol is valid on Yahoo Finance (e.g., `AAPL`, not `Apple`) |
| Display not updating | The device sleeps between refreshes; press the button to wake it immediately |
| Battery draining fast | Increase the refresh interval in Dashboard settings; the device sleeps between updates |

---

## License

(c) 2026 Parker Redding & Clingman Ave Labs
