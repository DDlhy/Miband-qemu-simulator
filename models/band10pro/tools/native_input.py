"""Exercise this emulator's visible SDL window using the real desktop mouse."""
import argparse
import ctypes
from ctypes import wintypes as w
import json
from pathlib import Path
import time

from probe_boot import ROOT


def main():
    p=argparse.ArgumentParser(__doc__)
    p.add_argument('gesture',choices=['tap','swipe'])
    p.add_argument('coordinates',nargs='+',type=int)
    args=p.parse_args()
    xy=args.coordinates
    if len(xy)!=(2 if args.gesture=='tap' else 4):p.error('tap x y / swipe x1 y1 x2 y2')
    out=Path((ROOT/'userdata/latest-session.txt').read_text('utf8'))
    launch=json.loads((out/'launch.json').read_text('utf8'))
    pid=launch['qemu_pid'];rotation=int(launch.get('display',{}).get('rotation',0))
    u=ctypes.WinDLL('user32',use_last_error=True)
    u.GetClientRect.argtypes=[w.HWND,ctypes.POINTER(w.RECT)]
    windows=[]
    @ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
    def each(h,l):
        process=w.DWORD()
        u.GetWindowThreadProcessId(h,ctypes.byref(process))
        if process.value==pid and u.IsWindowVisible(h):windows.append(h)
        return True
    u.EnumWindows(each,0)
    if len(windows)!=1:raise RuntimeError('Expected one visible SDL window: '+repr(windows))
    window=windows[0]
    rect=w.RECT();u.GetClientRect(window,ctypes.byref(rect))
    u.SetForegroundWindow.argtypes=[w.HWND]
    u.ClientToScreen.argtypes=[w.HWND,ctypes.POINTER(w.POINT)]
    u.SetForegroundWindow(window)
    time.sleep(.2)
    def send(message,x,y,pressed):
        # CLI coordinates remain native 336x480 watch coordinates. Convert them
        # to the rotated host surface before scaling to the actual SDL client.
        if rotation == 90:
            x,y=479-y,x
            logical_width,logical_height=480,336
        elif rotation == 180:
            x,y=335-x,479-y
            logical_width,logical_height=336,480
        elif rotation == 270:
            x,y=y,335-x
            logical_width,logical_height=480,336
        else:
            logical_width,logical_height=336,480
        x=round(x*(rect.right-1)/(logical_width-1));y=round(y*(rect.bottom-1)/(logical_height-1))
        point=w.POINT(x,y);u.ClientToScreen(window,ctypes.byref(point))
        u.SetCursorPos(point.x,point.y)
        if message!=0x200:u.mouse_event(2 if message==0x201 else 4,0,0,0,0)
    send(0x200,xy[0],xy[1],False)
    send(0x201,xy[0],xy[1],True)
    if args.gesture=='swipe':
        for i in range(1,17):
            time.sleep(.03)
            send(0x200,xy[0]+(xy[2]-xy[0])*i/16,xy[1]+(xy[3]-xy[1])*i/16,True)
    else:time.sleep(.15)
    send(0x202,xy[-2],xy[-1],False)
    time.sleep(2)
    (out/'capture.request').touch()
    with (out/'native-input.jsonl').open('a',encoding='utf8') as log:
        log.write(json.dumps({'time':time.time(),'method':'desktop mouse','gesture':args.gesture,
                              'coordinates':xy,'rotation':rotation,'hwnd':window})+'\n')
    print(out/'screen.ppm')


if __name__=='__main__':main()
