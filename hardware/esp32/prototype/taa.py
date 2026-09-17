import network
import socket
import gc
import time
import os
import sys
from neopixel import NeoPixel
from machine import Pin, SoftI2C, UART, SPI
import ssd1306
import st7735
import math
from drv import BME280, MAX30102
import buzzer_drv
from aht20 import AHT20

# ---------------- pins ----------------
TFT_SCL, TFT_SDA, TFT_RES, TFT_DC, TFT_CS, TFT_BL = 18, 8, 39, 38, 37, 40
GPS_RX_PIN = 2
EC11_S1_PIN, EC11_S2_PIN, EC11_KEY_PIN = 35, 36, 41
TTP223_PIN = 42
SD_CS_PIN, SD_SCK_PIN, SD_MOSI_PIN, SD_MISO_PIN = 45, 21, 10, 3
RGB_PIN, LED_PIN, BUZZER_PIN = 48, 45, 11
PIN_RED, PIN_GREEN, PIN_BLUE = 5, 6, 7
WIFI_LIST = [
    {"ssid": "ChinaNet-CHhE", "password": "cxl13924441489"},
    {"ssid": "步步高OPPOA5", "password": "czj123456"},
]

C_BLACK=0x0000; C_WHITE=0xFFFF; C_RED=0xF800; C_GREEN=0x07E0
C_BLUE=0x001F; C_YELLOW=0xFFE0; C_CYAN=0x07FF; C_ORANGE=0xFD20
C_GRAY=0x8410; C_DIM=0x3186; C_DARKBG=0x2104

# Hardware globals are initialized only from boot().
tft=oled=i2c=i2c2=bme=aht20=max30102=mpu=np=led_pin=None
led_red=led_green=led_blue=buzzer=None
ec11_s1=ec11_s2=ec11_key=ttp223=None
sd=None; sd_ok=False; sd_spi=None
page_index=0; last_ec11_s1=1; last_ttp_state=0  # 0主页面 1GPS 2立方体 3姿态数据 4心率页
hr_recording=False; last_hr_log_ms=0; last_gps_log_ms=0
gps_datetime_str="NO_TIME"
ip_addr="0.0.0.0"; wifi_name="No WiFi"; sw_state="NO"; act_state="IDLE"
is_running=False; wifi_connected=False; wlan=None; s=None


def init_hw():
    global tft, oled, i2c, i2c2, bme, aht20, max30102, mpu, np, led_pin
    global led_red, led_green, led_blue, buzzer, ec11_s1, ec11_s2, ec11_key, ttp223
    global sd, sd_ok, sd_spi, last_ec11_s1, last_ttp_state, sw_state
    from machine import Pin, SPI
    bl=Pin(TFT_BL, Pin.OUT); bl.value(1)
    spi=SPI(1, baudrate=20000000, polarity=0, phase=0, sck=Pin(TFT_SCL), mosi=Pin(TFT_SDA))
    tft=st7735.ST7735(spi, Pin(TFT_CS,Pin.OUT), Pin(TFT_DC,Pin.OUT), Pin(TFT_RES,Pin.OUT), width=160, height=128)
    i2c=SoftI2C(scl=Pin(16),sda=Pin(17),freq=400000)
    oled=ssd1306.SSD1306_I2C(128,64,i2c)
    i2c2=SoftI2C(scl=Pin(12),sda=Pin(13),freq=400000)
    try:
        aht20 = AHT20(i2c2)
        print("AHT20 present:", aht20.present)
    except Exception as e:
        print("AHT20 init error:", e)
        aht20 = None
    try: bme=BME280(i2c2); print("BMP/BME:",hex(bme.chip_id) if bme.present else "not found")
    except Exception as e: print("BME error",e); bme=None
    try: max30102=MAX30102(i2c2); print("MAX30102:",max30102.present)
    except Exception as e: print("MAX error",e); max30102=None
    try:
        from mpu6050 import MPU6050
        mpu=MPU6050(i2c); print("MPU6050 OK")
    except Exception as e: print("MPU error",e); mpu=None
    np=NeoPixel(Pin(RGB_PIN,Pin.OUT),1); np[0]=(0,0,0); np.write()
    led_pin=Pin(LED_PIN,Pin.OUT); led_pin.value(0)
    led_red=Pin(PIN_RED,Pin.OUT); led_green=Pin(PIN_GREEN,Pin.OUT); led_blue=Pin(PIN_BLUE,Pin.OUT)
    led_red.value(1); led_green.value(1); led_blue.value(1)
    ec11_s1=Pin(EC11_S1_PIN,Pin.IN,Pin.PULL_UP); ec11_s2=Pin(EC11_S2_PIN,Pin.IN,Pin.PULL_UP)
    ec11_key=Pin(EC11_KEY_PIN,Pin.IN,Pin.PULL_UP); ttp223=Pin(TTP223_PIN,Pin.IN)
    last_ec11_s1=ec11_s1.value(); last_ttp_state=ttp223.value()
    try:
        import sdcard
        sd_spi=SPI(2,baudrate=1000000,polarity=0,phase=0,sck=Pin(SD_SCK_PIN),mosi=Pin(SD_MOSI_PIN),miso=Pin(SD_MISO_PIN))
        sd=sdcard.SDCard(sd_spi,Pin(SD_CS_PIN,Pin.OUT)); os.mount(sd,"/sd"); sd_ok=True
    except Exception as e:
        sd=None; sd_ok=False; print("SD:",e)
    sw_state="OK" if sd_ok else "NO"
    print("I2C1",[hex(x) for x in i2c.scan()]); print("I2C2",[hex(x) for x in i2c2.scan()])


