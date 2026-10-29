# ESP32-S3: repeated SYSTIMER UNIT1 high-word jumps observed on both cores (instrumented Arduino-ESP32 3.3.7 / IDF 5.5.2)

Review material for [espressif/esp-idf#19333](https://github.com/espressif/esp-idf/issues/19333). This repository is a source-and-evidence snapshot, not a buildable firmware or a minimal reproducer. See [README.md](README.md) for scope and decoder commands.

## Summary

An ESP32-S3 application intermittently reads implausibly large forward jumps from SYSTIMER UNIT1, which supplies the FreeRTOS tick. The retained diagnostics contain six distinct capture-boot identities, each with a record from both cores. Five events have a stable unexpected high word throughout 32 subsequent snapshot attempts per core; one event has a varying high word.

The most recent retained event changes the high word from `0x00000015` to `0x00000237`, with both cores reporting the same initial counter value. UNIT0 remains readable and close to the preceding valid UNIT1 value during the fault probe.

**These observations come from a modified SysTick/HAL diagnostic build. They do not establish an ESP32-S3 silicon defect or a bug in unmodified ESP-IDF.** We are reporting the measurements and requesting guidance on the next isolating test.

## Environment

| Item | Value |
| --- | --- |
| Module | ESP32-S3-WROOM-1 N16R8; 16 MiB flash, 8 MiB OPI PSRAM |
| CPU / tick configuration | 240 MHz; SYSTIMER 16 MHz; observed tick period 16,000 counts (1 kHz) |
| Arduino-ESP32 | 3.3.7 |
| Exact runtime IDF version string | `v5.5.2-729-g87912cd291` |
| Application | ZOnlinE `v2.6.716-test1 (20261029)` |
| Source revision | [`124b85c0cdfdc478a226368d3fc6f642ed6b7bfe`](SOURCE_MANIFEST.json) |
| Build artifact | PlatformIO application ELF/MAP preserved |
| Silicon revision | Not established from the evidence reviewed; still needs to be recorded |
| Power / peripherals | Vehicle CAN gateway application with Wi-Fi, LTE/GNSS, I2C OLED/sensor and flash logging. Exact electrical conditions and active peripheral state at each retained event have not been isolated. |

This has **not** yet been reproduced with an unmodified current ESP-IDF release/master or a minimal peripheral-free application.

## Diagnostic modifications and source

The build overrides the SDK SysTick port and the SYSTIMER HAL object. Counter reads use UPDATE, a bounded VALID wait and a LO–HI–LO consistency check. The tick guard compares the resulting pending ticks and elapsed time against independent CPU-cycle accounting, attempts bounded re-reads, and stops deliberately if it cannot recover a plausible value. It does not fabricate time to continue scheduling.

After a fatal guard decision, each core can take a fault-only UNIT0 reference snapshot and up to 32 UNIT1 UPDATE/VALID/LO–HI–LO samples. The retained summary contains accepted-sample count, HI bitwise AND/OR, final sample and probe duration. **UNIT1 reads and fault probes are currently not serialized between cores.** This is a potential software explanation to test, not a proven cause.

Source files in this review snapshot (original identities in [SOURCE_MANIFEST.json](SOURCE_MANIFEST.json)):

- [Bounded counter reader](zonline_systick_read.h)
- [SysTick override and fatal path](zonline_systick_port.c)
- [HAL override](zonline_systimer_hal_port.c)
- [Fault/probe capture](zonline_fault_capture.c)
- [First-fault decoder](scripts/decode_first_fault.py) and [probe decoder](scripts/decode_systimer_probe.py)

## Observed values

The six capture-boot tokens below are taken from the retained record, **not** from the later boot printing it. Values are hexadecimal high words; the table shows core 0. The corresponding core 1 records have the same initial unexpected high word and the same HI AND/OR summaries; their low words need not match. Each core reports 32 accepted samples per event.

| Capture boot | Last-good HI | Unexpected HI | Sample HI AND | Sample HI OR |
| --- | --- | --- | --- | --- |
| `24291a8a` | `00000005` | `00002283` | `00002082` | `0000228b` |
| `01708d9f` | `00000004` | `00000204` | `00000204` | `00000204` |
| `4d1ff901` | `00000005` | `00020007` | `00020007` | `00020007` |
| `88aee6a3` | `00000039` | `00020239` | `00020239` | `00020239` |
| `f7ccdf67` | `00000004` | `00000026` | `00000026` | `00000026` |
| `918346d6` | `00000015` | `00000237` | `00000237` | `00000237` |

All 12 guard records report period `16000`, reason `4` (the local guard's excessive pending-tick count), and capture ELF prefix `290db2c0`. The runtime formatter reports valid retained checksums. The raw first-fault/probe words for the final row are also available and were independently checksum-validated with the linked decoders.

Detailed final retained event (`918346d6`):

```text
Core 0 last good UNIT1:  0x000000157377ac9f
Core 1 last good UNIT1:  0x000000157377cc09
Both cores first bad:   0x000002377377cf10
Core 0 confirm:         0x000002377377d5ea
Core 1 confirm:         0x000002377377d594
Core 0 UNIT0 reference: 0x00000015737dcb7c
Core 1 UNIT0 reference: 0x00000015737dcb23
```

For core 0, the apparent jump is `146565.7595270625` seconds at 16 MHz, while the low word advances only `0x2271` counts. The high word has additional bits `0x222` set. Both cores' summaries report `HI_AND == HI_OR == 0x237`, 32 accepted samples, no rejected/unstable samples, and no trace-budget expiry. The complete probes took 19,954 and 20,125 CPU cycles, approximately 83–84 microseconds at the configured 240 MHz. UNIT0 was valid, with no read timeout or instability reported. The recorded SYSTIMER/TIMG DATE values matched their startup baselines.

Both cores recorded one recovery attempt, zero successful recoveries and one rejection. These are fault-time retained counters, separate from counters reset at the next boot.

## Stored crash: consequence rather than initiating fault

The latest stored dump is 24,420 bytes and its CRC32 validates. Its recorded ELF prefix `290db2c0d` matches the privately preserved ELF's SHA-256 prefix. Address resolution against that ELF gives:

```text
0x40376d78  SysTickIsrHandler, zonline_systick_port.c:273
0x40377475  _xt_lowint1, xtensa_vectors.S:1240
0x42265d2c  pm_wake_up
```

Line 273 is the diagnostic port's intentional fatal wait after arming reset mechanisms. The dump reports the interrupted task as `wifi` and marks the backtrace corrupt. Neither fact establishes Wi-Fi as the root cause or independently proves memory corruption.

The retained first-fault/probe pair is associated by checksum, but the exporter explicitly marks its association with the **latest stored core dump** as `unverified`. An earlier unacknowledged first fault can survive subsequent resets. We therefore do not combine these into a claimed single-event timeline.

## Reproduction status and limitations

- The full application fails intermittently; there is no deterministic short reproducer yet. Run the pinned diagnostic build with its normal workload until a failure, then export crash diagnostics and the retained fault records. That is an observation procedure, not an isolated upstream reproducer.
- No controlled peripheral-by-peripheral or second-module comparison has yet established a trigger for this specific high-word signature.
- A successful LO–HI–LO check is not proof that the whole cross-core snapshot transaction is race-free. Both cores access a shared peripheral latch.
- The UNIT0 and UNIT1 snapshots are not simultaneous. A plausible UNIT0 reference and unchanged DATE registers do not prove the entire peripheral/bus is healthy.
- The 716 capsule layout reuses fields historically named `unit1_load_*`; it does **not** preserve the actual UNIT1 LOAD registers. An unintended counter-write path has not been excluded.
- The exact signature has not yet been demonstrated without our diagnostic overrides. We have not ruled out an override bug, another software/MMIO write, driver interaction or an electrical/board issue.

## Questions for Espressif

1. Are concurrent UPDATE/VALID/value-read transactions on the same SYSTIMER unit supported on ESP32-S3 without cross-core serialization? Is there a recommended locking/order pattern for SysTick ISR and fault-time readers?
2. Is there a known S3 issue or SDK change relevant to persistent, in-range high-word bit changes of this form?
3. Which additional registers or write-path instrumentation would best distinguish a stale/shared snapshot from an actual counter change or unintended LOAD operation?
4. Which minimal S3 test would you recommend first? We are considering a separate bounded-serialization experiment and a stock-IDF timer-only baseline, followed by adding peripheral activity one component at a time. These tests are proposed, not completed.

Related reports: [esp-idf #19036](https://github.com/espressif/esp-idf/issues/19036) concerns C6 UNIT0 backward jumps; [esp-idf #19046](https://github.com/espressif/esp-idf/issues/19046) concerns C6 UNIT1 VALID stalls. They motivated this comparison, but this report concerns **S3 UNIT1 forward jumps with successful VALID reads**, and we do not assume a common cause.

The original ELF, MAP and core dump have been preserved. They are not attached to this text. The appendices and evidence files contain selected timer diagnostics only; the application's GPS/network/vehicle logs and raw RAM dump are not included.

## Appendix A: source artifact identities

These identify the privately retained files; no full runtime ELF hash is claimed from the nine-character logged prefix.

```text
290db2c0d507dcf4306111ca45eb288628af6a639f581e783bb93b73a858f13b  ZOnlinE_v2.6.716_PlatformIO_zoe_can_lte.ino.elf
110a5dacc2decf6ec54e9f81b5da1f6a9c26d4a9c83a723cf48b2d803e9e4f45  ZOnlinE_v2.6.716_PlatformIO_zoe_can_lte.ino.map
392bfaf00e7f778d43a8e37959bcdea4614c9c12bd78901f5febbfd99e93924f  ZOnlinE_stored_crash_dump(5).bin
```

## Appendix B: selected fault records

These are the application formatter's diagnostic lines, with wall-clock/reporting-boot wrappers removed. `extra23` is the capture-boot token for these kind-4 records; `extra24` is the capture ELF prefix.

<details>
<summary>Expand all 12 guard/probe records</summary>

```text
FIRST FAULT 708: core=0 kind=systick_guard id=7cb98db6 checksum=ok rtc=6693019141 ccount=1823239516; held first event may predate the last boot.
SYSTICK GUARD 708: core=0 reason=4 period=16000 handled=1492176 pending=2370318631 counter=00002283af0cf1ed op_before=20000000 op_after=20000000 polls=15 retries=0 conf=ffc00000 raw=00000004 st=00000004 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=0 last=000000058f0cc81f last_cycles=1823074875 last_handled=1492176 confirm=00002283af0cf888 meta=c0000000 extra24=290db2c0 extra23=24291a8a (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=0 id=4cf5b5e3 fault=7cb98db6 match=1 samples=32 hi_and=00002082 hi_or=0000228b attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

FIRST FAULT 708: core=1 kind=systick_guard id=cbe9e535 checksum=ok rtc=6693019141 ccount=1703336715; held first event may predate the last boot.
SYSTICK GUARD 708: core=1 reason=4 period=16000 handled=1492176 pending=2370318631 counter=00002283af0cf1ed op_before=20000000 op_after=20000000 polls=10 retries=0 conf=ffc00000 raw=00000004 st=00000004 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=1 last=000000058f0ce778 last_cycles=1703292368 last_handled=1492176 confirm=00002283af0cf834 meta=c0010000 extra24=290db2c0 extra23=24291a8a (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=1 id=f1d37777 fault=cbe9e535 match=1 samples=32 hi_and=00002082 hi_or=0000228b attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

FIRST FAULT 708: core=0 kind=systick_guard id=9548fbc1 checksum=ok rtc=165582983 ccount=1470947350; held first event may predate the last boot.
SYSTICK GUARD 708: core=0 reason=4 period=16000 handled=1222306 pending=137438954 counter=000002048daec847 op_before=20000000 op_after=20000000 polls=13 retries=1 conf=ffc00000 raw=00000000 st=00000000 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=0 last=000000048dae8d32 last_cycles=1470716841 last_handled=1222306 confirm=000002048daece87 meta=c0010000 extra24=290db2c0 extra23=01708d9f (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=0 id=939b6e45 fault=9548fbc1 match=1 samples=32 hi_and=00000204 hi_or=00000204 attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

FIRST FAULT 708: core=1 kind=systick_guard id=2a8481df checksum=ok rtc=165582983 ccount=1302263432; held first event may predate the last boot.
SYSTICK GUARD 708: core=1 reason=4 period=16000 handled=1222306 pending=137438954 counter=000002048daec847 op_before=20000000 op_after=20000000 polls=6 retries=0 conf=ffc00000 raw=00000000 st=00000000 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=1 last=000000048daeadfd last_cycles=1302158500 last_handled=1222306 confirm=000002048daecedc meta=c0000000 extra24=290db2c0 extra23=01708d9f (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=1 id=358d25c4 fault=2a8481df match=1 samples=32 hi_and=00000204 hi_or=00000204 attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

FIRST FAULT 708: core=0 kind=systick_guard id=e5f11b1a checksum=ok rtc=370683833 ccount=2983071882; held first event may predate the last boot.
SYSTICK GUARD 708: core=0 reason=4 period=16000 handled=1514673 pending=825170592 counter=00020007a4815222 op_before=20000000 op_after=20000000 polls=12 retries=0 conf=ffc00000 raw=00000000 st=00000000 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=0 last=00000005a481369f last_cycles=2982962447 last_handled=1514673 confirm=00020007a48158b5 meta=c0000000 extra24=290db2c0 extra23=4d1ff901 (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=0 id=ff4c2c88 fault=e5f11b1a match=1 samples=32 hi_and=00020007 hi_or=00020007 attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

FIRST FAULT 708: core=1 kind=systick_guard id=419e0d1a checksum=ok rtc=370683833 ccount=2868021132; held first event may predate the last boot.
SYSTICK GUARD 708: core=1 reason=4 period=16000 handled=1514672 pending=825170593 counter=00020007a4815222 op_before=20000000 op_after=20000000 polls=9 retries=0 conf=ffc00000 raw=00000000 st=00000000 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=1 last=00000005a4811778 last_cycles=2867792462 last_handled=1514672 confirm=00020007a4815860 meta=c0010000 extra24=290db2c0 extra23=4d1ff901 (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=1 id=b4054eaa fault=419e0d1a match=1 samples=32 hi_and=00020007 hi_or=00020007 attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

FIRST FAULT 708: core=0 kind=systick_guard id=92e03038 checksum=ok rtc=2445236303 ccount=1756262147; held first event may predate the last boot.
SYSTICK GUARD 708: core=0 reason=4 period=16000 handled=15432682 pending=962072675 counter=000202397dc0d760 op_before=20000000 op_after=20000000 polls=10 retries=0 conf=ffc00000 raw=00000002 st=00000002 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=0 last=000000397dc0a140 last_cycles=1756051906 last_handled=15432682 confirm=000202397dc0db49 meta=c0010000 extra24=290db2c0 extra23=88aee6a3 (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=0 id=da1a2f0e fault=92e03038 match=1 samples=32 hi_and=00020239 hi_or=00020239 attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

FIRST FAULT 708: core=1 kind=systick_guard id=c2f1ba10 checksum=ok rtc=2445236333 ccount=1641261453; held first event may predate the last boot.
SYSTICK GUARD 708: core=1 reason=4 period=16000 handled=15432682 pending=962072675 counter=000202397dc0e553 op_before=20000000 op_after=20000000 polls=9 retries=0 conf=ffc00000 raw=00000000 st=00000000 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=1 last=000000397dc0c078 last_cycles=1641116152 last_handled=15432682 confirm=000202397dc0e932 meta=c0010000 extra24=290db2c0 extra23=88aee6a3 (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=1 id=0ba89968 fault=c2f1ba10 match=1 samples=32 hi_and=00020239 hi_or=00020239 attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

FIRST FAULT 708: core=0 kind=systick_guard id=9210b5b3 checksum=ok rtc=2651754631 ccount=1596076758; held first event may predate the last boot.
SYSTICK GUARD 708: core=0 reason=4 period=16000 handled=1222824 pending=9126805 counter=000000268e2d0d01 op_before=20000000 op_after=20000000 polls=10 retries=1 conf=ffc00000 raw=00000000 st=00000000 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=0 last=000000048e2d041f last_cycles=1596039081 last_handled=1222824 confirm=000000268e2d12a4 meta=c0000000 extra24=290db2c0 extra23=f7ccdf67 (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=0 id=fc43d5f9 fault=9210b5b3 match=1 samples=32 hi_and=00000026 hi_or=00000026 attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

FIRST FAULT 708: core=1 kind=systick_guard id=86428bbf checksum=ok rtc=2651754634 ccount=1481029372; held first event may predate the last boot.
SYSTICK GUARD 708: core=1 reason=4 period=16000 handled=1222823 pending=9126806 counter=000000268e2d0db4 op_before=20000000 op_after=20000000 polls=2 retries=1 conf=ffc00000 raw=00000000 st=00000000 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=1 last=000000048e2ce4f7 last_cycles=1480867449 last_handled=1222823 confirm=000000268e2d1476 meta=c0000000 extra24=290db2c0 extra23=f7ccdf67 (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=1 id=e2ad9c55 fault=86428bbf match=1 samples=32 hi_and=00000026 hi_or=00000026 attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

FIRST FAULT 708: core=0 kind=systick_guard id=b82c1016 checksum=ok rtc=3429946770 ccount=3465347417; held first event may predate the last boot.
SYSTICK GUARD 708: core=0 reason=4 period=16000 handled=5758221 pending=146565759 counter=000002377377cf10 op_before=20000000 op_after=20000000 polls=14 retries=0 conf=ffc00000 raw=00000000 st=00000000 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=0 last=000000157377ac9f last_cycles=3465210580 last_handled=5758221 confirm=000002377377d5ea meta=c0000000 extra24=290db2c0 extra23=918346d6 (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=0 id=4e0d1b5b fault=b82c1016 match=1 samples=32 hi_and=00000237 hi_or=00000237 attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

FIRST FAULT 708: core=1 kind=systick_guard id=2a7e7c9e checksum=ok rtc=3429946770 ccount=3349784945; held first event may predate the last boot.
SYSTICK GUARD 708: core=1 reason=4 period=16000 handled=5758221 pending=146565759 counter=000002377377cf10 op_before=20000000 op_after=20000000 polls=9 retries=0 conf=ffc00000 raw=00000000 st=00000000 ena=00000007 target0=c0003e80 target1=c0003e80.
SYSTICK DETAIL: core=1 last=000000157377cc09 last_cycles=3349769242 last_handled=5758221 confirm=000002377377d594 meta=c0010000 extra24=290db2c0 extra23=918346d6 (kind4: ELF prefix/boot; kind2: load HI/LO).
SYSTIMER TRACE 716: core=1 id=5c69ccec fault=2a7e7c9e match=1 samples=32 hi_and=00000237 hi_or=00000237 attempts=1 recovered=0 rejected=1; full capsule available in crash JSON.

```

</details>

## Appendix C: raw retained words for capture boot 918346d6

The following is a deliberately selected subset of the diagnostic JSON. Save the code block as `timer_evidence.json` and use the two pinned decoder scripts linked above. It contains no original application log or raw RAM dump.

<details>
<summary>Expand decoder-compatible JSON</summary>

```json
{
  "firmware": "v2.6.716-test1 (20261029)",
  "running_elf_sha256": "290db2c0d",
  "captured_elf_sha256": "290db2c0d",
  "first_fault_format": 708,
  "first_fault_words": [
    [
      1514538760,
      3089895446,
      4,
      0,
      3429946770,
      0,
      3465347417,
      4,
      16000,
      5758221,
      146565759,
      1937231632,
      567,
      536870912,
      536870912,
      14,
      0,
      4290772992,
      0,
      0,
      7,
      3221241472,
      3221241472,
      1937222815,
      21,
      3465210580,
      5758221,
      1937233386,
      567,
      3221225472,
      2441299670,
      688763584
    ],
    [
      1514538760,
      712932510,
      4,
      1,
      3429946770,
      0,
      3349784945,
      4,
      16000,
      5758221,
      146565759,
      1937231632,
      567,
      536870912,
      536870912,
      9,
      0,
      4290772992,
      0,
      0,
      7,
      3221241472,
      3221241472,
      1937230857,
      21,
      3349769242,
      5758221,
      1937233300,
      567,
      3221291008,
      2441299670,
      688763584
    ]
  ],
  "systimer_probe_words": [
    [
      1515409458,
      1309481819,
      132886,
      0,
      1937231632,
      567,
      3089895446,
      567,
      1,
      33628753,
      33566833,
      33566833,
      1937623932,
      21,
      32800,
      567,
      567,
      1937233386,
      567,
      3221225472,
      19954,
      567,
      1937234444,
      65767
    ],
    [
      1515409458,
      1550437612,
      132886,
      1,
      1937231632,
      567,
      712932510,
      567,
      1,
      33628753,
      33566833,
      33566833,
      1937623843,
      21,
      32800,
      567,
      567,
      1937233300,
      567,
      3221291008,
      20125,
      567,
      1937234390,
      65767
    ]
  ],
  "fault_identity": [
    {
      "source": "rtc_boot_copy",
      "capture_elf_prefix": "290db2c0",
      "capture_boot_token": 2441299670,
      "probe_matches_fault": true,
      "latest_dump_association": "unverified"
    },
    {
      "source": "rtc_boot_copy",
      "capture_elf_prefix": "290db2c0",
      "capture_boot_token": 2441299670,
      "probe_matches_fault": true,
      "latest_dump_association": "unverified"
    }
  ]
}
```

</details>
