#!/usr/bin/env bash
# Local equivalent of .github/workflows/ci.yml
set -e
cd "$(dirname "$0")/.."
python -X utf8 -W error::ResourceWarning -m unittest discover -s tests/unit -v
python -X utf8 tools/package_guard.py --git .
