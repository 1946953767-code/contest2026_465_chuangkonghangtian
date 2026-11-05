"""姿态解算 v2：陀螺积分 + 加速度计互补滤波（带静止检测与自动零偏）

相比 v1 的改进：
  1) 标定时检查"是否真的静止"（陀螺读数抖动 > 3°/s 就重试，最多 3 次）
  2) 互补滤波系数按 dt 计算（tau=0.35s），不再是固定 0.03
  3) 检测到静止时，强拉回加速度计角度（消除积分漂移）
  4) 输出平滑，避免模型抖动
"""

import math
import time

TAU = 0.35          # 互补滤波时间常数（秒）
REST_GYRO = 1.2     # |陀螺| < 2°/s 且 |a|≈1g 视为静止
REST_PULL = 0.15    # 静止时每帧朝加速度计靠拢的比例
YAW_DEADBAND = 0.8  # 偏航死区：小于这个角速度认为没转（°/s）
REST_BIAS = 0.0    # 静止时零偏在线跟踪系数


class Attitude:
    def __init__(self):
        self.pitch = 0.0
        self.roll = 0.0
        self.yaw = 0.0
        self.pitch_d = 0.0
        self.roll_d = 0.0
        self.bx = self.by = self.bz = 0.0
        self.t = time.ticks_ms()
        self.ready = False
        self.rest = False

    # ---------------- 标定 ----------------
    def calibrate(self, mpu, ms=1500, tries=3):
        for k in range(tries):
            n = 0
            sx = sy = sz = 0.0
            mx = my = mz = -1e9
            mnx = mny = mnz = 1e9
            t0 = time.ticks_ms()
            while time.ticks_diff(time.ticks_ms(), t0) < ms:
                g = mpu.read_gyro_data()
                v0, v1, v2 = g["x"], g["y"], g["z"]
                sx += v0; sy += v1; sz += v2
                mx = max(mx, v0); my = max(my, v1); mz = max(mz, v2)
                mnx = min(mnx, v0); mny = min(mny, v1); mnz = min(mnz, v2)
                n += 1
                time.sleep_ms(20)
            if not n:
                continue
            spread = max(mx - mnx, my - mny, mz - mnz)
            if spread < 3.0 or k == tries - 1:
                self.bx = sx / n
                self.by = sy / n
                self.bz = sz / n
                break
        a = mpu.read_accel_data()
        self.pitch = math.degrees(math.atan2(a["y"], a["z"]))
        self.roll = math.degrees(math.atan2(-a["x"], math.sqrt(a["y"] ** 2 + a["z"] ** 2)))
        self.pitch_d = self.pitch
        self.roll_d = self.roll
        self.t = time.ticks_ms()
        self.ready = True
        return (round(self.bx, 2), round(self.by, 2), round(self.bz, 2))

    # ---------------- 更新 ----------------
    def update(self, a, g):
        now = time.ticks_ms()
        dt = time.ticks_diff(now, self.t) / 1000.0
        self.t = now
        if dt <= 0 or dt > 0.5:
            dt = 0.02

        gx = g["x"] - self.bx
        gy = g["y"] - self.by
        gz = g["z"] - self.bz
        ax, ay, az = a["x"], a["y"], a["z"]
        an = math.sqrt(ax * ax + ay * ay + az * az)

        # 预测（陀螺积分）
        pred_p = self.pitch + gx * dt
        pred_r = self.roll + gy * dt

        # 偏航没有磁力计，只能积分；所以这里必须做两件事避免"没人动也一直在转"：
        #   a) 死区：|gz| 小于 0.8°/s 视为零（抵消残余零偏与噪声）
        #   b) 静止冻结：判定静止时完全不积分
        # v5：不再在线改零偏（误判静止会吃掉真实转动，导致姿态漂移）

        if abs(gz) < YAW_DEADBAND or self.rest:
            gz_use = 0.0
        else:
            gz_use = gz - (YAW_DEADBAND if gz > 0 else -YAW_DEADBAND)
        self.yaw += gz_use * dt

        # 观测（加速度计）
        if 0.80 < an < 1.20:
            ap = math.degrees(math.atan2(ay, az))
            ar = math.degrees(math.atan2(-ax, math.sqrt(ay * ay + az * az)))
            self.rest = (abs(gx) < REST_GYRO and abs(gy) < REST_GYRO and
                         abs(gz) < REST_GYRO)
            if self.rest:
                k = REST_PULL                     # 静止：强拉回
            else:
                k = dt / (TAU + dt)               # 运动：按时间常数融合
            dp = (ap - pred_p + 540.0) % 360.0 - 180.0
            dr = (ar - pred_r + 540.0) % 360.0 - 180.0
            self.pitch = pred_p + dp * k
            self.roll = pred_r + dr * k
        else:
            self.pitch = pred_p
            self.roll = pred_r
            self.rest = False

        # 归一
        self.pitch = (self.pitch + 540.0) % 360.0 - 180.0
        self.roll = (self.roll + 540.0) % 360.0 - 180.0
        self.yaw = (self.yaw + 540.0) % 360.0 - 180.0

        # 输出平滑
        alpha = 0.45
        self.pitch_d += ((self.pitch - self.pitch_d + 540.0) % 360.0 - 180.0) * alpha
        self.roll_d += ((self.roll - self.roll_d + 540.0) % 360.0 - 180.0) * alpha
        return self.pitch_d, self.roll_d, self.yaw