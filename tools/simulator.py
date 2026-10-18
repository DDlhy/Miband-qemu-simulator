"""One model-aware entry point for native Mi Band emulators."""
import argparse
from contextlib import contextmanager
import ctypes
import datetime
import json
import msvcrt
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
NO_WINDOW = subprocess.CREATE_NO_WINDOW


def models():
    return json.loads((ROOT / 'models/models.json').read_text('utf8'))


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf8')
    os.replace(tmp, path)


def paths(model):
    return ROOT / 'models' / model, ROOT / 'userdata' / model


@contextmanager
def instance_lock(model):
    _, local = paths(model)
    local.mkdir(parents=True, exist_ok=True)
    with (local / 'instance.lock').open('a+b') as lock:
        lock.seek(0)
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            raise RuntimeError('该型号已在运行，请先停止已有窗口。') from None
        try:
            yield
        finally:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


def running(model):
    try:
        with instance_lock(model):
            return False
    except RuntimeError:
        return True


def preflight(model):
    root, _ = paths(model)
    required = [ROOT / 'runtime/python/python.exe', root / 'core/qemu-system-arm.exe',
                root / 'firmware/vela_ap.bin', root / 'firmware/vela_resource.bin',
                root / 'firmware/vela_ota.bin',
                root / 'config/factory/factory.prop']
    if model == 'band10pro':
        required += [root / 'config/hardware.json', root / 'config/factory/nor-seed.bin',
                     root / 'config/factory/nv.img', root / 'config/factory/manifest.json']
    for p in required:
        if not p.is_file():
            raise FileNotFoundError('缺少运行文件：' + str(p.relative_to(ROOT)))
        with p.open('rb') as stream:
            if stream.read(80).startswith(b'version https://git-lfs.github.com/spec/v1'):
                raise RuntimeError('发现占位文件，需要真实构建输入：' + str(p.relative_to(ROOT)))
    # Import in a new process: model modules intentionally have overlapping names.
    code = ('import sys; sys.path.insert(0,sys.argv[1]); '
            'import initialize_factory, sensor_control, sensor_controller, device_tools, '
            'raw_hci_controller, intel_hci_firmware; import littlefs; '
            'print("model imports OK")')
    result = subprocess.run([sys.executable, '-X', 'utf8', '-B', '-c', code, str(root / 'tools')],
                            capture_output=True, text=True, encoding='utf8', creationflags=NO_WINDOW)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    env = dict(os.environ, PATH=str(ROOT / 'runtime/bin') + os.pathsep + os.environ['PATH'])
    core = subprocess.run([str(root / 'core/qemu-system-arm.exe'), '--version'], env=env,
                          capture_output=True, creationflags=NO_WINDOW)
    if core.returncode:
        raise RuntimeError('QEMU 或共享 DLL 无法载入，请检查编译结果和运行目录的依赖收集。')
    return {'model': model, 'ok': True, 'imports': result.stdout.strip()}


def session(model):
    root, _ = paths(model)
    parents = ('offline-sessions', 'raw-bluetooth-sessions') if model == 'band11' else ('sessions', 'verification')
    live = []
    for parent in parents:
        for p in (root / 'artifacts' / parent).glob('*/sensor-status.json'):
            try:
                state = json.loads(p.read_text('utf8'))
                if state.get('connected') and 0 <= time.time() - state['updated_at'] < 6:
                    live.append((state['updated_at'], p.parent, state))
            except (OSError, ValueError, KeyError):
                continue
    return max(live, key=lambda v: v[0])[1:] if live else (None, None)


def status(model):
    _, local = paths(model)
    value = {'model': model, 'running': running(model)}
    try:
        value.update(json.loads((local / 'state.json').read_text('utf8')))
    except (OSError, ValueError):
        pass
    value['running'] = running(model)
    out, state = session(model)
    value['connected'] = state is not None
    if out:
        value['session'] = str(out.relative_to(ROOT))
    return value


