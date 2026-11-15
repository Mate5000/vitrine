"""Expand the actual production Makefile without an Apple SDK or Theos install.

Only the external Theos includes are empty stand-ins; source discovery and the
flags under test come from tweak/Makefile. Actual compilation is a separate check.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
make = os.environ.get('MAKE') or shutil.which('gmake') or shutil.which('make')
if not make:
    raise SystemExit('GNU make is required for the compiler-flag check')
sources = ['Shared/QuickEQ/AudioDSP.cpp', 'Shared/QuickEQ/QuickEQAudio.cpp', 'Shared/QuickEQ/QuickEQSettings.mm',
           'Shared/LiveActivity/LiveActivityBridge.swift', 'App/ModSettings.x', 'Core/SGGlass.m']
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    makefiles = work / 'theos/makefiles'
    makefiles.mkdir(parents=True)
    (makefiles / 'common.mk').write_text(f'THEOS_MAKE_PATH := {makefiles}\n')
    (makefiles / 'tweak.mk').touch()
    for source in sources:
        path = work / 'Sources' / source
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
    probe = work / 'probe.mk'
    variables = ['spotifyglass_CFLAGS', 'spotifyglass_CCFLAGS', 'spotifyglass_OBJCFLAGS', 'spotifyglass_OBJCCFLAGS']
    variables += ['Sources/' + source + '_CFLAGS' for source in sources]
    probe.write_text('\n'.join(f'$(info PROBE:{name}=$({name}))' for name in variables) + '\n.PHONY: probe\nprobe:\n\t@:\n')
    result = subprocess.run([make, '--no-print-directory', '-f', str(root / 'tweak/Makefile'), '-f', str(probe),
                             'THEOS=' + str(work / 'theos'), 'probe'], cwd=work, check=True, capture_output=True, text=True)
    flags = dict(line[6:].split('=', 1) for line in result.stdout.splitlines() if line.startswith('PROBE:'))
    for name in variables:
        cxx = name.endswith('.cpp_CFLAGS') or name.endswith('.mm_CFLAGS')
        assert ('-std=c++17' in flags[name]) == cxx, (name, flags[name])
    assert '-fobjc-arc' in flags['spotifyglass_OBJCFLAGS']
    assert '-fobjc-arc' not in flags['spotifyglass_CFLAGS']
print('PASS: production Makefile scopes C++17 to C++ sources and ARC to Objective-C')
