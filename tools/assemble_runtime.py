"""Assemble compiled cores, external firmware and NEW flash into a new directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

from install_core import install

ROOT = Path(__file__).resolve().parents[1]
IMAGES = ('vela_ap.bin', 'vela_resource.bin', 'vela_ota.bin', 'vela_bl2.bin')


def extract_zip(archive, destination):
    with zipfile.ZipFile(archive) as package:
        for entry in package.infolist():
            if entry.is_dir():
                continue
            path = (destination / entry.filename).resolve()
            if not path.is_relative_to(destination.resolve()):
                raise ValueError('Unsafe ZIP member')
            path.parent.mkdir(parents=True, exist_ok=True)
            with package.open(entry) as source, path.open('wb') as target:
                shutil.copyfileobj(source, target)


def firmware(source, destination):
    destination.mkdir(parents=True)
    if source.is_dir():
        entries = {name: source / name for name in IMAGES if (source / name).is_file()}
        if not set(IMAGES[:3]).issubset(entries):
            raise ValueError('Firmware directory needs vela_ap.bin, vela_resource.bin, vela_ota.bin')
        for name, path in entries.items():
            shutil.copy2(path, destination / name)
    else:
        with zipfile.ZipFile(source) as package:
            for name in IMAGES:
                entries = [e for e in package.infolist() if not e.is_dir() and Path(e.filename).name == name]
                if not entries and name == 'vela_bl2.bin':
                    continue
                if len(entries) != 1:
                    raise ValueError('Missing or ambiguous firmware image: ' + name)
                with package.open(entries[0]) as src, (destination / name).open('wb') as dest:
                    shutil.copyfileobj(src, dest)
    return [{'file': p.name, 'size': p.stat().st_size,
             'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in destination.iterdir()]


def assemble(args):
    output = args.output.resolve()
    if output.exists():
        raise FileExistsError('Choose a new output directory; old images are preserved: ' + str(output))
    if any(output.is_relative_to(ROOT / d) for d in ('models', 'source', 'vendor', 'shared', 'tools')):
        raise ValueError('Use dist/ or a directory outside the source project')
    selected = ('band11', 'band10pro') if args.model == 'all' else (args.model,)
    for model in selected:
        if getattr(args, 'firmware_' + model) is None:
            raise ValueError('Missing --firmware-' + model)
        if not (args.build_root / model / 'build/qemu-system-arm.exe').is_file():
            raise FileNotFoundError('Compile first: ' + model)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=output.name + '-assembling-', dir=output.parent) as work:
        stage = Path(work) / 'runtime'
        stage.mkdir()
        for name in ('Launch.cmd', '启动模拟器.cmd', 'LICENSE', 'NOTICE.md'):
            shutil.copy2(ROOT / name, stage / name)
        shutil.copytree(ROOT / 'models', stage / 'models', ignore=shutil.ignore_patterns(
            'userdata', 'artifacts', 'core', 'firmware', '__pycache__', '*.original'))
        shutil.copytree(ROOT / 'shared', stage / 'shared', ignore=shutil.ignore_patterns('__pycache__'))
        (stage / 'tools').mkdir()
        for name in ('Launcher.ps1', 'simulator.py', 'create_images.py'):
            shutil.copy2(ROOT / 'tools' / name, stage / 'tools' / name)
        # Python and LittleFS are build dependencies, not old emulator flash.
        py = stage / 'runtime/python'
        py.mkdir(parents=True)
        if args.python_embed:
            extract_zip(args.python_embed, py)
        else:
            shutil.copytree(args.python_runtime, py, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns('__pycache__'))
        if not (py / 'python.exe').is_file() or not (py / 'pythonw.exe').is_file():
            raise ValueError('Use the Windows x64 CPython 3.11 embeddable distribution')
        pth = py / 'python311._pth'
        if not pth.is_file():
            raise ValueError('Expected CPython 3.11 embeddable _pth configuration')
        pth.write_text('python311.zip\nDLLs\n.\n../../shared/python\nimport site\n', encoding='utf8')
        littlefs = stage / 'shared/factory-python'
        if args.littlefs_wheel:
            littlefs.mkdir()
            extract_zip(args.littlefs_wheel, littlefs)
        else:
            shutil.copytree(args.littlefs_package, littlefs,
                            ignore=shutil.ignore_patterns('__pycache__'))
        (stage / 'runtime/data/share/qemu').mkdir(parents=True)
        report = {'models': {}, 'fresh_storage': True, 'old_storage_imported': False}
        if args.intel_firmware:
            target = stage / 'shared/controller-firmware/intel'
            target.mkdir(parents=True)
            for name in ('ibt-1040-4150.sfi', 'ibt-1040-4150.ddc', 'LICENCE.intel'):
                shutil.copy2(args.intel_firmware / name, target / name)
        for model in selected:
            model_root = stage / 'models' / model
            # Stale generated manifests never become input to a new image.
            (model_root / 'config/factory/manifest.json').unlink(missing_ok=True)
            prop = getattr(args, 'factory_' + model)
            if prop:
                shutil.copy2(prop, model_root / 'config/factory/factory.prop')
            images = firmware(getattr(args, 'firmware_' + model), model_root / 'firmware')
            core = install(model, args.build_root / model / 'build/qemu-system-arm.exe',
                           args.toolchain / 'ucrt64/bin', stage)
            subprocess.run([str(py / 'python.exe'), '-X', 'utf8', '-B',
                            str(stage / 'tools/create_images.py'), '--model', model], check=True)
            report['models'][model] = {'core': core, 'firmware': images}
            subprocess.run([str(py / 'python.exe'), '-X', 'utf8', '-B',
                            str(stage / 'tools/simulator.py'), 'check', '--model', model], check=True)
        catalog = json.loads((stage / 'models/models.json').read_text('utf8'))
        for model in set(catalog) - set(selected):
            shutil.rmtree(stage / 'models' / model)
            del catalog[model]
        (stage / 'models/models.json').write_text(json.dumps(catalog, ensure_ascii=False, indent=2), encoding='utf8')
        (stage / 'build-report.json').write_text(json.dumps(report, indent=2), encoding='utf8')
        (stage / 'README.txt').write_text('Built runtime. Double-click Launch.cmd. Flash was generated from firmware and properties; no old save was imported.\n', encoding='utf8')
        stage.rename(output)
    print(json.dumps({'output': str(output), 'fresh_storage': True, 'models': list(selected)}))


if __name__ == '__main__':
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('--model', choices=('all', 'band11', 'band10pro'), default='all')
    p.add_argument('--build-root', type=Path, default=ROOT / '.build')
    p.add_argument('--toolchain', type=Path, default=Path('C:/msys64'))
    p.add_argument('--output', type=Path, default=ROOT / 'dist/windows')
    py = p.add_mutually_exclusive_group(required=True)
    py.add_argument('--python-embed', type=Path)
    py.add_argument('--python-runtime', type=Path)
    fs = p.add_mutually_exclusive_group(required=True)
    fs.add_argument('--littlefs-wheel', type=Path)
    fs.add_argument('--littlefs-package', type=Path)
    for model in ('band11', 'band10pro'):
        p.add_argument('--firmware-' + model, type=Path, help='Stock full OTA ZIP or extracted firmware directory')
        p.add_argument('--factory-' + model, type=Path, help='Optional private factory.prop; otherwise use synthetic demo')
    p.add_argument('--intel-firmware', type=Path, help='Optional validated Intel SFI/DDC and licence directory')
    assemble(p.parse_args())
