# frostbee

Outdoor Zigbee temperature and humidity sensor for the nRF52840 Dongle.

Built with Zephyr RTOS / nRF Connect SDK, using the Sensirion SHT4X driver
and ZBOSS Zigbee stack (Sleepy End Device).

## Features

- 🌡️ **Temperature & Humidity** — Sensirion SHT40 sensor (±0.2°C, ±1.8% RH accuracy)
- 🔋 **Battery monitoring** — Real-time voltage/percentage via ADC (3× AA batteries)
- 🐝 **Zigbee 3.0** — Sleepy end device with persistent network storage
- ⏱️ **Power optimized** — 10-minute sensor reads, 24-hour battery reads (~150-200 µA idle)
- 🔘 **Reset button** — Short press: force read, Long press: factory reset
- 🏠 **Home Assistant** — Works with Zigbee2MQTT and ZHA out of the box
- 🔌 **UF2 bootloader** — Safe firmware updates via drag-and-drop (no programmer needed)

## Hardware

- **Board:** nRF52840 Dongle (PCA10059)
- **Sensor:** Sensirion SHT40-AD1B (I2C address 0x44)

### Pin Assignment

| Function | Pin | Description |
|----------|-----|-------------|
| **I2C SDA** | P0.24 | SHT40 sensor data |
| **I2C SCL** | P1.00 | SHT40 sensor clock (100 kHz) |
| **Battery ADC** | P0.29 (AIN5) | Battery voltage measurement |
| **Battery Enable** | P0.02 | Voltage divider control (active LOW = GND) |
| **Reset Button** | P0.31 | Factory reset / force read |
| **Status LED** | P0.15 | Blue LED (active LOW) |

### Battery Voltage Measurement

The device measures battery voltage (3× AA in series, 3.0V - 4.5V) using a voltage divider with GPIO control for power saving:

```
BAT+ ─── R1 (10kΩ) ─── [P0.29/ADC] ─┬─── R2 (10kΩ) ─── [P0.02/GPIO] ─── GND
                                     │
                                     └─── C (0.1µF) ─── GND
```

**How it works:**
- **P0.02** is configured as **INPUT** (high-Z) when not measuring → 0µA power consumption
- During measurement (once per 24h), **P0.02** is set to **OUTPUT LOW** → connects divider to GND
- ADC reads voltage on **P0.29**, then **P0.02** returns to INPUT mode
- Measurement duration: ~2ms per reading

**Components:**
- R1, R2: 10kΩ (voltage divider 1:2, scales 4.5V → 2.25V for ADC)
- C1: 0.1µF (noise filtering, RC time constant = 1ms)
- Power consumption: 150µA for ~2ms once/day (negligible)

**Voltage ranges:**
- 3× AA fresh: 4.5V → 2.25V at ADC → 100% battery
- 3× AA depleted: 3.0V → 1.5V at ADC → 0% battery
- Low battery alarm: 3.0V (1.0V per cell)

### Measurement Intervals

The device uses separate timers for optimal battery life:

| Measurement | Interval | Rationale |
|-------------|----------|-----------|
| **Temperature/Humidity** | 600s (10 min) | Outdoor temps change slowly; sufficient for weather monitoring |
| **Battery Voltage** | 86400s (24h) | Battery degrades over days/weeks; daily check is enough |

**Zigbee Reporting:** The coordinator (Z2M/ZHA) configures reporting independently via *Configure Reporting* command:
- **Min interval:** Don't report more often than X seconds (e.g., 60s)
- **Max interval:** Report at least once every X seconds (e.g., 3600s = 1h)
- **Reportable change:** Report immediately if value changes by threshold (e.g., ±0.5°C)

This means the device can send reports more frequently than the measurement interval if the coordinator requests it and values change significantly.

### Reset Button (P0.31)

- **Short press (< 1s):** Forces immediate sensor + battery read (does not affect periodic timers)
- **Long press (≥ 5s):** Factory reset — leaves network, erases NVRAM, reboots into pairing mode

## Build & Flash

### Standard build (unified dev/prod configuration)

```
west build -b nrf52840dongle/nrf52840 app
```

Copy the `.uf2` from `build/zephyr/` to the dongle in bootloader mode
(double-tap RESET, dongle mounts as USB drive).