def run(args):
    model = args.model
    config = models()[model]
    root, local = paths(model)
    with instance_lock(model):
        preflight(model)
        (root / 'userdata').mkdir(parents=True, exist_ok=True)
        stop = local / 'stop.request'
        stop.unlink(missing_ok=True)
        runner = config['runner' if args.mode == 'offline' else 'bluetooth_runner']
        command = [sys.executable, '-X', 'utf8', '-B', '-u', str(root / 'tools' / runner)]
        if model == 'band10pro':
            if args.mode == 'offline': command.append('--offline')
            if args.headless: command.append('--headless')
            if args.rotation: command += ['--rotation', str(args.rotation)]
        else:
            if args.rotation: raise ValueError('11 NFC 当前核心不支持显示旋转。')
            if args.headless: command += ['--display', 'none']
            if args.mode == 'bluetooth': command += ['--usb-vid', '8087', '--usb-pid', '0026']
        if args.seconds:
            command += ['--seconds', str(args.seconds)]
        record = {'model': model, 'mode': args.mode, 'phase': 'starting', 'supervisor_pid': os.getpid(),
                  'started_at': datetime.datetime.now().astimezone().isoformat(),
                  'log': str((local / 'startup.log').relative_to(ROOT))}
        write_json(local / 'state.json', record)
        with (local / 'startup.log').open('w', encoding='utf8') as log:
            env = dict(os.environ, MIBAND_STOP_REQUEST=str(stop))
            process = subprocess.Popen(command, cwd=root, env=env, stdout=log, stderr=log, creationflags=NO_WINDOW)
            record['runner_pid'] = process.pid
            write_json(local / 'state.json', record)
            try:
                while process.poll() is None:
                    out, _ = session(model)
                    phase = 'stopping' if stop.exists() else ('running' if out else 'starting')
                    if phase != record['phase'] or (out and record.get('session') != str(out.relative_to(ROOT))):
                        record['phase'] = phase
                        if out: record['session'] = str(out.relative_to(ROOT))
                        write_json(local / 'state.json', record)
                    if stop.exists() and out:
                        (out / 'stop.request').touch()
                    time.sleep(.3)
                record.update(phase='stopped' if process.returncode == 0 else 'failed',
                              exit_code=process.returncode,
                              ended_at=datetime.datetime.now().astimezone().isoformat())
                write_json(local / 'state.json', record)
            except BaseException:
                out, _ = session(model)
                if out: (out / 'stop.request').touch()
                try: process.wait(timeout=15)
                except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=10)
                raise
        if process.returncode:
            error = (local / 'startup.log').read_text('utf8', errors='replace')[-4000:]
            raise RuntimeError('启动或运行失败。日志：' + str(local / 'startup.log') + '\n\n' + error)


def open_sensors(model, tools=False):
    root, _ = paths(model)
    command = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-File',
               str(root / 'tools/SensorWindow.ps1')]
    if tools: command.append('-Tools')
    subprocess.Popen(command, cwd=ROOT, creationflags=NO_WINDOW)


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('action', choices=('start', 'stop', 'status', 'sensors', 'identity', 'check'))
    p.add_argument('--model', choices=tuple(models()), required=True)
    p.add_argument('--mode', choices=('offline', 'bluetooth'), default='offline')
    p.add_argument('--headless', action='store_true')
    p.add_argument('--seconds', type=float, default=0)
    p.add_argument('--rotation', type=int, choices=(0, 90, 180, 270), default=0)
    args = p.parse_args()
    if args.action == 'start': run(args)
    elif args.action == 'check': print(json.dumps(preflight(args.model), ensure_ascii=False))
    elif args.action == 'status': print(json.dumps(status(args.model), ensure_ascii=False))
    elif args.action in ('sensors', 'identity'): open_sensors(args.model, args.action == 'identity')
    elif args.action == 'stop':
        if running(args.model): (paths(args.model)[1] / 'stop.request').touch()


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        if sys.stderr is not None:
            traceback.print_exc()
        else:
            ctypes.windll.user32.MessageBoxW(None, str(exc), 'Mi Band QEMU Simulator', 0x10)
        sys.exit(1)
