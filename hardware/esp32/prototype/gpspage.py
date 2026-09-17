import time
from machine import UART, Pin

SEA_P0=101325.0
fused_alt=None
raw_baro_alt=None
baro_bias=0.0
baro_bias_valid=False
gps_alt_smooth=None
gps_valid_count=0

def baro_altitude(p):
    if p<=0:return None
    return 44330.0*(1.0-(p/SEA_P0)**(1.0/5.255))

def feed_baro(a):
    global raw_baro_alt,fused_alt
    raw_baro_alt=a; fused_alt=a+(baro_bias if baro_bias_valid else 0)

def feed_gps(a,ctx):
    global gps_alt_smooth,gps_valid_count,baro_bias,baro_bias_valid,fused_alt
    if gps_alt_smooth is None:gps_alt_smooth=a;gps_valid_count=1
    elif abs(a-gps_alt_smooth)<=40:
        gps_alt_smooth=.85*gps_alt_smooth+.15*a;gps_valid_count+=1
    if raw_baro_alt is not None and gps_valid_count>=3:
        target=gps_alt_smooth-raw_baro_alt
        if not baro_bias_valid:baro_bias=target;baro_bias_valid=True;print("BARO bias",baro_bias)
        else:baro_bias=.92*baro_bias+.08*target
    fused_alt=(raw_baro_alt+(baro_bias if baro_bias_valid else 0)) if raw_baro_alt is not None else gps_alt_smooth

def run(ctx):
    global fused_alt
    try:gps=UART(1,baudrate=9600,rx=Pin(ctx.GPS_RX_PIN),timeout=500,rxbuf=1024)
    except Exception as e:print("GPS UART",e);return
    buf=""; sv=0; lat="--"; lon="--"; speed="--"; tm="Waiting"; fix=False; last=0; lastlog=0; beep=False
    last_beep=0
    fix_melody_played=False
    while True:
        ctx.web_server_poll()
        if ctx.turn_page(ctx.poll_encoder_delta()):return
        if gps.any():
            raw=gps.read(gps.any())
            try:buf+=raw.decode()
            except:buf=""
            while '\n' in buf:
                line,buf=buf.split('\n',1);p=line.strip().split(',')
                if line.startswith(('$GNGGA','$GPGGA')) and len(p)>=10:
                    sv=int(p[7]) if p[7].isdigit() else 0
                    if p[6] in ('1','2') and p[9]:
                        try:feed_gps(float(p[9]),ctx)
                        except:pass
                if line.startswith(('$GNRMC','$GPRMC')) and len(p)>=10:
                    if p[2]=='A':
                        fix=True;utc=p[1];date=p[9]
                        try:
                            hh=int(utc[:2]);mm=int(utc[2:4]);ss=int(utc[4:6]);hb=(hh+8)%24;tm=f"{hb:02d}:{mm:02d}:{ss:02d}"
                            if len(date)>=6:ctx.gps_datetime_str=f"{int(date[4:6])+2000:04d}-{int(date[2:4]):02d}-{int(date[:2]):02d} {tm}"
                        except:pass
                        try:lat=f"{p[4]}{float(p[3][:2])+float(p[3][2:])/60:.6f}";lon=f"{p[6]}{float(p[5][:3])+float(p[5][3:])/60:.6f}"
                        except:lat=lon='--'
                        try:speed="{:.2f}".format(float(p[7])*.514444)
                        except:speed='--'
                    else:fix=False
        now=time.ticks_ms()
        # 蜂鸣器：搜星间歇 1047，定位成功三连音
        if fix:
            if not fix_melody_played:
                ctx.beep(1047); ctx.beep(1397); ctx.beep(1760,220,100)
                fix_melody_played=True
        else:
            fix_melody_played=False
            if time.ticks_diff(now,last_beep)>=1000:
                last_beep=now; ctx.beep(1047)
        if time.ticks_diff(now,last)>=500:
            last=now
            if ctx.bme is not None and ctx.bme.present:
                try:_,pp,_=ctx.bme.read();ba=baro_altitude(pp);feed_baro(ba) if ba is not None else None
                except:pass
            alt="{:.1f}".format(fused_alt) if fused_alt is not None else '--'
            if fix and ctx.sd_ok and time.ticks_diff(now,lastlog)>=5000:
                lastlog=now; a=ctx.mpu.read_accel_data() if ctx.mpu else {'x':0,'y':0,'z':0}; tx,ty,tz=ctx.calc_tilt_angles(a);ctx.log_gps_row(lat,lon,alt,0,speed,0,0,tx,ty,tz)
            ctx.oled.fill(0);ctx.oled.text("GPS "+tm,0,0,1);ctx.oled.text("SV:{} H:{}m".format(sv,alt),0,16,1);ctx.oled.text(lon,0,28,1);ctx.oled.text(lat,0,40,1);ctx.oled.text(speed+" m/s",0,52,1);ctx.oled.show()
            ctx.tft.fill(ctx.C_BLACK);ctx.tft.text("GPS",70,4,ctx.C_YELLOW);ctx.tft.text("SV:{} ALT:{}m".format(sv,alt),4,28,ctx.C_WHITE);ctx.tft.text("LAT:"+lat,4,50,ctx.C_WHITE);ctx.tft.text("LON:"+lon,4,68,ctx.C_WHITE);ctx.tft.text("ENCODER: NEXT",25,112,ctx.C_GRAY);ctx.tft.show()
        time.sleep(.02)