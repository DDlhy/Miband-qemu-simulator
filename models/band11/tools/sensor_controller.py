"""File-queue client for the native Windows sensor window; no HTTP service."""
import argparse
import json
from pathlib import Path
import time
import uuid
from sensor_control import ADC_DEFAULTS, EDITABLE_CHANNELS, HARDWARE_DEFAULTS, validate_update, write_json

ROOT = Path(__file__).resolve().parents[1]

def active_session(root, wanted=None):
    candidates = []
    for mode in ('raw-bluetooth-sessions', 'offline-sessions'):
        folder = Path(root) / 'artifacts' / mode
        if folder.exists():
            candidates.extend(sorted(folder.iterdir(), reverse=True)[:30])
    live = []
    for session in candidates:
        try:
            status = json.loads((session / 'sensor-status.json').read_text('utf8'))
            if wanted and status['session_id'] != wanted:
                continue
            if status['connected'] and 0 <= time.time()-status['updated_at'] < 5:
                live.append((status['updated_at'], session, status))
        except (OSError, ValueError, KeyError, TypeError):
            continue
    return max(live, key=lambda item: item[0])[1:] if live else (None, None)

def control(root, request, wanted=None):
    operation, value = validate_update(request)
    session, status = active_session(root, wanted)
    if session is None:
        if wanted:
            raise ValueError('指定模拟器会话已停止。')
        if operation not in ('adc', 'charging', 'sensors', 'microphone'):
            raise ValueError('模拟器未运行，请先启动模拟器。')
        path = Path(root) / 'userdata/sensor-profile.json'
        path.parent.mkdir(exist_ok=True)
        profile = json.loads(path.read_text('utf8')) if path.exists() else {
            'version': 2, 'channels': {str(n): ADC_DEFAULTS[n] for n in EDITABLE_CHANNELS},
            'charging': False}
        if operation == 'adc':
            profile['channels'].update({str(n): raw for n, raw in value.items()})
            if request['operation'] == 'defaults':
                profile['charging'] = False
                profile['hardware'] = dict(HARDWARE_DEFAULTS)
        elif operation == 'sensors':
            profile.setdefault('hardware', dict(HARDWARE_DEFAULTS)).update(value)
        elif operation == 'microphone':
            profile['microphone'] = value
        else:
            profile['charging'] = value
        profile['version'] = 3
        write_json(path, profile)
        return {'ok': True, 'stored_only': True}
    if not status['supported']:
        raise ValueError('请重启更新后的模拟器。')
    name = uuid.uuid4().hex + '.json'
    write_json(session / 'sensor-requests' / name, dict(request, expires_at=time.time()+6))
    reply = session / 'sensor-replies' / name
    deadline = time.monotonic()+7
    while time.monotonic() < deadline:
        if reply.exists():
            result = json.loads(reply.read_text('utf8')); reply.unlink()
            return result
        time.sleep(.05)
    raise TimeoutError('模拟器未及时响应。')

def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--request-file', type=Path, required=True)
    parser.add_argument('--reply-file', type=Path, required=True)
    parser.add_argument('--session-id')
    args = parser.parse_args()
    try:
        response = control(ROOT, json.loads(args.request_file.read_text('utf-8-sig')), args.session_id)
    except Exception as exc:
        response = {'ok': False, 'error': str(exc)}
    write_json(args.reply_file, response)

if __name__ == '__main__': main()
