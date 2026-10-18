"""Check the clean source delivery and its two synthetic identity inputs."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXCLUDE = {'.git', '.build', 'dist', '__pycache__', '.vs', '.idea'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def payload():
    for p in sorted(ROOT.rglob('*')):
        if not p.is_file():
            continue
        rel = p.relative_to(ROOT)
        if any(part in EXCLUDE for part in rel.parts):
            continue
        yield rel.as_posix(), p


def check(verify_hashes=True):
    files = dict(payload())
    record = json.loads((ROOT / 'docs/release-manifest.json').read_text('utf8'))
    identities = record['public_identity_files']
    if {n for n in files if n.endswith('.prop')} != identities.keys():
        raise RuntimeError('Expected exactly the two public demo factory.prop files')
    for name, value in identities.items():
        if sha(ROOT / name) != value:
            raise RuntimeError('Demo identity changed; supply private properties as external build inputs: ' + name)
    for name, path in files.items():
        if name.startswith(('runtime/', 'userdata/', 'shared/factory-python/', 'shared/controller-firmware/')) or '/userdata/' in name or '/artifacts/' in name:
            raise RuntimeError('Generated or private data mixed into source: ' + name)
        if path.stat().st_size > 25_000_000:
            raise RuntimeError('Oversized source file: ' + name)
        if name.endswith(('.exe', '.dll', '.pyd', '.img', '.bin', '.sfi', '.ddc', '.original', '.journal')):
            raise RuntimeError('Runtime binary or storage mixed into source: ' + name)
    if verify_hashes:
        sums = {}
        for line in (ROOT / 'checksums.sha256').read_text('utf8').splitlines():
            value, name = line.split('  ', 1)
            sums[name] = value
        if sums.keys() != files.keys() - {'checksums.sha256'}:
            raise RuntimeError('Source file set changed; refresh checksums after review')
        for name, value in sums.items():
            if sha(files[name]) != value:
                raise RuntimeError('Hash mismatch: ' + name)
    return {'ok': True, 'kind': 'source', 'files': len(files),
            'bytes': sum(p.stat().st_size for p in files.values())}


if __name__ == '__main__':
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('--refresh-checksums', action='store_true')
    args = p.parse_args()
    if args.refresh_checksums:
        check(False)
        (ROOT / 'checksums.sha256').write_text(''.join(f'{sha(path)}  {name}\n' for name, path in payload()
                                                     if name != 'checksums.sha256'), encoding='utf8', newline='\n')
    print(json.dumps(check(), ensure_ascii=False))
