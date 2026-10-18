"""Bounded H4 framing and BES initialization compatibility for a real controller."""
import struct


MAX_PACKET = 4096
# These controller-local initialization opcodes were observed in the stock AP.
# They configure BES RAM/analog hardware; never send them to another vendor.
BES_INITIALIZATION = frozenset((
    0xfc32, 0xfc71, 0xfc72, 0xfc77, 0xfc81, 0xfc82, 0xfc83, 0xfc84,
    0xfc86, 0xfc88, 0xfcb3, 0xfcb8, 0xfcc3, 0xfcc4, 0xfcc5,
    0xfccd, 0xfcfe, 0xfcff,
))


class H4Decoder:
    def __init__(self, allowed):
        self.allowed = set(allowed)
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        result = []
        while self.buffer:
            kind = self.buffer[0]
            if kind not in self.allowed:
                raise ValueError(f'Unsupported H4 packet type {kind}')
            header = 4 if kind == 1 else 5 if kind == 2 else 3
            if len(self.buffer) < header:
                break
            size = header + (struct.unpack_from('<H', self.buffer, 3)[0]
                             if kind == 2 else self.buffer[header - 1])
            if size > MAX_PACKET:
                raise ValueError(f'H4 packet exceeds QEMU transport limit: {size}')
            if len(self.buffer) < size:
                break
            result.append(bytes(self.buffer[:size]))
            del self.buffer[:size]
        return result

    def eof(self):
        if self.buffer:
            raise ValueError('Truncated H4 packet at transport EOF')


class UsbPacketDecoder:
    """USB endpoints omit the H4 type; transfers need not equal HCI packets."""
    def __init__(self, kind):
        self.kind = kind
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        packets = []
        header = 2 if self.kind == 4 else 4
        while len(self.buffer) >= header:
            size = header + (self.buffer[1] if self.kind == 4 else
                             struct.unpack_from('<H', self.buffer, 2)[0])
            if size + 1 > MAX_PACKET:
                raise ValueError('USB HCI packet exceeds QEMU transport limit')
            if len(self.buffer) < size:
                break
            packets.append(bytes([self.kind]) + bytes(self.buffer[:size]))
            del self.buffer[:size]
        return packets


def local_vendor_reply(packet):
    """Adapt hardware initialization only; all standard HCI goes to the radio."""
    if packet[0] != 1:
        return None
    opcode = struct.unpack_from('<H', packet, 1)[0]
    if opcode >> 10 != 0x3f:
        return None
    # Unknown vendor commands fail explicitly, rather than claiming support.
    status = 0 if opcode in BES_INITIALIZATION else 1
    if opcode in (0xfc32, 0xfc72) and packet[3] != (6 if opcode == 0xfc32 else 7):
        status = 0x12
    return b'\x04\x0e\x04\x01' + struct.pack('<H', opcode) + bytes([status])


def command_result(packet, opcode):
    if (len(packet) >= 7 and packet[:2] == b'\x04\x0e'
            and struct.unpack_from('<H', packet, 4)[0] == opcode):
        return packet[6:]
    return None
