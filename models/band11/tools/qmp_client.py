"""QMP client and local diagnostic capture; no Windows Bluetooth service."""
import datetime
import json
import socket
import struct

class Qmp:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.stream = self.sock.makefile('rwb')
        if 'QMP' not in json.loads(self.stream.readline()):
            raise RuntimeError('Missing QMP greeting')
        self.call('qmp_capabilities')

    def call(self, name, arguments=None):
        self.stream.write((json.dumps({'execute': name, 'arguments': arguments or {}}) + '\n').encode())
        self.stream.flush()
        while True:
            response = json.loads(self.stream.readline())
            if 'event' in response:
                continue
            if 'error' in response:
                raise RuntimeError(response['error'])
            return response['return']

    def close(self):
        self.stream.close()
        self.sock.close()


def capture_diagnostics(qmp, out, relative, reason):
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    folder = out / 'diagnostics' / stamp
    folder.mkdir(parents=True)
    target = relative + '/diagnostics/' + stamp
    # Use the supervisor's existing QMP connection. Guest memory is read only;
    # do not pause the AP or create another QMP client during authentication.
    qmp.call('screendump', {'filename': target + '/screen.ppm'})
    for address, length in ((0x20000000, 0x100000), (0x20100000, 0x60000)):
        qmp.call('human-monitor-command', {'command-line':
                 f'pmemsave {address:#x} {length:#x} "{target}/memory-{address:08x}-{length:x}.bin"'})
    state = {}
    for name, address, count in (('panel', 0x60000000, 9), ('gpu', 0x40223f00, 4)):
        filename = f'{name}-memory-{address:08x}-{count * 4:x}.bin'
        qmp.call('human-monitor-command', {'command-line':
                 f'pmemsave {address:#x} {count * 4:#x} "{target}/{filename}"'})
        state[name] = list(struct.unpack(f'<{count}I', (folder / filename).read_bytes()))
    state['registers'] = qmp.call('human-monitor-command', {'command-line': 'info registers'})
    (folder / 'state.json').write_text(json.dumps(state, indent=2), encoding='utf8')
    for name in ('radio-status.json', 'rfcomm-radio-status.json'):
        source = out / name
        if source.exists():
            (folder / name).write_bytes(source.read_bytes())
    receipt = {'captured_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'reason': reason, 'directory': str(folder), 'guest_memory_modified': False,
               'guest_paused': False}
    (folder / 'capture.json').write_text(json.dumps(receipt, indent=2), encoding='utf8')
    (out / 'latest-diagnostic.json').write_text(json.dumps(receipt, indent=2), encoding='utf8')
