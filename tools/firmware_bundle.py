"""Validate and extract stock firmware stored as small ZIP archive parts."""
import hashlib
import json
from pathlib import Path
import re
import tempfile
import zipfile

IMAGES = {'vela_ap.bin', 'vela_resource.bin', 'vela_ota.bin', 'vela_bl2.bin'}
REQUIRED = IMAGES - {'vela_bl2.bin'}


def load_manifest(path):
    data = json.loads(Path(path).read_text('utf8'))
    if data.get('format') != 1 or data.get('model') not in ('band11', 'band10pro'):
        raise ValueError('Invalid firmware bundle format or model')
    if not isinstance(data.get('version'), str) or not data['version']:
        raise ValueError('Missing firmware version')
    for entries, key in ((data['archive']['parts'], 'file'), (data['images'], 'file')):
        seen = set()
        if not entries:
            raise ValueError('Empty firmware bundle')
        for entry in entries:
            name = entry[key]
            if not isinstance(name, str) or not name or '/' in name or '\\' in name or name in ('.', '..') or name in seen:
                raise ValueError('Unsafe or duplicate firmware filename')
            seen.add(name)
            if not isinstance(entry['size'], int) or entry['size'] <= 0 or not re.fullmatch(r'[0-9a-f]{64}', entry['sha256']):
                raise ValueError('Invalid firmware size or SHA256')
    names = {entry['file'] for entry in data['images']}
    if not REQUIRED <= names <= IMAGES:
        raise ValueError('Firmware bundle must contain stock AP, resources and OTA images only')
    archive = data['archive']
    if not isinstance(archive['size'], int) or archive['size'] <= 0 or not re.fullmatch(r'[0-9a-f]{64}', archive['sha256']):
        raise ValueError('Invalid archive size or SHA256')
    if sum(part['size'] for part in archive['parts']) != archive['size']:
        raise ValueError('Firmware part sizes do not match archive size')
    return data


def verify_parts(manifest_path, output=None):
    """Check every part and the joined archive; optionally write the ZIP stream."""
    manifest_path = Path(manifest_path)
    data = load_manifest(manifest_path)
    archive_hash = hashlib.sha256()
    for part in data['archive']['parts']:
        digest = hashlib.sha256()
        size = 0
        with (manifest_path.parent / part['file']).open('rb') as source:
            while block := source.read(1024 * 1024):
                size += len(block)
                digest.update(block)
                archive_hash.update(block)
                if output is not None:
                    output.write(block)
        if size != part['size'] or digest.hexdigest() != part['sha256']:
            raise ValueError('Firmware archive part failed verification: ' + part['file'])
    if archive_hash.hexdigest() != data['archive']['sha256']:
        raise ValueError('Joined firmware archive SHA256 mismatch')
    return data


def extract_bundle(manifest_path, destination, expected_model=None):
    manifest_path, destination = Path(manifest_path), Path(destination)
    data = load_manifest(manifest_path)
    if expected_model is not None and data['model'] != expected_model:
        raise ValueError('Firmware bundle is for a different model')
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='miband-stock-firmware-') as work:
        archive = Path(work) / 'firmware.zip'
        with archive.open('xb') as output:
            verify_parts(manifest_path, output)
        with zipfile.ZipFile(archive) as package:
            entries = package.infolist()
            expected = {item['file']: item for item in data['images']}
            if len(entries) != len(expected) or {e.filename for e in entries} != expected.keys():
                raise ValueError('Unexpected files in stock firmware archive')
            for entry in entries:
                item = expected[entry.filename]
                if entry.is_dir() or entry.file_size != item['size']:
                    raise ValueError('Stock firmware image size mismatch: ' + entry.filename)
                digest = hashlib.sha256()
                with package.open(entry) as source, (destination / entry.filename).open('xb') as output:
                    while block := source.read(1024 * 1024):
                        digest.update(block)
                        output.write(block)
                if digest.hexdigest() != item['sha256']:
                    raise ValueError('Stock firmware image SHA256 mismatch: ' + entry.filename)
    return data['images']
