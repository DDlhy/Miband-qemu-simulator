"""Import p67 factory properties as data; keep credentials out of reports."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parents[1] / 'shared/factory-python'))

NV_OFFSET = 0xfe0000
NV_SIZE = 0x20000
TARGET_ADDRESS = '02:00:00:10:00:01'


def read_properties(path):
    raw = Path(path).read_bytes()
    props = {}
    for number, line in enumerate(raw.decode('utf-8-sig').splitlines(), 1):
        if not line.strip() or line.lstrip().startswith(('#', ';')):
            continue
        key, sep, value = line.partition('=')
        key = key.strip()
        if not sep or not re.fullmatch(r'[A-Za-z0-9_.-]+', key) or '\0' in value:
            raise ValueError(f'Invalid factory property at line {number}')
        if key in props:
            raise ValueError(f'Duplicate factory property at line {number}')
        props[key] = value
    return raw, props


def create_factory(source, out, address):
    from littlefs import LittleFS
    raw, props = read_properties(source)
    for key in ('ro.factory.did', 'ro.factory.psn', 'ro.factory.key', 'ro.factory.sku'):
        if not props.get(key):
            raise ValueError(f'Missing factory property: {key}')
    if not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', address):
        raise ValueError('Invalid Bluetooth address')
    ap = (ROOT / 'firmware/vela_ap.bin').read_bytes()
    start = ap.index(b'{\n    "product_info": [')
    products, _ = json.JSONDecoder().raw_decode(ap[start:start+4096].decode('ascii', 'replace'))
    profile = next(p for p in products['product_info'] if p['board_id'] == 20)
    if profile['miwear_model'] != 'miwear.watch.p67cn':
        raise ValueError('Wrong firmware profile')
    for key, expected in (('ro.factory.board_id', '20'),
                          ('ro.factory.model', profile['miwear_model']),
                          ('ro.factory.pid', profile['miwear_pid'])):
        if key in props and props[key] != expected:
            raise ValueError(f'Factory property conflicts with p67cn: {key}')
    overrides = []
    if props.get('ro.factory.mac_bt') != address.upper():
        overrides.append('ro.factory.mac_bt')
    props['ro.factory.mac_bt'] = address.upper()
    source_keys = list(props)
    # Unit-specific sensor/ADC/oscillator calibration and manufacturing
    # flags describe physical hardware absent from this board model. Keep
    # identity and the GNSS selector implemented by the local UART backend.
    identity_keys = ('ro.factory.did', 'ro.factory.psn', 'ro.factory.key',
                     'ro.factory.mac_bt', 'ro.factory.sku')
    if props.get('ro.factory.gnss.chip') == 'AG3335M':
        identity_keys += ('ro.factory.gnss.chip',)
    props = {key: props[key] for key in identity_keys}
    content = ''.join(f'{key}={value}\n' for key, value in props.items()).encode('utf8')
    # p67's running LittleFS is built with LFS_NAME_MAX=96.  The
    # library default (255) is rejected by its mount operation.
    fs = LittleFS(block_size=8192, block_count=16, read_size=512,
                  prog_size=512, cache_size=512, lookahead_size=8, name_max=96)
    for name in ('factory.prop', 'factory.prop.bkup'):
        with fs.open(name, 'wb') as f:
            f.write(content)
    fs.unmount()
    nv = bytes(fs.context.buffer)
    nor = bytearray(b'\xff' * 0x1000000)
    nor[NV_OFFSET:NV_OFFSET+NV_SIZE] = nv
    out.mkdir(parents=True, exist_ok=False)
    (out/'factory.prop.original').write_bytes(raw)
    (out/'factory.prop').write_bytes(content)
    (out/'nv.img').write_bytes(nv)
    (out/'nor-seed.bin').write_bytes(nor)
    report = {
        'source_property_file': str(source.resolve()),
        'source_sha256': hashlib.sha256(raw).hexdigest(),
        'derived_sha256': hashlib.sha256(content).hexdigest(),
        'ap_sha256': hashlib.sha256(ap).hexdigest(),
        'profile': profile, 'source_sku': props['ro.factory.sku'],
        'property_keys': list(props), 'property_overrides': overrides,
        'import_policy': 'identity-and-confirmed-gnss-chip',
        'source_property_keys': source_keys,
        'not_imported_hardware_properties': [k for k in source_keys if k not in props],
        'identity_present': all(bool(props[k]) for k in
                                ('ro.factory.did', 'ro.factory.psn', 'ro.factory.key')),
        'bluetooth_address': props['ro.factory.mac_bt'],
        'partition': 'nv', 'mount_point': '/misc',
        'partition_offset': hex(NV_OFFSET), 'size': NV_SIZE,
        'littlefs_block_size': 8192, 'littlefs_block_count': 16,
        'littlefs_name_max': 96,
        'nv_sha256': hashlib.sha256(nv).hexdigest(),
        'nor_seed_sha256': hashlib.sha256(nor).hexdigest(),
        'original_firmware_modified': False,
    }
    (out/'manifest.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--factory-prop', type=Path, default=ROOT/'config/factory/factory.prop')
    parser.add_argument('--output', type=Path, default=ROOT/'config/factory')
    parser.add_argument('--bt-address', default=TARGET_ADDRESS)
    args = parser.parse_args()
    print(json.dumps(create_factory(args.factory_prop, args.output, args.bt_address), indent=2))
