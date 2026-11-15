#!/usr/bin/env python3
"""Check the packaged EQ, resources and Spotify's actual dylib load command.

This proves packaging, not that Spotify reaches the PCM callback on a device.
Uses only Python's standard library; never extracts an untrusted IPA to disk.
"""
import plistlib
import struct
import sys
import zipfile
from pathlib import PurePosixPath


def arm64(data):
    if data[:4] == b'\xca\xfe\xba\xbe':
        count, = struct.unpack_from('>I', data, 4)
        for index in range(count):
            cpu, _, offset, size, _ = struct.unpack_from('>IIIII', data, 8 + index * 20)
            if cpu == 0x0100000C:
                if offset + size > len(data):
                    raise ValueError('Truncated arm64 slice')
                return arm64(data[offset:offset + size])
        raise ValueError('No arm64 slice')
    if len(data) < 32 or data[:4] != b'\xcf\xfa\xed\xfe':
        raise ValueError('Expected a 64-bit little-endian Mach-O')
    if struct.unpack_from('<I', data, 4)[0] != 0x0100000C:
        raise ValueError('Expected arm64 code')
    return data


def load_commands(data, runpaths=False):
    data = arm64(data)
    count, size = struct.unpack_from('<II', data, 16)
    offset, end = 32, 32 + size
    if end > len(data):
        raise ValueError('Truncated load commands')
    for _ in range(count):
        if offset + 8 > end:
            raise ValueError('Truncated load command header')
        kind, length = struct.unpack_from('<II', data, offset)
        if length < 8 or offset + length > end:
            raise ValueError('Invalid load command size')
        # cyan deliberately injects LC_LOAD_WEAK_DYLIB with both its backends:
        # https://github.com/asdfzxcvbn/pyzule-rw/blob/main/cyan/tbhtypes/main_executable.py
        kinds = (0x8000001C,) if runpaths else (0xC, 0x80000018)
        if kind in kinds:  # A real load command / LC_RPATH, not an embedded string
            header_size = 12 if runpaths else 24
            if length < header_size:
                raise ValueError('Truncated dylib/path command')
            name, = struct.unpack_from('<I', data, offset + 8)
            if not header_size <= name < length:
                raise ValueError('Invalid dylib/path name offset')
            yield data[offset + name:offset + length].split(b'\0', 1)[0].decode('utf-8')
        offset += length


def verify(path):
    with zipfile.ZipFile(path) as ipa:
        names = ipa.namelist()
        roots = [p for p in names if p.startswith('Payload/') and p.endswith('.app/Info.plist') and p.count('/') == 2]
        if len(roots) != 1:
            raise ValueError('Expected exactly one main app Info.plist')
        app = roots[0][:-len('Info.plist')]
        info = plistlib.loads(ipa.read(roots[0]))
        executable = app + info['CFBundleExecutable']
        libraries = [p for p in names if p.startswith(app) and PurePosixPath(p).name == 'spotifyglass.dylib']
        if len(libraries) != 1:
            raise ValueError('Expected exactly one injected spotifyglass.dylib')
        dylib = arm64(ipa.read(libraries[0]))
        if struct.unpack_from('<I', dylib, 12)[0] != 6:  # MH_DYLIB
            raise ValueError('spotifyglass.dylib is not a dynamic library')
        for marker in (b'SGQuickEQViewController', b'Vitrine Quick EQ 1.0.0'):
            if marker not in dylib:
                raise ValueError('The injected tweak does not contain Quick EQ: ' + marker.decode())
        loads = [p for p in load_commands(ipa.read(executable)) if PurePosixPath(p).name == 'spotifyglass.dylib']
        if len(loads) != 1:
            raise ValueError('Spotify must load spotifyglass.dylib exactly once')
        relative = libraries[0][len(app):]
        # cyan uses @rpath with the Frameworks location. Explicit executable/
        # loader-relative paths to the same file are valid too.
        expected = {'@executable_path/' + relative, '@loader_path/' + relative}
        if relative.startswith('Frameworks/'):
            expected.add('@rpath/' + relative[len('Frameworks/'):])
        if loads[0] not in expected:
            raise ValueError('The dylib load path does not point at the packaged tweak')
        if loads[0].startswith('@rpath/'):
            runpaths = {p.rstrip('/') for p in load_commands(ipa.read(executable), runpaths=True)}
            if not runpaths.intersection({'@executable_path/Frameworks', '@loader_path/Frameworks'}):
                raise ValueError('The injected @rpath library has no matching Frameworks search path')
        bundle = app + 'SGQuickEQ.bundle/'
        for member in ('Info.plist', 'en.lproj/Localizable.strings', 'hu.lproj/Localizable.strings'):
            if names.count(bundle + member) != 1:
                raise ValueError('Missing or duplicate Quick EQ resource: ' + member)
        bundles = [p for p in names if p.endswith('/SGQuickEQ.bundle/Info.plist')]
        if bundles != [bundle + 'Info.plist']:
            raise ValueError('Quick EQ resources must appear once, in the main app')
        if plistlib.loads(ipa.read(bundle + 'Info.plist'))['CFBundleIdentifier'] != 'dev.vitrine.quickeq.resources':
            raise ValueError('Incorrect Quick EQ resource bundle')
    print('PASS: Quick EQ code, single dylib load command, English/Hungarian resources')


if __name__ == '__main__':
    try:
        verify(sys.argv[1])
    except (IndexError, KeyError, ValueError, struct.error, zipfile.BadZipFile) as error:
        sys.exit('Quick EQ IPA verification failed: ' + str(error))
