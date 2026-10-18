"""Real HCI controller to QEMU H4 relay; never import a Windows Bluetooth API."""
import argparse
import asyncio
from collections import defaultdict, deque
from contextlib import nullcontext
import datetime
import json
import os
from pathlib import Path
import struct
import sys
import time
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parent))
from raw_hci_codec import H4Decoder, command_result, local_vendor_reply
from raw_hci_controller import LinuxHci, WindowsUsb, list_windows_usb
from intel_hci_firmware import prepare_intel


def query(controller, opcode, parameters=b''):
    controller.send(b'\x01' + struct.pack('<H', opcode) + bytes([len(parameters)]) + parameters)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        packet = controller.recv()
        if packet is None:
            continue
        result = command_result(packet, opcode)
        if result is not None:
            if not result or result[0]:
                raise RuntimeError(f'Real controller rejected HCI {opcode:04x}: {result.hex()}')
            return result[1:]
    raise TimeoutError(f'Real controller did not answer HCI {opcode:04x}')


def identity(controller):
    query(controller, 0x0c03)
    version = query(controller, 0x1001)
    address = query(controller, 0x1009)
    features = query(controller, 0x1003)
    if len(version) != 8 or len(address) != 6 or len(features) != 8:
        raise RuntimeError('Invalid physical HCI identity response')
    if not features[4] & 0x40 or features[4] & 0x20:
        raise RuntimeError('A dual-mode BR/EDR + LE controller is required')
    return {'controller_address': ':'.join(f'{b:02X}' for b in address[::-1]),
            'hci_version': version[0], 'manufacturer_id': int.from_bytes(version[4:6], 'little'),
            'dual_mode_controller': True}


def configure_address(controller, info, address):
    """Set the Intel runtime public address and verify via Read_BD_ADDR."""
    if address is None:
        return info
    try:
        octets = bytes.fromhex(address.replace(':', ''))
    except ValueError:
        raise ValueError('Invalid requested controller address') from None
    if len(octets) != 6 or len(address.split(':')) != 6:
        raise ValueError('Invalid requested controller address')
    requested = ':'.join(f'{b:02X}' for b in octets)
    before = info['controller_address']
    if before != requested:
        if info['manufacturer_id'] != 2:
            raise RuntimeError('Runtime public address programming requires the Intel controller')
        query(controller, 0xfc31, octets[::-1])
    actual = query(controller, 0x1009)
    if actual != octets[::-1]:
        raise RuntimeError('Physical controller address does not match the factory identity')
    return {**info, 'controller_address_before': before, 'controller_address': requested,
            'physical_address_verified': True}


