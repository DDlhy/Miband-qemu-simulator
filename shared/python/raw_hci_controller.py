"""Exclusive controller transports. No Windows Bluetooth or BlueZ host stack."""
import ctypes as C
import os
import queue
import re
import socket
import sys
import threading

from raw_hci_codec import UsbPacketDecoder


class LinuxHci:
    def __init__(self, index):
        if not sys.platform.startswith('linux'):
            raise RuntimeError('HCI user channel requires Linux')
        self.socket = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
        try:
            # HCI_CHANNEL_USER requires the controller to be down and exclusive.
            self.socket.bind((index, 1))
            self.socket.settimeout(0.25)
        except BaseException:
            self.socket.close()
            raise

    def recv(self):
        try:
            return self.socket.recv(4097)
        except socket.timeout:
            return None

    def send(self, packet):
        if self.socket.send(packet) != len(packet):
            raise IOError('Partial HCI user-channel write')

    def close(self):
        self.socket.close()


class WindowsUsb:
    """Bluetooth USB class transport using a controller bound to WinUSB."""
    def __init__(self, device):
        if os.name != 'nt':
            raise RuntimeError('WinUSB requires Windows')
        if device['service'].lower() != 'winusb':
            raise RuntimeError('Controller is owned by ' + device['service'] + '; WinUSB binding required')
        api = winusb_api()
        self.api = api
        self.file = api['kernel'].CreateFileW(device['path'], 0xc0000000, 0, None, 3, 0x40000000, None)
        if self.file == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        self.handle = C.c_void_p()
        self.closed = False
        self.stop = threading.Event()
        self.packets = queue.Queue(maxsize=256)
        self.threads = []
        self.pipes = []
        self.write_lock = threading.Lock()
        self.intel_bootloader = False
        try:
            check(api['usb'].WinUsb_Initialize(self.file, C.byref(self.handle)))
            descriptor = api['Interface']()
            check(api['usb'].WinUsb_QueryInterfaceSettings(self.handle, 0, C.byref(descriptor)))
            if (descriptor.bInterfaceClass, descriptor.bInterfaceSubClass, descriptor.bInterfaceProtocol) != (0xe0, 1, 1):
                raise RuntimeError('Selected WinUSB interface is not Bluetooth HCI USB class')
            event_pipe = input_pipe = output_pipe = None
            for index in range(descriptor.bNumEndpoints):
                pipe = api['Pipe']()
                check(api['usb'].WinUsb_QueryPipe(self.handle, 0, index, C.byref(pipe)))
                self.pipes.append(pipe.PipeId)
                if pipe.PipeType == 3 and pipe.PipeId & 0x80:
                    event_pipe = pipe.PipeId
                elif pipe.PipeType == 2:
                    if pipe.PipeId & 0x80:
                        input_pipe = pipe.PipeId
                    else:
                        output_pipe = pipe.PipeId
                timeout = C.c_ulong(250)
                check(api['usb'].WinUsb_SetPipePolicy(self.handle, pipe.PipeId, 3, 4, C.byref(timeout)))
            if None in (event_pipe, input_pipe, output_pipe):
                raise RuntimeError('Bluetooth event/ACL USB endpoints are missing')
            self.output_pipe = output_pipe
            for pipe, kind in ((event_pipe, 4), (input_pipe, 2)):
                thread = threading.Thread(target=self.read_pipe, args=(pipe, kind), daemon=True)
                self.threads.append(thread)
                thread.start()
        except BaseException:
            self.close()
            raise

    def put(self, item):
        while not self.stop.is_set():
            try:
                self.packets.put(item, timeout=0.25)
                return
            except queue.Full:
                continue

    def read_pipe(self, pipe, kind):
        decoder = UsbPacketDecoder(kind)
        buffer = (C.c_ubyte * 4096)()
        try:
            while not self.stop.is_set():
                transferred = C.c_ulong()
                ok = self.api['usb'].WinUsb_ReadPipe(self.handle, pipe, buffer, len(buffer), C.byref(transferred), None)
                if not ok:
                    error = C.get_last_error()
                    if error == 121:  # PIPE_TRANSFER_TIMEOUT
                        continue
                    if self.stop.is_set():
                        return
                    raise C.WinError(error)
                if transferred.value:
                    # Intel boot ROM sends secure-download events on the ACL
                    # bulk endpoint. Operational firmware uses normal ACL.
                    selected_kind = 4 if kind == 2 and self.intel_bootloader else kind
                    if decoder.kind != selected_kind:
                        decoder = UsbPacketDecoder(selected_kind)
                    for packet in decoder.feed(bytes(buffer[:transferred.value])):
                        self.put(packet)
        except Exception as exc:
            self.put(exc)

    def recv(self):
        try:
            item = self.packets.get(timeout=0.25)
        except queue.Empty:
            return None
        if isinstance(item, Exception):
            raise item
        return item

    def send(self, packet):
        payload = packet[1:]
        buffer = (C.c_ubyte * len(payload)).from_buffer_copy(payload)
        transferred = C.c_ulong()
        with self.write_lock:
            secure_send = (packet[0] == 1 and self.intel_bootloader and
                           packet[1:3] == b'\x09\xfc')
            if packet[0] == 1 and not secure_send:
                setup = self.api['Setup'](0x20, 0, 0, 0, len(payload))
                ok = self.api['usb'].WinUsb_ControlTransfer(self.handle, setup, buffer, len(payload), C.byref(transferred), None)
            elif packet[0] == 2 or secure_send:
                ok = self.api['usb'].WinUsb_WritePipe(self.handle, self.output_pipe, buffer, len(payload), C.byref(transferred), None)
            else:
                raise ValueError('Only HCI Command/ACL USB output is supported')
            check(ok)
            if transferred.value != len(payload):
                raise IOError('Partial USB HCI write')

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.stop.set()
        if self.handle.value:
            for pipe in self.pipes:
                self.api['usb'].WinUsb_AbortPipe(self.handle, pipe)
        self.api['kernel'].CancelIoEx(self.file, None)
        for thread in self.threads:
            thread.join(timeout=2)
        if any(thread.is_alive() for thread in self.threads):
            # Do not free native handles while an I/O worker still uses them.
            raise RuntimeError('USB read worker did not stop after cancellation')
        if self.handle.value:
            self.api['usb'].WinUsb_Free(self.handle)
        self.api['kernel'].CloseHandle(self.file)