def beep(freq=2500, duration=120, pause=35):
    buzzer_drv.beep(freq, duration, pause)

def beep_on():
    buzzer_drv.on()

def beep_off():
    buzzer_drv.off()
def turn_page(d):
    global page_index
    if d:
        page_index=(page_index+d)%5
        print("PAGE",page_index)
        return True
    return False

def poll_encoder_delta():
    global last_ec11_s1
    if ec11_s1 is None: return 0
    v=ec11_s1.value()
    if v!=last_ec11_s1:
        time.sleep_ms(2); a=ec11_s1.value(); b=ec11_s2.value()
        if a!=last_ec11_s1:
            last_ec11_s1=a
            if a==0: return 1 if b else -1
    return 0

def poll_ttp_toggle():
    global last_ttp_state, hr_recording
    v=ttp223.value()
    if v!=last_ttp_state:
        time.sleep_ms(10); n=ttp223.value()
        if n!=last_ttp_state:
            last_ttp_state=n
            if n: hr_recording=not hr_recording; return True
    return False

def ts_now(): return gps_datetime_str or "NO_TIME"
def refresh_sd():
    global sd_ok, sw_state
    sd_ok=sd is not None; sw_state="OK" if sd_ok else "NO"

def append_csv(path,line,header=None):
    if not sd_ok: return
    try:
        if header is not None:
            try: os.stat(path)
            except OSError:
                with open(path,"w") as f: f.write(header+"\n")
        with open(path,"a") as f: f.write(line+"\n")
    except Exception as e: print("SD write",e)

def log_gps_row(*v):
    append_csv("/sd/gps_log.csv",",".join(str(x) for x in v),"time,lat,lon,altitude,pressure,speed,roll,pitch,tilt_x,tilt_y,tilt_z")
def log_hr_event(state,hr="--"):
    append_csv("/sd/hr_log.csv", "{},{},{},{}".format(ts_now(),hr,"--",state), "time,hr,spo2,state")

def act_color(a):
    return C_GREEN if a in ("Ready","Success") else C_YELLOW if a in ("Running","Trigger") else C_RED if a=="Error" else C_CYAN

