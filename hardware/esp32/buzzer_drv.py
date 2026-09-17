from machine import Pin, PWM
import time

# 无源蜂鸣器：S 接 GPIO35，- 接 GND
BUZZER_PIN = 35
POWER_PIN = None

_power = None

_buzzer = PWM(Pin(BUZZER_PIN))
_buzzer.duty_u16(0)


def beep(freq=2500, duration=120, pause=35):
    try:
        _buzzer.freq(freq)
        _buzzer.duty_u16(30000)
        time.sleep_ms(duration)
        _buzzer.duty_u16(0)
        time.sleep_ms(pause)
    except Exception as e:
        print("beep err GPIO35:", e)


def on():
    try:
        _buzzer.freq(2500)
        _buzzer.duty_u16(30000)
    except Exception as e:
        print("beep on err:", e)


def off():
    try:
        _buzzer.duty_u16(0)
    except Exception as e:
        print("beep off err:", e)