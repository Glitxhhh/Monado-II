#!/usr/bin/env bash
# Build the launcher: build/xcde_launcher.exe (SDL3 + Dear ImGui, SDL renderer). Run it with "Play Monado.bat".
cd "$(dirname "$0")/../.."
Z=".venv/Scripts/python -m ziglang c++"
I=third_party/imgui
M=/c/msys64/ucrt64
mkdir -p build/launcher
for f in $I/imgui.cpp $I/imgui_draw.cpp $I/imgui_tables.cpp $I/imgui_widgets.cpp $I/backends/imgui_impl_sdl3.cpp \
         $I/backends/imgui_impl_sdlrenderer3.cpp tools/launcher/launcher.cpp; do
  o=build/launcher/$(basename ${f%.cpp}).o
  [ "$f" -nt "$o" ] || [ ! -f "$o" ] || continue
  $Z -c $f -o $o -O2 -march=x86_64_v2 -std=c++17 -I$I -I$I/backends -I$M/include -w || exit 1
done
$Z -o build/xcde_launcher.exe build/launcher/*.o $M/lib/libSDL3.dll.a -march=x86_64_v2 -Wl,--subsystem,windows -static-libstdc++ 2> build/launcher/link.err
rc=$?; [ $rc = 0 ] || cat build/launcher/link.err | head -20; exit $rc
