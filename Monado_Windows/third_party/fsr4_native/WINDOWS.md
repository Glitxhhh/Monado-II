# Optional native FSR4 Lite component for Windows

This is the reviewed accepted GPL-2.0-or-later Vulkan Lite backend, copied from
the accepted Linux reference. Its actual component is a native AMD64 PE DLL.
The Windows adaptation uses Unicode filesystem paths; no ELF loader, Proton
prefix, environment model selection or vendor/model download is included.
The original README and full GPL/AMD SDK license texts are preserved beside it.

Source provenance: bbport 0fc2921c45f8ff3900f6bfa43a299e9a4f532af4, reviewed through
Monado Reborn 3cc9eeb46dc879fe36929d13758754c429b8377b.

Building a component does not prepare or authenticate any model and does not
enable the FSR4 option. Four complete locally prepared Lite sets, validated
receipts, actual device features and successful native GPU dispatch are separate
requirements. Missing models or unsupported capabilities retain ordinary fallback.
Distribution with this optional component requires applicable GPL terms and the
corresponding combined source/build scripts; dynamic linkage is not an exemption.
No vendor/model output belongs in the public source package.
