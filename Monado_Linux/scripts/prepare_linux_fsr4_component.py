#!/usr/bin/env python3
"""Prepare only the source-built native FSR4 module; never download models."""
import argparse
import json
import re
import setup_linux_fsr4 as component


def prepare(identity):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}', identity):
        raise ValueError('invalid FSR4 component build identity')
    try:
        return component.verify_existing()
    except (OSError, ValueError, RuntimeError):
        pass
    component.build(component.ROOT / '.deps/fsr4/build' / identity)
    return component.verify_existing()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-id', required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(prepare(args.build_id), indent=2))
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, str(error) + '\n')
