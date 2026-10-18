"""Native sensor-window tools for the Q66 NFC factory identity.

Only DID and PSN are editable. Existing NOR is mounted without auto-format;
all changes are backed up and restricted to its facmode partition.
"""
import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import datetime
import hashlib
import json
import msvcrt
import os
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(ROOT.parents[1] / 'shared/factory-python'))
from littlefs import LittleFS, UserContext

OFFSET, SIZE, NOR_SIZE = 0xfc0000, 0x20000, 0x1000000
SOURCE = 'config/factory/factory.prop'
TARGETS = {'bluetooth': 'userdata/nand-bluetooth-nor.img',
           'standalone': 'userdata/nand-data-nor.img'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def atomic_write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


def json_write(path, data):
    atomic_write(path, (json.dumps(data, ensure_ascii=False, indent=2) + '\n').encode('utf8'))


def properties(raw):
    result = {}
    for number, line in enumerate(raw.decode('utf-8-sig').splitlines(), 1):
        if not line.strip() or line.lstrip().startswith(('#', ';')):
            continue
        key, sep, value = line.partition('=')
        key = key.strip()
        if not sep or not re.fullmatch(r'[A-Za-z0-9_.-]+', key) or '\0' in value or key in result:
            raise ValueError(f'出厂配置第 {number} 行格式无效。')
        result[key] = value
    for key, expected in (('ro.factory.model', 'miwear.watch.q66nfc'),
                          ('ro.factory.pid', '34835'), ('ro.factory.board_id', '1')):
        if key in result and result[key] != expected:
            raise ValueError('出厂配置不属于小米手环 11 NFC。')
    return result


def identity(raw):
    p = properties(raw)
    return {'did': p.get('ro.factory.did', ''), 'serial': p.get('ro.factory.psn', ''),
            'model': p.get('ro.factory.model', 'miwear.watch.q66nfc'),
            'address': p.get('ro.factory.mac_bt', ''),
            'credential_present': bool(p.get('ro.factory.psk'))}


def edited(raw, did, serial):
    before = properties(raw)
    changes = {'ro.factory.did': did, 'ro.factory.psn': serial}
    # Keep comments, calibration, credentials, and line endings verbatim.
    text = raw.decode('utf-8-sig')
    newline = '\r\n' if '\r\n' in text else '\n'
    seen = set()
    lines = []
    for line in text.splitlines(keepends=True):
        key = line.partition('=')[0].strip()
        if key in changes and not line.lstrip().startswith(('#', ';')):
            end = '\r\n' if line.endswith('\r\n') else '\n' if line.endswith('\n') else ''
            line = key + '=' + changes[key] + end
            seen.add(key)
        lines.append(line)
    content = ''.join(lines)
    for key in changes.keys() - seen:
        if content and not content.endswith(('\r', '\n')):
            content += newline
        content += key + '=' + changes[key] + newline
    result = (b'\xef\xbb\xbf' if raw.startswith(b'\xef\xbb\xbf') else b'') + content.encode('utf8')
    after = properties(result)
    assert {k: v for k, v in before.items() if k not in changes} == {
        k: v for k, v in after.items() if k not in changes}
    return result


def mount(image):
    if len(image) != SIZE:
        raise ValueError('出厂分区长度不正确。')
    fs = LittleFS(context=UserContext(buffer=bytearray(image)), mount=False,
                  block_size=8192, block_count=16, read_size=512, prog_size=512,
                  cache_size=512, lookahead_size=8)
    fs.mount()  # Never use the library's format-on-mount-failure default here.
    return fs


def partition_properties(image):
    fs = mount(image)
    try:
        with fs.open('factory.prop', 'rb') as f:
            return f.read()
    finally:
        fs.unmount()


def patch_partition(image, did, serial):
    fs = mount(image)
    try:
        names = set(fs.listdir('/'))
        for name in ('factory.prop', 'factory.prop.bkup'):
            if name not in names:
                continue
            with fs.open(name, 'rb') as f:
                raw = f.read()
            with fs.open(name, 'wb') as f:
                f.write(edited(raw, did, serial))
    finally:
        fs.unmount()
    result = bytes(fs.context.buffer)
    value = identity(partition_properties(result))
    assert value['did'] == did and value['serial'] == serial
    return result


def new_partition(raw):
    p = properties(raw)
    for key, value in (('ro.factory.board_id', '1'), ('persist.boardid', '1'),
                       ('ro.factory.model', 'miwear.watch.q66nfc'), ('ro.factory.pid', '34835')):
        p.setdefault(key, value)
    fs = LittleFS(block_size=8192, block_count=16, read_size=512, prog_size=512,
                  cache_size=512, lookahead_size=8)
    with fs.open('factory.prop', 'wb') as f:
        f.write(''.join(f'{k}={v}\n' for k, v in p.items()).encode('utf8'))
    fs.unmount()
    return bytes(fs.context.buffer)


def target_path(root, target):
    if target not in TARGETS:
        raise ValueError('请选择日常存档或独立固件存档。')
    return root / TARGETS[target]


def read_region(path):
    if path.stat().st_size != NOR_SIZE:
        raise ValueError('NOR 存档长度不正确。')
    with path.open('rb') as f:
        f.seek(OFFSET)
        return f.read(SIZE)


def inspect(root, target):
    root = Path(root)
    path = target_path(root, target)
    source = identity((root / SOURCE).read_bytes())
    saved = identity(partition_properties(read_region(path))) if path.exists() else None
    return {'ok': True, 'source': source, 'saved': saved, 'target': target,
            'nor': str(path), 'backup_root': str(root / 'userdata/identity-backups'),
            'values_match': saved is None or all(source[k] == saved[k] for k in ('did', 'serial'))}


def ensure_stopped(root):
    from sensor_controller import active_session
    if active_session(root)[0] is not None:
        raise ValueError('请先关闭此文件夹的模拟器窗口，再修改身份；传感器窗口可以保留。')


@contextmanager
def exclusive_nor(path):
    """Share mode zero also excludes QEMU launched outside our supervisor."""
    if not path.exists():
        yield None
        return
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                   ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    kernel.CreateFileW.restype = wintypes.HANDLE
    handle = kernel.CreateFileW(str(path), 0xc0000000, 0, None, 3, 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ValueError('存档正被模拟器或其他程序占用，请关闭后重试。')
    fd = msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY)
    with os.fdopen(fd, 'r+b') as f:
        if os.fstat(f.fileno()).st_size != NOR_SIZE:
            raise ValueError('NOR 存档长度不正确。')
        yield f


@contextmanager
def edit_guard(root, target):
    (root / 'userdata').mkdir(exist_ok=True)
    # Portable launcher holds this same lock for the complete emulator lifetime.
    with (root / 'userdata/running.lock').open('a+b') as f:
        f.seek(0)
        try:
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            raise ValueError('模拟器正在运行，请先关闭模拟器窗口。') from None
        try:
            ensure_stopped(root)
            with exclusive_nor(target_path(root, target)) as nor:
                yield nor
        finally:
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)


