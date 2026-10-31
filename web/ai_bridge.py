# -*- coding: utf-8 -*-
"""P4X 答辩 AI 助手 · 本地代理（只用标准库）

作用：网页把问题发到这里，这里再调 DeepSeek —— **API Key 只在本进程内**，前端拿不到。

接口：
  GET  /health   -> {"ok":true,"key":true,"model":"deepseek-chat"}
  POST /ask      -> {"ok":true,"answer":"...","usage":{...}}
       body: {"q":"问题","ctx":"当前遥测/控制台片段","history":[{"role","text"},...]}

启动：python ai_bridge.py [port]        默认 8766
Key ：环境变量 DEEPSEEK_API_KEY，或 ~/.dsh/.credentials.yaml
"""

import json
import os
import re
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-chat"

SYSTEM = """你是「P4X 单芯片火箭遥测系统」的现场 AI 助手，正在配合参赛队做答辩与联调。

风格与能力：
1. 中文回答，像一个懂嵌入式和火箭工程的队友：先给判断，再给依据，最后给"下一步怎么做"；
2. 支持连续追问：把 history 视为同一场对话，接着往下说，不要每轮重新自我介绍；
3. 会读数据：ctx 里可能有实时遥测（高度/速度/卫星/LoRa/姿态/阶段）和控制台日志，要引用具体数字；
4. 不确定就说明"需要看哪几个量"，并列出 1-3 个具体字段，不要编造；
5. 默认 3-5 句、150 字内；用户说"详细/展开"时才写长；
6. 不用 Markdown 标题和代码块，用短句分号；列表最多 3 条。

系统背景（可直接引用，不必复述）：
- 箭载端：ESP32-S3 单芯片，MPU6050(六轴, I2C 40/39)、BMP280(0x77)、AHT20(0x38)、GNSS GP21 多模多频(115200, 模块 TXD→IO10)、TF 卡、LoRa A28(9600 8N1)；
- 地面站：ESP32-P4X + openvela(NuttX)，1024x600 MIPI-DSI；任务 tchsamp(200)/p4x_gs(100)/lora_rx(110)/panel(112)；
- 遥测：full 帧 175B@2Hz、quick 帧 26B@8Hz，9600bps 理论 960B/s，实测 455→960B/s；
- 姿态：互补滤波（陀螺积分+加速度计修正），静止漂移已压到 0.0°/10s；偏航无磁力计只能相对积分；
- 已知短板：单芯片无冗余、LoRa 半双工无重传、3D 界面受面板带宽限制约 2-3fps、GNSS 室内 0 星正常。
"""

MAX_HISTORY = 10


def find_key():
    key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if key:
        return key
    for path in (
        os.path.expanduser("~/.dsh/.credentials.yaml"),
        os.path.expanduser("~/.dsh/credentials.yaml"),
    ):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        m = re.search(r"^\s*DEEPSEEK_API_KEY\s*:\s*[\"']?([A-Za-z0-9\-_\.]+)", text, re.M)
        if m:
            return m.group(1)
    return ""


KEY = find_key()


def ask_deepseek(question, ctx, history=None):
    if not KEY:
        return None, "没找到 DEEPSEEK_API_KEY（环境变量或 ~/.dsh/.credentials.yaml）"

    messages = [{"role": "system", "content": SYSTEM}]
    if ctx:
        messages.append({
            "role": "system",
            "content": "现场实时数据（供引用，不要原样复述）：\n" + ctx[:5000],
        })

    for h in (history or [])[-MAX_HISTORY:]:
        role = "assistant" if h.get("role") == "assistant" else "user"
        text = (h.get("text") or "").strip()
        if text:
            messages.append({"role": role, "content": text[:1500]})

    messages.append({"role": "user", "content": question[:2500]})

    body = json.dumps({
        "model": MODEL,
        "messages": messages,
        "temperature": 0.5,
        "max_tokens": 900,
        "presence_penalty": 0.2,
        "frequency_penalty": 0.2,
    }).encode("utf-8")

    req = urllib.request.Request(API_URL, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + KEY)

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:300]
        return None, "DeepSeek 返回 %d：%s" % (e.code, detail)
    except Exception as e:                                    # noqa: BLE001
        return None, "调用失败：%s" % e

    try:
        answer = data["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError):
        return None, "返回格式异常：" + json.dumps(data, ensure_ascii=False)[:200]
    return {"answer": answer, "usage": data.get("usage", {}),
            "model": data.get("model", MODEL)}, None


class Handler(BaseHTTPRequestHandler):
    server_version = "P4XAI/2.0"

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")

    def _json(self, obj, code=200):
        raw = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self._cors()
        self.end_headers()
        self.wfile.write(raw)

    def do_OPTIONS(self):                                     # noqa: N802
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):                                         # noqa: N802
        if self.path.startswith("/health"):
            self._json({"ok": True, "key": bool(KEY), "model": MODEL})
        else:
            self._json({"ok": False, "error": "unknown path"}, 404)

    def do_POST(self):                                        # noqa: N802
        if not self.path.startswith("/ask"):
            self._json({"ok": False, "error": "unknown path"}, 404)
            return
        try:
            n = int(self.headers.get("Content-Length") or 0)
            payload = json.loads(self.rfile.read(n).decode("utf-8") or "{}")
        except Exception as e:                                # noqa: BLE001
            self._json({"ok": False, "error": "bad json: %s" % e}, 400)
            return

        q = (payload.get("q") or "").strip()
        if not q:
            self._json({"ok": False, "error": "empty question"}, 400)
            return

        result, err = ask_deepseek(q, (payload.get("ctx") or "").strip(), payload.get("history"))
        if err:
            self._json({"ok": False, "error": err}, 502)
        else:
            self._json({"ok": True, **result})

    def log_message(self, fmt, *args):                        # 安静
        # pythonw.exe 下 sys.stderr 是 None，这里绝不能写，否则每个请求都会断开
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8766
    out = sys.stdout if sys.stdout is not None else open(os.devnull, "w")

    # Windows 默认允许重复绑定同一端口，会出现"两个进程都在听"的假象；
    # 关掉它，端口被占时直接报错，便于发现旧进程。
    class Srv(ThreadingHTTPServer):
        allow_reuse_address = False

    try:
        srv = Srv(("127.0.0.1", port), Handler)
    except OSError as e:
        out.write("端口 %d 被占用（先关掉旧的 ai_bridge）：%s\n" % (port, e))
        return

    out.write("P4X AI bridge v2 on http://127.0.0.1:%d   key=%s\n" % (port, "yes" if KEY else "NO"))
    out.flush()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()