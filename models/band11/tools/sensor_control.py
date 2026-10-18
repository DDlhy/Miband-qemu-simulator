"""Sensor controls serviced by the supervisor's existing QMP connection."""
import datetime
import json
import math
from pathlib import Path
import time

BATTERY_DIVIDER = 7
ADC_DEFAULTS = [9180, 9180, 11421, 9180, 3364, 9180, 9180]
NTC_CHANNELS = (0, 1, 3, 5, 6)
EDITABLE_CHANNELS = (0, 1, 2, 3, 5, 6)
HARDWARE_DEFAULTS = dict(accel_x=0., accel_y=0., accel_z=9.80665,
    gyro_x=0., gyro_y=0., gyro_z=0., mag_x=30., mag_y=0., mag_z=40.,
    pd_0=0., pd_1=0., pd_2=0., pd_3=0., ambient_lux=100., optical_bias=100.,
    pulse_bpm=72., pulse_amplitude=0.)


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf8')
    # PowerShell may briefly have the old status file open without delete sharing.
    for attempt in range(6):
        try:
            temporary.replace(path)
            break
        except PermissionError:
            if attempt == 5:
                raise
            time.sleep(.005 * (attempt + 1))


def mv_to_raw(mv):
    # Stock AP calibration: raw 15968 -> 800 mV, raw 20816 -> 1050 mV.
    return round(15968 + (mv - 800) * 4848 / 250)


def raw_to_mv(raw):
    return round(800 + (raw - 15968) * 250 / 4848, 2)


def validate_update(request):
    if not isinstance(request, dict):
        raise ValueError('控制请求格式错误。')
    operation = request.get('operation')
    if operation == 'nfc':
        updates = {}
        if 'connected' in request:
            if type(request['connected']) is not bool:
                raise ValueError('NFC 实验总线开关格式错误。')
            updates['connected'] = request['connected']
        if 'hex' in request:
            try:
                data = bytes.fromhex(request['hex'])
            except (ValueError, TypeError):
                raise ValueError('NFC HEX 格式错误。')
            if not data or len(data) > 1024:
                raise ValueError('每次注入 1～1024 字节。')
            updates['hex'] = data.hex()
        if not updates:
            raise ValueError('没有 NFC 控制输入。')
        return operation, updates
    if operation == 'preview':
        if type(request.get('enabled')) is not bool:
            raise ValueError('屏幕预览开关格式错误。')
        return operation, request['enabled']
    if operation == 'touch':
        x, y, down = request.get('x'), request.get('y'), request.get('down')
        if type(x) is not int or type(y) is not int or not 0 <= x <= 211 or not 0 <= y <= 519 or type(down) is not bool:
            raise ValueError('触摸坐标需要位于屏幕内。')
        return operation, (x, y, down)
    if operation == 'microphone':
        if type(request.get('enabled')) is not bool or type(request.get('device')) is not int or request['device'] < -1:
            raise ValueError('请选择录音输入设备和路由开关。')
        return operation, dict(enabled=request['enabled'], device=request['device'])
    if operation == 'sensors':
        updates = {}
        for key in HARDWARE_DEFAULTS:
            if key in request:
                value = request[key]
                if type(value) not in (float, int) or not math.isfinite(value):
                    raise ValueError('传感器输入应为有限数值。')
                updates[key] = value
        if not updates:
            raise ValueError('没有传感器输入。')
        return operation, updates
    if operation == 'uart':
        index = request.get('uart')
        if type(index) is not int or index not in (0, 1, 2):
            raise ValueError('请选择 UART0、UART1 或 UART2。')
        try:
            payload = bytes.fromhex(request.get('hex', ''))
        except (ValueError, TypeError):
            raise ValueError('UART HEX 格式错误，例如 55 AA 01。')
        if not payload or len(payload) > 1024:
            raise ValueError('每次 UART 发送 1～1024 字节。')
        return operation, (index, payload)
    if operation == 'adc':
        channels = request.get('channels')
        if not isinstance(channels, dict) or not channels or len(channels) > 6:
            raise ValueError('请选择要调整的传感器。')
        updates = {}
        for name, raw in channels.items():
            if name not in {str(n) for n in EDITABLE_CHANNELS}:
                raise ValueError('板型识别通道不能修改。')
            if type(raw) is not int:
                raise ValueError('ADC 输入应为整数。')
            updates[int(name)] = raw
        return operation, updates
    if operation == 'defaults':
        return 'adc', {n: ADC_DEFAULTS[n] for n in EDITABLE_CHANNELS}
    if operation == 'inputs':
        updates = {}
        for key in ('battery_mv', 'ntc_mv'):
            if key not in request:
                continue
            value = request[key]
            if type(value) not in (int, float) or not math.isfinite(value):
                raise ValueError('请输入有效数值。')
            if key == 'battery_mv':
                updates[2] = mv_to_raw(value / BATTERY_DIVIDER)
            else:
                for n in NTC_CHANNELS:
                    updates[n] = mv_to_raw(value)
        if not updates:
            raise ValueError('请输入电池或探头电压。')
        return 'adc', updates
    if operation == 'charging':
        if type(request.get('connected')) is not bool:
            raise ValueError('充电开关应为开或关。')
        return operation, request['connected']
    if operation == 'reboot':
        return operation, None
    if operation == 'gesture':
        gesture = request.get('gesture')
        if gesture not in ('wake', 'tap', 'up', 'down', 'left', 'right'):
            raise ValueError('不支持的触摸操作。')
        return operation, gesture
    raise ValueError('不支持的控制操作。')


