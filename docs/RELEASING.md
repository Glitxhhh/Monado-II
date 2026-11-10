# Source releases

Monado II release assets are `monado-ii-<version>-linux-setup.tar.gz` and
`monado-ii-<version>-windows-setup.zip` **source/setup kits**. They contain reviewed authored/licensed source and tooling,
not playable game binaries. Each player generates game code locally from their
own legally dumped input. No game content, generated code, translated shaders,
caches, seeds, keys, firmware, saves, mods or private SDK binaries are distributed.

Version 0.1.12 is a regular release, with GitHub's prerelease option disabled.
A regular release does not establish Windows game-build or full-game coverage.
The exact reviewed generated launcher illustration is included with its
[provenance](../Monado_Linux/tools/launcher/assets/ARTWORK_PROVENANCE.md) and root
notice; other unreviewed imagery remains excluded.

The 0.1.12 current-only updater, held-L2/HUD/party controls and new-selection
DLAA / Native (100%) default are Linux-only. Explicit saved DLSS qualities are
preserved. Scaled DLSS shaking at 67% remains unresolved; Native (100%) is the
current workaround. Windows parity is approved but deferred until development
on Windows. The Windows implementation is unchanged. Quick Step remains stable
following manual acceptance and **off by default**; frame generation remains
experimental and off by default. Valak/Makna/pipe rendering reports remain open.
Do not imply full-game, other-hardware or actual Bazzite qualification.

After successful activation, the new launcher removes obsolete updater-owned
versions and downloads, including accumulated history from older clients. It
retains the active game payload and protects input, saves, hidden Quick Step
metadata, settings, mods and useful caches. Unknown, modified or busy trees are
deferred safely. Local rollback history is discontinued; manual downloads/builds
of older GitHub releases remain available. The first cleanup can delay opening:
an isolated long-history upgrade rehearsal took about 75 seconds for maintenance
and checks. State this in release notes, alongside the full rebuild and temporary
old-plus-new storage requirement. An old updater may place the first rebuilt game
in its version workspace, requiring one more full rebuild on the following update
to establish the independently reusable payload.

The native/generator/runtime changes require a full local game rebuild through
the Linux updater while retaining the original saves/settings profile and
read-only input. Do not package profiles or perform save conversion as part of
an update. Preserve optional Quick Step assignment metadata with its save folder.
The builder fetches the compatible media libraries privately; source kits and
launcher images do not distribute those runtime libraries or vendor SDK binaries.

The Linux kit includes `Build_Monado_II.sh` and `Play_Monado_II.sh`. Users create
their launcher AppImage locally beside their own input/build workspace; neither
that image nor its receipt/staging files is a source release asset. Use a fresh
reviewed export for release checks, not an end-user folder containing input or
generated builds. Never remove a user's input/state to make a release guard pass.