class RawRelay:
    def __init__(self, controller, out, info):
        self.controller = controller
        self.out = Path(out)
        self.info = info
        self.logfile = (self.out / 'raw-hci.jsonl').open('a', encoding='utf8', buffering=1)
        self.report = {'transport': 'exclusive physical controller HCI',
                       'windows_bluetooth_api_used': False,
                       'application_protocol_translated': False,
                       'physical_controller_verified': True, **info,
                       'state': 'ready', 'guest_commands': 0, 'guest_acl_packets': 0,
                       'controller_events': 0, 'controller_acl_packets': 0,
                       'controller_accepted_le_advertising': False,
                       'controller_accepted_extended_advertising': {},
                       'controller_accepted_classic_scan_enable': 0,
                       'bes_initialization_adapted': 0,
                       'status_save_failures': 0, 'status_save_deferred': False,
                       'telemetry_log_write_failures': 0,
                       'authentication_or_connection_success_synthesized': False}
        self.writer = None
        self.finished = asyncio.Event()
        self.tasks = set()
        self.used = False
        self.pending_settings = defaultdict(deque)
        self.next_save_at = 0
        self.save(force=True)

    def save(self, force=False):
        now = time.monotonic()
        if not force and now < self.next_save_at:
            return False
        # A Windows reader may deny delete sharing. Retry on the next poll;
        # optional diagnostics must never interrupt physical HCI traffic.
        self.next_save_at = now + 0.5
        target = self.out / 'raw-hci-status.json'
        temporary = target.with_suffix('.tmp')
        was_deferred = self.report['status_save_deferred']
        self.report['status_save_deferred'] = False
        try:
            temporary.write_text(json.dumps(self.report, indent=2, ensure_ascii=False), encoding='utf8')
            temporary.replace(target)
        except OSError as exc:
            self.report['status_save_deferred'] = True
            self.report['status_save_failures'] += 1
            self.report['status_save_last_error'] = str(exc)
            if not was_deferred:
                self.log('status-save-deferred', error=str(exc))
            return False
        if was_deferred:
            self.log('status-save-recovered')
        return True

    def log(self, kind, **values):
        try:
            self.logfile.write(json.dumps({'time': datetime.datetime.now().astimezone().isoformat(),
                                          'kind': kind, **values}, ensure_ascii=False) + '\n')
        except OSError as exc:
            self.report['telemetry_log_write_failures'] += 1
            self.report['telemetry_log_last_error'] = str(exc)

    async def client(self, reader, writer):
        if self.used:
            writer.close()
            await writer.wait_closed()
            return
        self.used = True
        self.writer = writer
        task = asyncio.current_task()
        self.tasks.add(task)
        receiver = asyncio.create_task(self.receive_controller())
        self.report['state'] = 'qemu-connected'
        self.save(force=True)
        self.log('qemu-connected')
        try:
            async def input_packets():
                decoder = H4Decoder((1, 2))
                while True:
                    data = await reader.read(4096)
                    if not data:
                        decoder.eof()
                        return
                    for packet in decoder.feed(data):
                        response = local_vendor_reply(packet)
                        if packet[0] == 1:
                            opcode = struct.unpack_from('<H', packet, 1)[0]
                            if opcode == 0xfc32 and self.info.get('physical_address_verified'):
                                guest_address = ':'.join(f'{b:02X}' for b in packet[4:][::-1])
                                if guest_address != self.info['controller_address']:
                                    raise RuntimeError('Guest factory address differs from the physical radio')
                            self.report['guest_commands'] += 1
                            self.log('guest-command', opcode=f'{opcode:04x}', length=packet[3],
                                     destination='BES compatibility' if response else 'physical controller')
                        else:
                            self.report['guest_acl_packets'] += 1
                        if response is not None:
                            self.report['bes_initialization_adapted'] += 1
                            writer.write(response)
                            await writer.drain()
                        else:
                            if packet[0] == 1 and opcode in (0x0c03, 0x0c1a, 0x200a, 0x2039):
                                self.pending_settings[opcode].append(packet[4:])
                            job = asyncio.create_task(asyncio.to_thread(self.controller.send, packet))
                            try:
                                await asyncio.shield(job)
                            except asyncio.CancelledError:
                                await job
                                raise
            sender = asyncio.create_task(input_packets())
            tasks = (sender, receiver)
            try:
                done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                for completed in done:
                    await completed
            finally:
                for pending in tasks:
                    pending.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
        except Exception as exc:
            self.report['error'] = repr(exc)
            self.log('transport-failed', error=repr(exc), traceback=traceback.format_exc())
        finally:
            receiver.cancel()
            await asyncio.gather(receiver, return_exceptions=True)
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass
            self.report['state'] = 'disconnected'
            self.save(force=True)
            self.tasks.discard(task)
            self.finished.set()

    async def receive_controller(self):
        decoder = H4Decoder((2, 4))
        while True:
            job = asyncio.create_task(asyncio.to_thread(self.controller.recv))
            try:
                packet = await asyncio.shield(job)
            except asyncio.CancelledError:
                await job
                raise
            if packet is None:
                self.save()
                continue
            if not packet:
                raise ConnectionError('Physical HCI transport closed')
            for data in decoder.feed(packet):
                if data[0] == 4:
                    self.report['controller_events'] += 1
                    metadata = {}
                    if data[1] == 0x0e and len(data) >= 7:
                        opcode = struct.unpack_from('<H', data, 4)[0]
                        metadata.update(opcode=f'{opcode:04x}', status=data[6])
                        if self.pending_settings[opcode]:
                            setting = self.pending_settings[opcode].popleft()
                            if data[6] == 0:
                                if opcode == 0x0c03:
                                    self.report.update(controller_accepted_le_advertising=False,
                                                       controller_accepted_extended_advertising={},
                                                       controller_accepted_classic_scan_enable=0)
                                elif opcode == 0x200a and len(setting) == 1:
                                    self.report['controller_accepted_le_advertising'] = bool(setting[0])
                                elif opcode == 0x0c1a and len(setting) == 1:
                                    self.report['controller_accepted_classic_scan_enable'] = setting[0]
                                elif opcode == 0x2039 and len(setting) >= 2:
                                    enabled, count = setting[:2]
                                    states = self.report['controller_accepted_extended_advertising']
                                    if not enabled and not count:
                                        states.clear()
                                    elif len(setting) == 2 + count * 4:
                                        for index in range(count):
                                            states[str(setting[2 + index * 4])] = bool(enabled)
                    elif data[1] == 0x0f and len(data) >= 7:
                        metadata.update(opcode=f'{struct.unpack_from("<H", data, 5)[0]:04x}', status=data[3])
                    elif data[1] in (3, 5, 6, 8) and len(data) >= 6:
                        metadata.update(status=data[3], handle=struct.unpack_from('<H', data, 4)[0])
                    elif data[1] == 0x3e and len(data) >= 4:
                        metadata['subevent'] = f'{data[3]:02x}'
                        if data[3] == 0x12 and len(data) >= 9:
                            self.report['controller_accepted_extended_advertising'][str(data[5])] = False
                    self.log('controller-event', code=f'{data[1]:02x}', length=data[2], **metadata)
                else:
                    self.report['controller_acl_packets'] += 1
                self.writer.write(data)
            await self.writer.drain()
            self.save()

    async def close(self):
        for task in list(self.tasks):
            task.cancel()
        await asyncio.gather(*list(self.tasks), return_exceptions=True)
        try:
            # End any physical advertising/links before releasing controller.
            await asyncio.to_thread(query, self.controller, 0x0c03)
            original = self.info.get('controller_address_before')
            if original and original != self.info['controller_address']:
                await asyncio.to_thread(query, self.controller, 0xfc31,
                                        bytes.fromhex(original.replace(':', ''))[::-1])
                restored = await asyncio.to_thread(query, self.controller, 0x1009)
                self.report['original_controller_address_restored'] = (
                    restored == bytes.fromhex(original.replace(':', ''))[::-1])
        except Exception as exc:
            self.log('controller-reset-on-exit-failed', error=repr(exc))
        try:
            await asyncio.to_thread(self.controller.close)
        finally:
            self.report['state'] = 'stopped'
            self.save(force=True)
            self.logfile.close()


