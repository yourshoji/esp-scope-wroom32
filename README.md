# ESP-Scope (ESP32 WROOM-32D)

A stable, plug-and-play port of [MatAtBread’s ESP-Scope](https://github.com/MatAtBread/esp-scope) for the **classic ESP32 (WROOM-32D)**.  
Works on most 30/38-pin ESP32 dev boards without crashes, or Wi-Fi lockups.

This fork adapts the original RISC-V (ESP32-C6) design to **Xtensa-based ESP32** hardware by rewriting core drivers and fixing memory and networking issues.

![esp-scope](screenshot.png)

---

## Why This Port
The original project is solid but hardware-specific. On WROOM boards it often crashes or hangs. This version focuses on **stability and universal compatibility**.

### Key Changes
- Type-1 DMA and ADC floor locked to **20 kHz** for WROOM limits
- Stack increased to **6144 bytes**, large buffers moved to static RAM
- WebSocket auto-connect on page load
- `lru_purge` enabled to prevent server lockups on refresh
- Frame skipping to keep Wi-Fi stable during streaming

---

## Getting Started

### Build & Flash
```bash
idf.py set-target esp32
idf.py fullclean
idf.py build flash monitor
```

### First Boot
1. ESP32 creates an AP named **ESP-Scope**
2. Connect and open `192.168.4.1`
3. Enter Wi-Fi credentials
4. Open `http://esp-scope.local` (or the IP shown in serial output)

---

## Pinout (WROOM)

| Function    | GPIO | Notes |
|------------|------|-------|
| Signal In  | 36 (VP) | Analog input, 0–3.3V max |
| Test Out   | 18 | 100 Hz PWM (calibration) |
| Status LED | 2 | Blinks during streaming |
| Reset      | 0 | Hold 3s for factory reset |

---

## Troubleshooting
- **Black screen**: Not on the same 2.4 GHz Wi-Fi network
- **White screen**: `idf.py fullclean` was skipped
- **Stuttering**: Expected. Frames are skipped to protect Wi-Fi stability

---

## 3D Case
Two-part enclosure with AA battery space.  
Originally designed for XIAO, adaptable to ESP32 boards.  
Designed in Fusion 360, printed on a Bambu Labs A1 Mini.

---

## Credits
- **Original Project**: MatAtBread
- **WROOM Port**: Stability fixes for classic ESP32 hardware
- **License**: MIT

Compatibility build.  
It works now. Please don’t touch the stack size.
