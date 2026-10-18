"""Three BES UART character streams, owned by the existing supervisor."""
import collections
import socket
import time


class UartBridge:
    def __init__(self, out):
        self.out = out
        self.ports = []
        self.events = collections.deque(maxlen=80)
        self.sequence = 0
        for index in range(3):
            listener = socket.socket()
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.setblocking(False)
            self.ports.append(dict(listener=listener, connection=None, tx=0, rx=0,
                                   pending=bytearray(), port=listener.getsockname()[1]))

    def arguments(self):
        args = []
        for index, state in enumerate(self.ports):
            args += ['-chardev', f'socket,id=bes-uart{index},host=127.0.0.1,port={state["port"]}',
                     '-serial', f'chardev:bes-uart{index}']
        return args

    def event(self, index, direction, data):
        self.sequence += 1
        self.events.append(dict(seq=self.sequence, at=time.time(), uart=index,
                                direction=direction, hex=data.hex(' '), size=len(data)))

    def service(self):
        for index, state in enumerate(self.ports):
            connection = state['connection']
            if connection is None:
                try:
                    connection, _ = state['listener'].accept()
                    connection.setblocking(False)
                    state['connection'] = connection
                except BlockingIOError:
                    continue
            try:
                if state['pending']:
                    count = connection.send(state['pending'][:4096])
                    packet = bytes(state['pending'][:count])
                    del state['pending'][:count]
                    state['rx'] += count
                    if count:
                        self.event(index, 'RX', packet)
                for _ in range(8):
                    try:
                        data = connection.recv(4096)
                    except BlockingIOError:
                        break
                    if not data:
                        connection.close()
                        state['connection'] = None
                        break
                    state['tx'] += len(data)
                    with (self.out / f'uart{index}.log').open('ab') as stream:
                        stream.write(data)
                    self.event(index, 'TX', data)
            except (ConnectionError, OSError):
                connection.close()
                state['connection'] = None

    def send(self, index, data):
        if self.ports[index]['connection'] is None:
            raise ValueError('UART 尚未连接 QEMU。')
        if len(self.ports[index]['pending']) + len(data) > 65536:
            raise ValueError('UART RX 队列尚未发送完毕。')
        self.ports[index]['pending'].extend(data)
        self.service()

    def state(self):
        return dict(ports=[dict(uart=i, connected=s['connection'] is not None,
                               tx_bytes=s['tx'], rx_bytes=s['rx'], queued=len(s['pending']))
                           for i, s in enumerate(self.ports)], trace=list(self.events))

    def close(self):
        for state in self.ports:
            if state['connection']:
                state['connection'].close()
            state['listener'].close()
