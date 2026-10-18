"""File queue client for the native p67 control window."""
import argparse,json,time,uuid,msvcrt
from pathlib import Path
from sensor_control import validate_update,write_json,default_profile,apply_profile
ROOT=Path(__file__).resolve().parents[1]

def active_session(root=ROOT,wanted=None):
    candidates=[]
    for parent in ('artifacts/sessions','artifacts/verification'):
        for p in sorted((Path(root)/parent).glob('*/sensor-status.json'),key=lambda p:p.stat().st_mtime,reverse=True)[:30]:
            try:
                s=json.loads(p.read_text('utf8'))
                if s['connected'] and 0<=time.time()-s['updated_at']<5 and (not wanted or wanted==s['session_id']):
                    candidates.append((s['updated_at'],p.parent,s))
            except (OSError,ValueError,KeyError):pass
    return max(candidates,key=lambda v:v[0])[1:] if candidates else (None,None)

def control(request,wanted=None,root=ROOT):
    op,value=validate_update(request)
    folder,status=active_session(root,wanted=wanted)
    if folder is None:
        if op=='rotation':raise ValueError('请先启动模拟器；屏幕旋转只作用于当前运行窗口。')
        if wanted:raise ValueError('指定的模拟器已停止。')
        path=Path(root)/'userdata/sensor-profile.json'
        path.parent.mkdir(parents=True,exist_ok=True)
        # Separate native UI requests may arrive from multiple Python processes.
        # Lock the entire read/modify/replace transaction, not just the final write.
        with path.with_suffix('.lock').open('a+b') as lock:
            lock.seek(0);msvcrt.locking(lock.fileno(),msvcrt.LK_LOCK,1)
            try:
                profile=json.loads(path.read_text('utf-8-sig'))if path.exists()else default_profile()
                write_json(path,apply_profile(profile,op,value))
            finally:
                lock.seek(0);msvcrt.locking(lock.fileno(),msvcrt.LK_UNLCK,1)
        return {'ok':True,'stored_only':True}
    name=uuid.uuid4().hex+'.json'
    write_json(folder/'sensor-requests'/name,dict(request,expires_at=time.time()+6))
    reply=folder/'sensor-replies'/name
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        if reply.exists():
            result=json.loads(reply.read_text('utf8'));reply.unlink();return result
        time.sleep(.04)
    raise TimeoutError('控制请求超时。')

def main():
    p=argparse.ArgumentParser(__doc__)
    p.add_argument('--request-file',type=Path,required=True)
    p.add_argument('--reply-file',type=Path,required=True)
    p.add_argument('--session-id')
    args=p.parse_args()
    try:reply=control(json.loads(args.request_file.read_text('utf-8-sig')),args.session_id)
    except Exception as exc:reply={'ok':False,'error':str(exc)}
    write_json(args.reply_file,reply)
if __name__=='__main__':main()
