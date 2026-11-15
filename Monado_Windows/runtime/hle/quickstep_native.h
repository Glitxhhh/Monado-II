// [XCDE] LINUX-040: separately owned native equipment gem control.
#pragma once
#include "quickstep_native_ui.h"

typedef enum {
    QUICKSTEP_NATIVE_OTHER, QUICKSTEP_NATIVE_RIGHT, QUICKSTEP_NATIVE_LEFT,
    QUICKSTEP_NATIVE_CONFIRM, QUICKSTEP_NATIVE_BACK,
} QuickstepNativeAction;

// Called after the original equipment UI has been constructed and published.
// Checked post-constructor ABI: X19 = Seq, X20 = UI.
void quickstep_native_construct(Cpu *c);
// Native equipment UI virtual update/destructor hooks use X0 = original UI.
void quickstep_native_tick(Cpu *c);
void quickstep_native_release(Cpu *c);
// Sequence pre-destructor: X0 = Seq, before native loader/resource destruction.
void quickstep_native_release_sequence(Cpu *c);
// Called only after native popups/list children have declined the event.
// A consumed event must follow the original sequence's handled epilogue.
bool quickstep_native_input(Cpu *c, uint64_t sequence, QuickstepNativeAction action);
// Mid-router ABI at 2F87AC: X19 = Seq, X20 = native event.
bool quickstep_native_input_event(Cpu *c);
// Mid-draw ABI75E868: X19=lower GPLA row. A true result skips only that
// private row's baked graphics, through native mask cleanup at75E99C.
bool quickstep_native_filter_background(Cpu *c);