Version 0.1.3 introduces the Linux stable-release updater. The 0.1.2 launcher has
no update client, so users must manually apply/build 0.1.3 or newer once. The verified
source kit can be merged into the same original installation without deleting
its input/build/profile directories; the Linux guide gives the checksum and
clean-kit checks. A fresh kit in another folder remains a separate profile.
Later managed updates leave installed source directories untouched and
download reviewed source and rebuild locally, retaining a compatible game only
for conservatively classified launcher-only changes. No precompiled game or
vendor SDK is delivered by this updater, and it does not update Windows.
Release-owned source modifications stop automatic installation; user profiles
remain outside the source manifest. See the
[Linux update guide](BUILD_LINUX.md#4-update-and-roll-back).

Managed workspaces, downloads, generated outputs and logs under
`Monado_Linux/updates/` are runtime artifacts, excluded from tracked source and
release kits. Never package an end-user update directory. Version paths remain
fixed after building; activation changes a checked local pointer rather than
moving or replacing source, game input or saves. Release discovery accepts newer
stable versions and requires the official Linux source asset's size/SHA-256 plus
its exact source manifest and policy checks. Publish the matching version metadata
and Linux source-kit asset together; retaining a stable version tag alone does not
make an incompatible archive suitable for the updater.

Every Linux release must also include
`monado-ii-<version>-linux-source.tar.gz`, a **byte-identical compatibility alias**
of the Linux setup archive. Installed 0.1.3/0.1.4 clients require that filename and
the existing `monado-ii-<version>-linux-source/` internal directory. New clients
prefer the setup filename and retain the guarded source-name fallback. Keep both
Linux filenames in future stable releases so users can skip versions safely.
The names do not change the source-only policy or local rebuild requirements.
The packager emits SHA256SUMS entries for both identical Linux files; Windows
uses only its canonical setup name. Do not regenerate the compatibility archive
independently or alter its internal root.

From a reviewed clean public checkout:

```sh
# The strict packager requires canonical paths, without '..' or symlink aliases.
MONADO_RELEASE_SOURCE="$(pwd -P)"
MONADO_RELEASE_PARENT="$(dirname "$MONADO_RELEASE_SOURCE")"
python -B tools/publication_guard.py "$MONADO_RELEASE_SOURCE"
python -B tools/package_source_release.py --source "$MONADO_RELEASE_SOURCE" --platform linux --version 0.1.12 --output "$MONADO_RELEASE_PARENT/monado-ii-linux-release"
python -B tools/package_source_release.py --source "$MONADO_RELEASE_SOURCE" --platform windows --version 0.1.12 --output "$MONADO_RELEASE_PARENT/monado-ii-windows-release"
```

Outputs must be fresh. The packager verifies the exact publication manifest and
source policy before staging, selects one port plus common source, creates a
platform-specific manifest/status note, and checks archive readback. Artifact
checksums accompany output. Do not upload the whole development workspace or
reuse its Git history. The separately prepared public tree has a reviewed fresh
history; development notes, unreviewed artwork and personal paths remain local.

The manual **Source release kits** GitHub Actions workflow produces
`monado-ii-linux-setup` and `monado-ii-windows-setup` workflow artifacts after
checks; its read-only token does not publish a GitHub release, compile game code,
access secrets or fetch game input. The Linux workflow artifact contains both
Linux filenames and its checksum list. Download the artifacts from the exact
reviewed tag/commit. A workflow artifact alone is not available to the updater:
the maintainer must attach the verified archives to the matching GitHub Release.
CI runs authored source-tooling tests on Linux and Windows; it is not a Windows
game-build or gameplay pass.

For a normal release, verify both platform checksum lists, combine them into one
`SHA256SUMS`, and upload **all three archives plus that checksum file**. The Linux
setup and compatibility alias must have equal bytes and digests. For example,
from the parent of the two fresh local packaging output directories:

```sh
MONADO_RELEASE_VERSION=0.1.12
(cd monado-ii-linux-release && sha256sum -c SHA256SUMS)
(cd monado-ii-windows-release && sha256sum -c SHA256SUMS)
cmp "monado-ii-linux-release/monado-ii-${MONADO_RELEASE_VERSION}-linux-setup.tar.gz" "monado-ii-linux-release/monado-ii-${MONADO_RELEASE_VERSION}-linux-source.tar.gz"
cat monado-ii-linux-release/SHA256SUMS monado-ii-windows-release/SHA256SUMS > SHA256SUMS
# After reviewing release-notes.md and the existing reviewed Git tag:
gh release create "v${MONADO_RELEASE_VERSION}" --verify-tag --repo brendonbusker/Monado-II --title "Monado II ${MONADO_RELEASE_VERSION}" --notes-file release-notes.md \
  "monado-ii-linux-release/monado-ii-${MONADO_RELEASE_VERSION}-linux-setup.tar.gz" \
  "monado-ii-linux-release/monado-ii-${MONADO_RELEASE_VERSION}-linux-source.tar.gz" \
  "monado-ii-windows-release/monado-ii-${MONADO_RELEASE_VERSION}-windows-setup.zip" SHA256SUMS
```

The same four files may be attached through GitHub's release editor instead.
Leave the prerelease option disabled for the normal stable updater channel;
label frame generation experimental in the notes and keep its warning/default.
After publication verify the remote filenames, byte sizes and digests, including
the compatibility alias, then exercise both current and older updater clients
against that release. Never overwrite an existing published archive to repair it;
publish a new version with a matching source manifest and version metadata.

Never use the inherited `scripts/make_dist.sh`: it bundled game/user data and is
excluded from the public export. A future binary launcher/installer needs its own
dependency/asset/license review; a compiled game-derived executable remains outside
the allowed release contents. Hash/signature guards supplement, rather than
replace, provenance review. Any uncertain file stays out until resolved.