def update_oled(ip=None,wifi=None,sdv=None,act=None,log=None):
    global ip_addr,wifi_name,sw_state,act_state
    if ip is not None: ip_addr=ip
    if wifi is not None: wifi_name=wifi
    if sdv is not None: sw_state=sdv
    if act is not None: act_state=act
    try:
        oled.fill(0); oled.rect(0,0,128,16,1); oled.text(" TAA STROBE",10,4,1); oled.hline(0,16,128,1)
        oled.text(ip_addr,2,20,1); oled.text("WiFi: "+wifi_name,2,32,1); oled.text("SD : "+sw_state,2,44,1); oled.text("LOG: "+(log or ""),2,56,1); oled.show()
    except: pass

def update_tft(ip=None,wifi=None,sdv=None,act=None,log=None):
    try:
        tft.fill(C_BLACK); tft.fill_rect(0,0,160,16,C_BLUE); tft.text("TAA STROBE",40,4,C_WHITE); tft.hline(0,16,160,C_CYAN)
        tft.text("IP:",6,24,C_CYAN); tft.text(ip_addr,32,24,C_WHITE); tft.text("WiFi:",6,38,C_CYAN); tft.text(wifi_name[:12],44,38,C_GREEN if wifi_connected else C_RED)
        tft.text("SD:",20,52,C_CYAN); tft.text(sw_state,44,52,C_GREEN if sd_ok else C_RED); tft.text("ACT:",6,66,C_CYAN); tft.text(act_state,44,66,act_color(act_state))
        tft.fill_rect(0,86,160,16,C_DARKBG); tft.text("LOG:",6,90,C_YELLOW); tft.text((log or "")[:16],44,90,C_WHITE); tft.show()
    except: pass

def startup_animation():
    update_oled(None,None,sw_state,"Animating","Startup"); update_tft(None,None,sw_state,"Animating","Startup")

def connect_wifi():
    global wlan,wifi_connected,wifi_name,ip_addr
    try:
        wlan=network.WLAN(network.STA_IF); wlan.active(True)
        if wlan.isconnected(): ip_addr=wlan.ifconfig()[0]; wifi_connected=True; wifi_name="Connected"; return True
        for n in WIFI_LIST:
            try:
                wlan.connect(n["ssid"],n["password"]); start=time.ticks_ms()
                while time.ticks_diff(time.ticks_ms(),start)<4000:
                    if wlan.isconnected(): ip_addr=wlan.ifconfig()[0]; wifi_name=n["ssid"]; wifi_connected=True; return True
                    time.sleep(0.05)
            except Exception as e: print("WiFi",e)
    except Exception as e: print("WiFi",e)
    wifi_connected=False; wifi_name="No WiFi"; ip_addr="0.0.0.0"; return False

def web_page():
    return """<!doctype html><html><head><meta charset=utf-8><title>TAA STROBE</title>
<style>
body { font-family:Arial; text-align:center; background:#1a1a2e; color:white; }
button { font-size:30px; padding:12px 40px; background:#e94560; border:none; border-radius:10px; color:white; margin:20px; }
.box { display:inline-block; border:1px solid #fff; border-radius:8px; padding:12px 20px; margin:10px; background:#16213e; }
h3 { margin:5px; }
canvas { background:#0f0f23; border:2px solid #e94560; border-radius:8px; margin:10px; }
</style>
<script>
async function refresh(){
  try{
    const d = await (await fetch('/data')).json();
    document.getElementById('hr').textContent = d.hr ?? '--';
    document.getElementById('spo2').textContent = d.spo2 ?? '--';
    document.getElementById('rec').textContent = d.recording ? 'ON' : 'OFF';
    const canvas = document.getElementById('ecg');
    const ctx = canvas.getContext('2d');
    ctx.clearRect(0,0,canvas.width,canvas.height);
    ctx.strokeStyle = '#00ff88';
    ctx.lineWidth = 2;
    if(d.trend && d.trend.length > 1){
      const vals = d.trend.slice(-300);  // 最多显示300个点
      const min = Math.min(...vals);
      const max = Math.max(...vals);
      const range = (max - min) || 1;
      const xStep = canvas.width / (vals.length - 1);
      ctx.beginPath();
      for(let i=0;i<vals.length;i++){
        const x = i * xStep;
        const y = canvas.height - ((vals[i] - min) / range) * (canvas.height - 10) - 5;
        if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
      }
      ctx.stroke();
    }
  }catch(e){}
}
setInterval(refresh, 500);
refresh();
</script>
</head>
<body>
<h2>🚀 TAA STROBE</h2>
<button onclick="fetch('/run')">▶ 启动程序</button>
<div class="box"><h3>HR</h3><h1 id="hr">--</h1></div>
<div class="box"><h3>SpO2</h3><h1 id="spo2">--</h1></div>
<div class="box"><h3>Recording</h3><h1 id="rec">--</h1></div>
<div><canvas id="ecg" width="600" height="160"></canvas></div>
<p style="color:#888">实时心率波形（最近300个数据点）</p>
</body></html>"""

