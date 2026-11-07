# Contributing to Monado II

Keep all inherited copyright and license notices. Credit upstream work and record
portable fixes and platform-specific behavior accurately. The existing per-port
`CONTRIBUTING.md` contracts apply to the authored runtime/generator.

Never submit game files, keys, firmware, Nintendo fonts, extracted assets,
generated game C, compiled game executables, translated game shaders/caches,
pipeline seeds from gameplay, screenshots/videos showing game assets, saves or
third-party mods. Do not submit download links or game-acquisition instructions.
Use authored synthetic fixtures for tests. Do not attach full logs/reports without
reviewing them for private or game-derived content.

Before submission, review every changed file and run:

```sh
python -B tools/publication_guard.py .
python -B -m unittest discover -s tests -p 'test_*.py' -v
```

The publication manifest binds reviewed file hashes. Update it only after reviewing
the provenance and license of each change; a hash scanner is not a rights audit.
Do not weaken source/package guards to admit unknown data. Keep game input and
generated/build/runtime state outside tracked source and release archives.

Use separate Linux/Windows outputs and disposable test profiles. Gameplay tests
and actual Windows builds must be reported as such; source inspection and synthetic
tests are not equivalent evidence. Frame-generation performance fixes and Windows
parity work are tracked in [TODO](TODO.md).