def refresh_package_hashes(root):
    manifest_path = root / 'manifest.json'
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text('utf-8-sig'))
        for entry in manifest.get('payload_files', []):
            if entry['path'] == SOURCE:
                raw = (root / SOURCE).read_bytes()
                entry.update(size=len(raw), sha256=sha(raw))
        json_write(manifest_path, manifest)
    checks = root / 'checksums.sha256'
    if checks.exists():
        lines = []
        for line in checks.read_text('utf8').splitlines():
            digest, sep, name = line.partition('  ')
            if sep and name in (SOURCE, 'manifest.json'):
                digest = sha((root / name).read_bytes())
                line = digest + sep + name
            lines.append(line)
        atomic_write(checks, ('\n'.join(lines) + '\n').encode('utf8'))


def write_region(nor, image):
    nor.seek(OFFSET)
    nor.write(image)
    nor.flush()
    os.fsync(nor.fileno())
    nor.seek(OFFSET)
    if nor.read(SIZE) != image:
        raise RuntimeError('出厂分区写入校验失败。')


def backup(root, target, source, region):
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    folder = root / 'userdata/identity-backups' / stamp
    folder.mkdir(parents=True)
    atomic_write(folder / 'factory.prop.before', source)
    if region is not None:
        atomic_write(folder / 'facmode.before.img', region)
    report = {'version': 1, 'model': 'miwear.watch.q66nfc', 'target': target,
              'created_at': datetime.datetime.now().astimezone().isoformat(), 'state': 'prepared',
              'source_before_sha256': sha(source),
              'region_before_sha256': sha(region) if region is not None else None}
    json_write(folder / 'backup.json', report)
    return folder, report


