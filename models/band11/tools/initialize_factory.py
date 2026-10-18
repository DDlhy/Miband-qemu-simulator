"""Create an emulator factory partition from the stock AP's Q66 NFC profile.

Without an input property file this contains only simulated board configuration.
A supplied unit factory file is preserved separately and loaded as properties;
the optional Bluetooth address maps the QR to the bridge's controller identity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parents[1] / 'shared/factory-python'))
from littlefs import LittleFS


def read_factory_properties(path):
    """Parse property data only; never evaluate or print credential values."""
    raw = Path(path).read_bytes()
    properties = {}
    for number, line in enumerate(raw.decode('utf-8-sig').splitlines(), 1):
        if not line.strip() or line.lstrip().startswith(('#', ';')):
            continue
        key, separator, value = line.partition('=')
        key = key.strip()
        if not separator or not re.fullmatch(r'[A-Za-z0-9_.-]+', key) or '\0' in value:
            raise ValueError(f'Invalid factory property at line {number}')
        if key in properties:
            raise ValueError(f'Duplicate factory property: {key}')
        properties[key] = value
    return raw, properties


def create_factory(out, bluetooth_address=None, factory_prop=None):
    ap = (ROOT / 'firmware/vela_ap.bin').read_bytes()
    begin = ap.index(b'{\n    "product_info": [')
    products, _ = json.JSONDecoder().raw_decode(ap[begin:begin + 4096].decode('ascii', 'replace'))
    profile = next(p for p in products['product_info'] if p['board_id'] == 1)
    assert profile['miwear_model'] == 'miwear.watch.q66nfc'
    assert profile['miwear_pid'] == '34835' and profile['isnfc']
    # Live bes_partition_init registers facmode at first block 32256,
    # with 256 blocks of 512 bytes. /misc mounts it read-only.
    block_size, block_count = 8192, 16
    defaults = {'ro.factory.board_id': '1', 'persist.boardid': '1',
                'ro.factory.model': profile['miwear_model'],
                'ro.factory.pid': profile['miwear_pid']}
    source_raw = None
    overrides = []
    if factory_prop is not None:
        source_raw, properties = read_factory_properties(factory_prop)
        for key, expected in defaults.items():
            if key in properties and properties[key] != expected:
                raise ValueError(f'Factory property {key} does not match the Q66 NFC profile')
            properties.setdefault(key, expected)
    else:
        properties = defaults.copy()
    if bluetooth_address is not None:
        if not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', bluetooth_address):
            raise ValueError('Bluetooth address must contain six colon-separated hex octets')
        bluetooth_address = bluetooth_address.upper()
        if properties.get('ro.factory.mac_bt') != bluetooth_address:
            overrides.append('ro.factory.mac_bt')
        properties['ro.factory.mac_bt'] = bluetooth_address
    content = '\n'.join(f'{key}={value}' for key, value in properties.items()) + '\n'
    fs = LittleFS(block_size=block_size, block_count=block_count,
                  read_size=512, prog_size=512, cache_size=512, lookahead_size=8)
    with fs.open('factory.prop', 'w') as stream:
        stream.write(content)
    fs.unmount()
    image = bytes(fs.context.buffer)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    path = out / 'q66nfc.img'
    if path.exists() and path.read_bytes() != image:
        raise RuntimeError('Existing factory fixture differs; preserve it before replacing')
    path.write_bytes(image)
    if source_raw is not None:
        preserved = out / 'factory.prop.original'
        if preserved.exists() and preserved.read_bytes() != source_raw:
            raise RuntimeError('Existing original factory properties differ; preserve them first')
        preserved.write_bytes(source_raw)
    (out / 'factory.prop').write_text(content, encoding='utf8')
    report = {'source': ('User-provided unit factory properties' if source_raw is not None else
                         'Simulated factory board configuration selected from the unchanged AP JSON'),
              'ap_sha256': hashlib.sha256(ap).hexdigest(), 'profile': profile,
              'partition_offset': '0xfc0000', 'load_address': '0x2cfc0000',
              'size': len(image), 'littlefs_block_size': block_size,
              'littlefs_block_count': block_count,
              'image_sha256': hashlib.sha256(image).hexdigest(),
              'contains_real_device_credentials': source_raw is not None and bool(properties.get('ro.factory.psk')),
              'contains_device_identity': bool(properties.get('ro.factory.did')),
              'contains_serial_number': bool(properties.get('ro.factory.psn')),
              'source_property_file': str(Path(factory_prop).resolve()) if factory_prop is not None else None,
              'source_property_sha256': hashlib.sha256(source_raw).hexdigest() if source_raw is not None else None,
              'property_keys': list(properties), 'property_overrides': overrides,
              'bluetooth_address': bluetooth_address,
              'qemu_load_address': '0x2cfc0000'}
    (out / 'manifest.json').write_text(json.dumps(report, indent=2), encoding='utf8')
    return report


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'artifacts/factory')
    parser.add_argument('--bt-address', help='Emulator controller identity, not a real band factory dump')
    parser.add_argument('--factory-prop', type=Path, help='Read a user-supplied factory property file')
    args = parser.parse_args()
    report = create_factory(args.output, args.bt_address, args.factory_prop)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
