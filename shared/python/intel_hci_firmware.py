"""Load signed RAM firmware for the observed Intel 8087:0026 boot ROM."""
import hashlib
import json
from pathlib import Path
import struct
import time

from raw_hci_codec import command_result

FIRMWARE = Path(__file__).resolve().parents[1] / 'controller-firmware/intel'
HASHES = {'ibt-1040-4150.sfi': 'f2fe5f27866fa7f51d0817ce57d822810772e576a1896c55fe3fc65bfd315ff8',
          'ibt-1040-4150.ddc': 'fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077'}


def parse_tlv(data):
    result = {}
    position = 0
    while position < len(data):
        if position + 2 > len(data):
            raise ValueError('Truncated Intel version TLV')
        kind, size = data[position:position + 2]
        position += 2
        if position + size > len(data) or kind in result:
            raise ValueError('Invalid Intel version TLV')
        result[kind] = data[position:position + size]
        position += size
    return result


def prepare_intel(controller, device, output):
    if not device or (device['vid'], device['pid']) != ('8087', '0026'):
        return
    events = []
    receipt = {'firmware_written_to': 'controller volatile RAM',
               'windows_bluetooth_api_used': False, 'loaded': False}
    path = Path(output) / 'intel-firmware.json'

    def read():
        packet = controller.recv()
        if packet and packet[:2] == b'\x04\xff':
            events.append(packet)
        return packet

    def command(opcode, parameters=b''):
        controller.send(b'\x01' + struct.pack('<HB', opcode, len(parameters)) + parameters)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            packet = read()
            if packet is None:
                continue
            result = command_result(packet, opcode)
            if result is not None:
                if not result or result[0]:
                    raise RuntimeError(f'Intel HCI {opcode:04x} rejected: {result.hex()}')
                return result[1:]
        raise TimeoutError(f'Intel HCI {opcode:04x} timed out')

    def wait_vendor(kind):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            for index, packet in enumerate(events):
                if len(packet) > 3 and packet[3] == kind:
                    return events.pop(index)[4:]
            read()
        raise TimeoutError(f'Intel firmware vendor event {kind:02x} timed out')

    def secure(kind, data):
        for position in range(0, len(data), 252):
            command(0xfc09, bytes([kind]) + data[position:position + 252])

    try:
        tlv = parse_tlv(command(0xfc05, b'\xff'))
        image_type = int.from_bytes(tlv[0x1c], 'little')
        receipt['initial_image_type'] = image_type
        if image_type == 3:
            receipt['already_operational'] = True
            return
        cnvi = int.from_bytes(tlv[0x10], 'little')
        cnvr = int.from_bytes(tlv[0x11], 'little')
        hw_variant = (int.from_bytes(tlv[0x12], 'little') >> 16) & 0x3f
        sbe = int.from_bytes(tlv[0x2f], 'little')
        receipt.update(cnvi_top=f'{cnvi:08x}', cnvr_top=f'{cnvr:08x}',
                       hardware_variant=hw_variant, secure_boot_engine=sbe)
        if (image_type, cnvi, cnvr, hw_variant, sbe) != (1, 0x00080401, 0x01300504, 0x19, 0):
            raise RuntimeError('Unvalidated Intel boot ROM: no firmware was sent')
        blobs = {}
        for name, expected in HASHES.items():
            data = (FIRMWARE / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError('Intel signed firmware checksum failed: ' + name)
            blobs[name] = data
        firmware = blobs['ibt-1040-4150.sfi']
        if (len(firmware) < 964 or struct.unpack_from('<I', firmware, 8)[0] != 0x10000 or
                struct.unpack_from('<I', firmware, 652)[0] != 0x20000):
            raise ValueError('Invalid Intel RSA/ECDSA firmware headers')
        fragments = []
        position = start = 964
        boot_address = None
        while position < len(firmware):
            if position + 3 > len(firmware):
                raise ValueError('Truncated Intel firmware command')
            opcode, size = struct.unpack_from('<HB', firmware, position)
            end = position + 3 + size
            if end > len(firmware):
                raise ValueError('Intel firmware command exceeds image')
            if opcode == 0xfc0e:
                if size != 7 or boot_address is not None:
                    raise ValueError('Invalid Intel firmware boot parameters')
                boot_address = struct.unpack_from('<I', firmware, position + 3)[0]
            position = end
            if (position - start) % 4 == 0:
                fragments.append(firmware[start:position])
                start = position
        if start != len(firmware) or boot_address is None:
            raise ValueError('Incomplete Intel firmware payload')
        ddc = blobs['ibt-1040-4150.ddc']
        ddc_records = []
        position = 0
        while position < len(ddc):
            size = ddc[position] + 1
            if size < 3 or size > 255 or position + size > len(ddc):
                raise ValueError('Invalid Intel DDC record')
            ddc_records.append(ddc[position:position + size])
            position += size

        controller.intel_bootloader = True
        secure(0, firmware[:128])
        secure(3, firmware[128:384])
        secure(2, firmware[388:644])
        for fragment in fragments:
            secure(1, fragment)
        verification = wait_vendor(6)
        if len(verification) != 4 or verification[0] or verification[3]:
            raise RuntimeError('Physical Intel firmware verification failed: ' + verification.hex())
        receipt.update(controller_signature_verification_passed=True,
                       firmware_sha256=HASHES['ibt-1040-4150.sfi'],
                       boot_address=f'{boot_address:08x}')
        # Intel boot ROM acknowledges this reset with a real bootup event,
        # rather than a standard Command Complete. Do not synthesize one.
        parameters = b'\0\x01\0\x01' + struct.pack('<I', boot_address)
        controller.send(b'\x01\x01\xfc\x08' + parameters)
        bootup = wait_vendor(2)
        if len(bootup) != 6 or bootup[0]:
            raise RuntimeError('Invalid physical Intel firmware bootup event')
        controller.intel_bootloader = False
        for record in ddc_records:
            command(0xfc8b, record)
        receipt.update(loaded=True, physical_bootup_event_received=True,
                       ddc_records_applied=len(ddc_records))
    except BaseException as exc:
        receipt['error'] = repr(exc)
        raise
    finally:
        path.write_text(json.dumps(receipt, indent=2), encoding='utf8')
