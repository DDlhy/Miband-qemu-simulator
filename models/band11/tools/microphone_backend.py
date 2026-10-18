"""WinMM input -> stock BES CODEC PCM. Captures only while guest ADC runs."""
import ctypes as c
from ctypes import wintypes as w
import math
import struct


class WaveFormat(c.Structure):
    _fields_ = [('tag', w.WORD), ('channels', w.WORD), ('rate', w.DWORD),
                ('bytes_second', w.DWORD), ('align', w.WORD), ('bits', w.WORD), ('extra', w.WORD)]


class WaveCaps(c.Structure):
    _fields_ = [('mid', w.WORD), ('pid', w.WORD), ('version', w.DWORD),
                ('name', w.WCHAR * 32), ('formats', w.DWORD), ('channels', w.WORD), ('reserved', w.WORD)]


class WaveHeader(c.Structure):
    _fields_ = [('data', c.c_void_p), ('length', w.DWORD), ('recorded', w.DWORD),
                ('user', c.c_size_t), ('flags', w.DWORD), ('loops', w.DWORD),
                ('next', c.c_void_p), ('reserved', c.c_size_t)]


class MicrophoneRouter:
    def __init__(self):
        self.api = c.WinDLL('winmm')
        self.api.waveInOpen.argtypes = [c.POINTER(c.c_void_p), w.UINT, c.POINTER(WaveFormat), c.c_size_t, c.c_size_t, w.DWORD]
        self.api.waveInGetDevCapsW.argtypes = [c.c_size_t, c.POINTER(WaveCaps), w.UINT]
        for name in ('waveInPrepareHeader', 'waveInUnprepareHeader', 'waveInAddBuffer'):
            getattr(self.api, name).argtypes = [c.c_void_p, c.POINTER(WaveHeader), w.UINT]
        for name in ('waveInStart', 'waveInStop', 'waveInReset', 'waveInClose'):
            getattr(self.api, name).argtypes = [c.c_void_p]
        self.handle = None
        self.buffers = []
        self.enabled = True
        self.device = -1
        self.error = None
        self.samples = 0
        self.level = 0.
        self.devices = [dict(id=-1, name='系统默认录音设备')]
        for index in range(self.api.waveInGetNumDevs()):
            caps = WaveCaps()
            if not self.api.waveInGetDevCapsW(index, c.byref(caps), c.sizeof(caps)):
                self.devices.append(dict(id=index, name=caps.name))

    def check(self, code, operation):
        if code:
            text = c.create_unicode_buffer(256)
            self.api.waveInGetErrorTextW(code, text, len(text))
            raise OSError(f'{operation}: {text.value} ({code})')

    def configure(self, enabled, device):
        if self.enabled != enabled or self.device != device:
            self.close()
        self.enabled, self.device, self.error = enabled, device, None

    def open(self):
        handle = c.c_void_p()
        fmt = WaveFormat(1, 1, 16000, 32000, 2, 16, 0)
        self.check(self.api.waveInOpen(c.byref(handle), self.device & 0xffffffff, c.byref(fmt), 0, 0, 0), '打开录音输入')
        self.handle = handle
        try:
            for _ in range(8):
                data = c.create_string_buffer(1280)  # 40 ms mono PCM
                header = WaveHeader(data=c.cast(data, c.c_void_p).value, length=len(data))
                self.check(self.api.waveInPrepareHeader(handle, c.byref(header), c.sizeof(header)), '准备录音缓冲')
                self.buffers.append((data, header))
                self.check(self.api.waveInAddBuffer(handle, c.byref(header), c.sizeof(header)), '加入录音缓冲')
            self.check(self.api.waveInStart(handle), '开始录音')
        except Exception:
            self.close()
            raise

    def service(self, qmp, audio):
        active = bool(audio.get('capture_active'))
        if not self.enabled or not active:
            if self.handle:
                self.close()
            return
        if self.error:
            return
        try:
            if not self.handle:
                self.open()
            for data, header in self.buffers:
                if header.flags & 1:
                    pcm = data.raw[:header.recorded & ~1]
                    if pcm:
                        values = struct.unpack('<' + 'h' * (len(pcm)//2), pcm)
                        self.level = math.sqrt(sum(x*x for x in values) / len(values)) / 32768
                        qmp.call('qom-set', dict(path='/machine', property='audio-pcm', value=pcm.hex()))
                        self.samples += len(values)
                    header.recorded = 0
                    self.check(self.api.waveInAddBuffer(self.handle, c.byref(header), c.sizeof(header)), '重用录音缓冲')
        except Exception as exc:
            self.error = str(exc)
            self.close()

    def state(self):
        return dict(enabled=self.enabled, device=self.device, devices=self.devices,
                    capturing=self.handle is not None, error=self.error, samples=self.samples, level=self.level)

    def close(self):
        if self.handle:
            self.api.waveInStop(self.handle)
            self.api.waveInReset(self.handle)
            for _, header in self.buffers:
                self.api.waveInUnprepareHeader(self.handle, c.byref(header), c.sizeof(header))
            self.api.waveInClose(self.handle)
        self.handle = None
        self.buffers = []
        self.level = 0.
