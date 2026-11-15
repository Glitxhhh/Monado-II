#!/usr/bin/env python3
"""Read-only FSR4 model selection bound to the original installation owner.

Prepared models survive runtime/workspace garbage collection. They are local
optional data, never source/archive/runtime-bundle resources. Verification does
not create directories, download SDKs or execute Proton.
"""
import importlib.util
from pathlib import Path
import sys


def storage(port):
    port = Path(port).resolve(strict=True)
    spec = importlib.util.spec_from_file_location('_fsr4_install_layout', port / 'scripts/install_layout.py')
    layout = importlib.util.module_from_spec(spec); spec.loader.exec_module(layout)
    return layout.installation_root(port) / 'Monado_Linux/.deps/fsr4'


def helper(port):
    port = Path(port).resolve(strict=True)
    sys.path.insert(0, str(port / 'scripts'))
    try:
        spec = importlib.util.spec_from_file_location('_fsr4_local_preparation', port / 'scripts/prepare_linux_fsr4.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        module.configure_storage(storage(port))
        return module
    finally:
        sys.path.pop(0)


def available(port, receipt):
    if not receipt.get('fsr4_enabled', False):
        return None, 'This build does not include the optional FSR4 component.'
    try:
        selected = helper(port).verify_assets()
        return selected, ''
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        # Absence/stale local optional models never prevents a normal game run.
        return None, 'Prepare FSR4 Lite models locally to enable this option. ' + str(error)[:300]