**Current configuration:**
- ✅ **Logging enabled** (USB CDC serial, 115200 baud) — for development/debugging
- ✅ **ZBOSS NVRAM included** (32KB + 16KB) — network persists across reboots
- ✅ **RAM power-down off** — safe for UF2 bootloader (double-tap reset works)
- ✅ **Production intervals** — 600s sensor, 86400s battery
- ⚡ **Idle current:** ~150-200 µA (good for 1-2 years on 3× AA batteries)

### Future battery optimizations (requires SWD access)

Additional ~25-40 µA savings possible by disabling logging/serial and enabling RAM power-down.
See [`TODO_battery.txt`](TODO_battery.txt) for details.

> **Warning:** These optimizations can prevent the UF2 bootloader from detecting
> double-tap reset. Only flash with these settings when you have SWD/J-Link access
> for recovery.

## Flash Partitioning

Flash layout defined in [`pm_static.yml`](app/pm_static.yml):

```
0x000000 - 0x001000  MBR              (  4 KB)  — Nordic MBR
0x001000 - 0x0cc000  Application      (812 KB)  — firmware
0x0cc000 - 0x0d4000  ZBOSS NVRAM      ( 32 KB)  — Zigbee network data (persists)
0x0d4000 - 0x0d8000  ZBOSS product cfg( 16 KB)  — Zigbee product config
0x0d8000 - 0x100000  Bootloader       (160 KB)  — UF2 bootloader (protected)
```

**NVRAM placement:**
- ZBOSS NVRAM partitions are placed safely **below** the bootloader region
- Network credentials, bindings, and reporting config persist across reboots
- No rejoin required after reboot (unless factory reset via button)

**Bootloader protection:**
- The 160 KB bootloader reservation is deliberately oversized for safety
- After SWD recovery you can check the actual start address and reclaim flash:
  ```
  nrfjprog --memrd 0x10001014   # reads UICR.BOOTLOADERADDR
  ```

## Recovery (bricked dongle)

If double-tap reset no longer enters bootloader mode:

1. Connect a J-Link, ST-Link, or other SWD debugger to the dongle's
   SWD pads (SWDIO, SWDCLK, GND).
2. Re-flash the UF2 bootloader hex with `nrfjprog` or OpenOCD:
   ```
   nrfjprog --program <bootloader.hex> --chiperase --verify --reset
   ```
3. After recovery, use the **development build** (no release overlay)
   until the firmware is validated.

## Serial Output

115200 baud, USB CDC serial (enabled by default for debugging):

```
[00:00:00.000,000] <inf> frostbee: Frostbee starting - Zigbee SHT40 sensor
[00:00:00.100,000] <inf> frostbee: SHT40 sensor ready
[00:00:00.200,000] <inf> frostbee: ADC ready on P0.29 (AIN5) for battery voltage
[00:00:00.250,000] <inf> frostbee: Battery voltage divider control ready on P0.02
[00:00:00.300,000] <inf> frostbee: Reset button ready on P0.31 (initial state: released)
```

Monitor with: `screen /dev/ttyACM0 115200` or `minicom -D /dev/ttyACM0 -b 115200`

## Home Assistant Integration

Frostbee works with both Zigbee2MQTT and ZHA:

| Integration | Setup | Folder |
|---|---|---|
| **Zigbee2MQTT** | External converter (copy + config edit) | [`zigbee2mqtt/`](zigbee2mqtt/) |
| **ZHA** | Works out of the box; optional quirk for device naming | [`zha/`](zha/) |

See the README in each folder for step-by-step instructions.

## Project Structure

```
frostbee/
├── app/
│   ├── src/
│   │   ├── main.c              # Main application (sensor, battery, Zigbee)
│   │   ├── zb_frostbee.h       # Zigbee cluster definitions
│   │   └── zb_mem_config_custom.h  # ZBOSS memory configuration
│   ├── prj.conf                # Zephyr project configuration
│   └── pm_static.yml           # Flash partition layout (with NVRAM)
├── zigbee2mqtt/                # Zigbee2MQTT external converter
├── zha/                        # ZHA optional quirk
├── TODO_battery.txt            # Future power optimizations (requires SWD)
└── README.md                   # This file
```

## License

MIT
