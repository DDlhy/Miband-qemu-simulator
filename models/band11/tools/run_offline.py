"""Run the same saved firmware profile without claiming the host Bluetooth radio."""
import argparse
import datetime
import json
import os
from pathlib import Path
import socket
import subprocess
import time

from initialize_factory import create_factory
from initialize_storage import initialize_storage, flash_drive_arguments
from qmp_client import Qmp, capture_diagnostics
from sensor_control import SensorSession
from uart_backend import UartBridge

ROOT = Path(__file__).resolve().parents[1]


def main(args):
    launch_start=time.monotonic()
    out = ROOT / 'artifacts/offline-sessions' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    out.mkdir(parents=True)
    relative = out.relative_to(ROOT).as_posix()
    uart = UartBridge(out)
    create_factory(out / 'factory', factory_prop=ROOT / 'config/factory/factory.prop')
    data = args.data_image.resolve()
    subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                    str(ROOT / 'tools/Initialize-NandData.ps1'), '-Path', str(data)],
                   check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    flash = initialize_storage(ROOT, data, out / 'factory/q66nfc.img')
    with socket.socket() as reserved:
        reserved.bind(('127.0.0.1', 0))
        port = reserved.getsockname()[1]
    machine = 'xiaomi-q66nfc,ota-image=firmware/vela_ota.bin'
    if args.trace_sensor_bus:
        machine += ',sensor-bus-trace=on'
    command = [str(args.qemu.resolve()), '-M', machine,
               '-name', 'Xiaomi Smart Band 11 NFC - Offline',
               '-display', 'sdl,gl=off,show-cursor=on' if args.display == 'sdl' else 'none',
               '-kernel', 'firmware/vela_ap.bin', '-initrd', 'firmware/vela_resource.bin',
               '-qmp', f'tcp:127.0.0.1:{port},server=on,wait=off',
               '-d', 'guest_errors', '-D', relative + '/mmio.log']
    command += flash_drive_arguments(ROOT, data)
    command += uart.arguments()
    if args.gdb_port is not None:
        command += ['-gdb', f'tcp:127.0.0.1:{args.gdb_port}']
    env = os.environ.copy()
    system = Path(os.environ['SystemRoot'])
    env['PATH'] = os.pathsep.join((str(ROOT.parents[1] / 'runtime/bin'), str(system / 'System32'), str(system)))
    receipt = {'started_at': datetime.datetime.now().astimezone().isoformat(),
               'physical_bluetooth_used': False, 'flash': flash, 'qemu_command': command}
    qmp = sensors = None
    with (out / 'qemu-stderr.log').open('w', encoding='utf8') as stderr:
        qemu = subprocess.Popen(command, cwd=ROOT, env=env, stderr=stderr, stdout=subprocess.DEVNULL,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        receipt['qemu_pid'] = qemu.pid
        (out / 'launch.json').write_text(json.dumps(receipt, indent=2), encoding='utf8')
        try:
            deadline = time.monotonic() + 15
            while qmp is None:
                if qemu.poll() is not None:
                    raise RuntimeError('Offline QEMU failed to start')
                try:
                    qmp = Qmp(port)
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.1)
            sensors = SensorSession(qmp, out, ROOT, qemu.pid, args.sensor_profile, uart=uart)
            receipt['timings']={'qmp_ready_seconds':round(time.monotonic()-launch_start,3)}
            started = time.monotonic()
            captured = False
            while qemu.poll() is None:
                sensors.service()
                if sensors.state.get('panel',{}).get('frames') and 'first_frame_seconds' not in receipt['timings']:
                    receipt['timings']['first_frame_seconds']=round(time.monotonic()-launch_start,3)
                    (out/'launch.json').write_text(json.dumps(receipt,indent=2),encoding='utf8')
                if (out / 'stop.request').exists() or (args.seconds and time.monotonic() - started >= args.seconds):
                    qmp.call('quit')
                    break
                request = out / 'capture.request'
                if request.exists() or (args.capture and not captured and time.monotonic() - started >= 35):
                    request.unlink(missing_ok=True)
                    capture_diagnostics(qmp, out, relative, 'offline writable flash verification')
                    captured = True
                time.sleep(0.02)
            qemu.wait(timeout=10)
            if qemu.returncode:
                raise RuntimeError('Offline QEMU exited with an error')
        finally:
            if sensors:
                sensors.close()
            if qemu.poll() is None:
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
            receipt.update(qemu_exit_code=qemu.returncode, ended_at=datetime.datetime.now().astimezone().isoformat())
            (out / 'launch.json').write_text(json.dumps(receipt, indent=2), encoding='utf8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--display', choices=('sdl', 'none'), default='sdl')
    parser.add_argument('--seconds', type=float, default=0)
    parser.add_argument('--data-image', type=Path, default=ROOT / 'userdata/nand-bluetooth.img')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--sensor-profile', type=Path,
                        help='Optional separate sensor input profile for isolated replays')
    parser.add_argument('--qemu', type=Path, default=ROOT / 'core/qemu-system-arm.exe',
                        help='Optional exact build for isolated local verification')
    parser.add_argument('--trace-sensor-bus', action='store_true',
                        help='Record actual sensor register traffic in the session MMIO log')
    parser.add_argument('--gdb-port', type=int,
                        help='Optional loopback GDB port for an isolated diagnostic VM')
    main(parser.parse_args())
