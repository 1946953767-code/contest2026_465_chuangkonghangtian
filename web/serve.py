# -*- coding: utf-8 -*-
"""P4X 静态页面服务（可在 pythonw 下运行，不依赖 stdout）

为什么不用 `python -m http.server`：
  在 pythonw.exe 下没有 stdout/stderr，http.server 的日志调用会抛异常，
  结果是"端口在监听但每个请求都被立刻关闭"（浏览器显示 ERR_EMPTY_RESPONSE）。

用法：
  pythonw serve.py [port]     默认 8765，只监听 127.0.0.1（localhost）
"""

import os
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))


class QuietHandler(SimpleHTTPRequestHandler):
    """不写 stdout/stderr，避免 pythonw 下崩溃。"""

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

    def do_GET(self):
        if self.path in ('', '/'):
            self.path = '/dashboard.html'
        return super().do_GET()


class Srv(ThreadingHTTPServer):
    allow_reuse_address = False
    daemon_threads = True


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    try:
        srv = Srv(('127.0.0.1', port), QuietHandler)
    except OSError as e:
        sys.exit('端口 %d 被占用：%s' % (port, e))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()