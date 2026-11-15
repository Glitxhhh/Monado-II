# 0013: Optional native FSR 4 Lite on Linux

Status: implementation and component validation in progress. No gameplay claim.

The optional component uses the GPL-2.0-or-later bbport Vulkan replay backend,
reviewed through Monado Reborn commit `3cc9eeb46dc879fe36929d13758754c429b8377b`.
Upstream bbport commit is `0fc2921c45f8ff3900f6bfa43a299e9a4f532af4`.
Copyright, GPL text, upstream authorship and the complete AMD SDK license remain
beside the component. Existing MIT notices remain intact. This is an intentional
GPL optional component, not a claim that dynamically linking avoids copyleft.
Distributing the combined program with this component requires the applicable
GPL distribution terms and corresponding source for the combined work. Include
the matching source and build scripts; never distribute it as an opaque DLL.

The model is prepared locally from the hash-pinned official AMD FidelityFX 2.4
SDK DLL, using a private Proton prefix and authored synthetic frames. Models,
the vendor DLL, translated SPIR-V and preparation captures are local outputs,
not tracked source or assets in the normal release bundle. AMD terms apply to
the user's SDK/model preparation. This process never reads game files.

The runtime is native Vulkan. Proton is used only by optional model preparation.
Only INT8 Lite is selected. No environment variable may select another model.
The half-input/fp32 mixed-dot operation is expanded in memory to standard SPIR-V
conversions, dot and add, avoiding a mandatory VALVE extension. It must pass
SPIR-V validation and native GPU output checks before being advertised.

Defaults and existing DLSS, spatial FSR and FSR3 paths remain unchanged. Missing
model files, an invalid receipt, unsupported features, allocation failure or a
failed build/dispatch must report a reason and preserve the existing fallback.
An available option is not evidence of gameplay correctness on every GPU.

Preparation/build outputs use fresh ordinary project-local directories, fixed
source/archive hashes, bounded subprocess timeouts and atomic completion markers.
The user's Steam installation is read only; prefixes and caches are private.
No sudo, package installation, automatic Proton installation or host service is
part of this component. Cancellation never promotes incomplete assets.
