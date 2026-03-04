# Frostbee Agent Context

## Current Direction
- The existing `app/` Zigbee firmware is considered unstable (reliability, reporting, configuration, OTA behavior).
- We are intentionally stepping back to a fresh start.
- UF2 bootloader has been restored and is the preferred iteration path for now.
- UF2 battlefield validation is successful (sensor reads, battery reads, button behavior, USB logs).

## Active Development Workflow
- Primary sandbox for firmware iteration is `app_uf2/`.
- `app_uf2/` starts as minimal UF2 firmware and is now the place to incrementally add/harden Zigbee SED behavior.
- Keep OTA out of `app_uf2/` until core Zigbee SED behavior is stable.
- Focus in `app_uf2/`:
  - Sensor reads (SHT40)
  - Battery measurement and calibration
  - Button interaction behavior
  - Configuration correctness
  - High-quality USB logs for debugging
  - Zigbee SED join/rejoin/sleep and reliability hardening (incremental)
- Treat `app_uf2/` as a battlefield/test harness for correctness and stability.

## Migration Strategy
- Add Zigbee SED behavior to `app_uf2/` in small, testable steps (join/rejoin/sleep/reporting first, OTA later).
- Once behavior is stable and validated in `app_uf2/`, port proven logic into `app/`.
- `app/` remains the production Zigbee target; `app_uf2/` remains the fast debug target.
- Next phase: start hardening Zigbee SED behavior from this stable UF2 baseline.

## Notes For Future Edits
- Prefer small, testable changes in `app_uf2/`.
- Keep logs explicit (what was measured, when, and why).
- When uncertain, optimize for observability and determinism over feature breadth.
- Execution checklist and phase status are tracked in `ROADMAP_UF2_TO_OTA.md`.
