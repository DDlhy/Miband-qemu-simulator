"""Generate fresh model flash from stock firmware and factory properties.

Run the copy inside a generated runtime, using its embedded Python.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def create(model):
    root = ROOT / 'models' / model
    # Import each model in its own process: module names overlap.
    sys.path.insert(0, str(root / 'tools'))
    factory = root / 'config/factory'
    if (root / 'userdata').exists():
        raise FileExistsError('Existing storage is preserved; assemble into a new runtime directory')
    if model == 'band11':
        from initialize_factory import create_factory
        from initialize_storage import _seed, initialize_storage, NAND_IMAGE_SIZE
        prop = factory / 'factory.prop'
        create_factory(factory, factory_prop=prop)
        data = root / 'userdata/nand-bluetooth.img'
        _seed(data, NAND_IMAGE_SIZE, lambda stream: None)
        initialize_storage(root, data, factory / 'q66nfc.img')
        generated = [factory / 'q66nfc.img', *sorted(data.parent.glob('*.img'))]
    else:
        from initialize_factory import create_factory, read_properties
        prop = factory / 'factory.prop'
        _, properties = read_properties(prop)
        temporary = root / 'config/factory-generated'
        create_factory(prop, temporary, properties.get('ro.factory.mac_bt', '02:00:00:10:00:01'))
        # Generation succeeded before replacing the text-only input directory.
        for path in temporary.iterdir():
            path.replace(factory / path.name)
        temporary.rmdir()
        generated = [factory / 'nv.img', factory / 'nor-seed.bin']
    report = {'model': model, 'fresh': True, 'files': [
        {'path': p.relative_to(ROOT).as_posix(), 'size': p.stat().st_size,
         'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in generated]}
    output = ROOT / 'userdata' / (model + '-image-build.json')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2), encoding='utf8')
    print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--model', choices=('band11', 'band10pro'), required=True)
    create(parser.parse_args().model)
