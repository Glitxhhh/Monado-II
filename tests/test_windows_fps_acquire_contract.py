"""Native Game meter placement in the actual production acquire path.

The source check rejects speculative/headless/failed acquisitions and duplicate
frame events. Counter arithmetic/native SDK measurements have separate fixtures.
"""
from pathlib import Path
import re
import unittest

SOURCE=Path(__file__).resolve().parents[1]/'Monado_Windows/runtime/nvn/vulkan/device.c'

def validate(source):
    # Preserve offsets while masking comments and string literals; braces in a
    # diagnostic message cannot forge the successful-acquisition scope.
    masked=re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                  lambda m:' '*len(m[0]),source,flags=re.S)
    acquired=re.search(r'\bbool\s+have\s*=\s*want_window\s*&&\s*vk_frame_begin\(black\)\s*;',masked)
    if not acquired:raise ValueError('Missing production native acquisition result')
    branch=re.search(r'\bif\s*\(have\)\s*\{',masked[acquired.end():])
    if not branch:raise ValueError('Missing successful native acquisition branch')
    opened=acquired.end()+branch.end()-1;depth=1;closed=opened+1
    while depth and closed<len(masked):
        depth+=(masked[closed]=='{')-(masked[closed]=='}');closed+=1
    if depth:raise ValueError('Unbalanced successful native acquisition branch')
    events=list(re.finditer(r'\bfps_overlay_frame\s*\(',masked))
    if len(events)!=1 or not opened<events[0].start()<closed:
        raise ValueError('Game meter must publish exactly once, inside successful native acquisition')
    menu=masked.find('menu_render(',opened,closed)
    if menu<0 or events[0].start()>menu:raise ValueError('Publish native Game event before rendering its overlay')
    unavailable=masked[closed:]
    if not re.match(r'\s*else\s*\{',unavailable) or 'fps_overlay_diagnostics(' not in unavailable:
        raise ValueError('Unavailable selected-provider samples must retain their cadence')

class NativeAcquirePlacement(unittest.TestCase):
    def test_actual_production_game_event_requires_successful_native_acquisition(self):
        validate(SOURCE.read_text(encoding='utf-8'))

    def test_injected_preacquire_and_duplicate_events_are_rejected(self):
        source=SOURCE.read_text(encoding='utf-8')
        misplaced=source.replace('fps_overlay_frame(SDL_GetTicksNS());','',1).replace(
            'bool have = want_window && vk_frame_begin(black);',
            'fps_overlay_frame(SDL_GetTicksNS());\n    bool have = want_window && vk_frame_begin(black);',1)
        with self.assertRaisesRegex(ValueError,'inside successful'):validate(misplaced)
        duplicate=source.replace('bool have = want_window && vk_frame_begin(black);',
            'fps_overlay_frame(SDL_GetTicksNS());\n    bool have = want_window && vk_frame_begin(black);',1)
        with self.assertRaisesRegex(ValueError,'exactly once'):validate(duplicate)

if __name__=='__main__':unittest.main(verbosity=2)
