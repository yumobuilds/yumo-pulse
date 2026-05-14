# YUMO BUILDS — Weather Station

**ESP32-C3 Super Mini · SHT3X · 0.96" OLED · v2.0**

---

## Hardware

| Component | Model |
|-----------|-------|
| MCU | ESP32-C3 Super Mini |
| Sensor | SHT3X — temperature + humidity (I2C) |
| Display | 0.96" OLED 128×32 SSD1306 (I2C) |

---

## Pin Wiring

### I2C Bus — OLED + SHT3X (shared)

Both the OLED and the SHT3X sensor share the same I2C bus.

| Signal | GPIO | Connects to |
|--------|------|-------------|
| SDA | 8 | OLED SDA + SHT3X SDA |
| SCL | 9 | OLED SCL + SHT3X SCL |

> **OLED I2C address:** `0x3C`  
> **SHT3X I2C address:** `0x44`

---

### LEDs — all with 330 Ω series resistor to GND

| GPIO | Colour | Name | Behaviour |
|------|--------|------|-----------|
| 3 | Yellow | SECS | Blinks in sync with clock colon — every 500 ms |
| 0 | Purple | GLOW | Pulses with YUMO brand page outer frame (PWM) |
| 10 | Blue | FLASH | Full brightness burst × 3 per minute |
| 1 | Red | PROX | Glows when hand near antenna wire (PWM fade) |
| 2 | Green | TEXT | Slow sine-wave while web message displays (PWM) |

> All LED GPIOs use `ledcAttach` / `ledcWrite` (ESP32-C3 PWM API, 1 kHz, 8-bit).

---

### Touch Antenna

| GPIO | Connection | Notes |
|------|-----------|-------|
| 7 | Bare wire | **No resistor.** Wire reads HIGH on touch. Drives PROX LED (GPIO 1) via smooth ramp up / slow fade. |

---

## Wiring Diagram

```
ESP32-C3 Super Mini
┌─────────────────────┐
│ GPIO 8  (SDA) ──────┼──┬── OLED SDA
│                     │  └── SHT3X SDA
│ GPIO 9  (SCL) ──────┼──┬── OLED SCL
│                     │  └── SHT3X SCL
│                     │
│ GPIO 3  ────[330Ω]──┼── LED Yellow (SECS)  → GND
│ GPIO 0  ────[330Ω]──┼── LED Purple (GLOW)  → GND
│ GPIO 10 ────[330Ω]──┼── LED Blue   (FLASH) → GND
│ GPIO 1  ────[330Ω]──┼── LED Red    (PROX)  → GND
│ GPIO 2  ────[330Ω]──┼── LED Green  (TEXT)  → GND
│                     │
│ GPIO 7  ────────────┼── bare wire (antenna / touch)
└─────────────────────┘
```

---

## Night Mode (00:00 – 05:00)

- OLED dims to minimum contrast
- SECS (GPIO 3) and PROX (GPIO 1) reduce to very low brightness
- GLOW (GPIO 0) dims on brand page
- FLASH (GPIO 10) continues its 3× per minute pulse
- Sensor refresh slows from 2 s → 60 s

---

## Display Pages

| Page | Duration | Description |
|------|----------|-------------|
| CLOCK | 6 s | Auto-detected city + NTP time — HH:MM flashing colon + seconds |
| HUMIDITY | 4 s | Live reading from SHT3X |
| TEMPERATURE | 4 s | Live reading from SHT3X |
| YUMO | 9 s | Brand page — animated frame + sine contrast pulse *(skipped at night)* |
| TEXT | Until read | Web message typed character by character |

---

## WiFi Setup

| Step | Action |
|------|--------|
| First boot | ESP32-C3 creates hotspot **`YUMO-WEATHER`** |
| Connect | Join `YUMO-WEATHER` on your phone |
| Configure | Open browser at `192.168.4.1` → enter home WiFi credentials → saved to flash |
| Next boots | Connects automatically — no portal needed |
| Change WiFi | Uncomment `wm.resetSettings()` once, upload, re-configure, comment back out |

---

## Web Interface

After connecting, the IP address is shown on the OLED.  
Visit `http://<IP>` in a browser to:

- See live temperature and humidity readings (auto-refreshes every 5 s)
- Send any text — appears on the OLED one character at a time
- Green LED (GPIO 2) sine-waves while the message is displayed

---

## Libraries

Install via **Arduino IDE → Library Manager:**

| Library | Purpose |
|---------|---------|
| `Adafruit SSD1306` | OLED display driver |
| `Adafruit GFX Library` | Graphics primitives |
| `Adafruit SHT31 Library` | SHT3X sensor |
| `WiFiManager` by tzapu | WiFi portal + credential storage |

---

## Board Settings

| Setting | Value |
|---------|-------|
| FQBN | `esp32:esp32:esp32c3` |
| Port | `cu.usbmodem...` (native USB-C, no CH340) |
