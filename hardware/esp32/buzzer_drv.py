from machine import Pin, PWM
import time

# 无源蜂鸣器：S 接 GPIO35，- 接 GND
# ★ 2026-10 修正：曾一度把这里改成 13，但**实际接线一直在 35**，
#   改代码没改线 => 蜂鸣器全程静音。现按实际接线改回 35。
#   （GPIO33~37 是 S3 八线 PSRAM 的脚，但本板 PSRAM 初始化失败
#    并已 bail out，所以 35 可以正常当普通 GPIO 用。）
BUZZER_PIN = 35
POWER_PIN = None

_power = None

_buzzer = PWM(Pin(BUZZER_PIN))
_buzzer.duty_u16(0)

# ============================================================================
#  非阻塞播放器（2026-10 新增）
# ----------------------------------------------------------------------------
#  为什么必须改：原来的 beep() 里是 time.sleep_ms()，**纯阻塞**。
#  一段旋律（READY 四声 / IGNITE 五声 / ARM 长鸣 600ms）会把主循环
#  整段卡住 700ms 以上，期间不收 LoRa 命令、不发遥测。
#
#  而阶段音效是**边沿触发**：某个阶段的命令如果在"上一条旋律还没播完"
#  时到达没被处理，那个阶段的音效就永久丢了、也不会补 —— 因为下一个
#  阶段一来 mission_phase 已经变了，直接响下一个。这就是"蜂鸣器丢步"。
#
#  新机制：
#    play(notes)  把一段旋律排进队列，**立即返回**，一声都不 sleep
#    tick()       主循环每轮调一次，按时间推进音符/停顿，绝不阻塞
#  这样主循环永远不被堵，命令不丢；而队列保证每个阶段的音效都留着、
#  按顺序响完，一步不漏。
# ============================================================================

MAX_QUEUE = 8                  # 队列上限：堆太多就丢最旧的，防止音效无限滞后

_queue = []                    # 待播旋律队列，每项 [(freq, dur_ms, pause_ms), ...]
_cur = None                    # 当前正在播的旋律
_cur_i = 0                     # 当前音符下标
_cur_until = 0                 # 当前"发声"或"停顿"时段的截止 tick
_cur_sounding = False          # 当前是发声(true)还是停顿(false)

_stat_played = 0               # 累计播完的旋律段数（诊断用）
_stat_dropped = 0              # 因队列满被丢弃的段数（诊断用）


def _start_next():
    """从队列取下一段旋律开始播；没有就置空。"""
    global _cur, _cur_i, _cur_until, _cur_sounding
    if _cur is not None or not _queue:
        return
    _cur = _queue.pop(0)
    _cur_i = 0
    _cur_sounding = False
    _cur_until = time.ticks_ms()     # 立刻生效，tick() 下一轮就发第一个音


def play(notes):
    """非阻塞：把一段旋律排进队列后立即返回。

    notes 形如 ((1200,120,80), (1600,120,80), (2000,200,120))
    即每项 (频率Hz, 发声毫秒, 停顿毫秒)。
    """
    global _stat_dropped
    if len(_queue) >= MAX_QUEUE:
        _queue.pop(0)
        _stat_dropped += 1
    _queue.append(list(notes))
    _start_next()


def tick():
    """主循环每轮调用一次。按时间推进播放，**不 sleep、不阻塞**。

    返回 True 表示当前有音效在播或有积压。
    """
    global _cur, _cur_i, _cur_until, _cur_sounding, _stat_played
    now = time.ticks_ms()

    if _cur is None:
        _start_next()
        if _cur is None:
            return False

    if time.ticks_diff(now, _cur_until) >= 0:
        if _cur_sounding:
            # 当前音发声完毕 -> 关声，进入停顿段
            _buzzer.duty_u16(0)
            _cur_sounding = False
            _cur_i += 1
            if _cur_i >= len(_cur):
                _stat_played += 1
                _cur = None
                _start_next()
                return True
            pause = _cur[_cur_i][2] if len(_cur[_cur_i]) > 2 else 0
            _cur_until = time.ticks_add(now, pause)
        else:
            # 停顿结束 -> 发下一个音（50% 占空比才是无源蜂鸣器的正确驱动）
            f, d = _cur[_cur_i][0], _cur[_cur_i][1]
            _buzzer.freq(f)
            _buzzer.duty_u16(32768)
            _cur_sounding = True
            _cur_until = time.ticks_add(now, d)
    return True


def busy():
    return _cur is not None or len(_queue) > 0


def status():
    """诊断：(正在播, 队列长度, 已播完段数, 丢弃段数)"""
    return (_cur is not None, len(_queue), _stat_played, _stat_dropped)


def stop():
    """立即停声并清空队列。"""
    global _cur, _cur_sounding
    _queue.clear()
    _cur = None
    _cur_sounding = False
    _buzzer.duty_u16(0)


# ---- 以下为阻塞版，仅用于"主循环还没起来"的场合（开机自检）----

def beep(freq=2500, duration=120, pause=35):
    try:
        _buzzer.freq(freq)
        _buzzer.duty_u16(32768)      # 50% 占空比（原来写 30000，接近但非对称）
        time.sleep_ms(duration)
        _buzzer.duty_u16(0)
        time.sleep_ms(pause)
    except Exception as e:
        print("beep err GPIO%d:" % BUZZER_PIN, e)


def on():
    try:
        _buzzer.freq(2500)
        _buzzer.duty_u16(32768)
    except Exception as e:
        print("beep on err:", e)


def off():
    try:
        _buzzer.duty_u16(0)
    except Exception as e:
        print("beep off err:", e)
