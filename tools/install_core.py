"""Install a freshly compiled QEMU and its DLL closure into a new runtime tree."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess


def install(model, executable, bin_dir, output):
    # Keep ASCII junction paths intact: MinGW objdump may reject the Chinese
    # physical path behind them, although Windows can load the same binary.
    executable, bin_dir, output = map(lambda p: Path(p).absolute(), (executable, bin_dir, output))
    if not executable.is_file():
        raise FileNotFoundError(executable)
    bins = {p.name.lower(): p for p in bin_dir.glob('*.dll')}
    queue, seen, resolved = [executable], set(), []
    while queue:
        binary = queue.pop()
        if binary.name.lower() in seen:
            continue
        seen.add(binary.name.lower())
        raw = subprocess.check_output([str(bin_dir / 'objdump.exe'), '-p', str(binary)])
        resolved.append(binary)
        for name in re.findall(r'DLL Name:\s*(\S+)', raw.decode('utf8', 'replace')):
            if name.lower() in bins:
                queue.append(bins[name.lower()])
            elif not name.lower().startswith(('api-ms-', 'ext-ms-')) and not (Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32' / name).is_file():
                raise RuntimeError('Missing dependency: ' + name)
    target = output / 'models' / model / 'core/qemu-system-arm.exe'
    if target.exists():
        raise FileExistsError('Choose a new runtime output; existing cores are preserved: ' + str(target))
    dll_dir = output / 'runtime/bin'
    for binary in resolved[1:]:
        existing = dll_dir / binary.name
        if existing.exists() and existing.read_bytes() != binary.read_bytes():
            raise RuntimeError('Shared DLL version conflict: ' + binary.name)
    target.parent.mkdir(parents=True, exist_ok=True)
    dll_dir.mkdir(parents=True, exist_ok=True)
    for binary in resolved[1:]:
        shutil.copy2(binary, dll_dir / binary.name)
    shutil.copy2(executable, target)
    return {'model': model, 'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
            'dll_count': len(resolved) - 1}


if __name__ == '__main__':
    import json
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('--model', choices=('band11', 'band10pro'), required=True)
    p.add_argument('--executable', type=Path, required=True)
    p.add_argument('--bin-dir', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    print(json.dumps(install(a.model, a.executable, a.bin_dir, a.output)))
