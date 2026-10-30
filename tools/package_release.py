"""Create a Windows or Linux release package from an explicit file list."""
import argparse
import hashlib
import io
from pathlib import Path
import re
import tarfile
import zipfile


NAME = 'OpenShao'


def runtime_files(root):
    files = [(root / 'resources/readme.txt', 'readme.txt'),
             (root / 'LICENSE', 'licenses/LICENSE.txt')]
    files.extend((path, path.relative_to(root).as_posix())
                 for path in (root / 'licenses').rglob('*')
                 if path.is_file() and path.suffix in ('.txt', '.md'))
    return sorted(files, key=lambda item: item[1])


def checksum(output, artifact):
    record = f'{hashlib.sha256(artifact.read_bytes()).hexdigest()}  {artifact.name}\n'
    destination = output / 'checksums.txt'
    existing = destination.read_text().splitlines() if destination.exists() else []
    existing = [line for line in existing if not line.endswith('  ' + artifact.name)]
    destination.write_text(''.join(line + '\n' for line in existing) + record, encoding='utf-8')


def platform_of(exe):
    """'windows' for a PE executable, 'linux' for an ELF one."""
    magic = exe.read_bytes()[:4]
    if magic[:2] == b'MZ':
        return 'windows'
    if magic == b'\x7fELF':
        return 'linux'
    raise ValueError(f'{exe} is neither a Windows nor a Linux executable')


def project_version(root):
    """The version in CMakeLists.txt, which the executable reports to the updater."""
    text = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
    found = re.search(r'project\(\s*OpenShao\s+VERSION\s+(\S+)', text)
    if not found:
        raise ValueError('CMakeLists.txt does not declare the project version')
    # The launcher compares RecompVersion with the latest release tag.
    header = (root / 'src/mods/version.h').read_text(encoding='utf-8')
    recomp = re.search(r'RecompVersion\s*=\s*"([^"]+)"', header)
    if not recomp or recomp.group(1) != found.group(1):
        raise ValueError('src/mods/version.h RecompVersion must match the CMakeLists.txt project version')
    return found.group(1)


def check_version(version):
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?', version):
        raise ValueError('Use major.minor.patch with an optional prerelease suffix')


def portable_package(root, exe, output, version):
    check_version(version)
    output.mkdir(parents=True, exist_ok=True)
    destination = output / f'{NAME}-{version}-Windows-x64.zip'
    with zipfile.ZipFile(destination, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(exe, NAME + '.exe')
        for source, name in runtime_files(root):
            archive.write(source, name)
    checksum(output, destination)
    return destination


def linux_package(root, exe, output, version):
    """A .tar.gz with one top folder; the executable keeps its execute bit, owners are not recorded."""
    check_version(version)
    output.mkdir(parents=True, exist_ok=True)
    destination = output / f'{NAME}-{version}-Linux-x64.tar.gz'

    def add(archive, data, name, mode):
        info = tarfile.TarInfo(f'{NAME}/{name}')
        info.size = len(data)
        info.mode = mode
        info.mtime = 0
        info.uname = info.gname = ''
        archive.addfile(info, io.BytesIO(data))

    with tarfile.open(destination, 'w:gz', format=tarfile.PAX_FORMAT) as archive:
        add(archive, exe.read_bytes(), NAME, 0o755)
        for source, name in runtime_files(root):
            add(archive, source.read_bytes(), name, 0o644)
    checksum(output, destination)
    return destination


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    # The updater looks for the package named after the release tag, so the name must carry the
    # version the executable was built with.
    parser.add_argument('--version', help='defaults to the project version in CMakeLists.txt')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    exe = args.exe.resolve(strict=True)
    output = args.output.resolve() if args.output else root / 'dist'
    package = linux_package if platform_of(exe) == 'linux' else portable_package
    print(package(root, exe, output, args.version or project_version(root)))


if __name__ == '__main__':
    main()
