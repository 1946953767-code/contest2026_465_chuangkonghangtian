# -*- coding: utf-8 -*-
"""P4X 静态页面服务 + 同源 AI 代理（可在 pythonw 下运行，不依赖 stdout）

为什么自己做：
  `python -m http.server` 在 pythonw.exe 下没有 stdout，写日志会抛异常，
  结果是"端口在监听但每个请求被立刻关闭"（浏览器 ERR_EMPTY_RESPONSE）。

为什么带 AI 代理：
  页面与 AI 桥（8766）跨端口会被浏览器的 Private Network Access 拦，
  这里把 /ai/* 转发到 8766，页面只访问同源的 /ai/ask，彻底避开 CORS/PNA。

用法：
  pythonw serve.py [port]     默认 8765，只监听 127.0.0.1
"""

import json
import os
import sys
import urllib.error
import urllib.request
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
AI_BASE = 'http://127.0.0.1:8766'


class QuietHandler(SimpleHTTPRequestHandler):
    """静态文件 + /ai/* 代理；不写 stdout/stderr。"""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=HERE, **kwargs)

    def log_message(self, fmt, *args):
        pass

    def log_error(self, fmt, *args):
        pass

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except (ConnectionResetError, BrokenPipeError):
            self.close_connection = True

    # ---------- 同源 AI 代理 ----------
    def _proxy_ai(self, path, body=None, method='GET'):
        url = AI_BASE + path
        req = urllib.request.Request(url, data=body, method=method)
        if body is not None:
            req.add_header('Content-Type', 'application/json')
        try:
            with urllib.request.urlopen(req, timeout=90) as resp:
                raw = resp.read()
                status = resp.status
        except urllib.error.HTTPError as e:
            raw = e.read()
            status = e.code
        except Exception as e:                                # AI 桥没起
            raw = json.dumps({'ok': False, 'error': 'AI 服务未启动(8766)：%s' % e},
                             ensure_ascii=False).encode('utf-8')
            status = 200
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(raw)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path.startswith('/ai/'):
            return self._proxy_ai(self.path[3:])
        if self.path in ('', '/'):
            self.path = '/dashboard.html'
        return super().do_GET()

    def do_POST(self):
        if self.path.startswith('/ai/'):
            n = int(self.headers.get('Content-Length') or 0)
            body = self.rfile.read(n) if n else b''
            return self._proxy_ai(self.path[3:], body, 'POST')
        self.send_error(404)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Private-Network', 'true')
        self.end_headers()


class Srv(ThreadingHTTPServer):
    allow_reuse_address = False
    daemon_threads = True


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    try:
        srv = Srv(('127.0.0.1', port), QuietHandler)
    except OSError as e:
        out = sys.stdout or open(os.devnull, 'w')
        out.write('端口 %d 被占用：%s\n' % (port, e))
        return
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()