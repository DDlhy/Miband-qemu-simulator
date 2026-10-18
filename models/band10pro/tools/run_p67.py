"""Native-window launcher with independent persistent stock p67 storage."""
import argparse
import ctypes
import datetime
import json
import msvcrt
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import traceback
from initialize_factory import read_properties
from safe_serial import SafeSerialLog
from sensor_control import SensorSession
import shutil
import re

ROOT=Path(__file__).resolve().parents[1]
from qmp_client import Qmp


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0))
        return s.getsockname()[1]


def main():
    launch_start=time.monotonic()
    p=argparse.ArgumentParser(__doc__)
    p.add_argument('--offline',action='store_true')
    p.add_argument('--seconds',type=float,default=0)
    p.add_argument('--headless',action='store_true')
    p.add_argument('--verification',help='独立测试会话名称；复制存档，日常存档不写入')
    p.add_argument('--core',type=Path,help='独立测试所用 QEMU 核心')
    p.add_argument('--trace-sensors',action='store_true')
    p.add_argument('--sensor-no-inputs',action='store_true',help=argparse.SUPPRESS)
    p.add_argument('--gdb-port',type=int,help='Loopback debugger for an isolated verification session')
    p.add_argument('--rotation',type=int,choices=(0,90,180,270),default=0,
                   help='Host display rotation; 90/270 swaps the SDL window and touch axes')
    p.add_argument('--landscape',action='store_true',help='Shortcut for --rotation 90')
    args=p.parse_args()
    if args.landscape:
        if args.rotation not in (0,90):
            raise ValueError('--landscape cannot be combined with a different --rotation')
        args.rotation=90
    (ROOT/'userdata').mkdir(exist_ok=True)
    with (ROOT/'userdata/run.lock').open('a+b') as lock:
        lock.seek(0)
        try:msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1)
        except OSError:raise RuntimeError('10 Pro 模拟器已在运行，请先关闭已有窗口。')
        if args.verification:
            if not re.fullmatch(r'[A-Za-z0-9_-]{1,96}',args.verification):raise ValueError('测试名称仅允许字母、数字、下划线和连字符。')
            if not args.offline:raise ValueError('独立传感器测试需要 --offline。')
            out=ROOT/'artifacts/verification'/args.verification
        else:out=ROOT/'artifacts/sessions'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
        out.mkdir(parents=True,exist_ok=False)
        if args.verification:
            journal=out/'flash.journal';profile=out/'sensor-profile.json'
            if (ROOT/'userdata/flash.journal').exists():shutil.copy2(ROOT/'userdata/flash.journal',journal)
            if (ROOT/'userdata/sensor-profile.json').exists():shutil.copy2(ROOT/'userdata/sensor-profile.json',profile)
        else:
            journal=ROOT/'userdata/flash.journal';profile=ROOT/'userdata/sensor-profile.json'
            (ROOT/'userdata/latest-session.txt').write_text(str(out),encoding='utf8')
        rel=out.relative_to(ROOT).as_posix()
        qemu=relay=qmp=None
        sensors=None
        streams=[]
        serial_logs=[]
        receipt={'mode':'offline' if args.offline else 'WinUSB raw HCI',
                 'stock_firmware_modified':False,'started_at':datetime.datetime.now().astimezone().isoformat()}
        def log(name):
            f=(out/name).open('w',encoding='utf8')
            streams.append(f)
            return f
        def save():
            (out/'launch.json').write_text(json.dumps(receipt,indent=2,ensure_ascii=False),encoding='utf8')
        try:
            factory=ROOT/'config/factory'
            manifest=json.loads((factory/'manifest.json').read_text('utf8'))
            _, properties=read_properties(factory/'factory.prop')
            address=manifest['bluetooth_address']
            if properties.get('ro.factory.mac_bt')!=address:
                raise RuntimeError('出厂身份与蓝牙地址配置不一致。')
            receipt['factory']={'source_sha256':manifest['source_sha256'],
                                'bluetooth_address':address,'profile':manifest['profile'],
                                'uart_credentials_redacted':True}
            machine=('bes1503,hardware-config=config/hardware.json,resource-image=firmware/vela_resource.bin,'
                     f'nor-fs-image=config/factory/nor-seed.bin,flash-state={journal.relative_to(ROOT).as_posix()},'
                     f'gpu-scanout-bridge=off,display-rotation={args.rotation}')
            if args.core and not args.verification:raise ValueError('--core 只能用于独立测试。')
            command=[str(args.core or ROOT/'core/qemu-system-arm.exe')]
            if not args.offline:
                relay_command=[sys.executable,'-X','utf8','-B',str(ROOT/'tools/raw_hci_bridge.py'),
                               '--usb-vid','8087','--usb-pid','0026',
                               '--bt-address',address,'--stop-request-name','bridge.stop.request',
                               '--output',str(out)]
                relay=subprocess.Popen(relay_command,cwd=ROOT,creationflags=subprocess.CREATE_NO_WINDOW,
                                       stdout=log('bridge-stdout.log'),stderr=log('bridge-stderr.log'))
                receipt['relay_pid']=relay.pid
                save()
                deadline=time.monotonic()+120
                while not (out/'ready.json').exists():
                    if os.environ.get('MIBAND_STOP_REQUEST') and Path(os.environ['MIBAND_STOP_REQUEST']).exists():return
                    if relay.poll() is not None:
                        error=out/'backend-error.json'
                        detail=json.loads(error.read_text('utf8'))['message'] if error.exists() else 'HCI 后端启动失败'
                        raise RuntimeError(detail+'\n\n可使用“启动离线模式.cmd”查看屏幕和触摸。')
                    if time.monotonic()>deadline:raise TimeoutError('蓝牙控制器启动超时。')
                    time.sleep(.1)
                ready=json.loads((out/'ready.json').read_text('utf8'))
                receipt.setdefault('timings',{})['hci_ready_seconds']=round(time.monotonic()-launch_start,3)
                if ready['pid']!=relay.pid:raise RuntimeError('HCI 会话身份不匹配。')
                receipt['controller']=ready
                machine+=',bt-backend=chardev:hci'
                command+=['-chardev',f'socket,id=hci,host=127.0.0.1,port={ready["port"]}']
            qmp_port=free_port()
            serial_arguments=[]
            for index in range(2):
                capture=SafeSerialLog(out/f'uart{index}.log',properties)
                serial_logs.append(capture)
                serial_arguments+=['-chardev',f'socket,id=uart{index},host=127.0.0.1,port={capture.port}',
                                   '-serial',f'chardev:uart{index}']
            command+=['-M',machine,'-kernel','firmware/vela_ap.bin','-L',str(ROOT.parents[1]/'runtime/data/share/qemu'),
                      '-S',
                      '-name','Xiaomi Smart Band 10 Pro - '+receipt['mode'],
                      '-display','none' if args.headless else 'sdl,gl=off,show-cursor=on',
                      '-monitor','none',*serial_arguments,
                      '-qmp',f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
                      '-d','unimp,guest_errors' if args.trace_sensors else 'guest_errors','-D',f'{rel}/guest-errors.log']
            if args.gdb_port:
                if not args.verification:raise ValueError('GDB is only enabled for private verification sessions')
                command+=['-gdb',f'tcp:127.0.0.1:{args.gdb_port}']
            receipt.update(command=command,qmp_port=qmp_port)
            env=dict(os.environ,PATH=str(ROOT.parents[1]/'runtime/bin')+os.pathsep+os.environ['PATH'])
            qemu=subprocess.Popen(command,cwd=ROOT,env=env,creationflags=subprocess.CREATE_NO_WINDOW,
                                  stdout=subprocess.DEVNULL,stderr=log('qemu-stderr.log'))
            receipt['qemu_pid']=qemu.pid
            save()
            qmp=Qmp(('127.0.0.1',qmp_port),qemu)
            receipt.setdefault('timings',{})['qmp_ready_seconds']=round(time.monotonic()-launch_start,3)
            receipt['display']={'gpu_scanout_bridge':qmp.call('qom-get',{
                'path':'/machine','property':'gpu-scanout-bridge'}),
                'rotation':args.rotation,
                'host_size':[480,336] if args.rotation in (90,270) else [336,480]}
            save()
            if receipt['display']['gpu_scanout_bridge']:
                raise RuntimeError('屏幕诊断直出仍开启，请检查 config/hardware.json。')
            sensors=SensorSession(qmp,out,ROOT,qemu.pid,serial_logs,profile,not args.sensor_no_inputs)
            qmp.call('cont')
            start=time.monotonic()
            while qemu.poll() is None:
                if os.environ.get('MIBAND_STOP_REQUEST') and Path(os.environ['MIBAND_STOP_REQUEST']).exists():
                    qmp.call('quit');break
                sensors.service()
                if sensors.state.get('panel',{}).get('frames') and 'first_frame_seconds' not in receipt['timings']:
                    receipt['timings']['first_frame_seconds']=round(time.monotonic()-launch_start,3)
                    save()
                if relay and relay.poll() is not None:raise RuntimeError('原始蓝牙通道已断开，详见本次会话日志。')
                request=out/'actions.request.json'
                if request.exists():
                    actions=json.loads(request.read_text('utf8'))
                    request.unlink()
                    results=[]
                    for action in actions:
                        if 'delay' in action:time.sleep(min(float(action['delay']),2))
                        else:results.append(qmp.call(action['execute'],action.get('arguments')))
                    (out/'actions.result.json').write_text(json.dumps(results,indent=2),encoding='utf8')
                if (out/'capture.request').exists():
                    (out/'capture.request').unlink()
                    qmp.call('screendump',{'filename':f'{rel}/screen.ppm'})
                if (out/'stop.request').exists() or (args.seconds and time.monotonic()-start>=args.seconds):
                    qmp.call('quit')
                    break
                time.sleep(.02)
            qemu.wait(timeout=10)
            receipt['exit_code']=qemu.returncode
            if qemu.returncode:raise RuntimeError('QEMU 运行异常，请查看日志：\n'+str(out))
        except BaseException as exc:
            receipt['error']=str(exc)
            (out/'launcher-error.log').write_text(traceback.format_exc(),encoding='utf8')
            raise
        finally:
            if sensors:sensors.close()
            if qemu and qemu.poll() is None:
                try:
                    if qmp:qmp.call('quit')
                    qemu.wait(timeout=8)
                except Exception:qemu.terminate();qemu.wait(timeout=5)
            if qmp:qmp.close()
            for capture in serial_logs:capture.close()
            if relay:
                (out/'bridge.stop.request').touch()
                try:relay.wait(timeout=8)
                except subprocess.TimeoutExpired:relay.terminate();relay.wait(timeout=5)
            receipt['stopped_at']=datetime.datetime.now().astimezone().isoformat()
            save()
            for stream in streams:stream.close()
            lock.seek(0)
            msvcrt.locking(lock.fileno(),msvcrt.LK_UNLCK,1)


if __name__=='__main__':
    try:main()
    except Exception as exc:
        if '--headless' in sys.argv:print(str(exc),file=sys.stderr)
        else:ctypes.windll.user32.MessageBoxW(None,str(exc),'小米手环 10 Pro 模拟器',0x10)
        sys.exit(1)
