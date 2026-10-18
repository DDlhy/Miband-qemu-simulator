"""p67 native sensor controller, served by the launcher's sole QMP connection."""
import json
import math
from pathlib import Path
import time
import uuid
from gnss_backend import GNSS_DEFAULTS, GnssBackend

WIDTH, HEIGHT = 336, 480
BATTERY_DIVIDER = 7
NTC_CHANNELS = (0, 1, 3, 5, 6)
EDITABLE_CHANNELS = (0, 1, 2, 3, 5, 6)
HARDWARE_DEFAULTS = dict(accel_x=0.,accel_y=0.,accel_z=9.80665,
    gyro_x=0.,gyro_y=0.,gyro_z=0.,mag_x=30.,mag_y=0.,mag_z=40.,
    pd_0=0.,pd_1=0.,ambient_lux=100.,optical_bias=100.,pulse_bpm=72.,pulse_amplitude=0.)

def mv_to_raw(mv):return round(15968+(mv-800)*4848/250)
def raw_to_mv(raw):return round(800+(raw-15968)*250/4848,2)
ADC_DEFAULTS={n:mv_to_raw(3960/7 if n==2 else 450) for n in EDITABLE_CHANNELS}

def write_json(path,value):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    tmp=path.with_suffix(path.suffix+'.tmp')
    tmp.write_text(json.dumps(value,ensure_ascii=False,indent=2),encoding='utf8')
    for attempt in range(6):
        try:tmp.replace(path);return
        except PermissionError:
            if attempt==5:raise
            time.sleep(.005*(attempt+1))

def validate_update(request):
    if not isinstance(request,dict):raise ValueError('控制请求格式错误。')
    op=request.get('operation')
    if op=='inputs':
        values={}
        for key in ('battery_mv','ntc_mv'):
            if key not in request:continue
            value=request[key]
            if type(value) not in (int,float) or not math.isfinite(value):raise ValueError('请输入有限数值。')
            if key=='battery_mv':values[2]=mv_to_raw(value/7)
            else:values.update({n:mv_to_raw(value) for n in NTC_CHANNELS})
        if not values:raise ValueError('没有输入数值。')
        return 'adc',values
    if op=='adc':
        values=request.get('channels',{})
        if not isinstance(values,dict) or not values:raise ValueError('没有 ADC 输入。')
        if any(str(n) not in {str(x)for x in EDITABLE_CHANNELS} or type(v)is not int for n,v in values.items()):
            raise ValueError('ADC 需要整数；板型识别通道只读。')
        return op,{int(n):v for n,v in values.items()}
    if op=='sensors':
        values={k:v for k,v in request.items() if k in HARDWARE_DEFAULTS}
        if not values:raise ValueError('没有传感器输入。')
        if any(type(v)not in(int,float)or not math.isfinite(v)for v in values.values()):raise ValueError('请输入有限数值。')
        return op,values
    if op=='charging':
        if type(request.get('connected'))is not bool:raise ValueError('充电开关格式错误。')
        return op,request['connected']
    if op=='gnss':
        values={k:v for k,v in request.items()if k in GNSS_DEFAULTS}
        if not values:raise ValueError('没有 GNSS 输入。')
        for k,v in values.items():
            if isinstance(GNSS_DEFAULTS[k],bool):
                if type(v)is not bool:raise ValueError('GNSS 开关格式错误。')
            elif type(v)not in(int,float)or not math.isfinite(v):raise ValueError('请输入有限数值。')
        return op,values
    if op=='rotation':
        degrees=request.get('degrees')
        if type(degrees)is not int or degrees not in (0,90,180,270):
            raise ValueError('屏幕旋转角度只能是 0、90、180 或 270 度。')
        return op,degrees
    if op=='gesture':
        if request.get('gesture')not in('wake','tap','up','down','left','right'):raise ValueError('不支持的手势。')
        return op,request['gesture']
    if op=='touch':
        x,y,down=request.get('x'),request.get('y'),request.get('down')
        if type(x)is not int or type(y)is not int or not 0<=x<WIDTH or not 0<=y<HEIGHT or type(down)is not bool:
            raise ValueError('触摸坐标超出 336×480 屏幕。')
        return op,(x,y,down)
    if op=='uart':
        if request.get('uart')not in(1,2):raise ValueError('10 Pro 仅接出 UART1 / UART2。')
        try:data=bytes.fromhex(request['hex'])
        except (KeyError,ValueError,TypeError):raise ValueError('HEX 格式错误。')
        if not 1<=len(data)<=1024:raise ValueError('每次发送 1～1024 字节。')
        return op,(request['uart'],data)
    if op=='preview':return op,bool(request.get('enabled'))
    if op in('defaults','status','reboot'):return op,None
    raise ValueError('10 Pro 尚未支持此硬件操作。')