class ControllerSelectionError(RuntimeError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def select_usb(args):
    candidates = list_windows_usb()
    if args.usb_path:
        candidates = [d for d in candidates if d['path'].lower() == args.usb_path.lower()]
    if args.usb_vid:
        candidates = [d for d in candidates if d['vid'] == args.usb_vid.lower()]
    if args.usb_pid:
        candidates = [d for d in candidates if d['pid'] == args.usb_pid.lower()]
    if not any((args.usb_path, args.usb_vid, args.usb_pid)):
        ready = [d for d in candidates if d['raw_hci_ready']]
        if ready:
            candidates = ready
    if not candidates:
        target = (f'USB {args.usb_vid or "*"}:{args.usb_pid or "*"}'
                  if args.usb_vid or args.usb_pid else 'WinUSB 蓝牙控制器')
        raise ControllerSelectionError('controller_missing',
            f'未检测到 {target}。请在设备管理器扫描检测硬件改动；'
            '若重启后仍未出现，请保存工作后完整关机，再断电冷启动。')
    if len(candidates) > 1:
        raise ControllerSelectionError('controller_ambiguous',
            '检测到多个匹配的蓝牙控制器，请通过 --usb-path 指定一个；--list 可查看设备列表。')
    device = candidates[0]
    if not device['raw_hci_ready']:
        raise ControllerSelectionError('controller_not_winusb',
            f"USB {device['vid']}:{device['pid']} 当前驱动为 {device['service'] or '未知'}，"
            '原始 HCI 直连需要 WinUSB。请核对驱动绑定。')
    return device


async def main(args):
    if args.list:
        print(json.dumps(list_windows_usb(), ensure_ascii=False, indent=2))
        return
    args.output.mkdir(parents=True, exist_ok=True)
    if (args.output / 'ready.json').exists():
        raise RuntimeError('Use a new raw HCI output directory')
    controller = None
    relay = None
    try:
        if args.linux_hci is not None:
            controller = LinuxHci(args.linux_hci)
            transport = {'backend': 'Linux HCI_CHANNEL_USER', 'controller_index': args.linux_hci}
        else:
            device = select_usb(args)
            controller = WindowsUsb(device)
            transport = {'backend': 'WinUSB raw Bluetooth USB',
                         'usb_vid': device['vid'], 'usb_pid': device['pid']}
            await asyncio.to_thread(prepare_intel, controller, device, args.output)
        info = await asyncio.to_thread(identity, controller)
        info = await asyncio.to_thread(configure_address, controller, info, args.bt_address)
        relay = RawRelay(controller, args.output, {**info, **transport})
        server = await asyncio.start_server(relay.client, args.listen_host, args.port)
        port = server.sockets[0].getsockname()[1]
        (args.output / 'ready.json').write_text(json.dumps({'pid': os.getpid(), 'port': port,
                                                           **info, **transport}, indent=2), encoding='utf8')
        async def stopped():
            while not (args.output / args.stop_request_name).exists():
                if relay.finished.is_set():
                    return
                await asyncio.sleep(0.25)
        async with server:
            if args.seconds:
                try:
                    await asyncio.wait_for(stopped(), args.seconds)
                except asyncio.TimeoutError:
                    pass
            else:
                await stopped()
        if relay.report.get('error'):
            raise RuntimeError('Physical HCI transport failed; inspect raw-hci.jsonl')
    except Exception as exc:
        (args.output / 'backend-error.json').write_text(json.dumps({
            'code': getattr(exc, 'code', 'controller_transport_error'),
            'message': str(exc)}, ensure_ascii=False, indent=2), encoding='utf8')
        raise
    finally:
        if relay:
            await relay.close()
        elif controller:
            controller.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--list', action='store_true', help='Read-only USB driver inventory')
    parser.add_argument('--usb-path')
    parser.add_argument('--usb-vid')
    parser.add_argument('--usb-pid')
    parser.add_argument('--bt-address', help='Verify/program the physical Intel runtime public address')
    parser.add_argument('--linux-hci', type=int, help='Exclusive Linux HCI controller index; bring it down first')
    parser.add_argument('--listen-host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--seconds', type=float, default=0)
    parser.add_argument('--stop-request-name', default='stop.request')
    parser.add_argument('--output', type=Path, default=Path('artifacts/raw-hci-live'))
    args = parser.parse_args()
    if args.list:
        context = nullcontext()
    elif os.name == 'nt':
        from radio_guard import RadioGuard
        context = RadioGuard()
    else:
        context = nullcontext()
    with context:
        asyncio.run(main(args))
