#!/usr/bin/env python3
"""Host-selected, cancellable optional FSR4 model preparation worker.

Invoked by the trusted launcher supervisor only after an explicit button click.
No IPC field supplies a command, source path, environment value or Proton path.
"""
import json
import os
from pathlib import Path
import re
import signal
import sys
import uuid

import fsr4_assets_linux as assets
from prepare_linux_fsr4 import user_home

ROOT = Path(__file__).resolve().parents[1]


def find_proton(home=None):
    home = Path(home if home is not None else user_home())
    roots = [home / '.local/share/Steam', home / '.steam/steam', home / '.steam/root',
             home / '.var/app/com.valvesoftware.Steam/.local/share/Steam']
    libraries = set()
    for root in roots:
        if not root.is_dir():
            continue
        root = root.resolve(); libraries.add(root)
        path = root / 'steamapps/libraryfolders.vdf'
        if path.is_file() and path.stat().st_size <= 131072:
            for raw in re.findall(r'"path"\s*"([^"\r\n]{1,4095})"', path.read_text(errors='replace'))[:64]:
                candidate = Path(raw.replace('\\\\', '\\'))
                if candidate.is_absolute() and candidate.is_dir():
                    libraries.add(candidate.resolve())
    found = set()
    for root in sorted(libraries):
        for directory in (root / 'steamapps/common', root / 'compatibilitytools.d'):
            if not directory.is_dir():
                continue
            entries = list(directory.iterdir())
            if len(entries) > 512:
                continue
            for entry in entries:
                if not entry.name.lower().startswith(('proton', 'ge-proton')):
                    continue
                script = entry / 'proton'
                if (script.is_file() and script.stat().st_size <= 2_000_000
                        and (entry / 'files/share/default_pfx').is_dir()
                        and (entry / 'files/bin/wineserver').is_file()):
                    found.add(script.resolve())
    if not found:
        raise ValueError('Install a complete Proton version in Steam (for example Proton Experimental), then retry from the desktop. The game remains native Linux; Proton is used only to prepare optional FSR4 models.')
    return sorted(found, key=lambda p: ('experimental' in p.parent.name.lower(), p.stat().st_mtime_ns, str(p)), reverse=True)[0]


def main():
    def cancelled(_signum, _frame):
        raise InterruptedError('FSR4 model preparation cancelled')
    signal.signal(signal.SIGTERM, cancelled)
    signal.signal(signal.SIGINT, cancelled)
    module = assets.helper(ROOT)
    proton = find_proton()
    if not os.environ.get('DISPLAY'):
        raise ValueError('Run preparation from a desktop session with X11/XWayland available. No game window will be opened.')
    identity = 'launcher-' + uuid.uuid4().hex
    module.BASE.mkdir(parents=True, exist_ok=True)
    module.progress('tools', 0, 0, 'Checking optional local FSR4 preparation tools')
    try:
        module.tools.verify_existing()
    except (OSError, ValueError, RuntimeError):
        module.progress('tools', 0, 0, 'Downloading verified source and building optional FSR4 preparation tools')
        module.tools.build(module.BASE / 'tools' / identity, jobs=4)
    module.progress('download', 0, 0, 'Checking the pinned official AMD SDK for local model preparation')
    module.prepare(module.BASE / 'assets' / identity, proton, display=os.environ['DISPLAY'])
    selected = module.verify_assets()
    print(json.dumps({'status': 'passed', 'asset_root': selected['asset_root']}), flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (KeyboardInterrupt, InterruptedError) as error:
        print(str(error) or 'FSR4 model preparation cancelled', file=sys.stderr, flush=True)
        sys.exit(130)
    except (OSError, ValueError, RuntimeError) as error:
        print(str(error), file=sys.stderr, flush=True)
        sys.exit(1)
