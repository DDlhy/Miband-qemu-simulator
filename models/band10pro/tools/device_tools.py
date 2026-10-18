"""Native device tools for p67cn: edit DID/PSN in seeds and the saved NV.

The flash journal is append-only. Never replace its header, AP or NAND data.
LittleFS is mounted explicitly without automatic formatting.
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
import struct
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parents[1] / 'shared/factory-python'))
from littlefs import LittleFS, UserContext

OFFSET, SIZE, NOR_SIZE = 0xfe0000, 0x20000, 0x1000000
SOURCE = 'config/factory/factory.prop'
JOURNAL = 'userdata/flash.journal'
CONFIG_FILES = (SOURCE, 'config/factory/nv.img', 'config/factory/nor-seed.bin',
                'config/factory/manifest.json')


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


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode('utf8')


def json_write(path, value):
    atomic_write(path, json_bytes(value))


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
    for key, expected in (('ro.factory.model', 'miwear.watch.p67cn'),
                          ('ro.factory.pid', '29598'), ('ro.factory.board_id', '20')):
        if key in result and result[key] != expected:
            raise ValueError('出厂配置不属于小米手环 10 Pro。')
    return result


def identity(raw):
    p = properties(raw)
    return {'did': p.get('ro.factory.did', ''), 'serial': p.get('ro.factory.psn', ''),
            'model': 'miwear.watch.p67cn', 'address': p.get('ro.factory.mac_bt', ''),
            'credential_present': bool(p.get('ro.factory.key'))}


def edited(raw, did, serial):
    before = properties(raw)
    changes = {'ro.factory.did': did, 'ro.factory.psn': serial}
    text = raw.decode('utf-8-sig')
    newline = '\r\n' if '\r\n' in text else '\n'
    seen, lines = set(), []
    for line in text.splitlines(keepends=True):
        key = line.partition('=')[0].strip()
        if key in changes and not line.lstrip().startswith(('#', ';')):
            end = '\r\n' if line.endswith('\r\n') else '\n' if line.endswith('\n') else ''
            line = key + '=' + changes[key] + end
            seen.add(key)
        lines.append(line)
    content = ''.join(lines)
    for key in changes:
        if key not in seen:
            if content and not content.endswith(('\r', '\n')):
                content += newline
            content += key + '=' + changes[key] + newline
    result = (b'\xef\xbb\xbf' if raw.startswith(b'\xef\xbb\xbf') else b'') + content.encode('utf8')
    after = properties(result)
    if {k: v for k, v in before.items() if k not in changes} != {
            k: v for k, v in after.items() if k not in changes}:
        raise ValueError('出厂属性保护校验失败。')
    return result


def mount(image):
    if len(image) != SIZE:
        raise ValueError('NV 出厂分区长度不正确。')
    fs = LittleFS(context=UserContext(buffer=bytearray(image)), mount=False,
                  block_size=8192, block_count=16, read_size=512, prog_size=512,
                  cache_size=512, lookahead_size=8, name_max=96)
    fs.mount()
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
        # Both paths are consumed by p67's stock factory import/recovery code.
        names = set(fs.listdir('/'))
        if 'factory.prop' not in names:
            raise ValueError('NV 中未找到 factory.prop。')
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
    if value['did'] != did or value['serial'] != serial:
        raise ValueError('NV 身份写入校验失败。')
    return result


def journal_nor(f, allow_pending=False):
    """Replay exactly the persisted format, validating every complete record."""
    f.seek(0)
    limit = os.fstat(f.fileno()).st_size
    header = f.read(64)
    if len(header) != 64 or header[:8] != b'MBFLASH1':
        raise ValueError('存档 journal 头无效。')
    nor_size, page_size, page_count, crc = struct.unpack_from('<IIII', header, 8)
    if nor_size != NOR_SIZE or page_size not in (2112, 2176) or not 0 < page_count <= 0x1000000 or page_count % 64:
        raise ValueError('存档几何参数不属于支持的 p67 存档。')
    nor = bytearray(f.read(nor_size))
    if len(nor) != nor_size or zlib.crc32(nor) != crc:
        raise ValueError('存档初始 NOR 校验失败。')
    while f.tell() < limit:
        remaining = limit - f.tell()
        if remaining < 16:
            if allow_pending:
                break
            raise ValueError('存档尾部未写完；请先正常启动并退出模拟器后重试。')
        record = f.read(16)
        kind, address, size, crc = struct.unpack('<IIII', record)
        valid = ((kind == 1 and 0 < size <= nor_size and address <= nor_size-size) or
                 (kind == 2 and address < page_count and size == page_size) or
                 (kind == 3 and address < page_count//64 and size == 0) or
                 (kind == 4 and address <= 255 and 26 <= size <= 64*1024*1024+26))
        if not valid:
            raise ValueError('存档记录类型或地址无效。')
        if size > limit-f.tell():
            if allow_pending:
                break
            raise ValueError('存档尾部未写完；请先正常启动并退出模拟器后重试。')
        payload = f.read(size)
        if zlib.crc32(payload, zlib.crc32(record[:12])) != crc:
            raise ValueError('存档记录 CRC 校验失败，未作修改。')
        if kind == 1:
            nor[address:address+size] = payload
    return nor


def append_nv(f, image):
    if len(image) != SIZE:
        raise ValueError('NV 长度不正确。')
    record = struct.pack('<III', 1, OFFSET, SIZE)
    f.seek(0, os.SEEK_END)
    f.write(record + struct.pack('<I', zlib.crc32(image, zlib.crc32(record))) + image)
    f.flush()
    os.fsync(f.fileno())
    if journal_nor(f)[OFFSET:OFFSET+SIZE] != image:
        raise ValueError('journal NV 写入校验失败。')


@contextmanager
def exclusive_journal(path):
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
        yield f


@contextmanager
def edit_guard(root):
    (root / 'userdata').mkdir(exist_ok=True)
    with (root / 'userdata/run.lock').open('a+b') as lock:
        lock.seek(0)
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            raise ValueError('模拟器正在运行，请先关闭模拟器窗口；传感器窗口可以保留。') from None
        try:
            with exclusive_journal(root / JOURNAL) as journal:
                yield journal
        finally:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


def inspect(root):
    root = Path(root)
    source = identity((root / SOURCE).read_bytes())
    saved = None
    path = root / JOURNAL
    if path.exists():
        with path.open('rb') as f:
            nv = journal_nor(f, allow_pending=True)[OFFSET:OFFSET+SIZE]
        saved = identity(partition_properties(nv))
    return {'ok': True, 'source': source, 'saved': saved, 'journal': str(path),
            'values_match': saved is None or all(source[k] == saved[k] for k in ('did', 'serial')),
            'backup_root': str(root / 'userdata/identity-backups')}


def prepare_config(root, did, serial):
    before = {name: (root / name).read_bytes() for name in CONFIG_FILES}
    manifest = json.loads(before[CONFIG_FILES[3]].decode('utf-8-sig'))
    if manifest.get('profile', {}).get('miwear_model') != 'miwear.watch.p67cn':
        raise ValueError('配置清单不属于小米手环 10 Pro。')
    seed = bytearray(before[CONFIG_FILES[2]])
    if len(seed) != NOR_SIZE:
        raise ValueError('NOR 种子长度不正确。')
    updated = edited(before[SOURCE], did, serial)
    nv = patch_partition(before[CONFIG_FILES[1]], did, serial)
    seed[OFFSET:OFFSET+SIZE] = patch_partition(seed[OFFSET:OFFSET+SIZE], did, serial)
    manifest.update(derived_sha256=sha(updated), nv_sha256=sha(nv),
                    nor_seed_sha256=sha(seed), property_keys=list(properties(updated)))
    overrides = manifest.setdefault('property_overrides', [])
    for key in ('ro.factory.did', 'ro.factory.psn'):
        if key not in overrides:
            overrides.append(key)
    manifest['identity_editor'] = {'tool': 'p67-device-tools-v1', 'editable': ['ro.factory.did', 'ro.factory.psn']}
    after = dict(zip(CONFIG_FILES, (updated, nv, bytes(seed), json_bytes(manifest))))
    return before, after


def rollback(root, journal, length, files):
    if journal is not None:
        journal.seek(length)
        journal.truncate()
        journal.flush()
        os.fsync(journal.fileno())
    for name, content in files.items():
        atomic_write(root / name, content)


def apply(root, did, serial):
    root = Path(root)
    if not isinstance(did, str) or not re.fullmatch(r'[0-9]{1,20}', did) or not 0 < int(did) < 2**64:
        raise ValueError('DID 请填写非零的十进制设备编号（64 位）。')
    if not isinstance(serial, str) or not re.fullmatch(r'[\x21-\x7e]{1,127}', serial):
        raise ValueError('序列号请填写 1～127 个非空格 ASCII 字符，可包含 /。')
    with edit_guard(root) as journal:
        before, after = prepare_config(root, did, serial)
        length = os.fstat(journal.fileno()).st_size if journal else None
        old_nv = bytes(journal_nor(journal)[OFFSET:OFFSET+SIZE]) if journal else None
        new_nv = patch_partition(old_nv, did, serial) if journal else None
        folder = root / 'userdata/identity-backups' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
        folder.mkdir(parents=True)
        for name, content in before.items():
            atomic_write(folder / name, content)
        if old_nv is not None:
            atomic_write(folder / 'journal-nv.before.img', old_nv)
        report = {'version': 1, 'model': 'miwear.watch.p67cn', 'state': 'prepared',
                  'created_at': datetime.datetime.now().astimezone().isoformat(),
                  'files': {name: {'before': sha(before[name]), 'after': sha(after[name])} for name in before},
                  'journal_size_before': length, 'journal_nv_before': sha(old_nv) if old_nv is not None else None,
                  'journal_nv_after': sha(new_nv) if new_nv is not None else None,
                  'seed_nv_after': sha(after[CONFIG_FILES[2]][OFFSET:OFFSET+SIZE])}
        json_write(folder / 'backup.json', report)
        try:
            if journal:
                append_nv(journal, new_nv)
            for name, content in after.items():
                atomic_write(root / name, content)
            report['state'] = 'applied'
            json_write(folder / 'backup.json', report)
        except BaseException:
            rollback(root, journal, length, before)
            report['state'] = 'rolled_back'
            json_write(folder / 'backup.json', report)
            raise
    return {'ok': True, 'backup': str(folder),
            'message': 'DID 和序列号已同步到启动配置、NV 种子与已有存档，下次启动生效。'}


def restore(root):
    root = Path(root)
    with edit_guard(root) as journal:
        folder = None
        for candidate in sorted((root / 'userdata/identity-backups').glob('*/backup.json'), reverse=True):
            report = json.loads(candidate.read_text('utf8'))
            if report.get('state') == 'applied' and report.get('model') == 'miwear.watch.p67cn':
                folder = candidate.parent
                break
        if folder is None:
            raise ValueError('没有可撤销的身份修改。')
        current, previous = {}, {}
        for name in CONFIG_FILES:
            current[name] = (root / name).read_bytes()
            previous[name] = (folder / name).read_bytes()
            if sha(current[name]) != report['files'][name]['after']:
                raise ValueError('出厂配置已被其他操作修改，不能覆盖；请打开备份目录核对。')
            if sha(previous[name]) != report['files'][name]['before']:
                raise ValueError('出厂配置备份校验失败。')
        length = os.fstat(journal.fileno()).st_size if journal else None
        old_nv = None
        if journal:
            current_nv = bytes(journal_nor(journal)[OFFSET:OFFSET+SIZE])
            expected = report['journal_nv_after'] or report['seed_nv_after']
            if sha(current_nv) != expected:
                raise ValueError('存档出厂区已发生其他变化，不能覆盖；请打开备份目录核对。')
            if report['journal_nv_before']:
                old_nv = (folder / 'journal-nv.before.img').read_bytes()
                if sha(old_nv) != report['journal_nv_before']:
                    raise ValueError('存档 NV 备份校验失败。')
            else:
                old_nv = previous[CONFIG_FILES[2]][OFFSET:OFFSET+SIZE]
        elif report['journal_nv_before']:
            raise ValueError('原存档已不在当前位置，不能自动撤销。')
        try:
            if journal:
                # Append only NV: later app/binding/NAND activity remains intact.
                append_nv(journal, old_nv)
            for name, content in previous.items():
                atomic_write(root / name, content)
            report['state'] = 'restored'
            json_write(folder / 'backup.json', report)
        except BaseException:
            rollback(root, journal, length, current)
            raise
    return {'ok': True, 'backup': str(folder), 'message': '已撤销上次身份修改，下次启动生效。'}


def invoke(root, request):
    tool = request.get('tool')
    if tool == 'identity.read':
        return inspect(root)
    if tool == 'identity.apply':
        return apply(root, request.get('did'), request.get('serial'))
    if tool == 'identity.restore':
        return restore(root)
    raise ValueError('未知工具。')


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('--root', type=Path, default=ROOT)
    p.add_argument('--request-file', type=Path, required=True)
    p.add_argument('--reply-file', type=Path, required=True)
    args = p.parse_args()
    try:
        result = invoke(args.root.resolve(), json.loads(args.request_file.read_text('utf-8-sig')))
    except Exception as exc:
        result = {'ok': False, 'error': str(exc) if isinstance(exc, ValueError) else
                  '工具执行失败（' + type(exc).__name__ + '），原有备份已保留。'}
    json_write(args.reply_file, result)


if __name__ == '__main__':
    main()
