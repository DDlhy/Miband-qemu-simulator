"""Expand the pinned common dependency and apply visible model source files."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def prepare(model, output, verify=True):
    output = output.resolve()
    if output.exists():
        raise FileExistsError('Use a new output directory: ' + str(output))
    if output.is_relative_to(ROOT / 'source') or output.is_relative_to(ROOT / 'vendor'):
        raise ValueError('Output must be outside source/ and vendor/')
    vendor = json.loads((ROOT / 'vendor/manifest.json').read_text('utf8'))['qemu']
    for item in vendor['archives']:
        archive = ROOT / 'vendor' / item['archive']
        if hashlib.sha256(archive.read_bytes()).hexdigest() != item['sha256']:
            raise ValueError('Common source archive hash mismatch: ' + item['archive'])
    output.mkdir(parents=True)
    for item in vendor['archives']:
        with tarfile.open(ROOT / 'vendor' / item['archive'], 'r:xz') as tar:
            for member in tar:
                target = (output / member.name).resolve()
                if not member.isfile() or not target.is_relative_to(output):
                    raise ValueError('Unsafe source archive member: ' + member.name)
                target.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as source, target.open('wb') as dest:
                    shutil.copyfileobj(source, dest)
                target.chmod(member.mode & 0o777)
    band11 = json.loads((ROOT / 'source/band11.json').read_text('utf8'))
    for name in band11['replace_or_add']:
        dest = output / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / 'source/overlays/band11' / name, dest)
    record = band11
    if model == 'band10pro':
        record = json.loads((ROOT / 'source/overlays/band10pro.json').read_text('utf8'))
        for name in record['remove']:
            (output / name).unlink()
        for name in record['replace_or_add']:
            dest = output / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / 'source/overlays/band10pro' / name, dest)
    if verify:
        files = {p.relative_to(output).as_posix(): p for p in output.rglob('*') if p.is_file()}
        if files.keys() != record['files'].keys():
            raise RuntimeError('Source file set does not match its model manifest')
        for name, path in files.items():
            if hashlib.sha256(path.read_bytes()).hexdigest() != record['files'][name]:
                raise RuntimeError('Source hash mismatch: ' + name)
    print(json.dumps({'model': model, 'source': str(output), 'verified': verify,
                      'files': len(record['files'])}, ensure_ascii=False))


if __name__ == '__main__':
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('--model', choices=('band11', 'band10pro'), required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--no-verify', action='store_true', help='For reviewed local source edits')
    args = p.parse_args()
    prepare(args.model, args.output, not args.no_verify)
