"""Convert QEMU P6 screenshots to PNG using only the bundled standard library."""
import struct
import zlib


def ppm_to_png(source, destination):
    data = source.read_bytes()
    offset = 0
    tokens = []
    while len(tokens) < 4:
        while offset < len(data) and data[offset:offset+1].isspace():
            offset += 1
        if offset >= len(data):
            raise ValueError('Incomplete PPM header')
        if data[offset] == 35:
            end = data.find(b'\n', offset)
            if end < 0:
                raise ValueError('Incomplete PPM comment')
            offset = end + 1
            continue
        start = offset
        while offset < len(data) and not data[offset:offset+1].isspace():
            offset += 1
        tokens.append(data[start:offset])
    if tokens[0] != b'P6' or tokens[3] != b'255':
        raise ValueError('Expected 8-bit RGB P6 screenshot')
    width, height = int(tokens[1]), int(tokens[2])
    if not 0 < width <= 4096 or not 0 < height <= 4096:
        raise ValueError('Unexpected screenshot dimensions')
    if offset >= len(data) or not data[offset:offset+1].isspace():
        raise ValueError('Missing PPM raster separator')
    offset += 2 if data[offset:offset+2] == b'\r\n' else 1
    pixels = data[offset:]
    stride = width * 3
    if len(pixels) != stride * height:
        raise ValueError('Incomplete PPM raster')

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    raw = b''.join(b'\0' + pixels[row*stride:(row+1)*stride] for row in range(height))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 3)) + chunk(b'IEND', b''))
    destination.write_bytes(png)
