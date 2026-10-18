"""Seed writable flash only when missing; never replace existing device state."""
import os
from pathlib import Path
import shutil
import tempfile

NOR_SIZE = 16 * 1024 * 1024
NAND_MAIN_SIZE = 128 * 1024 * 1024
NAND_IMAGE_SIZE = NAND_MAIN_SIZE + 8 * 1024 * 1024


def storage_paths(data):
    data = Path(data)
    return (data.with_name(data.stem + '-nor.img'),
            data.with_name(data.stem + '-resource.img'))


def _seed(path, size, populate):
    path = Path(path)
    if path.exists():
        if path.stat().st_size != size:
            raise ValueError(f'Existing flash image has wrong size: {path}')
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'w+b') as stream:
            erased = b'\xff' * (1024 * 1024)
            for _ in range(size // len(erased)):
                stream.write(erased)
            populate(stream)
            stream.flush()
            os.fsync(stream.fileno())
        # On Windows rename fails if another owner created the destination.
        os.rename(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return True


def initialize_storage(root, data, factory):
    root = Path(root)
    nor, resource = storage_paths(data)
    firmware = root / 'firmware'
    data = Path(data)
    if not data.exists() or data.stat().st_size != NAND_IMAGE_SIZE:
        raise ValueError(f'User NAND must contain main and OOB: {data}')

    def populate_nor(stream):
        for path, offset, limit in ((firmware / 'vela_ap.bin', 0xc0000, 0xf00000),
                                    (Path(factory), 0xfc0000, 0x20000)):
            if path.stat().st_size > limit:
                raise ValueError(f'Image exceeds partition: {path.name}')
            stream.seek(offset)
            with path.open('rb') as source:
                shutil.copyfileobj(source, stream)

    def populate_resource(stream):
        path = firmware / 'vela_resource.bin'
        if path.stat().st_size > NAND_MAIN_SIZE:
            raise ValueError('Resource image exceeds partition')
        stream.seek(0)
        with path.open('rb') as source:
            shutil.copyfileobj(source, stream)

    return {'nor': str(nor), 'resource': str(resource),
            'nor_created': _seed(nor, NOR_SIZE, populate_nor),
            'resource_created': _seed(resource, NAND_IMAGE_SIZE, populate_resource)}


def flash_drive_arguments(root, data):
    arguments = []
    for name, path in zip(('nand-data', 'nor-flash', 'nand-resource'),
                          (Path(data), *storage_paths(data))):
        relative = os.path.relpath(path, root).replace('\\', '/')
        relative.encode('ascii')
        if ',' in relative:
            raise ValueError('QEMU flash paths must not contain commas')
        arguments += ['-drive', f'if=none,id={name},format=raw,file={relative}']
    return arguments


if __name__ == '__main__':
    import argparse
    import json
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--factory', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(initialize_storage(Path(__file__).resolve().parents[1],
                                        args.data, args.factory)))
