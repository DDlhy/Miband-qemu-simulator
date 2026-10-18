"""Launch unchanged AP with an exclusive physical HCI controller backend."""
import argparse
import datetime
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from initialize_factory import create_factory
from initialize_storage import initialize_storage, flash_drive_arguments
from qmp_client import Qmp, capture_diagnostics
from sensor_control import SensorSession
from uart_backend import UartBridge

ROOT = Path(__file__).resolve().parents[1]
NO_WINDOW = subprocess.CREATE_NO_WINDOW


def main(args):
    launch_start = time.monotonic()
    out = ROOT / 'artifacts/raw-bluetooth-sessions' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    out.mkdir(parents=True)
    relative = out.relative_to(ROOT).as_posix()
    data = args.data_image.resolve()
    data_relative = os.path.relpath(data, ROOT).replace('\\', '/')
    data_relative.encode('ascii')
    receipt = {'started_at': datetime.datetime.now().astimezone().isoformat(),
               'mode': 'exclusive raw HCI', 'windows_bluetooth_api_used': False,
               'data_image': str(data)}
    def save():
        (out / 'launch.json').write_text(json.dumps(receipt, indent=2), encoding='utf8')
    bridge = qemu = qmp = sensors = None
    uart = UartBridge(out)
    logs = []
    def log(name):
        stream = (out / name).open('w', encoding='utf8')
        logs.append(stream)
        return stream
    try:
        command = [sys._base_executable, '-X', 'utf8', '-B', '-u', str(ROOT / 'tools/raw_hci_bridge.py'),
                   '--output', str(out)]
        if args.usb_vid:
            command += ['--usb-vid', args.usb_vid]
        if args.usb_pid:
            command += ['--usb-pid', args.usb_pid]
        if args.usb_path:
            command += ['--usb-path', args.usb_path]
        env = os.environ.copy()
        env['PYTHONDONTWRITEBYTECODE'] = '1'
        bridge = subprocess.Popen(command, cwd=ROOT, env=env, creationflags=NO_WINDOW,
                                  stdout=log('bridge-stdout.log'), stderr=log('bridge-stderr.log'))
        receipt.update(bridge_pid=bridge.pid, bridge_command=command)
        save()
        deadline = time.monotonic() + 120
        while not (out / 'ready.json').exists():
            if os.environ.get('MIBAND_STOP_REQUEST') and Path(os.environ['MIBAND_STOP_REQUEST']).exists():
                return
            if bridge.poll() is not None:
                failure = out / 'backend-error.json'
                if failure.exists():
                    details = json.loads(failure.read_text('utf8'))
                    receipt['backend_error_code'] = details['code']
                    raise RuntimeError(details['message'] + '\n详细日志：' + str(out))
                error = (out / 'bridge-stderr.log').read_text('utf8').strip().splitlines()
                raise RuntimeError(error[-1] if error else 'Raw HCI backend failed; inspect session logs')
            if time.monotonic() > deadline:
                raise TimeoutError('Raw HCI backend startup timed out')
            time.sleep(0.1)
        ready = json.loads((out / 'ready.json').read_text('utf8'))
        receipt['timings'] = {'hci_ready_seconds': round(time.monotonic() - launch_start, 3)}
        assert ready['pid'] == bridge.pid
        create_factory(out / 'factory', ready['controller_address'],
                       ROOT / 'config/factory/factory.prop')
        subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        str(ROOT / 'tools/Initialize-NandData.ps1'), '-Path', str(data)],
                       check=True, creationflags=NO_WINDOW)
        receipt['flash'] = initialize_storage(ROOT, data, out / 'factory/q66nfc.img')
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1', 0))
            qmp_port = reserved.getsockname()[1]
        command = [str(ROOT / 'core/qemu-system-arm.exe'), '-M',
                   'xiaomi-q66nfc,hci-chardev=hci,ota-image=firmware/vela_ota.bin',
                   '-chardev', f'socket,id=hci,host=127.0.0.1,port={ready["port"]}',
                   '-name', 'Xiaomi Smart Band 11 NFC - Raw HCI',
                   '-display', 'sdl,gl=off,show-cursor=on' if args.display == 'sdl' else 'none',
                   '-kernel', 'firmware/vela_ap.bin', '-initrd', 'firmware/vela_resource.bin',
                   '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
                   '-d', 'guest_errors', '-D', relative + '/mmio.log']
        command += flash_drive_arguments(ROOT, data)
        command += uart.arguments()
        system = Path(os.environ['SystemRoot'])
        env['PATH'] = str(ROOT.parents[1] / 'runtime/bin') + os.pathsep + str(system / 'System32') + os.pathsep + str(system)
        qemu = subprocess.Popen(command, cwd=ROOT, env=env, creationflags=NO_WINDOW,
                                stderr=log('qemu-stderr.log'), stdout=subprocess.DEVNULL)
        receipt.update(qemu_pid=qemu.pid, controller=ready, qemu_command=command)
        save()
        deadline = time.monotonic() + 15
        while qmp is None:
            if qemu.poll() is not None:
                raise RuntimeError('QEMU failed to start')
            try:
                qmp = Qmp(qmp_port)
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        sensors = SensorSession(qmp, out, ROOT, qemu.pid, uart=uart)
        receipt['timings']['qmp_ready_seconds'] = round(time.monotonic() - launch_start, 3)
        save()
        started = time.monotonic()
        captured = False
        while qemu.poll() is None:
            sensors.service()
            if sensors.state.get('panel', {}).get('frames') and 'first_frame_seconds' not in receipt['timings']:
                receipt['timings']['first_frame_seconds'] = round(time.monotonic() - launch_start, 3)
                save()
            if bridge.poll() is not None:
                raise RuntimeError('Physical HCI transport stopped; closing QEMU')
            if (out / 'stop.request').exists() or (args.seconds and time.monotonic() - started >= args.seconds):
                receipt['termination_reason'] = ('stop-request' if (out / 'stop.request').exists()
                                                 else 'time-limit')
                qmp.call('quit')
                break
            request = out / 'capture.request'
            if request.exists() or (args.capture and not captured and time.monotonic() - started >= 35):
                request.unlink(missing_ok=True)
                capture_diagnostics(qmp, out, relative, 'raw HCI read-only runtime verification')
                captured = True
            time.sleep(0.02)
        qemu.wait(timeout=10)
        if qemu.returncode:
            raise RuntimeError('QEMU exited with an error')
        receipt.setdefault('termination_reason', 'emulator-window-closed')
    except BaseException as exc:
        receipt['error'] = repr(exc)
        receipt['error_message'] = str(exc)
        receipt['termination_reason'] = 'failure'
        raise
    finally:
        if sensors:
            sensors.close()
        if qemu and qemu.poll() is None:
            try:
                if qmp:
                    qmp.call('quit')
                qemu.wait(timeout=10)
            except Exception:
                qemu.terminate()
                qemu.wait(timeout=10)
        uart.close()
        if qmp:
            qmp.close()
        if bridge:
            (out / 'stop.request').touch()
            try:
                bridge.wait(timeout=10)
            except subprocess.TimeoutExpired:
                bridge.terminate()
                bridge.wait(timeout=10)
            receipt['bridge_exit_code'] = bridge.returncode
            if bridge.returncode:
                receipt.setdefault('error', 'Physical HCI backend exited with code ' + str(bridge.returncode))
                receipt['termination_reason'] = 'failure'
        if qemu:
            receipt['qemu_exit_code'] = qemu.returncode
        receipt['ended_at'] = datetime.datetime.now().astimezone().isoformat()
        save()
        for stream in logs:
            stream.close()
        if bridge and bridge.returncode and sys.exc_info()[0] is None:
            raise RuntimeError(receipt['error'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--usb-vid')
    parser.add_argument('--usb-pid')
    parser.add_argument('--usb-path')
    parser.add_argument('--data-image', type=Path, default=ROOT / 'userdata/nand-bluetooth.img')
    parser.add_argument('--display', choices=('sdl', 'none'), default='sdl')
    parser.add_argument('--seconds', type=float, default=0)
    parser.add_argument('--capture', action='store_true', help='Read-only screen/guest-state capture after boot')
    main(parser.parse_args())
