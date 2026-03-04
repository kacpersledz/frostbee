# Frostbee Roadmap: `app_uf2` to Full MCUboot/OTA

This is the execution checklist from current UF2 battlefield testing to production OTA firmware.

## How To Use
- Keep this file as the working checklist (status + notes).
- Keep `AGENTS.md` as stable context/policy.
- Only mark items done when validated on real hardware.
- `app_uf2` is the implementation source-of-truth for all phases in this roadmap; `app/` is a downstream production port target only.

## Phase 0: UF2 Baseline (Current)
- [x] UF2 app boots reliably and logs over USB (`screen`).
- [x] Sensor reads are stable in fast test mode.
- [x] Battery divider on/off measurement path works.
- [x] Button short press triggers forced read.
- [x] Button long press path is deterministic (no reset side effects).
- [x] Fixed bug where periodic loop stopped after forced read.
- [ ] Deferred: 6-12h continuous run with no stalls/resets.
- [ ] Deferred: Power-cycle stress test (20+ cycles) with expected startup behavior.

## Phase 1: Harden Non-Zigbee App Logic in `app_uf2`
- [x] Add runtime counters (successful reads, failed reads, button events, uptime snapshots).
- [x] Add fault injection checks:
  - sensor disconnect/reconnect recovery
  - I2C error recovery without reboot
- [x] Validate button threshold boundaries around short/long cutoffs.
- [x] Confirm battery measurement noise bounds and averaging strategy.
- [x] Freeze a "known good" baseline tag/commit.

Exit criteria:
- repeatable behavior over long run and fault/recovery tests
- clear baseline commit to branch from

## Phase 2: Add Zigbee SED Core in `app_uf2` (No OTA Yet)
- [ ] Integrate Zigbee stack minimally (single endpoint + basic clusters).
- [ ] Join commissioning flow works reliably.
- [ ] Rejoin after reset/power-cycle works reliably.
- [ ] Sleepy End Device behavior works as intended.
- [ ] Reporting cadence and attribute updates are reliable.
- [ ] Keep USB logs clean and actionable during Zigbee operation.
- [ ] Validate behavior with Z2M (smoke tests).

Exit criteria:
- stable join/rejoin/sleep/reporting in repeated cycles
- no unexplained lockups or silent failures

## Phase 3: Migrate Stable Logic to Production `app/`
- [ ] Port proven modules from `app_uf2` into `app/` incrementally.
- [ ] Re-enable MCUboot/sysbuild/partition layout in production app.
- [ ] Verify ST-Link flash flow for production image (bootloader + app).
- [ ] Validate persistent Zigbee storage behavior across reboots.
- [ ] Re-test join/rejoin/reporting in production build.

Exit criteria:
- production `app/` behavior matches hardened `app_uf2` behavior

## Phase 4: OTA Enablement & Release Hardening
- [ ] Re-enable Zigbee OTA path (`mcuboot`, image generation, metadata).
- [ ] Validate OTA upgrade success path.
- [ ] Validate OTA interrupted/retry/recovery scenarios.
- [ ] Validate settings/NVRAM persistence across OTA swap.
- [ ] Validate downgrade/rollback expectations.
- [ ] Final battery-life profile in production intervals.

Exit criteria:
- OTA update process is repeatable and safe
- release checklist complete

## Test Matrix (Recommended Minimal Set)
- [ ] USB powered + battery powered
- [ ] Cold boot + warm reset
- [ ] Short run + long run
- [ ] Good RF + weak RF
- [ ] Z2M smoke coverage

## Notes
- Some items are intentionally deferred until ST-Link + production bootloader flow.
- `app_uf2` is for speed and observability; final power profile must be validated on production settings.
- Do not treat `app/` as implementation source-of-truth for any `app_uf2` work; build and validate behavior in `app_uf2` first, then port proven logic into `app/`.