def ensure_web_server():
    global s
    if s is not None:
        return
    try:
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(('0.0.0.0', 80))
        s.listen(1)
        s.settimeout(0.01)
        print('Web server ready at http://%s' % ip_addr)
    except Exception as e:
        print('Web server init failed:', e)
        try:
            s.close()
        except:
            pass
        s = None

def web_data_response():
    hr_text = "null"
    spo2_text = "null"
    trend_text = "[]"
    try:
        hr_page = sys.modules.get('hrpage')
        if hr_page is not None:
            now = time.ticks_ms()
            if hr_page.valid or now - hr_page.last_valid < 15000:
                hr_text = str(hr_page.bpm_disp)
            if getattr(hr_page, 'spo2_valid', False):
                spo2_text = str(hr_page.spo2_disp)
            trend_text = "[" + ",".join(str(v) for v in hr_page.trend[-120:]) + "]"
    except Exception as e:
        print("web_data hr error:", e)
    return '{"hr":%s,"spo2":%s,"recording":%s,"sd":%s,"trend":%s}' % (
        hr_text, spo2_text,
        'true' if hr_recording else 'false',
        'true' if sd_ok else 'false',
        trend_text)

def web_server_poll():
    global s
    if s is None:
        return
    try:
        cl, addr = s.accept()
        req = cl.recv(1024).decode('utf-8', 'ignore')
        if '/run' in req:
            do_action()
            header = (
                'HTTP/1.1 302 Found\r\n'
                'Location: /\r\n'
                'Content-Length: 0\r\n'
                'Connection: close\r\n\r\n'
            )
            cl.send(header.encode())
        elif '/data' in req:
            body = web_data_response().encode('utf-8')
            header = (
                'HTTP/1.1 200 OK\r\n'
                'Content-Type: application/json; charset=utf-8\r\n'
                'Content-Length: %d\r\n'
                'Connection: close\r\n\r\n' % len(body)
            )
            cl.send(header.encode('utf-8'))
            cl.send(body)
        else:
            body = web_page().encode('utf-8')
            header = (
                'HTTP/1.1 200 OK\r\n'
                'Content-Type: text/html; charset=utf-8\r\n'
                'Content-Length: %d\r\n'
                'Connection: close\r\n\r\n' % len(body)
            )
            cl.send(header.encode('utf-8'))
            cl.send(body)
        try:
            cl.close()
        except:
            pass
    except OSError:
        pass
    except Exception as e:
        print("web client error (ignore):", e)
        
def main_page_loop():
    refresh_sd()
    update_oled(None, wifi_name, sw_state, 'Listening', 'Server OK')
    update_tft(None, wifi_name, sw_state, 'Listening', 'Server OK')
    while True:
        if turn_page(poll_encoder_delta()):
            return
        refresh_sd()
        update_oled(None, None, sw_state, None, 'Server OK')
        update_tft(None, None, sw_state, None, 'Server OK')
        web_server_poll()
        time.sleep(0.02)

