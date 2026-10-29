#!/usr/bin/env python3
"""Decode CRC-valid ZSP711 and compact ZSP721 UNIT0/UNIT1 crash probes from Pi USB logs.

This is a fault-only hardware observation, not proof of a CPU/silicon defect.
Usage: python3 scripts/decode_systimer_probe.py ./ZOnlinE_USB*.log
"""
import argparse
import json
import re
from pathlib import Path

LINE = re.compile(rb'(ZSP711|ZSP721|ZSP715|ZSP716):([01]):(\d{2}):([0-9a-fA-F]{8}):([0-9a-fA-F]{8}):([0-9a-fA-F]{8})')
MAGIC = (0x5a535031, 0x5a535045)
COMPACT_MAGIC = (0x5a535032, 0x5a535046)
VERSION = 0x00020711
COMPACT_VERSION = 0x00020721
TRACE_VERSION = 0x00020715
TRACE716_VERSION = 0x00020716
NAMES = (
    'magic checksum version core first_lo first_hi u1_latched_pre_lo '
    'u1_latched_pre_hi u1_op_pre u1_conf_pre u0_latched_pre_lo '
    'u0_latched_pre_hi u0_op_pre u0_snapshot_lo u0_snapshot_hi '
    'u0_polls u0_retries u0_result u0_op_post u1_latched_post_lo '
    'u1_latched_post_hi u1_op_post confirm_lo confirm_hi confirm_meta '
    'ccount_enter ccount_exit rtc_lo rtc_hi unit1_load_hi unit1_load_lo flags'
).split()
assert len(NAMES) == 32
COMPACT_NAMES = (
    'magic checksum version core first_lo first_hi u1_latched_pre_lo '
    'u1_latched_pre_hi u1_op_pre u0_latched_pre_lo u0_latched_pre_hi '
    'u0_op_pre u0_snapshot_lo u0_snapshot_hi u0_polls u1_latched_post_lo '
    'u1_latched_post_hi confirm_lo confirm_hi confirm_meta ccount_duration '
    'unit1_load_hi unit1_load_lo flags'
).split()
assert len(COMPACT_NAMES) == 24


def checksum(words):
    h = 2166136261
    for word in words[2:]:
        h = ((h ^ word) * 16777619) & 0xffffffff
    return h


