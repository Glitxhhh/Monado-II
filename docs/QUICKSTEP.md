# Dedicated Quick Step slot — Linux 0.1.11

Enable **Dedicated Quick Step gem slot** in the launcher's **Mods** tab before
starting the game. The feature is **stable following the maintainer's manual
acceptance** and remains **off by default** for players who prefer the original
equipment setup. Broader character, equipment, Future Connected and save-recovery
coverage remains incomplete.

Open **Change Equipment** and highlight the boots gem area. A rounded native
mini-table slides out beyond the table's right border. Press Right to focus it,
then Confirm to open the game's normal gem menu, filtered to eligible owned Quick Step gems of
ranks I–VI. When boots have no socket, press Right from the boots item row. The
slot belongs to the character, including when no boots are equipped. Left or Back
returns from the extra slot to the ordinary equipment selection. Native list
navigation, icons, descriptions, sorting and favourites are retained. Keyboard
controls follow the launcher's configured bindings. The empty socket's native
outer underlay is green; its hexagon keeps its ordinary fill. Equipped gems use
their ordinary native colours and socket badge. Gem rows and character portraits
show the native **E** equipped indicator with the correct current-character or
other-character ownership state. Opening, confirming and cancelling the bonus
picker use native menu sounds. Leaving it restores the ordinary gem chooser,
including its normal filtering and controls. The small frame uses an original
native nine-part window loaded from your own game files at runtime; no game
texture or other game asset is included in the source distribution.

Use **Submenu** while the bonus slot is selected, then the native **Remove gem**
action to clear its association. An empty bonus slot remains accessible even
when the character has no boots, socketless drones, or unique armour. Remove is
disabled when there is no bonus assignment. Ordinary bulk removal and equipment/socket swaps do not operate on this character-owned bonus
slot. Those ordinary bulk/swap entries may still appear in the native submenu,
disabled. Existing equipment and gem slots remain available. Each bonus slot uses
one real owned Quick Step gem; a gem cannot serve two bonus slots or a bonus slot and
ordinary equipment at once. Gems already attached to ordinary gear or another
character's bonus slot must be removed there first. Favourites remain eligible.
Normal ranks, potency, stacking and caps apply. Equipping or consuming that gem
through ordinary inventory actions clears its bonus assignment. No item is
created, removed from the bag or refunded by this feature.

Assignments are separate for the main story and Future Connected, and are saved
with the specific save slot. Load Game and Continue restore both the assignment
and its actual movement bonus from that save's matching metadata, including when
the native loader uses a cached save. Native `.sav` formats and filenames remain
unchanged; no conversion is required. Optional metadata lives inside the same
private save mount in `.monado-quickstep`; back up the **complete save folder,
including hidden files**, to preserve assignments. Updates keep the original
profile in place and do not move this metadata to a new profile.
An older save without this metadata remains loadable with an empty bonus slot.
Turning the option off disables the bonus while keeping the actual gems.

The assigned bonus also appears in **See Gem Effects**, combining with an
ordinary Quick Step effect up to the normal cap. If eight other effects already
occupy the panel, a separate native ninth line is used with closer line spacing;
the original eight-record summary allocations and font size stay unchanged.
Optional native UI construction checks memory headroom first and records local
diagnostics if it cannot proceed. The check is not an atomic reservation against
other native allocations. Actual low-memory and concurrent-allocation behaviour
still needs gameplay testing.

For manual testing, check the right-side reveal and filled icon, newly crafted
rank II gems, socketless Fiora drones, and a character with eight other displayed
effects. Check the submenu both before and inside the gem list, including Cancel
and Remove. Check all character and equipment cases: no armour, no socket,
unique armour, an empty ordinary socket and an occupied ordinary socket.
Check character switching, boots changes/removal, assigning
and clearing gems, ordinary gem use, and movement after equipment previews or
battles. Save in a spare slot, restart and load it, then check Continue and both
stories. Also test with the launcher option off. Component and filesystem-fault
tests do not replace these gameplay checks.

The 0.1.11 fixes require a full local game rebuild. The normal Linux updater
performs it while retaining the existing input, saves and settings.
