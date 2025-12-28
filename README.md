# ESP-Scope (ESP32 WROOM-32D Port)

A stable port of [MatAtBread's ESP-Scope](https://github.com/MatAtBread/esp-scope) adapted for ESP32 WROOM-32D (Xtensa). Fixes crashes, memory overflows, and network instability that occur when running the original RISC-V code on classic WROOM boards.

![ESP-Scope Interface](image.png)

## Key Changes

**Hardware Adaptation**
- Reconfigured ADC driver for Type 1 DMA (single-unit ADC requirement)
- Set 20 kHz sampling rate for hardware stability
- Moved 4KB telemetry buffers from stack to static RAM
- Increased task stack to 6KB to prevent boot loops

**Network Fixes**
- Enabled socket recycling with LRU purging to prevent server hangs
- Relaxed WebSocket handshake for immediate streaming
- Implemented frame skipping (every 2nd frame) for Wi-Fi stability

## Quick Start

```bash
idf.py set-target esp32
idf.py fullclean
idf.py build flash monitor
```

1. Connect to **ESP-Scope** Wi-Fi AP
2. Navigate to **192.168.4.1** to access the scope

**Optional:** Configure your own Wi-Fi network through the interface. The device will restart and the new IP will be shown in the serial monitor.
![ESP-Scope CMD](cmd.png)

## Pinout

| Function | GPIO | Notes |
|----------|------|-------|
| Signal Input | 36 (VP) | Analog input displayed on scope (0-3.3V max) |
| Test Signal | 18 | 100 Hz test waveform for calibration |
| Status LED | 2 | Streaming indicator |
| Factory Reset | 0 | Hold BOOT button 3s |

## Troubleshooting

- **White screen:** Run `idf.py fullclean` and rebuild
- **No waveform:** Verify 2.4 GHz network connection
- **Stuttering:** Intentional frame skipping for stability
- **Boot loops:** Check power supply (500mA minimum)

## Important Notes

- Sampling capped at 20 kHz (hardware limitation)
- Frame rate is half of capture rate by design
- **Do not reduce task stack below 6KB**
- For ESP32-C3/C6/S3, use the original project for better performance

![ESP-Scope GIF](gif.gif)

## Credits

**Original:** [MatAtBread](https://github.com/MatAtBread/esp-scope) – UI, architecture, and concept  
**License:** MIT

This is a hardware-specific adaptation for Xtensa-based ESP32 modules.
