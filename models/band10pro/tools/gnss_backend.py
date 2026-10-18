"""AG3335M UART reference source: PAIR command replies and NMEA fixes.

Coordinates are simulated. Guest DMA counters are published separately so
socket transmission is never mistaken for a fix consumed by the stock AP.
"""
from collections import deque
import datetime
import threading
import time

GNSS_DEFAULTS=dict(gnss_enabled=True,gnss_fix=True,gnss_latitude=31.2304,
    gnss_longitude=121.4737,gnss_altitude=12.,gnss_speed=0.,gnss_heading=0.,
    gnss_satellites=12.,gnss_hdop=.8)

def sentence(body):
    data=body.encode('ascii');checksum=0
    for b in data:checksum^=b
    return b'$'+data+f'*{checksum:02X}\r\n'.encode('ascii')

def coordinate(value, latitude):
    value=abs(value);degrees=int(value);minutes=(value-degrees)*60
    return f'{degrees:0{2 if latitude else 3}d}{minutes:07.4f}'

class GnssBackend:
    def __init__(self,serial,profile):
        self.serial=serial;self.profile=profile;self.pending=deque(maxlen=256)
        self.lock=threading.Lock();self.buffer=bytearray();self.next_emit=time.monotonic()+2
        self.commands=0;self.sent_bytes=0;self.sent_fixes=0;self.last_command=''
        self.ready=False;self.boot_sent=False;self.powered=False
        self.serial.observer=self.receive

    def receive(self,data):
        # Called by UART capture thread; retain only complete PAIR commands.
        with self.lock:
            self.buffer.extend(data)
            while b'\n' in self.buffer:
                line,_,tail=self.buffer.partition(b'\n');self.buffer=bytearray(tail)
                start=line.find(b'$PAIR')
                if start>=0:self.pending.append(bytes(line[start:]).decode('ascii','replace'))
            if len(self.buffer)>4096:del self.buffer[:-256]

    def send(self,body):
        data=sentence(body);self.serial.send(data);self.sent_bytes+=len(data)

    def reset(self):
        self.ready=False;self.boot_sent=False
        with self.lock:self.pending.clear();self.buffer.clear()

    def update_link(self,active,powered):
        if self.powered and not powered:self.reset()
        self.powered=powered;self.ready=active

    def service(self):
        if not self.ready:return
        if not self.boot_sent:
            self.send('PAIR001,2,0')
            self.send('PAIR010,1,0,0,0');self.boot_sent=True
        with self.lock:commands=list(self.pending);self.pending.clear()
        for line in commands:
            body=line.split('*')[0][1:];code=body.split(',')[0][4:]
            if not code.isdigit():continue
            self.commands+=1;self.last_command=body
            number=int(code)
            if number in (6,20):self.send('PAIR020,AG3335M_V3.1.0.c17sp_6_20260511')
            self.send(f'PAIR001,{number},0')
        now=time.monotonic()
        if now<self.next_emit:return
        self.next_emit=now+1
        if not self.profile['gnss_enabled'] or self.serial.connection is None:return
        p=self.profile;utc=datetime.datetime.now(datetime.timezone.utc)
        lat=coordinate(p['gnss_latitude'],True);lon=coordinate(p['gnss_longitude'],False)
        ns='N'if p['gnss_latitude']>=0 else'S';ew='E'if p['gnss_longitude']>=0 else'W'
        fix=p['gnss_fix'];sats=round(p['gnss_satellites']);hdop=p['gnss_hdop']
        clock=utc.strftime('%H%M%S')+'.00';date=utc.strftime('%d%m%y')
        self.send(f'GNRMC,{clock},{"A"if fix else"V"},{lat},{ns},{lon},{ew},{p["gnss_speed"]*1.943844:.3f},{p["gnss_heading"]:.2f},{date},,,{"A"if fix else"N"}')
        self.send(f'GNGGA,{clock},{lat},{ns},{lon},{ew},{1 if fix else 0},{sats:02d},{hdop:.2f},{p["gnss_altitude"]:.2f},M,0.0,M,,')
        self.send(f'GNGSA,A,{3 if fix else 1},01,02,03,04,05,06,07,08,09,10,11,12,{hdop*1.5:.2f},{hdop:.2f},{hdop*1.2:.2f},1')
        self.sent_fixes+=1

    def state(self):
        return dict(self.profile,driver='AG3335M',uart=1,commands=self.commands,
                    last_command=self.last_command,tx_to_guest_bytes=self.sent_bytes,
                    emitted_epochs=self.sent_fixes,receiving=self.ready,simulated=True)
