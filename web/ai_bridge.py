# -*- coding: utf-8 -*-
"""P4X 答辩 AI 助手 · 本地代理（只用标准库）

作用：
  网页（p4x_web.html）里的「AI 助手」把问题发到这里，这里再去调 DeepSeek，
  这样 **API Key 永远不会出现在网页/前端代码里**。

接口：
  GET  /health        -> {"ok":true,"key":true,"model":"deepseek-chat"}
  POST /ask           -> {"ok":true,"answer":"...","model":"...","usage":{...}}
       body: {"q":"问题","ctx":"最近遥测/日志片段"}

启动：
  python ai_bridge.py            # 默认 127.0.0.1:8766
  python ai_bridge.py 8766

Key 来源（按顺序）：
  1) 环境变量 DEEPSEEK_API_KEY
  2) ~/.dsh/.credentials.yaml 里的 DEEPSEEK_API_KEY
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

SYSTEM = (
    "你是「P4X 单芯片火箭遥测系统」的现场 AI 助手，服务对象是参赛答辩现场。\n"
    "要求：\n"
    "1) 回答用中文，短句，先给结论再给依据；\n"
    "2) 只基于给定的事实与通用工程常识，不要编造没有的数据；\n"
    "3) 涉及遥测数字时，先解释这个数字说明什么，再给建议；\n"
    "4) 如果数据不足以判断，直说需要看哪几个量；\n"
    "5) 单次回答不超过 180 字，除非用户要求展开。\n"
    "背景事实：箭载端是 ESP32-S3 单芯片（MPU6050/BMP280/AHT20/GNSS GP21/TF 卡/LoRa A28），"
    "地面站是 ESP32-P4X + openvela(NuttX) + 1024x600 MIPI-DSI；"
    "LoRa 9600 8N1，遥测 8 Hz，full 帧 175 B、quick 帧 26 B；"
    "姿态用互补滤波（陀螺积分 + 加速度计修正），偏航无磁力计只能相对积分。"
)


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
        messages.append({"role": "system",
                         "content": "下面是现场数据/日志片段，回答时可以参考：\n" + ctx[:4000]})
    for h in (history or [])[-4:]:
        role = "assistant" if h.get("role") == "assistant" else "user"
        text = (h.get("text") or "").strip()
        if text:
            messages.append({"role": role, "content": text[:1500]})
    messages.append({"role": "user", "content": question[:2000]})

    body = json.dumps({
        "model": MODEL,
        "messages": messages,
        "temperature": 0.3,
        "max_tokens": 600,
    }).encode("utf-8")

    req = urllib.request.Request(API_URL, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + KEY)

    try:
        with urllib.request.urlopen(req, timeout=45) as resp:
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
    return {"answer": answer, "usage": data.get("usage", {}), "model": data.get("model", MODEL)}, None


class Handler(BaseHTTPRequestHandler):
    server_version = "P4XAI/1.0"

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

    def log_message(self, fmt, *args):                        # 安静一点
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8766
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print("P4X AI bridge on http://127.0.0.1:%d  key=%s" % (port, "yes" if KEY else "NO"))
    print("  GET  /health")
    print("  POST /ask   {\"q\":\"...\",\"ctx\":\"...\"}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()