def do_action():
    global is_running
    if is_running:
        return
    is_running = True

    try:
        log_to_screen(None, None, None, "Running", "Actioning...")
    except:
        pass

    # 蜂鸣器
    try:
        beep_on()
        time.sleep(0.1)
        beep_off()
    except Exception as e:
        print("beep err:", e)

    # 红色 LED（GPIO5）
    try:
        led_red.value(0)
        time.sleep(0.3)
        led_red.value(1)
    except Exception as e:
        print("led_red err:", e)

    # RGB NeoPixel 亮红色
    try:
        np[0] = (255, 0, 0)
        np.write()
        time.sleep(0.3)
    except Exception as e:
        print("np red err:", e)

    # 普通灯 GPIO45 亮
    try:
        led_pin.value(1)
        time.sleep(0.3)
        led_pin.value(0)
    except Exception as e:
        print("led_pin err:", e)

    # 蜂鸣器两声
    try:
        beep_on()
        time.sleep(0.2)
        beep_off()
        time.sleep(0.1)
        beep_on()
        time.sleep(0.2)
        beep_off()
    except Exception as e:
        print("beep2 err:", e)

    # 绿色 LED（GPIO6）
    try:
        led_green.value(0)
        time.sleep(0.3)
        led_green.value(1)
    except Exception as e:
        print("led_green err:", e)

    # RGB 亮绿色
    try:
        np[0] = (0, 255, 0)
        np.write()
        time.sleep(0.3)
        np[0] = (0, 0, 0)
        np.write()
    except Exception as e:
        print("np green err:", e)

    try:
        log_to_screen(None, None, None, "Success", "OK Done!")
    except:
        pass

    is_running = False
def calc_tilt_angles(a):
    ax,ay,az=a["x"],a["y"],a["z"]
    return math.atan2(ax,math.sqrt(ay*ay+az*az))*180/math.pi, math.atan2(ay,math.sqrt(ax*ax+az*az))*180/math.pi, math.atan2(az,math.sqrt(ax*ax+ay*ay))*180/math.pi

def get_attitude(a):
    ax,ay,az=a["x"],a["y"],a["z"]
    pitch=math.atan2(ay,az)
    roll=math.atan2(-ax, math.sqrt(ay*ay+az*az))
    return roll,pitch

cube_vertices=[[-1,-1,-1],[1,-1,-1],[1,-1,1],[-1,-1,1],[-1,1,-1],[1,1,-1],[1,1,1],[-1,1,1]]
cube_edges=[0,1,1,2,2,3,3,0,4,5,5,6,6,7,7,4,0,4,1,5,2,6,3,7]

def draw_cube_oled():
    if mpu is None:
        oled.fill(0); oled.text("MPU6050 Err",0,0,1); oled.show(); return
    accel=mpu.read_accel_data(); pitch,roll=get_attitude(accel)
    cx,cy,scale=64,32,20; pts=[]
    cxp,sxp=math.cos(pitch),math.sin(pitch); cyr,syr=math.cos(roll),math.sin(roll)
    for v in cube_vertices:
        x,y,z=v; y1=y*cxp-z*sxp; z1=y*sxp+z*cxp; x1=x*cyr+z1*syr; pts.append((int(cx+x1*scale), int(cy+y1*scale)))
    oled.fill(0); oled.rect(0,0,128,64,1)
    for i in range(0,len(cube_edges),2):
        p1=pts[cube_edges[i]]; p2=pts[cube_edges[i+1]]; oled.line(p1[0],p1[1],p2[0],p2[1],1)
    oled.show()

def draw_cube_tft():
    if mpu is None:
        tft.fill(C_BLACK); tft.text("MPU6050 Err",24,60,C_RED); tft.show(); return
    accel=mpu.read_accel_data(); pitch,roll=get_attitude(accel)
    cx,cy,scale=80,66,20; pts=[]
    cxp,sxp=math.cos(pitch),math.sin(pitch); cyr,syr=math.cos(roll),math.sin(roll)
    for v in cube_vertices:
        x,y,z=v; y1=y*cxp-z*sxp; z1=y*sxp+z*cxp; x1=x*cyr+z1*syr; pts.append((int(cx+x1*scale), int(cy+y1*scale)))
    tft.fill(C_BLACK); tft.text("3D CUBE",56,4,C_YELLOW)
    for i in range(0,len(cube_edges),2):
        p1=pts[cube_edges[i]]; p2=pts[cube_edges[i+1]]; tft.line(p1[0],p1[1],p2[0],p2[1],C_CYAN if i<16 else C_ORANGE)
    tft.text("ENCODER: NEXT",25,112,C_GRAY); tft.show()