def decode(words):
    compact = len(words) == 24
    if (len(words) not in (24, 32) or
            words[0] not in (COMPACT_MAGIC if compact else MAGIC) or
            words[2] not in ((TRACE_VERSION, TRACE716_VERSION, COMPACT_VERSION) if compact else (VERSION,)) or
            words[3] > 1 or words[1] != checksum(words) or words[-1] & 3 != 3):
        raise ValueError('Invalid probe capsule (header/version/CRC/incomplete)')
    w = dict(zip(COMPACT_NAMES if compact else NAMES, words))
    val = lambda prefix: w[prefix + '_lo'] | (w[prefix + '_hi'] << 32)
    is_trace = compact and words[2] in (TRACE_VERSION, TRACE716_VERSION)
    fields = dict(core=w['core'], crc=f'{w["checksum"]:08x}',
                  unit1_first=val('first'),
                  unit1_pre_latched=val('u1_latched_pre'),
                  unit1_post_latched=val('u1_latched_post'),
                  unit1_confirm=val('confirm'),
                  unit0_pre_latched=val('u0_latched_pre'),
                  unit0_snapshot=val('u0_snapshot'),
                  unit0_valid=bool(w['flags'] & 4),
                  unit0_poll_timeout=bool(w['flags'] & 8),
                  unit0_unstable_read=bool(w['flags'] & 16),
                  unit0_attempted=bool(w['flags'] & 32),
                  unit0_polls=w['u0_polls'], unit0_retries=w.get('u0_retries'),
                  unit0_result=w.get('u0_result', 1 if w['flags'] & 8 else (2 if w['flags'] & 16 else 0)),
                  rtc_ticks=val('rtc') if not compact else None,
                  ccount_probe_cycles=w['ccount_duration'] if compact else (w['ccount_exit']-w['ccount_enter']) & 0xffffffff,
                  unit1_load=(w['unit1_load_hi'] << 32) | w['unit1_load_lo'],
                  unit1_op_pre=f'{w["u1_op_pre"]:08x}',
                  unit1_op_post=f'{w["u1_op_post"]:08x}' if not compact else None,
                  unit0_op_pre=f'{w["u0_op_pre"]:08x}',
                  unit0_op_post=f'{w["u0_op_post"]:08x}' if not compact else None,
                  unit1_conf_pre=f'{w["u1_conf_pre"]:08x}' if not compact else None,
                  unit1_confirm_meta=f'{w["confirm_meta"]:08x}',
                  raw_words=[f'{v:08x}' for v in words],
                  capsule_format=("ZSP715" if is_trace else "ZSP721") if compact else "ZSP711")
    if is_trace:
        meta=w['u0_polls']
        fields.update(
            samples_ok=meta&63,
            samples_unstable=(meta>>6)&63,
            trace_expired=bool(meta&(1<<16)),
            date_baseline_valid=bool(meta&(1<<15)),
            date_mismatch_mask=(meta>>12)&7,
            systimer_date=w['u0_latched_pre_lo'],
            timg0_date=w['u0_latched_pre_hi'],
            timg1_date=w['u0_op_pre'],
            sample_hi_and=w['u1_latched_post_lo'],
            sample_hi_or=w['u1_latched_post_hi'],
            sample_last=(w['unit1_load_hi']<<32)|w['unit1_load_lo'],
        )
        for k in ('unit0_pre_latched','unit0_polls','unit1_load','unit0_op_pre','unit1_post_latched'):
            fields.pop(k,None)
    if compact and words[2]==TRACE716_VERSION:
        fields['capsule_format']='ZSP716'
        fields['first_fault_checksum']=f'{w["u1_latched_pre_lo"]:08x}'
        fields['recovery_attempts_saturated']=w['u1_op_pre']&65535
        fields['recovery_recovered_saturated']=w['u1_op_pre']>>16
        fields['recovery_rejected_saturated']=w['flags']>>16
        fields['recovery_counters_valid']=bool(w['flags']&128)
        fields.pop('unit1_pre_latched',None)
        fields.pop('unit1_op_pre',None)
    # Do not claim UNIT0/UNIT1 were sampled simultaneously: esp_timer may
    # replace the shared UNIT0 snapshot at any point, even during the probe.
    if fields['unit0_valid']:
        fields['unit1_minus_unit0_ticks_approx'] = (
            fields['unit1_confirm'] - fields['unit0_snapshot'])
    return fields


def collect(paths):
    groups, emitted = {}, set()
    for name in paths:
        p = Path(name)
        if p.suffix.lower()=='.json':
            metadata=json.loads(p.read_text())
            for words in metadata.get('systimer_probe_words',[]):
                if words is None: continue
                try: result=decode(words)
                except (ValueError,TypeError): continue
                key=(result['capsule_format'],result['core'],words[1])
                if key not in emitted:
                    emitted.add(key);yield dict(file=p.name,**result)
            continue
        with p.open('rb') as f:
            for line in f:
                for m in LINE.finditer(line):
                    fmt = m[1].decode('ascii')
                    core, row, crc = int(m[2]), int(m[3]), int(m[4], 16)
                    total = 24 if fmt in ('ZSP721','ZSP715','ZSP716') else 32
                    if row >= total//2: continue
                    key = fmt, core, crc
                    words = groups.setdefault(key, [None] * total)
                    words[2 * row: 2 * row + 2] = [int(m[5], 16), int(m[6], 16)]
                    if None in words or key in emitted: continue
                    try:
                        result = decode(words)
                    except ValueError: continue
                    if words[1] != crc or result['core'] != core: continue
                    emitted.add(key)
                    yield dict(file=p.name, **result)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('files', nargs='+', help='raw/decoded USB log files, in capture order')
    args = ap.parse_args()
    records = list(collect(args.files))
    print(json.dumps({'verified_probes': len(records), 'probes': records}, indent=2))


if __name__ == '__main__':
    main()