def apply(root, target, did, serial):
    root = Path(root)
    if not isinstance(did, str) or not re.fullmatch(r'[0-9]{1,20}', did) or not 0 < int(did) < 2**64:
        raise ValueError('DID 请填写非零的十进制设备编号（64 位）。')
    if not isinstance(serial, str) or not re.fullmatch(r'[\x21-\x7e]{1,127}', serial):
        raise ValueError('序列号请填写 1～127 个非空格 ASCII 字符，可包含 /。')
    source = root / SOURCE
    path = target_path(root, target)
    with edit_guard(root, target) as nor:
        old = source.read_bytes()
        updated = edited(old, did, serial)
        original_region = None
        if nor is not None:
            nor.seek(OFFSET)
            original_region = nor.read(SIZE)
            region = patch_partition(original_region, did, serial)
            new_nor = None
        else:
            region = new_partition(updated)
            ap = (root / 'firmware/vela_ap.bin').read_bytes()
            if len(ap) > 0xf00000:
                raise ValueError('AP 固件长度不正确。')
            new_nor = bytearray(b'\xff' * NOR_SIZE)
            new_nor[0xc0000:0xc0000+len(ap)] = ap
            new_nor[OFFSET:OFFSET+SIZE] = region
        # Validate both copies before creating a transaction or writing any state.
        assert identity(updated)['did'] == did
        folder, report = backup(root, target, old, original_region)
        original = source.with_name('factory.prop.original')
        if not original.exists():
            atomic_write(original, old)
        created_nor = False
        try:
            if nor is not None:
                write_region(nor, region)
            else:
                # Do not replace an image concurrently created by a launcher.
                with path.open('xb') as f:
                    created_nor = True
                    f.write(new_nor)
                    f.flush()
                    os.fsync(f.fileno())
            atomic_write(source, updated)
            refresh_package_hashes(root)
            report.update(state='applied', source_after_sha256=sha(updated), region_after_sha256=sha(region))
            json_write(folder / 'backup.json', report)
        except BaseException:
            if nor is not None:
                write_region(nor, original_region)
            elif created_nor:
                path.unlink()
            atomic_write(source, old)
            refresh_package_hashes(root)
            report['state'] = 'rolled_back'
            json_write(folder / 'backup.json', report)
            raise
    return {'ok': True, 'backup': str(folder), 'did': did, 'serial': serial,
            'message': 'DID 和序列号已写入配置与选中存档。下次启动生效，未清除绑定或用户数据。'}


def restore(root, target):
    root = Path(root)
    folder = None
    for candidate in sorted((root / 'userdata/identity-backups').glob('*/backup.json'), reverse=True):
        report = json.loads(candidate.read_text('utf8'))
        if report.get('state') == 'applied' and report.get('target') == target:
            folder = candidate.parent
            break
    if folder is None:
        raise ValueError('选中存档没有可撤销的身份修改。')
    source = root / SOURCE
    path = target_path(root, target)
    with edit_guard(root, target) as nor:
        current_source = source.read_bytes()
        if sha(current_source) != report['source_after_sha256']:
            raise ValueError('配置已被后续操作修改，不能覆盖；请从备份目录人工核对。')
        if nor is None:
            raise ValueError('选中存档已不存在，不能自动撤销。')
        nor.seek(OFFSET)
        current = nor.read(SIZE)
        if sha(current) != report['region_after_sha256']:
            raise ValueError('出厂区已发生其他变化，不能覆盖；请从备份目录人工核对。')
        old = (folder / 'factory.prop.before').read_bytes()
        if sha(old) != report['source_before_sha256']:
            raise ValueError('身份配置备份校验失败。')
        previous = (folder / 'facmode.before.img').read_bytes() if report['region_before_sha256'] else None
        if previous is not None and sha(previous) != report['region_before_sha256']:
            raise ValueError('出厂区备份校验失败。')
        # For a newly created NOR, keep its AP and restore the old source identity.
        # This avoids throwing away any subsequent OTA or user activity.
        replacement = previous if previous is not None else new_partition(old)
        try:
            write_region(nor, replacement)
            atomic_write(source, old)
            refresh_package_hashes(root)
            report['state'] = 'restored'
            json_write(folder / 'backup.json', report)
        except BaseException:
            write_region(nor, current)
            atomic_write(source, current_source)
            refresh_package_hashes(root)
            raise
    return {'ok': True, 'backup': str(folder), 'message': '已撤销上次身份修改，下次启动生效。'}


def invoke(root, request):
    target = request.get('target', 'bluetooth')
    if request.get('tool') == 'identity.read':
        return inspect(root, target)
    if request.get('tool') == 'identity.apply':
        return apply(root, target, request.get('did'), request.get('serial'))
    if request.get('tool') == 'identity.restore':
        return restore(root, target)
    raise ValueError('未知工具。')


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--request-file', required=True, type=Path)
    parser.add_argument('--reply-file', required=True, type=Path)
    args = parser.parse_args()
    try:
        result = invoke(ROOT, json.loads(args.request_file.read_text('utf-8-sig')))
    except Exception as exc:
        # No property data or traceback is copied to the UI/log.
        result = {'ok': False, 'error': str(exc) if isinstance(exc, ValueError) else
                  '工具执行失败（' + type(exc).__name__ + '），原有备份已保留。'}
    json_write(args.reply_file, result)


if __name__ == '__main__':
    main()
