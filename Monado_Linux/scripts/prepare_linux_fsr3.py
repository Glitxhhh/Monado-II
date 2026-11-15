#!/usr/bin/env python3
"""Prepare the hash-locked native FSR3 provider, reusing only verified builds.

No host installation or vendor DLL execution. A changed recipe is rebuilt in a
fresh owned directory; modified cached payloads are never accepted for reuse.
"""
import argparse
import json
from pathlib import Path
import re
import sys

import setup_linux_fsr3 as fsr3


def prepare(identity, jobs):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}', identity):
        raise ValueError('invalid native dependency build identity')
    try:
        return fsr3.verify_existing()
    except (OSError, ValueError, RuntimeError):
        # The previous output remains intact. Building a new checked payload is
        # safe even if an old recipe or an incomplete/corrupt cache is present.
        pass
    fsr3.common.fetch(json.loads(fsr3.common.LOCK.read_text()))
    fsr3.build(fsr3.ROOT / '.deps/fsr3/build' / identity, jobs)
    return fsr3.verify_existing()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-id', required=True)
    ap.add_argument('--jobs', type=int, choices=range(1, 9), default=4)
    args = ap.parse_args()
    try:
        print(json.dumps(prepare(args.build_id, args.jobs), indent=2))
    except (OSError, ValueError, RuntimeError) as error:
        ap.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
