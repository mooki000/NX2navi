"""Build and run tests/report_page_check.cpp: the menu's bug report window (runtime/ui/report_page.cpp) without the
game, driven as the menu drives it. The offer after a fatal error is still shown once the menu has opened; a report the
player saw and left closes with the menu; the test hook's Send sends once and its Not now sends nothing.

Usage: report_page_check.py   (needs clang++ or g++; no game files)
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'runtime'
SOURCES = [ROOT / 'tests' / 'report_page_check.cpp', RT / 'ui' / 'option_rows.cpp', RT / 'ui' / 'menu_skin.cpp',
           RT / 'ui_text.c'] + sorted((RT / 'third_party' / 'imgui').glob('imgui*.cpp'))


def main():
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if not cxx:
        sys.exit('needs clang++ or g++ on PATH')
    work = ROOT / 'build' / 'test' / 'report_page'
    work.mkdir(parents=True, exist_ok=True)
    objs = []
    for src in SOURCES:
        obj = work / (src.stem + '.o')
        lang = ['-x', 'c', '-std=gnu11'] if src.suffix == '.c' else ['-x', 'c++', '-std=gnu++17']
        if subprocess.run([cxx, *lang, '-c', '-O1', '-g', '-w', '-I', str(RT), '-I', str(RT / 'third_party' / 'imgui'),
                           str(src), '-o', str(obj)]).returncode:
            sys.exit(f'{src.name} did not compile')
        objs.append(str(obj))
    exe = work / ('report_page_check.exe' if os.name == 'nt' else 'report_page_check')
    if subprocess.run([cxx, *objs, '-o', str(exe), '-lm']).returncode:
        sys.exit('the link failed')
    r = subprocess.run([str(exe)])
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
