import time, math

ir_hist=[];red_hist=[];trend=[];med=[];bpm=0;bpm_disp=0;valid=False;signal=False;last_valid=0;last_proc=0;samples=0;t0=0;on=False;pa_ir=0x26;pa_red=0x26
spo2=0;spo2_disp=0;spo2_valid=False;spo2_hist=[]

def start(ctx):
 global ir_hist,red_hist,trend,med,bpm,bpm_disp,valid,signal,last_valid,samples,t0,on,last_proc
 global spo2,spo2_disp,spo2_valid,spo2_hist
 ir_hist=[];red_hist=[];trend=[];med=[];bpm=0;bpm_disp=0;valid=False;signal=False;last_valid=0;samples=0;t0=time.ticks_ms();last_proc=0;on=True
 spo2=0;spo2_disp=0;spo2_valid=False;spo2_hist=[]
 if ctx.max30102 and ctx.max30102.present:ctx.max30102.enable(pa_red,pa_ir)

def stop(ctx):
 global on
 on=False
 if ctx.max30102 and ctx.max30102.present:ctx.max30102.disable()

def poll(ctx):
 global samples
 if not ctx.max30102 or not ctx.max30102.present:return
 for r,i in ctx.max30102.check():red_hist.append(r);ir_hist.append(i);samples+=1
 if len(ir_hist)>240:del ir_hist[:-240];del red_hist[:-240]

def compute_spo2():
 global spo2,spo2_disp,spo2_valid,spo2_hist
 if len(red_hist)<50 or len(ir_hist)<50:return
 rr=red_hist[-50:];ii=ir_hist[-50:]
 dc_r=sum(rr)/len(rr);dc_i=sum(ii)/len(ii)
 if dc_r<1000 or dc_i<1000:return
 ac_r=[v-dc_r for v in rr];ac_i=[v-dc_i for v in ii]
 rms_r=(sum(x*x for x in ac_r)/len(ac_r))**0.5
 rms_i=(sum(x*x for x in ac_i)/len(ac_i))**0.5
 if rms_i==0:return
 R=(rms_r/dc_r)/(rms_i/dc_i)
 val=110-25*R
 if not (90<=val<=100):spo2_valid=False;return
 spo2=val;spo2_valid=True;spo2_hist.append(spo2)
 if len(spo2_hist)>7:spo2_hist.pop(0)
 spo2_disp=sorted(spo2_hist)[len(spo2_hist)//2]

def process(ctx):
 global bpm,bpm_disp,valid,signal,last_valid,pa_ir,pa_red
 if len(ir_hist)<60:return
 di=sum(ir_hist)/len(ir_hist);dr=sum(red_hist)/len(red_hist);iok=3000<=di<=400000;rok=3000<=dr<=400000;signal=iok or rok
 if dr>250000:pa_ir=max(0x10,pa_ir-5);pa_red=max(0x10,pa_red-5)
 elif not signal:pa_ir=min(0xA0,pa_ir+4);pa_red=min(0x30,pa_ir)
 if ctx.max30102 and ctx.max30102.present:ctx.max30102.set_pa(pa_red,pa_ir)
 src=ir_hist if iok else red_hist;dc=di if iok else dr;ac=[v-dc for v in src];span=max(ac)-min(ac);peaks=[];rate=samples/max(1,time.ticks_diff(time.ticks_ms(),t0)/1000)
 if span>20:
  th=min(ac)+span*.4;md=max(4,int(rate*.25))
  for x in range(1,len(ac)-1):
   if ac[x]>ac[x-1] and ac[x]>=ac[x+1] and ac[x]>th:
    if peaks and x-peaks[-1]<md:
     if ac[x]>ac[peaks[-1]]:peaks[-1]=x
    else:peaks.append(x)
 if len(peaks)>=2:
  raw=60*rate/(sum(peaks[x+1]-peaks[x] for x in range(len(peaks)-1))/len(peaks)-1)
  if not iok:raw*=.75
  if 30<=raw<=240:
   bpm=int(raw);valid=True;last_valid=time.ticks_ms();med.append(bpm)
   if len(med)>7:med.pop(0)
   bpm_disp=sorted(med)[len(med)//2]
   trend.append(bpm_disp)
  else:valid=False
 else:valid=False
 if len(trend)>300:trend.pop(0)
 compute_spo2()

def draw(ctx):
 global last_proc
 poll(ctx);now=time.ticks_ms()
 if now-last_proc>=1000:process(ctx);last_proc=now
 spo2_txt=str(spo2_disp) if spo2_valid else "--"
 ctx.tft.fill(ctx.C_BLACK);ctx.tft.fill_rect(0,0,160,24,ctx.C_BLUE)
 ctx.tft.text("HR:{}".format(bpm_disp if valid or now-last_valid<15000 else '--'),6,8,ctx.C_GREEN)
 ctx.tft.text("SpO2:"+spo2_txt,86,8,ctx.C_CYAN);ctx.tft.hline(0,24,160,ctx.C_CYAN)
 vals=trend[-132:];lo=min(vals)-5 if vals else 50;hi=max(vals)+5 if vals else 150;ctx.tft.vline(27,30,82,ctx.C_DIM)
 for j in range(5):
  v=lo+(hi-lo)*(4-j)/4;y=30+j*20;ctx.tft.text(str(int(v)),0,y-4,ctx.C_GRAY);ctx.tft.hline(28,y,132,ctx.C_DIM)
 prev=None
 for j,v in enumerate(vals):
  x=28+j;y=112-int((v-lo)/(hi-lo)*82);ctx.tft.pixel(x,y,ctx.C_WHITE)
  if prev:ctx.tft.line(prev[0],prev[1],x,y,ctx.C_GREEN)
  prev=(x,y)
 ctx.tft.text("REC ON",8,116,ctx.C_GRAY);ctx.tft.show()

def run(ctx):
 global on
 while True:
  ctx.web_server_poll()          # 关键：心率页也要处理网页
  if ctx.turn_page(ctx.poll_encoder_delta()):stop(ctx);return
  if ctx.poll_ttp_toggle():
   if ctx.hr_recording:start(ctx);ctx.log_hr_event('SESSION_START')
   else:stop(ctx);trend.clear();ctx.log_hr_event('SESSION_END')
  if ctx.hr_recording:
   if not on:start(ctx)
   draw(ctx)
   if ctx.sd_ok and valid and time.ticks_diff(time.ticks_ms(),ctx.last_hr_log_ms)>=2000:ctx.last_hr_log_ms=time.ticks_ms();ctx.log_hr_event('RUN',bpm_disp)
  else:
   stop(ctx);trend.clear();ctx.tft.fill(ctx.C_BLACK);ctx.tft.fill_rect(0,0,160,24,ctx.C_BLUE);ctx.tft.text('HR:--',6,8,ctx.C_WHITE);ctx.tft.text('SpO2:--',86,8,ctx.C_CYAN);ctx.tft.text('TTP223: TOUCH TO REC',8,60,ctx.C_ORANGE);ctx.tft.show()
  time.sleep(.05)