def check(ok):
    if not ok:
        raise C.WinError(C.get_last_error())


def winusb_api():
    from ctypes import wintypes as W

    class Interface(C.Structure):
        _pack_ = 1
        _fields_ = [(name, C.c_ubyte) for name in (
            'bLength', 'bDescriptorType', 'bInterfaceNumber', 'bAlternateSetting',
            'bNumEndpoints', 'bInterfaceClass', 'bInterfaceSubClass',
            'bInterfaceProtocol', 'iInterface')]

    class Pipe(C.Structure):
        _fields_ = [('PipeType', C.c_int), ('PipeId', C.c_ubyte),
                    ('MaximumPacketSize', C.c_ushort), ('Interval', C.c_ubyte)]

    class Setup(C.Structure):
        _pack_ = 1
        _fields_ = [('RequestType', C.c_ubyte), ('Request', C.c_ubyte),
                    ('Value', C.c_ushort), ('Index', C.c_ushort), ('Length', C.c_ushort)]

    kernel = C.WinDLL('kernel32', use_last_error=True)
    usb = C.WinDLL('winusb', use_last_error=True)
    kernel.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
    kernel.CreateFileW.restype = W.HANDLE
    kernel.CloseHandle.argtypes = [W.HANDLE]
    kernel.CancelIoEx.argtypes = [W.HANDLE, C.c_void_p]
    kernel.CloseHandle.restype = kernel.CancelIoEx.restype = W.BOOL
    signatures = {
        'WinUsb_Initialize': [W.HANDLE, C.POINTER(C.c_void_p)],
        'WinUsb_QueryInterfaceSettings': [C.c_void_p, C.c_ubyte, C.POINTER(Interface)],
        'WinUsb_QueryPipe': [C.c_void_p, C.c_ubyte, C.c_ubyte, C.POINTER(Pipe)],
        'WinUsb_SetPipePolicy': [C.c_void_p, C.c_ubyte, W.ULONG, W.ULONG, C.c_void_p],
        'WinUsb_ReadPipe': [C.c_void_p, C.c_ubyte, C.c_void_p, W.ULONG, C.POINTER(W.ULONG), C.c_void_p],
        'WinUsb_WritePipe': [C.c_void_p, C.c_ubyte, C.c_void_p, W.ULONG, C.POINTER(W.ULONG), C.c_void_p],
        'WinUsb_ControlTransfer': [C.c_void_p, Setup, C.c_void_p, W.ULONG, C.POINTER(W.ULONG), C.c_void_p],
        'WinUsb_AbortPipe': [C.c_void_p, C.c_ubyte],
        'WinUsb_Free': [C.c_void_p],
    }
    for name, signature in signatures.items():
        getattr(usb, name).argtypes = signature
        getattr(usb, name).restype = W.BOOL
    return dict(kernel=kernel, usb=usb, Interface=Interface, Pipe=Pipe, Setup=Setup)


