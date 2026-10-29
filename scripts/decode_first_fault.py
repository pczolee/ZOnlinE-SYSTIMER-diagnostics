#!/usr/bin/env python3
"""Decode verified ZFF708 capsules from Pi .raw/.log files. No ELF required."""
import argparse
import json
import re
from pathlib import Path

LINE = re.compile(rb'ZFF708:([01]):(\d{2}):([0-9a-fA-F]{8}):([0-9a-fA-F]{8}):([0-9a-fA-F]{8})')


def checksum(words):
    result = 2166136261
    for word in words[2:]:
        result = ((result ^ word) * 16777619) & 0xffffffff
    return result


def decode(words):
    if (len(words) != 32 or words[0] not in (0x5a460708, 0x5a46e708)
            or words[1] != checksum(words) or words[3] > 1 or words[2] not in (1, 2, 3, 4, 5)):
        raise ValueError('Invalid capsule checksum/header')
    d = words[7:]
    result = dict(core=words[3], kind={1:'panic',2:'systick_guard',3:'systimer_hal',4:'systick_guard',5:'panic'}[words[2]],
                  checksum=f'{words[1]:08x}', rtc_ticks=(words[5] << 32) | words[4],
                  ccount=words[6], raw_words=[f'{w:08x}' for w in words])
    if words[2] in (1,5):
        result.update(pc=f'{d[0]:08x}', sp=f'{d[3]:08x}', exccause=d[19], excvaddr=f'{d[20]:08x}')
    elif words[2] == 3:
        result.update(reason=d[0], counter_unit=d[1], counter=(d[3]<<32)|d[2],
                      polls=d[4], retries=d[5], operation=d[6], config=d[7])
    else:
        result.update(reason=d[0], period=d[1], handled=d[2], pending=d[3],
                      counter=(d[5] << 32) | d[4], last_good=(d[17] << 32) | d[16],
                      last_good_ccount=d[18], last_good_handled=d[19],
                      confirm=(d[21] << 32) | d[20], confirm_reason=d[22] & 255,
                      confirm_retries=(d[22] >> 8) & 255, confirm_polls=(d[22] >> 16) & 8191,
                      detail_complete=bool(d[22] & 0x40000000), initialized=bool(d[22] & 0x80000000), load=(d[24] << 32) | d[23])
    if words[2] in (3,4,5):
        result['capture_elf_prefix']=f'{d[24]:08x}' if d[24] else None
        result['capture_boot_token']=d[23] if words[2] in (3,4) and d[23] else None
        result.pop('load',None)
    return result


def collect(paths):
    groups, emitted = {}, set()
    for path in paths:
        if Path(path).suffix.lower()=='.json':
            metadata=json.loads(Path(path).read_text())
            if metadata.get('first_fault_format')==708:
                for words in metadata.get('first_fault_words',[]):
                    if words is None: continue
                    try: result=decode(words)
                    except (ValueError,TypeError): continue
                    key=(result['core'],words[1])
                    if key not in emitted:
                        emitted.add(key)
                        yield dict(file=Path(path).name,**result)
            continue
        with Path(path).open('rb') as source:
            for line in source:
                for match in LINE.finditer(line):
                    core, row = int(match[1]), int(match[2])
                    if row >= 16:
                        continue
                    key = (core, int(match[3], 16))
                    words = groups.setdefault(key, [None] * 32)
                    words[2*row:2*row+2] = [int(match[4],16), int(match[5],16)]
                    if None in words or key in emitted:
                        continue
                    try:
                        result = decode(words)
                    except ValueError:
                        continue
                    if words[1] != key[1] or result['core'] != core:
                        continue
                    emitted.add(key)
                    yield dict(file=Path(path).name, **result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+', help='Pi .raw/.log in capture order, or 708 crash diagnostics .json')
    args = parser.parse_args()
    results = list(collect(args.files))
    print(json.dumps({'verified_capsules': len(results), 'faults': results}, indent=2))

if __name__ == '__main__':
    main()