def default_profile():
    return dict(version=1,model='p67cn',channels={str(n):v for n,v in ADC_DEFAULTS.items()},
                charging=False,hardware=dict(HARDWARE_DEFAULTS),gnss=dict(GNSS_DEFAULTS))

def apply_profile(profile,op,value):
    if op=='adc':profile['channels'].update({str(n):v for n,v in value.items()})
    elif op=='sensors':profile['hardware'].update(value)
    elif op=='charging':profile['charging']=value
    elif op=='gnss':profile.setdefault('gnss',dict(GNSS_DEFAULTS)).update(value)
    elif op=='defaults':return default_profile()
    else:raise ValueError('请先启动 10 Pro 模拟器。')
    return profile

class SensorSession:
    def __init__(self,qmp,out,root,qemu_pid,serial,profile=None,apply_initial=True):
        self.qmp,self.out,self.root=qmp,Path(out),Path(root)
        self.pid,self.serial=qemu_pid,serial
        self.profile_path=Path(profile) if profile else self.root/'userdata/sensor-profile.json'
        self.profile=default_profile()
        self.requests=self.out/'sensor-requests';self.replies=self.out/'sensor-replies'
        self.requests.mkdir();self.replies.mkdir()
        self.next_status=0;self.preview_until=0;self.next_preview=0
        self.state={};self.connected=True;self.error=None
        if self.profile_path.exists():
            p=json.loads(self.profile_path.read_text('utf-8-sig'))
            self.profile['channels'].update(p.get('channels',{}))
            self.profile['hardware'].update(p.get('hardware',{}))
            self.profile['charging']=bool(p.get('charging',False))
            self.profile['gnss'].update(p.get('gnss',{}))
        _,adc=validate_update(dict(operation='adc',channels=self.profile['channels']))
        if apply_initial:self.apply_adc(adc)
        _,hw=validate_update(dict(operation='sensors',**self.profile['hardware']))
        if apply_initial:
            self.apply_hardware(hw)
            self.set('sensor-charging',self.profile['charging'])
        self.gnss=GnssBackend(self.serial[0],self.profile['gnss'])
        self.publish()

    def set(self,key,value):return self.qmp.call('qom-set',dict(path='/machine',property=key,value=value))
    def apply_adc(self,values):
        for n,v in values.items():self.set(f'sensor-adc-{n}',v&0xffff)
    def apply_hardware(self,values):
        for k,v in values.items():self.set('sensor-'+k.replace('_','-'),v)

    def pointer(self,x,y,down=None):
        events=[dict(type='abs',data=dict(axis=axis,value=round(v*32767/(size-1))))
                for axis,v,size in [('x',x,WIDTH),('y',y,HEIGHT)]]
        if down is not None:events.append(dict(type='btn',data=dict(button='left',down=down)))
        self.qmp.call('input-send-event',dict(events=events))

    def gesture(self,name):
        if name in('tap','wake'):
            for _ in range(2 if name=='wake' else 1):
                self.pointer(168,240,True);time.sleep(.07)
                self.pointer(168,240,False);time.sleep(.09)
            return
        start,end={'up':((168,390),(168,90)),'down':((168,90),(168,390)),
                   'left':((285,240),(50,240)),'right':((50,240),(285,240))}[name]
        self.pointer(*start,True)
        for i in range(1,17):
            time.sleep(.02)
            self.pointer(round(start[0]+(end[0]-start[0])*i/16),round(start[1]+(end[1]-start[1])*i/16))
        self.pointer(*end,False)

    def read_state(self):
        self.state=json.loads(self.qmp.call('qom-get',dict(path='/machine',property='sensor-state')))
        self.state['display_rotation']=int(self.qmp.call('qom-get',dict(path='/machine',property='display-rotation')))
        for ch in self.state['channels']:
            for source,target in [('input_raw','input_mv'),('sampled_raw','sampled_mv'),('last_read_raw','last_read_mv')]:
                ch[target]=raw_to_mv(ch[source])
        self.state['battery_mv']=round(self.state['channels'][2]['input_mv']*7,2)
        self.state['requested_channels']=self.profile['channels']
        self.state['uart']={'ports':[s.state(i+1) for i,s in enumerate(self.serial)]}
        dma=self.state.get('uart_dma',{})
        self.gnss.update_link(bool(dma.get('rx_active',[False])[0]),bool(dma.get('rx_enabled',[False])[0]))
        self.state['gnss']=self.gnss.state()
        return self.state

    def publish(self):
        if self.connected:self.read_state()
        write_json(self.out/'sensor-status.json',dict(version=1,session_id=self.out.relative_to(self.root).as_posix(),
            qemu_pid=self.pid,model='p67cn',connected=self.connected,supported=True,updated_at=time.time(),
            state=self.state,error=self.error))

    def service(self):
        self.gnss.service()
        for path in sorted(self.requests.glob('*.json'),key=lambda p:p.stat().st_mtime_ns):
            result={'ok':False}
            try:
                request=json.loads(path.read_text('utf-8-sig'))
                if request.get('expires_at',0)<time.time():raise ValueError('请求已过期。')
                op,value=validate_update(request)
                if op=='adc':self.apply_adc(value)
                elif op=='sensors':self.apply_hardware(value)
                elif op=='charging':self.set('sensor-charging',value)
                elif op=='gnss':self.profile['gnss'].update(value)
                elif op=='rotation':self.set('display-rotation',value)
                elif op=='gesture':self.gesture(value)
                elif op=='touch':self.pointer(*value)
                elif op=='uart':self.serial[value[0]-1].send(value[1])
                elif op=='reboot':self.gnss.reset();self.qmp.call('system_reset')
                elif op=='preview':self.preview_until=time.monotonic()+4 if value else 0
                elif op=='defaults':
                    self.apply_adc(ADC_DEFAULTS);self.apply_hardware(HARDWARE_DEFAULTS);self.set('sensor-charging',False)
                if op in('adc','sensors','charging','defaults','gnss'):
                    self.profile=apply_profile(self.profile,op,value)
                    self.gnss.profile=self.profile['gnss']
                    write_json(self.profile_path,self.profile)
                result={'ok':True,'operation':op}
                if op=='adc':result['wrapped_channels']=[n for n,v in value.items()if v!=v&0xffff]
            except Exception as exc:result['error']=str(exc)
            write_json(self.replies/path.name,result);path.unlink()
        now=time.monotonic()
        if now>=self.next_status:
            self.publish();self.next_status=now+.25
        if now<self.preview_until and now>=self.next_preview:
            self.next_preview=now+.6
            from screen_preview import ppm_to_png
            ppm=self.out/'sensor-preview.ppm';png=self.out/'sensor-preview.png'
            self.qmp.call('screendump',dict(filename=ppm.relative_to(self.root).as_posix()))
            ppm_to_png(ppm,png.with_suffix('.tmp'));png.with_suffix('.tmp').replace(png)

    def close(self):
        self.connected=False
        self.publish()
