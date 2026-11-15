"""Synthetic IPA fixtures test the packaging gate, including negative cases."""
import importlib.util
import plistlib
import struct
import tempfile
import zipfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('verify_eq', root / 'scripts/verify-quick-eq.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def macho(commands=b'', count=0, filetype=2):
    return struct.pack('<IIIIIIII', 0xFEEDFACF, 0x0100000C, 0, filetype, count, len(commands), 0, 0) + commands


def load(path, weak=False):
    name = path.encode() + b'\0'
    size = (24 + len(name) + 7) & ~7
    return struct.pack('<IIIIII', 0x80000018 if weak else 0xC, size, 24, 0, 0, 0) + name + bytes(size - 24 - len(name))


def rpath(path):
    name = path.encode() + b'\0'
    size = (12 + len(name) + 7) & ~7
    return struct.pack('<III', 0x8000001C, size, 12) + name + bytes(size - 12 - len(name))


def fixture(ipa, case):
    prefix = 'Payload/Spotify.app/'
    command = load('@rpath/spotifyglass.dylib', weak=True) if case in ('cyan', 'missing-rpath', 'wrong-rpath') else load('@executable_path/Frameworks/spotifyglass.dylib')
    commands = command * (2 if case == 'duplicate-load' else 1)
    count = 2 if case == 'duplicate-load' else 1
    if case in ('cyan', 'wrong-rpath'):
        commands += rpath('@executable_path/Frameworks' if case == 'cyan' else '@executable_path/Missing')
        count += 1
    with zipfile.ZipFile(ipa, 'w') as out:
        out.writestr(prefix + 'Info.plist', plistlib.dumps({'CFBundleExecutable': 'Spotify'}))
        out.writestr(prefix + 'Spotify', macho() + command if case == 'string-only' else macho(commands, count))
        markers = b'SGQuickEQViewController\0Vitrine Quick EQ 1.0.0\0'
        out.writestr(prefix + 'Frameworks/spotifyglass.dylib', macho(filetype=2 if case == 'wrong-filetype' else 6) + (b'' if case == 'old-tweak' else markers))
        if case == 'duplicate-tweak':
            out.writestr(prefix + 'spotifyglass.dylib', macho() + markers)
        for path in (root / 'tweak/Resources/SGQuickEQ.bundle').rglob('*'):
            if not path.is_file() or (case == 'missing-hu' and path.parent.name == 'hu.lproj'):
                continue
            out.writestr(prefix + 'SGQuickEQ.bundle/' + path.relative_to(root / 'tweak/Resources/SGQuickEQ.bundle').as_posix(), path.read_bytes())


with tempfile.TemporaryDirectory() as directory:
    ipa = Path(directory) / 'fixture.ipa'
    fixture(ipa, 'good')
    module.verify(ipa)
    fixture(ipa, 'cyan')
    module.verify(ipa)
    for case in ('string-only', 'duplicate-load', 'old-tweak', 'duplicate-tweak', 'missing-hu', 'missing-rpath', 'wrong-rpath', 'wrong-filetype'):
        fixture(ipa, case)
        try:
            module.verify(ipa)
        except ValueError:
            pass
        else:
            raise AssertionError('Bad IPA was accepted: ' + case)
print('PASS: packaging gate rejects missing EQ, duplicates, missing localization, invalid dylib/load type and unresolved @rpath')
