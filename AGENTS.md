# Frostbee Agent Context

## Current Direction
- The existing `app/` Zigbee firmware is considered unstable (reliability, reporting, configuration, OTA behavior).
- We are intentionally stepping back to a fresh start.
- UF2 bootloader has been restored and is the preferred iteration path for now.

## Active Development Workflow
- Primary sandbox for firmware iteration is `app_uf2/`.
- `app_uf2/` must stay Zigbee-free and OTA-free.
- Focus in `app_uf2/`:
  - Sensor reads (SHT40)
  - Battery measurement and calibration
  - Button interaction behavior
  - Configuration correctness
  - High-quality USB logs for debugging
- Treat `app_uf2/` as a battlefield/test harness for correctness and stability.

## Migration Strategy
- Do not add Zigbee join/rejoin/sleep/OTA complexity to `app_uf2/`.
- Once behavior is stable and validated in `app_uf2/`, port proven logic into `app/`.
- `app/` remains the production Zigbee target; `app_uf2/` remains the fast debug target.

## Notes For Future Edits
- Prefer small, testable changes in `app_uf2/`.
- Keep logs explicit (what was measured, when, and why).
- When uncertain, optimize for observability and determinism over feature breadth.
