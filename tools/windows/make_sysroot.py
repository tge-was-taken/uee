#!/usr/bin/env python3
"""Copy the MSVC and Windows SDK headers and libraries into a clang-cl /winsysroot."""

import argparse
import os
import re
import shutil
from pathlib import Path

INCLUDE = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
PRAGMA_LIB = re.compile(rb'#\s*pragma\s+comment\s*\(\s*lib\s*,\s*"([^"]+)"', re.IGNORECASE)
LIB_NAME = re.compile(rb'\b([A-Za-z0-9_.+-]+\.lib)\b', re.IGNORECASE)
SOURCE_SUFFIXES = {'.h', '.hpp', '.hxx', '.inl', '.c', '.cc', '.cpp', '.cxx', '.rc', '.in',
                   '.txt', '.cmake', ''}


def copy_tree(source: Path, destination: Path):
    if destination.exists():
        return
    partial = destination.with_name(destination.name + '.partial')
    shutil.rmtree(partial, ignore_errors=True)
    shutil.copytree(source, partial, ignore=shutil.ignore_patterns('*.pdb'))
    partial.rename(destination)


def get_spellings(roots, pattern):
    spellings = set()
    for root in roots:
        for path, _, files in os.walk(root):
            for name in files:
                if Path(name).suffix.lower() not in SOURCE_SUFFIXES:
                    continue
                try:
                    data = (Path(path) / name).read_bytes()
                except OSError:
                    continue
                for match in pattern.finditer(data):
                    spellings.add(match.group(1).decode('latin-1').replace('\\', '/'))
    return spellings


# The copy is case-sensitive and Windows code spells names in any case, so each
# spelling found in the headers and the given sources gets a link.
def link_spellings(directory: Path, spellings):
    actual = {}
    for path, _, files in os.walk(directory):
        for name in files:
            relative = (Path(path) / name).relative_to(directory).as_posix()
            actual.setdefault(relative.lower(), relative)
    wanted = set(spellings) | {name.lower() for name in actual.values()}
    for spelling in wanted:
        real = actual.get(spelling.lower())
        if real is None or real == spelling:
            continue
        link = directory / spelling
        if link.exists() or link.is_symlink():
            continue
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(os.path.relpath(directory / real, link.parent))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--msvc', type=Path, required=True, help='VC/Tools/MSVC/<version>')
    parser.add_argument('--sdk', type=Path, required=True, help='Windows Kits/10')
    parser.add_argument('--sdk-version', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('sources', type=Path, nargs='*', help='trees whose spellings must resolve')
    args = parser.parse_args()

    msvc = args.out / 'VC/Tools/MSVC' / args.msvc.name
    sdk = args.out / 'Windows Kits/10'
    includes = [msvc / 'include']
    libraries = [msvc / 'lib/x64']
    copy_tree(args.msvc / 'include', msvc / 'include')
    copy_tree(args.msvc / 'lib/x64', msvc / 'lib/x64')
    for part in ('ucrt', 'um', 'shared', 'winrt', 'cppwinrt'):
        source = args.sdk / 'Include' / args.sdk_version / part
        includes.append(sdk / 'Include' / args.sdk_version / part)
        copy_tree(source, includes[-1])
    for part in ('ucrt', 'um'):
        libraries.append(sdk / 'Lib' / args.sdk_version / part / 'x64')
        copy_tree(args.sdk / 'Lib' / args.sdk_version / part / 'x64', libraries[-1])

    roots = includes + args.sources
    headers = get_spellings(roots, INCLUDE)
    for directory in includes:
        link_spellings(directory, headers)

    names = get_spellings(roots, PRAGMA_LIB) | get_spellings(args.sources, LIB_NAME)
    names |= {name + '.lib' for name in names if not name.lower().endswith('.lib')}
    for directory in libraries:
        link_spellings(directory, names)


if __name__ == '__main__':
    main()
