#!/usr/bin/env python3
"""
https_backend.py — 简单的 HTTPS 后端服务器，用于反向代理 HTTPS 测试。

用法: python3 https_backend.py <port> <cert_file> <key_file> <directory>
"""
import sys
import ssl
from http.server import HTTPServer, SimpleHTTPRequestHandler

if len(sys.argv) < 5:
    print("用法: python3 https_backend.py <port> <cert_file> <key_file> <directory>")
    sys.exit(1)

port = int(sys.argv[1])
cert_file = sys.argv[2]
key_file = sys.argv[3]
directory = sys.argv[4]

class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=directory, **kwargs)

    def log_message(self, format, *args):
        # 抑制日志输出
        pass

context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert_file, key_file)

server = HTTPServer(("localhost", port), Handler)
server.socket = context.wrap_socket(server.socket, server_side=True)
server.serve_forever()
