#!/usr/bin/env bash
# Unit checks plus an explicit complete reviewed publication export.
# Usage: scripts/ci.sh /absolute/path/to/reviewed/source
set -e
cd "$(dirname "$0")/.."
python -X utf8 -W error::ResourceWarning -m unittest discover -s tests/unit -v
python -X utf8 tools/package_guard.py "${1:?Provide the complete reviewed source export to check}"