def list_windows_usb():
    """Read-only enumeration: never change a driver or open a radio handle."""
    if os.name != 'nt':
        raise RuntimeError('USB inventory requires Windows')
    from ctypes import wintypes as W

    class Guid(C.Structure):
        _fields_ = [('Data1', W.DWORD), ('Data2', W.WORD), ('Data3', W.WORD), ('Data4', C.c_ubyte * 8)]

    class InterfaceData(C.Structure):
        _fields_ = [('cbSize', W.DWORD), ('InterfaceClassGuid', Guid),
                    ('Flags', W.DWORD), ('Reserved', C.c_size_t)]

    class DeviceData(C.Structure):
        _fields_ = [('cbSize', W.DWORD), ('ClassGuid', Guid),
                    ('DevInst', W.DWORD), ('Reserved', C.c_size_t)]

    # GUID_DEVINTERFACE_USB_DEVICE
    guid = Guid(0xa5dcbf10, 0x6530, 0x11d2, (C.c_ubyte * 8)(0x90, 0x1f, 0, 0xc0, 0x4f, 0xb9, 0x51, 0xed))
    setup = C.WinDLL('setupapi', use_last_error=True)
    setup.SetupDiGetClassDevsW.argtypes = [C.POINTER(Guid), W.LPCWSTR, W.HWND, W.DWORD]
    setup.SetupDiGetClassDevsW.restype = W.HANDLE
    setup.SetupDiEnumDeviceInterfaces.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(Guid), W.DWORD, C.POINTER(InterfaceData)]
    setup.SetupDiGetDeviceInterfaceDetailW.argtypes = [W.HANDLE, C.POINTER(InterfaceData), C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.POINTER(DeviceData)]
    setup.SetupDiGetDeviceRegistryPropertyW.argtypes = [W.HANDLE, C.POINTER(DeviceData), W.DWORD, C.POINTER(W.DWORD), C.c_void_p, W.DWORD, C.POINTER(W.DWORD)]
    setup.SetupDiDestroyDeviceInfoList.argtypes = [W.HANDLE]
    for name in ('SetupDiEnumDeviceInterfaces', 'SetupDiGetDeviceInterfaceDetailW',
                 'SetupDiGetDeviceRegistryPropertyW', 'SetupDiDestroyDeviceInfoList'):
        getattr(setup, name).restype = W.BOOL
    handle = setup.SetupDiGetClassDevsW(C.byref(guid), None, None, 0x12)
    if handle == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    devices = []
    try:
        index = 0
        while True:
            interface = InterfaceData()
            interface.cbSize = C.sizeof(interface)
            if not setup.SetupDiEnumDeviceInterfaces(handle, None, C.byref(guid), index, C.byref(interface)):
                if C.get_last_error() == 259:
                    break
                check(False)
            index += 1
            needed = W.DWORD()
            setup.SetupDiGetDeviceInterfaceDetailW(handle, C.byref(interface), None, 0, C.byref(needed), None)
            if C.get_last_error() != 122 or needed.value < 8:
                check(False)
            detail = C.create_string_buffer(needed.value)
            C.cast(detail, C.POINTER(W.DWORD))[0] = 8 if C.sizeof(C.c_void_p) == 8 else 6
            device = DeviceData()
            device.cbSize = C.sizeof(device)
            check(setup.SetupDiGetDeviceInterfaceDetailW(handle, C.byref(interface), detail, needed.value, None, C.byref(device)))
            path = C.wstring_at(C.addressof(detail) + 4)
            match = re.search(r'vid_([0-9a-f]{4})&pid_([0-9a-f]{4})', path, re.I)
            if not match:
                continue
            def property_value(key):
                data = C.create_string_buffer(4096)
                size = W.DWORD()
                typ = W.DWORD()
                if not setup.SetupDiGetDeviceRegistryPropertyW(handle, C.byref(device), key, C.byref(typ), data, len(data), C.byref(size)):
                    return ''
                return C.wstring_at(C.addressof(data))
            service = property_value(4)
            description = property_value(0)
            if service.lower() not in ('bthusb', 'winusb') and 'bluetooth' not in description.lower():
                continue
            devices.append({'path': path, 'vid': match[1].lower(), 'pid': match[2].lower(),
                            'service': service, 'description': description,
                            'raw_hci_ready': service.lower() == 'winusb'})
    finally:
        setup.SetupDiDestroyDeviceInfoList(handle)
    return devices
