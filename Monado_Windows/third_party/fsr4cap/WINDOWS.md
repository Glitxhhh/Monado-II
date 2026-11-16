# Windows adaptation of the accepted capture source

The adjacent upstream README describes its original import. This Windows copy
retains those notices and the accepted Linux dev104 R5 source; it adds
windows_capture_io.h and narrowly adapts recorder/shim path and DLL calls to
Unicode, module-local loads and ordinary, single-link, exclusive-new writes.
Each of the four fixed Lite captures runs in a separate fresh owned directory.
No driver, SDK, model, capture or translated shader is included here.

The recorder, trace extraction/model selection and class-binding patch retain
GPL-2.0-or-later. The AMD API headers and extension shim retain their MIT notices.
The native I/O adapter is MIT. See the complete LICENSE and ffx headers.

The pinned tool recipe builds native Win32 recorder/shim and patched native
DXIL/SPIR-V tools. Explicit local vkd3d-proton/DXVK support and the hash-pinned
AMD SDK are required for actual synthetic capture; no system driver is changed.
The separate authored Vulkan readback validator is in tools/fsr4. Native
compilation, structural preparation, GPU component checks and game qualification
are separate outcomes. Missing tools or failed checks never promote assets.
