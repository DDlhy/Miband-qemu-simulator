"""Capture UART text while removing imported factory credential values."""
import base64
import socket
import threading
import collections
import time
import codecs


class SafeSerialLog:
    def __init__(self, path, properties):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(.25)
        self.port = self.listener.getsockname()[1]
        self.stream = path.open('w', encoding='utf8')
        self.stopped = threading.Event()
        self.connection = None
        self.tx_bytes = self.rx_bytes = 0
        self.events = collections.deque(maxlen=40)
        self.event_lock = threading.Lock()
        self.sequence = 0
        self.observer = None
        self.decoder = codecs.getincrementaldecoder('utf8')('replace')
        self.secrets, self.byte_secrets = self.credential_patterns(properties)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    @staticmethod
    def credential_patterns(properties):
        """Shared redaction patterns for UART streams and offline RAM captures."""
        secrets = set()
        raw_secrets = set()
        for key, value in properties.items():
            if not value or not (key.endswith(('.key', '.psk', '.token', '.secret'))):
                continue
            secrets.add(value)
            if len(value) >= 8:
                secrets.update((value.lower(), value.upper()))
                try:
                    data = bytes.fromhex(value)
                except ValueError:
                    data = b''
                if data:
                    raw_secrets.add(data)
                    secrets.update((data.hex(' '), data.hex(' ').upper(),
                                    base64.b64encode(data).decode('ascii')))
        return (sorted(secrets, key=len, reverse=True),
                sorted({s.encode('utf8') for s in secrets} | raw_secrets,
                       key=len, reverse=True))

    def redact(self, data):
        for secret in self.byte_secrets:
            data = data.replace(secret, b'[factory credential redacted]')
        return data

    def safe_end(self, data):
        # Keep a possible credential prefix across recv calls, including binary
        # credentials containing a newline. Do not split a complete match either.
        end = len(data)
        for secret in self.byte_secrets:
            for n in range(min(len(data), len(secret)-1), 0, -1):
                if data.endswith(secret[:n]):
                    end = min(end, len(data)-n)
                    break
        while True:
            previous = end
            for secret in self.byte_secrets:
                start = data.find(secret, max(0, end-len(secret)+1))
                if 0 <= start < end < start+len(secret):
                    end = start
            if end == previous:
                return end

    def write(self, data):
        data = self.redact(bytes(data))
        text = self.decoder.decode(data)
        self.stream.write(text)
        self.stream.flush()
        # HEX retains actual bytes, including non-UTF8 values. Both views redact.
        with self.event_lock:
            self.sequence += 1
            self.events.append(dict(seq=self.sequence,at=time.time(),direction='TX',
                                    hex=data.hex(' '),text=text.rstrip()[:512]))

    def run(self):
        pending = bytearray()
        try:
            while not self.stopped.is_set():
                try:
                    self.connection, _ = self.listener.accept()
                    break
                except socket.timeout:
                    continue
                except OSError:
                    return
            if self.connection is None:
                return
            self.connection.settimeout(.25)
            while True:
                try:
                    chunk = self.connection.recv(8192)
                except socket.timeout:
                    if self.stopped.is_set():break
                    continue
                except OSError:
                    break
                if not chunk:break
                self.tx_bytes += len(chunk)
                if self.observer is not None:
                    self.observer(bytes(chunk))
                pending.extend(chunk)
                end = self.safe_end(pending)
                if end:
                    self.write(pending[:end])
                    del pending[:end]
        finally:
            if pending:self.write(pending)
            self.stream.write(self.decoder.decode(b'', final=True))
            if self.connection:self.connection.close()
            self.stream.close()

    def close(self):
        self.stopped.set()
        self.listener.close()
        self.thread.join(timeout=3)

    def send(self, data):
        if self.connection is None or self.stopped.is_set():
            raise ValueError('UART 尚未连接。')
        self.connection.sendall(data)
        self.rx_bytes += len(data)
        with self.event_lock:
            self.sequence += 1
            # Redact RX text too, before serializing any history.
            redacted=self.redact(data)
            value=redacted.decode('utf8','replace')
            self.events.append(dict(seq=self.sequence,at=time.time(),direction='RX',
                                    hex=redacted.hex(' '),text=value[:512]))

    def state(self, index):
        with self.event_lock:
            return dict(uart=index,connected=self.connection is not None and not self.stopped.is_set(),
                        tx_bytes=self.tx_bytes,rx_bytes=self.rx_bytes,queued=0,
                        trace=list(self.events))
