# ESP32-C3 Super Mini — AHT20 + BMP280 Servo Temperature Dashboard

An ESP32-C3 Super Mini running an asynchronous web dashboard that reads an **AHT20** (temperature + humidity) and a **BMP280** (temperature + pressure) over I²C, computes altitude, drives a servo-controlled lamp, and applies a sunrise/sunset schedule.

This repository also documents the debugging findings from getting the two sensors to behave reliably — most of which were **not** obvious from the datasheets.

![board](https://raw.githubusercontent.com/peff74/ESP_AHT20_BMP280/main/AHT20_BMP280.jpg)

---

## Hardware

| Item | Detail |
|---|---|
| Board | ESP32-C3 Super Mini |
| Display | Web dashboard (no physical display) |
| Sensor bus | I²C, **SDA = GPIO 4, SCL = GPIO 5** |
| AHT20 | address `0x38`, temperature + humidity |
| BMP280 | address `0x77` (also accepts `0x76`), temperature + pressure |
| Servo | GPIO 10 — lamp ON `60°`, OFF `95°` |
| Buzzer | GPIO 6 — WiFi connect/disconnect feedback |
| Network | Static IP `192.168.1.222`, gateway `192.168.1.1`, port 80 |

## Libraries

- `ESPAsyncWebServer` + `AsyncTCP` — use the **ESP32Async** forks
- `Adafruit_BMP280` (+ Adafruit Unified Sensor / BusIO)
- `ESP32Servo`
- Arduino core **3.3.9** recommended (4.0.0 also tested; see findings)

No library is used for the AHT20 — it is read directly over `Wire`.

## Before uploading

Open `Servotemperature.ino` and replace the placeholders:

```cpp
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";
```

The original values were removed before publishing this repository.

## Web API

| Endpoint | Returns |
|---|---|
| `GET /` | Dashboard HTML |
| `GET /data` | JSON: `t1, t2, h, p, rssi, alt, cpu, off, boff, ssid` |
| `GET /time` | NTP time in WIB |
| `GET /jadwal` | Current ON/OFF schedule |
| `GET /set?on=HH:MM&off=HH:MM` | Set schedule (validated, HTTP 400 on bad input) |
| `GET /on`, `GET /off` | Move servo / set lamp state |
| `GET /lamp` | `on` or `off` |
| `GET /sensor_rumah` | JSON for external logging |
| `GET /offset?v=-3.4&b=-4.4` | Set AHT20 / BMP280 temperature offsets (persisted in NVS) |
| `GET /i2c` | Scan result — detected I²C addresses |
| `GET /aht` | AHT20 raw wire diagnostics |
| `GET /bmp` | BMP280 chip ID, `CTRL_MEAS`, raw ADC, compensated values |

---

## Findings (the interesting part)

### 1. `Wire.setClock()` after `begin()` broke all I²C writes
Calling `Wire.setClock(100000)` after `Wire.begin(SDA, SCL)` put the ESP32-C3 I²C peripheral in a state where **address-only writes and reads worked, but any `Wire.write()` followed by `endTransmission()` returned error 4 ("other error")**. Both sensors were affected. The BMP280 stayed at a frozen default reading and the AHT20 never produced data.

**Fix:** remove the `setClock()` call — the default is already 100 kHz. It is left commented out in the sketch.

### 2. AHT20 requires correct timing and a busy-bit check
- Trigger: `0xAC 0x33 0x00`
- Wait **≥ 80 ms** (10 ms is far too short)
- Check **bit 7 of the status byte** (busy) before reading
- Read 7 bytes; data is 20-bit humidity and temperature

The sketch now does this in a **non-blocking state machine** (trigger on one 3 s tick, collect on the next) instead of blocking `loop()` with `delay(100)`.

### 3. BMP280 can silently fall into sleep mode
After an I²C glitch (in practice: a loose dupont cable), the BMP280 resets into **sleep mode**. It still acknowledges `0x77` and returns chip ID `0x58`, but the ADC registers are frozen at `0x80000` and the compensated temperature/pressure never change.

**Fix:** a watchdog reads `CTRL_MEAS` (`0xF4`) every 10 s. If the mode bits (`0x03`) are not `0b11`, `setSampling(MODE_NORMAL, ...)` is re-applied automatically.

### 4. Loose cable symptoms
A partially disconnected sensor can still ACK its address while failing data transfers. `chipId=0x00` or `reads=0` in the diagnostic endpoints means wiring, not code. Reseat VCC / GND / SDA / SCL.

### 5. CPU load calculation was the actual performance problem
Computing instantaneous CPU load inside the async HTTP handler made concurrent `/data` requests race on shared sampling state — producing bogus **100 %** values and a ~60 s web-server hang. It also allocated `String` objects on every sample.

**Fix:** sample once per second in `loop()`, cache in a `volatile float`, use `strstr()` instead of `String`. Steady load is now ~2–4 %.

### 6. Temperature calibration
Both sensors read warm because the ESP32's WiFi/regulator heats the shared board, and the AHT20 itself is specified at ±0.8 °C max error. Reference weather for Cebongan, Salatiga was **22–23.3 °C** (Open-Meteo / wttr.in).

Applied offsets (persisted in NVS):

| Sensor | Offset | Result |
|---|---|---|
| AHT20 | −3.40 °C | 23.2 °C |
| BMP280 | −4.40 °C | 23.5 °C |

The BMP280 needs ~1 °C more correction because it sits closer to the heat source. Tune live with `/offset?v=…&b=…`.

### 7. Altitude from pressure
```
altitude = 44330 × (1 − (P / 1013.25)^(1/5.255))
```
Uses ISA sea-level pressure 1013.25 hPa. Typical reading here: ~654 m.

### 8. Servo calibration
ON was originally 55°. Final ON = **60°**, OFF = **95°**, center = 75°. Exposed as `SERVO_ON` / `SERVO_OFF` constants.

### 9. Slow Arduino core download
The ESP32 core package pulls ~2 GB from GitHub Releases (822 MB RISC-V toolchain, 334 MB Xtensa). `package_esp32_index_mirror.json` in this repo rewrites all 1,440 GitHub URLs through `gh-proxy.com` (~10 MB/s vs ~1.5 MB/s direct). Serve it over HTTP and add its URL to *Additional boards manager URLs*. Checksums are unchanged, so Arduino still verifies every file. Third-party proxy — use at your own risk.

### 10. Schedule trigger
The original `tm_sec == 0` check could be missed by the drifting 1 s loop. Replaced with a minute latch so each ON/OFF time fires exactly once.

---

## References

- [peff74/ESP_AHT20_BMP280](https://github.com/peff74/ESP_AHT20_BMP280) — raw-register AHT20/BMP280 implementation with CRC check and chip-ID detection
- [LukaGitH/AHT20-BMP280](https://github.com/LukaGitH/AHT20-BMP280) — Adafruit-library example
- [icssw-org/MeshCom-Firmware PR #1200](https://github.com/icssw-org/MeshCom-Firmware/pull/1200) — AHT20 reads ~0.8 °C high; offset must be applied at read time
- [PyDevices/pydevices-examples #153](https://github.com/PyDevices/pydevices-examples/issues/153) — ~2 °C warm bias from board self-heating
- [J-A-A-M/jaam_fusion PR #91](https://github.com/J-A-A-M/jaam_fusion/pull/91) — BMP280 detection fixes on AHT20+BMP280 combo modules
- [Open-Meteo](https://open-meteo.com/) — reference weather data
- [wttr.in](https://wttr.in) — second weather reference

## License

MIT — see `LICENSE`.
