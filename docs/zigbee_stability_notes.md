# Zigbee SED Stability Notes (practical)

These notes summarize common patterns used by stable Zigbee sleepy sensors and how Frostbee should be operated/troubleshot when only Zigbee2MQTT logs are available.

## 1) Typical stable SED design patterns

| Area | Common stable pattern | Why it helps |
|---|---|---|
| Join / rejoin | Keep device awake for a short post-join window | Coordinator can finish interview, binding, and reporting setup before device sleeps |
| Reporting | Conservative min/max/change thresholds | Avoids both report spam and stale values |
| Poll/keepalive | Parent timeout and keepalive aligned | Reduces dropped queued messages for sleepy devices |
| OTA | Force awake during OTA download | Transfer does not stall between sleeps |
| Button | Clear short/long semantics + debounce | Predictable field service behavior |
| Timers | Cancel+reschedule on rejoin | Avoid duplicate jobs and race-like behavior |

## 2) Button behavior (what is useful and why)

Current Frostbee behavior:
- Short press (< 1s): force immediate sensor+battery read and stay awake briefly.
- Long press (>= 5s): factory reset.

What often appears in other sensors:
- **Double press**: service action without destructive reset (e.g., temporary join/interview window, identify blink, or reconfigure trigger).

When double press helps:
- You need a manual “service mode” in the field without risking factory reset.
- You want a user action that is safer than long hold.

If your current short/long mapping works for you, double press is optional.

## 3) Z2M 2.8.0 and external converters (.js vs .mjs)

Recommended practical approach:
- Keep **both files** in your repo (`frostbee.js` and `frostbee.mjs`) for portability.
- In your running Z2M instance, load **one converter file at a time** in `data/external_converters/`.

Why one-at-a-time in runtime:
- Avoid ambiguity and accidental duplicate matching if both definitions are loaded.
- Troubleshooting is simpler (you know exactly which format is active).

Suggested order for Z2M 2.8.0:
1. Start with `frostbee.js` (CommonJS).
2. If load/parsing problems persist, try `frostbee.mjs` instead.

## 4) Diagnostics using only Zigbee2MQTT logs (no USB logs)

### What to capture
- Pairing/join sequence (from permit join until interview completed/failed).
- Rejoin sequence after coordinator restart.
- OTA check and OTA start/fail events.
- Converter load lines at startup.

### What to look for in logs
- Interview failures or timeouts.
- Configure reporting failures.
- Missing/failed external converter load.
- OTA mismatch/unsupported image messages.

### Minimal command set (Docker examples)
```bash
# follow logs live

docker logs -f zigbee2mqtt

# converter-related lines

docker logs zigbee2mqtt | grep -Ei "external|converter|frostbee|syntax"

# interview/reporting/ota related lines

docker logs zigbee2mqtt | grep -Ei "interview|report|configure|ota|update"
```

## 5) Priority order (before battery optimization)

1. Stabilize join/rejoin/interview path.
2. Confirm converter loads reliably on every restart.
3. Confirm OTA matching + update flow from logs.
4. Only then tune battery intervals and thresholds.

