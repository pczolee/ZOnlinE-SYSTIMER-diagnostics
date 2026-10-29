# ZOnlinE SYSTIMER diagnostics

[Magyar leírás](README_HU.md)

Selected ESP32-S3 timer sources and retained measurements for [espressif/esp-idf#19333](https://github.com/espressif/esp-idf/issues/19333). The aim is to let reviewers inspect the diagnostic code and decode the published timer evidence without access to the private application repository.

**This is a review snapshot, not a buildable firmware, a minimal reproducer, or a demonstrated fix.** The observed high-word jumps were captured with custom SysTick/HAL overrides. The initiating cause remains unknown; these observations do not establish a silicon defect or a bug in unmodified ESP-IDF.

## Contents and provenance

- [REPORT.md](REPORT.md): measurements, limitations and questions for Espressif.
- Root `zonline_*.c` and `zonline_*.h`: SysTick/HAL overrides, counter reader, guard/recovery policies and retained fault/probe capture, including their local header dependencies.
- [scripts/decode_first_fault.py](scripts/decode_first_fault.py) and [scripts/decode_systimer_probe.py](scripts/decode_systimer_probe.py): the original offline decoders.
- [evidence/timer_evidence.json](evidence/timer_evidence.json): the already-published, selected diagnostic fields for capture boot `918346d6`.
- [evidence/selected_fault_records.txt](evidence/selected_fault_records.txt): 12 already-published formatter records from six capture boots, without reporting-boot wrappers.
- [SOURCE_MANIFEST.json](SOURCE_MANIFEST.json): original Git blob identities and SHA-256 hashes for every copied source file. [SHA256SUMS](SHA256SUMS) covers the published snapshot files other than itself.

The 11 source files are byte-for-byte copies from private repository `pczolee/Renault-Zoe-ZOnliNe`, commit `124b85c0cdfdc478a226368d3fc6f642ed6b7bfe`, firmware `v2.6.716-test1 (20261029)`. That original commit identifies provenance; it is not a commit in this separate repository. Original source paths and copyright/license notices are preserved. See [NOTICE.md](NOTICE.md).

The reported environment is Arduino-ESP32 3.3.7, runtime ESP-IDF `v5.5.2-729-g87912cd291`, ESP32-S3-WROOM-1-N16R8, 240 MHz CPU and a 16 MHz SYSTIMER with a 1 kHz tick. The silicon revision has not yet been recorded.

## Decode the retained evidence

From the repository root, using Python 3 (standard library only):

```sh
python3 scripts/decode_first_fault.py evidence/timer_evidence.json
python3 scripts/decode_systimer_probe.py evidence/timer_evidence.json
```

Expected results: `verified_capsules: 2` and `verified_probes: 2`, one per core. Both probes report 32 accepted samples, `sample_hi_and == sample_hi_or == 567` (`0x237`), and a checksum association with the corresponding first fault.

The retained first-fault/probe pair is **not verified as belonging to the latest stored core dump**. These fields deliberately retain `latest_dump_association: "unverified"`. The 716 layout reuses fields historically named `unit1_load_*`; it does not retain the actual UNIT1 LOAD registers. The decoder's historical `crc` label denotes its capsule checksum; do not confuse it with the stored core dump's CRC32.

The text log is a human-readable summary, not a raw `ZFF708`/`ZSP716` serial stream for these decoders. Raw capsule words are included only for the final event.

## Validation and limitations

Before publication, all 11 copied files were checked against their original Git blob hashes. The two decoders successfully verified both first faults and both probes from the selected JSON, and the probe-to-fault checksum links were checked. The selected records and JSON match the material already published in the issue.

This validates the packaged evidence and file provenance. No new hardware test or firmware build was performed for this publication. An unmodified current-IDF baseline, a minimal reproducer and a controlled cross-core serialization experiment remain outstanding. UNIT1 reads in this snapshot are not serialized across cores; that is a hypothesis to investigate, not a proven cause.

The repository omits the full application, SDK/build integration, credentials, application GPS/network/vehicle logs, ELF/MAP binaries and raw RAM core dump. SDK headers and application integration hooks are not supplied, so copying these files alone is insufficient to rebuild the original firmware.
