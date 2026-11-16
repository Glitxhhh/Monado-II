# Rebuild and relink the Windows setup frontend

The native source companion contains the frontend objects, Windows resource,
static SDL3/iconv libraries and exact relative `relink.json` recipe. It does not
need to include a second copy of the original portable setup payload. Work in a
copy of the companion's `relink` directory, using Zig 0.14.1 and Python 3.

1. Rebuild the library you want to modify for compatible AMD64 Windows headers
   and replace its archive in `static-libs`. Keep the applicable library notices
   and source obligations. The companion contains the corresponding source.
2. From that `relink` directory, run the retained relative recipe with Zig:

   ```text
   python -c "import json,subprocess; r=json.load(open('relink.json')); subprocess.run(['zig',*r['argv']],check=True)"
   ```

   This creates the unbundled `builder-base.exe`. The retained application
   objects contain the original frontend, fonts and artwork; rebuilding the
   game or obtaining game input is unnecessary for this step.
3. Verify the original published portable setup against the release checksums.
   Keep that original file unchanged. Attach its exact payload to the new base:

   ```text
   python relink_windows_setup.py --original-setup "ABSOLUTE ORIGINAL SETUP PATH" --base-executable builder-base.exe --output "MonadoII-Windows-Setup-Modified.exe"
   ```

   Choose a new output filename. Original setup, rebuilt base and output must
   be distinct ordinary files; existing outputs and linked/reparse paths are
   rejected. Spaces and Unicode are supported. Use ordinary absolute paths or
   relative paths without parent (`..`) components.

The repacker streams the new base followed by the original payload bytes and a
new `M2BNDL01` footer containing the adjusted offset and unchanged payload SHA256.
It verifies the original footer bounds, payload header and payload hash, then
checks the copied bytes and output readback. It neither decompresses nor changes
the payload or its manifests, runs an executable, accesses the network, selects
an SDK, or touches an installation/profile. The embedded hash checks integrity;
verify the original release checksums separately for its publisher identity.

The resulting EXE uses the modified frontend library and the same original
portable setup payload. Normal first-run/reopen behavior remains available.
These relink inputs and this step impose no requirement to distribute personal
game files, game-derived code, caches, settings or saves. If repacking fails,
any exclusively created partial output is retained; retry with another fresh
filename after inspecting the error. File and ancestor identities are checked
before/after I/O; this portable Python tool does not provide native directory
leases against a hostile concurrent filesystem writer.