class SensorSession:
    def __init__(self, qmp, out, root, qemu_pid, profile=None, uart=None):
        self.qmp, self.out, self.root = qmp, Path(out), Path(root)
        self.qemu_pid = qemu_pid
        self.uart = uart
        self.hardware = dict(HARDWARE_DEFAULTS)
        from microphone_backend import MicrophoneRouter
        self.microphone = MicrophoneRouter()
        self.requests = self.out / 'sensor-requests'
        self.replies = self.out / 'sensor-replies'
        self.requests.mkdir(exist_ok=True)
        self.replies.mkdir(exist_ok=True)
        self.profile = Path(profile) if profile else self.root / 'userdata/sensor-profile.json'
        self.next_status = 0
        self.preview_until = self.next_preview = 0
        self.preview_state = {'available': False}
        self.nfc_sequence = 0
        self.state = {}
        self.error = None
        self.supported = False
        self.requested = {n: ADC_DEFAULTS[n] for n in EDITABLE_CHANNELS}
        try:
            self.read_state()
            self.supported = True
            if self.profile.exists():
                profile = json.loads(self.profile.read_text('utf8'))
                _, updates = validate_update({'operation': 'adc', 'channels': profile['channels']})
                self.apply_adc(updates, persist=False)
                self.qmp.call('qom-set', {'path': '/machine',
                    'property': 'sensor-charging', 'value': bool(profile.get('charging', False))})
                if profile.get('hardware'):
                    _, updates = validate_update(dict(profile['hardware'], operation='sensors'))
                    self.apply_hardware(updates, persist=False)
                if profile.get('microphone'):
                    self.microphone.configure(**profile['microphone'])
        except Exception as exc:
            self.error = str(exc)
        self.publish()

    def read_state(self):
        self.state = json.loads(self.qmp.call('qom-get', {
            'path': '/machine', 'property': 'sensor-state'}))
        for channel in self.state['channels']:
            channel['input_mv'] = raw_to_mv(channel['input_raw'])
            channel['sampled_mv'] = raw_to_mv(channel['sampled_raw'])
            channel['last_read_mv'] = raw_to_mv(channel['last_read_raw'])
        self.state['battery_mv'] = round(self.state['channels'][2]['input_mv'] * BATTERY_DIVIDER, 2)
        self.state['requested_channels'] = {str(n): raw for n, raw in self.requested.items()}
        if self.uart:
            self.uart.service()
            self.state['uart'] = self.uart.state()
        self.state['microphone'] = self.microphone.state()
        self.state['preview'] = self.preview_state
        events = [event for event in self.state.get('nfc', {}).get('trace', [])
                  if event['seq'] > self.nfc_sequence]
        if events:
            with (self.out / 'nfc-traffic.jsonl').open('a', encoding='utf8') as log:
                for event in events:
                    log.write(json.dumps(dict(event, at=time.time()), ensure_ascii=False) + '\n')
                    self.nfc_sequence = event['seq']
        return self.state

    def service_preview(self):
        now = time.monotonic()
        if now >= self.preview_until or now < self.next_preview:
            return
        self.next_preview = now + .5
        try:
            from screen_preview import ppm_to_png
            ppm = self.out / 'sensor-preview.ppm'
            png = self.out / 'sensor-preview.png'
            self.qmp.call('screendump', {'filename': ppm.relative_to(self.root).as_posix()})
            ppm_to_png(ppm, png.with_suffix('.tmp'))
            png.with_suffix('.tmp').replace(png)
            self.preview_state = dict(available=True, path=png.relative_to(self.root).as_posix(),
                                      updated_at=time.time(), error=None)
        except Exception as exc:
            self.preview_state = dict(available=False, error=str(exc))

    def apply_hardware(self, updates, persist=True):
        previous = dict(self.hardware)
        applied = []
        try:
            for key, value in updates.items():
                self.qmp.call('qom-set', {'path': '/machine',
                    'property': 'sensor-' + key.replace('_', '-'), 'value': value})
                applied.append(key)
        except Exception:
            for key in applied:
                self.qmp.call('qom-set', {'path': '/machine',
                    'property': 'sensor-' + key.replace('_', '-'), 'value': previous[key]})
            raise
        self.hardware.update(updates)
        self.read_state()
        if persist:
            self.save_profile()

    def apply_adc(self, updates, persist=True):
        before = self.read_state()
        previous = {n: before['channels'][n]['input_raw'] for n in updates}
        applied = []
        try:
            for channel, raw in updates.items():
                self.qmp.call('qom-set', {'path': '/machine',
                    'property': f'sensor-adc-{channel}', 'value': raw % 65536})
                applied.append(channel)
        except Exception:
            for channel in applied:
                self.qmp.call('qom-set', {'path': '/machine',
                    'property': f'sensor-adc-{channel}', 'value': previous[channel]})
            raise
        self.requested.update(updates)
        state = self.read_state()
        if persist:
            self.save_profile()
        return state

    def save_profile(self):
        write_json(self.profile, {'version': 3, 'channels': {
            str(n): raw for n, raw in self.requested.items()},
            'hardware': self.hardware,
            'microphone': dict(enabled=self.microphone.enabled, device=self.microphone.device),
            'charging': self.state.get('charger', {}).get('connected', False)})

    def pointer(self, x, y, down):
        self.qmp.call('input-send-event', {'events': [
            {'type': 'abs', 'data': {'axis': 'x', 'value': round(x * 32767 / 211)}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': round(y * 32767 / 519)}},
            {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})

    def gesture(self, name):
        x, y = 106, 260
        try:
            if name in ('wake', 'tap'):
                for index in range(2 if name == 'wake' else 1):
                    self.pointer(x, y, True)
                    time.sleep(0.06)
                    self.pointer(x, y, False)
                    if name == 'wake' and index == 0:
                        time.sleep(0.12)
            else:
                paths = {'up': ((106, 410), (106, 120)),
                         'down': ((106, 120), (106, 410)),
                         'left': ((176, 260), (35, 260)),
                         'right': ((35, 260), (176, 260))}
                start, end = paths[name]
                for step in range(13):
                    x = round(start[0] + (end[0] - start[0]) * step / 12)
                    y = round(start[1] + (end[1] - start[1]) * step / 12)
                    self.pointer(x, y, True)
                    time.sleep(0.035)
        finally:
            self.pointer(x, y, False)

    def publish(self, connected=True):
        write_json(self.out / 'sensor-status.json', {
            'version': 1, 'updated_at': time.time(), 'connected': connected,
            'supported': self.supported, 'error': self.error,
            'session_id': self.out.relative_to(self.root).as_posix(),
            'qemu_pid': self.qemu_pid, 'state': self.state})
        self.next_status = time.monotonic() + .2

    def service(self):
        try:
            if self.uart:
                self.uart.service()
            self.microphone.service(self.qmp, self.state.get('audio', {}))
            self.service_preview()
            for path in sorted(self.requests.glob('*.json'),
                               key=lambda p: (p.stat().st_mtime_ns, p.name))[:16]:
                if len(path.stem) != 32 or any(c not in '0123456789abcdef' for c in path.stem):
                    continue
                response = {'request_id': path.stem, 'ok': False}
                request = {}
                try:
                    if path.stat().st_size > 4096:
                        raise ValueError('控制请求过大。')
                    request = json.loads(path.read_text('utf8'))
                    if request.get('expires_at', 0) < time.time():
                        raise ValueError('请求已过期，请重新操作。')
                    if not self.supported:
                        raise ValueError('当前模拟器版本没有传感器接口，请重启更新后的模拟器。')
                    operation, value = validate_update(request)
                    if operation == 'nfc':
                        if 'connected' in value:
                            self.qmp.call('qom-set', {'path': '/machine',
                                'property': 'nfc-connected', 'value': value['connected']})
                        if 'hex' in value:
                            self.qmp.call('qom-set', {'path': '/machine',
                                'property': 'nfc-rx', 'value': value['hex']})
                        self.read_state()
                    elif operation == 'preview':
                        self.preview_until = time.monotonic() + 5 if value else 0
                        self.read_state()
                    elif operation == 'adc':
                        self.apply_adc(value)
                        if request['operation'] == 'defaults':
                            self.apply_hardware(HARDWARE_DEFAULTS, persist=False)
                            self.qmp.call('qom-set', {'path': '/machine',
                                'property': 'sensor-charging', 'value': False})
                            self.read_state()
                            self.save_profile()
                    elif operation == 'touch':
                        self.pointer(*value)
                        self.read_state()
                    elif operation == 'microphone':
                        if value['device'] not in {d['id'] for d in self.microphone.devices}:
                            raise ValueError('录音输入设备不存在，请重新打开模拟器刷新设备列表。')
                        self.microphone.configure(**value)
                        self.read_state()
                        self.save_profile()
                    elif operation == 'sensors':
                        self.apply_hardware(value)
                    elif operation == 'uart':
                        if not self.uart:
                            raise ValueError('当前会话没有 UART 路由，请重新启动模拟器。')
                        self.uart.send(*value)
                        self.read_state()
                    elif operation == 'charging':
                        self.qmp.call('qom-set', {'path': '/machine',
                            'property': 'sensor-charging', 'value': value})
                        self.read_state()
                        self.save_profile()
                    elif operation == 'reboot':
                        self.qmp.call('system_reset')
                        self.read_state()
                    else:
                        self.gesture(value)
                        self.read_state()
                    self.error = None
                    response.update(ok=True, state=self.state)
                    if operation == 'adc':
                        response['wrapped_channels'] = [n for n, raw in value.items()
                                                       if not 0 <= raw <= 65535]
                except Exception as exc:
                    response['error'] = str(exc)
                write_json(self.replies / path.name, response)
                path.unlink(missing_ok=True)
                with (self.out / 'sensor-actions.jsonl').open('a', encoding='utf8') as stream:
                    stream.write(json.dumps({'at': datetime.datetime.now().astimezone().isoformat(),
                        'request_id': path.stem, 'ok': response['ok'],
                        'operation': request.get('operation') if isinstance(request, dict) else None,
                        'connected': request.get('connected') if isinstance(request, dict) else None,
                        'error': response.get('error')}, ensure_ascii=False) + '\n')
                self.publish()
            if time.monotonic() >= self.next_status:
                if self.supported:
                    self.read_state()
                self.publish()
        except Exception as exc:
            # A diagnostic/control failure must not tear down the firmware.
            self.error = str(exc)
            self.next_status = time.monotonic() + 2

    def close(self):
        self.microphone.close()
        if self.uart:
            self.uart.close()
        try:
            self.publish(connected=False)
        except OSError:
            pass