def draw_bar(val,x,y,color,width=70,height=6,maxv=180.0):
    if val>maxv: val=maxv
    if val<-maxv: val=-maxv
    cx=x+width//2; tft.fill_rect(x,y,width,height,0x18E3)
    if val>=0:
        l=int(val/maxv*(width//2-2)); tft.fill_rect(cx,y,l,height,color)
    else:
        l=int(-val/maxv*(width//2-2)); tft.fill_rect(cx-l,y,l,height,color)

def data_page_loop():
    while True:
        if turn_page(poll_encoder_delta()): return
        if mpu is not None:
            a=mpu.read_accel_data(); x,y,z=calc_tilt_angles(a); ax,ay,az=a['x'],a['y'],a['z']
            # ---- 环境数据：温度 / 湿度 / 气压 ----
            temp_str='--'; hum_str='--'; press_str='--'
            if aht20 is not None and aht20.present:
                try:
                    t,h=aht20.read()
                    if t is not None: temp_str='{:.1f}'.format(t)
                    if h is not None: hum_str='{:.0f}'.format(h)
                except Exception as e: print("AHT20 err",e)
            if bme is not None and bme.present:
                try:
                    bt,bp,_=bme.read()
                    if temp_str=='--': temp_str='{:.1f}'.format(bt)
                    press_str='{:.0f}'.format(bp/100.0)
                except Exception as e: print("BME err",e)
            # ---- OLED ----
            oled.fill(0); oled.rect(0,0,128,14,1); oled.text(" MPU6050 DATA",4,3,1); oled.hline(0,14,128,1)
            oled.text("X:{: 6.2f} {: .4f}".format(x,ax),2,20,1); oled.text("Y:{: 6.2f} {: .4f}".format(y,ay),2,32,1); oled.text("Z:{: 6.2f} {: .4f}".format(z,az),2,44,1)
            oled.text("T{} H{} P{}".format(temp_str,hum_str,press_str),0,56,1)
            oled.show()
            # ---- TFT ----
            tft.fill(C_BLACK); tft.fill_rect(0,0,160,16,C_BLUE); tft.text("MPU6050 DATA",40,4,C_WHITE); tft.hline(0,16,160,C_CYAN)
            rows=[('X',C_RED,x,ax),('Y',C_GREEN,y,ay),('Z',C_BLUE,z,az)]; yy=24
            for name,col,tv,rv in rows:
                tft.text(name,4,yy,col); tft.text('{:7.1f}'.format(tv),24,yy,C_WHITE); draw_bar(tv,84,yy+1,col); tft.text('{: .4f}'.format(rv),20,yy+10,C_GRAY); yy+=28
            tft.text('T{}C P{} H{}%'.format(temp_str,press_str,hum_str),4,104,C_YELLOW)
            tft.text('ENCODER: NEXT',25,116,C_GRAY); tft.show()
        time.sleep(0.05)

def cube_page_loop():
    while True:
        if turn_page(poll_encoder_delta()): return
        draw_cube_oled(); draw_cube_tft(); time.sleep(0.03)

def main_loop():
    global page_index
    while True:
        if not wifi_connected:
            connect_wifi()
        if wifi_connected and s is None:
            ensure_web_server()
        web_server_poll()
        if page_index==0:
            main_page_loop()
        elif page_index==1:
            import gpspage; gpspage.run(sys.modules[__name__])
        elif page_index==2:
            cube_page_loop()
        elif page_index==3:
            data_page_loop()
        else:
            import hrpage; hrpage.run(sys.modules[__name__])
        time.sleep(0.03)

def boot():
    gc.collect(); init_hw(); gc.collect(); print("taa free mem:",gc.mem_free()); main_loop()

if __name__=="__main__": boot()
