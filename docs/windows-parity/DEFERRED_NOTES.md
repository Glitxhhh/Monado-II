# Deferred notes after Windows completion

The maintainer accepts Linux dev103 + dev104 **R5 as-is** as the Windows parity
reference:1,219 source files, manifest
`582d67a37368a5917a6b271e827c025fda4c777e593545c6f90734e75f124b3a`.
Implement the complete applicable Windows feature set first. These notes preserve
later work; they do not authorize another Linux development pass, require cleanup
before Windows, or impose a performance gate on the first Windows implementation.
See the [implementation handoff](../WINDOWS_PORT_HANDOFF.md).

## Quality panel placement

After Windows is completed, move the extra quality controls into the existing
Quality section **immediately below Show FPS on screen**. **GPU selection stays
where it is.** This was a notes-only preference; it is not implemented in frozenR5.
Preserve all settings/defaults and the shared launcher/F1 contract when that later
change is authorized. Port the accepted layout first.

## Performance investigation retained from private record105

The maintainer reports approximately **5–10 FPS** cost from the overhaul, accepts
the overall fixes and wants to investigate recovery later. The comparison was
not a matched scene/camera/settings/cache benchmark; no exact percentage loss or
single cause is established. This accepted cost is **not a blocker for Windows
parity**, and there is no promise to recover it. A newly demonstrated native
Windows defect still needs ordinary correctness debugging.

Shader preparation succeeded before handoff and again manually. Some early hitches
compiled runtime variants, but later low-compilation intervals were still below
cap, so warmup alone does not establish the full reported steady cost. All new
quality settings were Original; that does not restore the old renderer. R5's
always-on legal feedback handling, world-space temporal history/motion/reactivity
and scaled-gather semantics remain enabled for correctness.

A bounded later performance pass should begin with counters and meaningful
production/GPU fixtures, then a maintainer-run matched A/B. Preserve all accepted
correctness behavior and profiles. Candidate investigations, not implemented fixes:

1. Measure snapshot cache hit/miss reasons, copied bytes/subresources, invalidation
   sources and misses per draw. Copies appearing where an older build sampled
   directly are a lead, not measured GPU cost or permission to use illegal feedback.
2. Avoid unrelated snapshot invalidation only where destination allocation is
   proven. Retain conservative global visibility boundaries for unknown effects.
   Naive per-write invalidation can break intended self-feedback snapshot reuse.
3. Investigate cross-draw reuse for texture objects aliasing the same backend/image,
   with exact lifetime/format/view/shape/allocation identity. Within-draw backend
   dedup already exists; pointer-keyed cross-draw lookup may duplicate work.
4. If prevalent, batch independent missed copies from one draw between shared
   barriers, retaining every mip/layer/aspect/layout dependency. Single-miss draws
   gain nothing; do not add complexity without counters.
5. Measure scaled-gather cost. The eligible scaled path uses four native gathers;
   unity scale has a one-gather fast path. A textureLod substitution has a preserved
   minLod counterexample and is not a proven equivalent optimization.

Required later fixtures include snapshotA→unrelatedB write→A reread with identical
pixels/no redundant copy; relevantA/alias writes, visibility/frame boundaries,
destroy/reallocation still invalidate; alias switches/two-target copies retain
all outputs/subresources. Keep the temporal mask used by DLSS. Broader native
feedback eligibility is a higher-risk follow-up, not the first experiment. Record
same-build noise and identical real-time scenarios before claiming gameFPS gains.

## Graphics limits and deferred evidence

- Colony9 railing-shadow flicker was confirmed by the maintainer on the older
  build too. It is **pre-existing**, not a proven dev103 regression. Preserve the
  symptom/evidence; do not disable shadows or claim the overhaul introduced it.
- Earlier Valak/Makna boxes, Makna sparkle flicker and blue pipe/plane reports
  remain unresolved. Fullscreen helped one symptom, not a general rendering fix.
  Component/GPU oracle passes do not establish that every game artifact is fixed.
- **Scaled DLSS67% whole-scene shaking remains unverified/unresolved.** Selecting
  DLSS defaults to **DLAA / Native100%**, retaining explicit saved quality.
  World-space history/motion/reactivity fixes are real R5 changes but are not
  manual proof that67% shaking is repaired. Native100% remains the workaround.
- Actual ultrawide issue12, all cutscene/effect/marker/resize paths, all devices,
  full DE/Future Connected and long sessions need explicit manual evidence.
- RebornPR72's newer own-pixel scaled-read default is **not part of acceptedR5**.
  It is a later reference candidate, not a required addition/optimization before
  Windows. Do not silently broaden the frozen parity target.
- No true Auto/3×/arbitrary-phase MFG or prerecorded-video interpolation is
  implemented. The prior variable-phase prototype failed its visual quality gate.
  NVIDIA generated-only identity/physical scanout is not inferred from counters.

Do not alter the accepted Linux reference or human profiles to prepare Windows.
The Windows implementation should retain these limits honestly, establish native
behavior and finish its feature/build/manual-validation milestones first.
