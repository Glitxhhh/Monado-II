# Source releases

Monado II release assets are separate Linux `.tar.gz` and Windows `.zip`
**source/setup kits**. They contain reviewed authored/licensed source and tooling,
not playable game binaries. Each player generates game code locally from their
own legally dumped input. No game content, generated code, translated shaders,
caches, seeds, keys, firmware, saves, mods or private SDK binaries are distributed.

Version 0.1.2 is a regular release, with GitHub's prerelease option disabled.
This channel change does not establish Windows game-build or full-game coverage.
The exact reviewed generated launcher illustration is included with its
[provenance](../Monado_Linux/tools/launcher/assets/ARTWORK_PROVENANCE.md) and root
notice; other unreviewed imagery remains excluded.

The Linux kit includes `Build_Monado_II.sh` and `Play_Monado_II.sh`. Users create
their launcher AppImage locally beside their own input/build workspace; neither
that image nor its receipt/staging files is a source release asset. Use a fresh
reviewed export for release checks, not an end-user folder containing input or
generated builds. Never remove a user's input/state to make a release guard pass.

From a reviewed clean public checkout:

```sh
# The strict packager requires canonical paths, without '..' or symlink aliases.
MONADO_RELEASE_SOURCE="$(pwd -P)"
MONADO_RELEASE_PARENT="$(dirname "$MONADO_RELEASE_SOURCE")"
python -B tools/publication_guard.py "$MONADO_RELEASE_SOURCE"
python -B tools/package_source_release.py --source "$MONADO_RELEASE_SOURCE" --platform linux --version 0.1.2 --output "$MONADO_RELEASE_PARENT/monado-ii-linux-release"
python -B tools/package_source_release.py --source "$MONADO_RELEASE_SOURCE" --platform windows --version 0.1.2 --output "$MONADO_RELEASE_PARENT/monado-ii-windows-release"
```

Outputs must be fresh. The packager verifies the exact publication manifest and
source policy before staging, selects one port plus common source, creates a
platform-specific manifest/status note, and checks archive readback. Artifact
checksums accompany output. Do not upload the whole development workspace or
reuse its Git history. The separately prepared public tree has a reviewed fresh
history; development notes, unreviewed artwork and personal paths remain local.

The manual **Source release kits** GitHub Actions workflow produces both source
artifacts after checks; it does not publish a GitHub release, compile game code,
access secrets or fetch game input. Creating a public repository or publishing
release assets is a separate maintainer action. CI runs authored source-tooling
tests on Linux and Windows; it is not a Windows game-build or gameplay pass.

Never use the inherited `scripts/make_dist.sh`: it bundled game/user data and is
excluded from the public export. A future binary launcher/installer needs its own
dependency/asset/license review; a compiled game-derived executable remains outside
the allowed release contents. Hash/signature guards supplement, rather than
replace, provenance review. Any uncertain file stays out until resolved.
