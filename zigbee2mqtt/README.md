# Zigbee2MQTT Integration

External converter for Frostbee sensor to make it a "supported" device in Zigbee2MQTT.

## Installation (Zigbee2MQTT v2.0+)

**Important:** Starting from Zigbee2MQTT v2.0, external converters are automatically loaded from the `data/external_converters/` folder. The `external_converters:` setting in `configuration.yaml` is **no longer used**.

### Docker

1. Create the external converters directory and copy the file:
   ```bash
   docker exec zigbee2mqtt mkdir -p /app/data/external_converters
   docker cp frostbee.js zigbee2mqtt:/app/data/external_converters/
   ```

   Or if using docker-compose with a mounted volume:
   ```bash
   mkdir -p /path/to/zigbee2mqtt/data/external_converters
   cp frostbee.js /path/to/zigbee2mqtt/data/external_converters/
   ```

2. Restart Zigbee2MQTT:
   ```bash
   docker restart zigbee2mqtt
   ```

3. Verify in Z2M logs:
   ```bash
   docker logs zigbee2mqtt | grep -i "external\|frostbee"
   ```

   Should show:
   ```
   Loaded external converter from '/app/data/external_converters/frostbee.js'
   ```

### Home Assistant Add-on

1. Access the Z2M data folder via Samba/SSH (usually `/config/zigbee2mqtt/`)

2. Create the `external_converters` subdirectory:
   ```bash
   mkdir -p /config/zigbee2mqtt/external_converters
   ```

3. Copy `frostbee.js` to that folder:
   ```bash
   cp frostbee.js /config/zigbee2mqtt/external_converters/
   ```

4. Restart the Z2M add-on from Home Assistant

### Standalone Installation

1. Navigate to your Zigbee2MQTT data directory (e.g., `/opt/zigbee2mqtt/data/`)

2. Create the external converters folder:
   ```bash
   mkdir -p external_converters
   ```

3. Copy the converter file:
   ```bash
   cp frostbee.js external_converters/
   ```

4. Restart Zigbee2MQTT:
   ```bash
   sudo systemctl restart zigbee2mqtt
   ```

## What this does

- Registers Frostbee as a supported device (no more "Unsupported" warning)
- Proper vendor name (Frostbee) and model (FBE_TH_1) in device list
- Configures standard temperature, humidity, and battery clusters
- Battery reporting with voltage and percentage

## Troubleshooting

### Device shows as "Not supported" or "Unsupported"

This means the external converter is not being loaded. Check:

1. **Verify file location:**
   ```bash
   # Docker
   docker exec zigbee2mqtt ls -la /app/data/external_converters/frostbee.js

   # Home Assistant
   ls -la /config/zigbee2mqtt/external_converters/frostbee.js
   ```

2. **Check Z2M logs** for errors when loading the converter:
   ```bash
   # Docker
   docker logs zigbee2mqtt | grep -i "converter\|frostbee"

   # Look for errors like:
   # "Failed to load external converter"
   # "SyntaxError"
   ```

3. **Use the CommonJS file first (`frostbee.js`):**
   - Z2M v2.x supports both CommonJS (`module.exports`) and ES modules (`export default`), but many installations are still more reliable with `.js` + CommonJS.
   - This repository ships both formats:
     - `frostbee.js` (recommended default)
     - `frostbee.mjs` (ES module alternative)
   - If your logs show converter parse/load errors, switch to the other format.

4. **Remove old configuration:**
   - If upgrading from Z2M v1.x, remove the `external_converters:` section from `configuration.yaml`
   - This setting is ignored in v2.x and may cause confusion

5. **Restart after adding the file:**
   - External converters are loaded at startup
   - After copying the file, always restart Z2M

### CommonJS Format (Alternative)

If the ES6 format doesn't work, try this CommonJS version:

```javascript
const {temperature, humidity, battery} = require('zigbee-herdsman-converters/lib/modernExtend');

const definition = {
    zigbeeModel: ['FBE_TH_1'],
    model: 'FBE_TH_1',
    vendor: 'Frostbee',
    description: 'Temperature & humidity sensor (SHT40)',
    extend: [
        battery({
            voltage: true,
            voltageReporting: true,
            percentageReportingConfig: {min: 3600, max: 65000, change: 2},
            voltageReportingConfig: {min: 3600, max: 65000, change: 2},
        }),
        temperature({reporting: {min: 600, max: 3600, change: 10}}),
        humidity({reporting: {min: 600, max: 3600, change: 50}}),
    ],
};

module.exports = definition;
```

### Device model doesn't match

If the converter loads but device still shows as unsupported:

1. Check if the `zigbeeModel` in the converter matches the device
2. In Z2M, go to the device → "Exposes" tab
3. Look for the model ID reported by the device
4. Update `zigbeeModel: ['FBE_TH_1']` to match exactly

### Battery not reporting

If temperature/humidity work but battery doesn't show:

1. Check ZCL Power Configuration cluster is bound
2. In Z2M device page → "Clusters" → ensure `genPowerCfg` is listed
3. Force a reading: Press the reset button briefly on the device
4. Wait for the next battery update (every 18 hours)

## Migration from v1.x

If upgrading from Zigbee2MQTT v1.x:

1. **Remove** the `external_converters:` section from `configuration.yaml`
2. **Move** converter files from `data/` to `data/external_converters/`
3. Restart Zigbee2MQTT

Example:
```bash
# Docker
docker exec zigbee2mqtt mkdir -p /app/data/external_converters
docker exec zigbee2mqtt mv /app/data/frostbee.js /app/data/external_converters/
```
