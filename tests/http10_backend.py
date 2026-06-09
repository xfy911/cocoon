#!/usr/bin/env python3
"""
http10_backend.py — HTTP/1.0 后端模拟器

模拟一个 HTTP/1.0 后端，响应后发送 Connection: close 并立即关闭连接。
用于验证反向代理连接池的 HTTP/1.0 兼容性。

用法: python3 http10_backend.py <port> <directory>
"""

import sys
import socket
import os
import mimetypes

HTTP10_RESPONSE = """HTTP/1.0 200 OK
Connection: close
Content-Type: {ctype}
Content-Length: {length}

""".replace("\n", "\r\n")

HTTP10_NOT_FOUND = """HTTP/1.0 404 Not Found
Connection: close
Content-Type: text/plain
Content-Length: 13

404 Not Found
""".replace("\n", "\r\n")


def handle_client(conn, addr, doc_root):
    try:
        data = conn.recv(4096)
        if not data:
            return

        # 解析请求行
        lines = data.decode("utf-8", errors="replace").split("\r\n")
        if not lines:
            return
        parts = lines[0].split()
        if len(parts) < 2:
            return

        path = parts[1]
        if path == "/":
            path = "/index.html"

        # 去掉路径中的前缀（如 /backend）
        if path.startswith("/backend"):
            path = path[len("/backend"):]
        if not path.startswith("/"):
            path = "/" + path

        # 安全路径
        safe_path = os.path.normpath(os.path.join(doc_root, path.lstrip("/")))
        print(f"[http10] doc_root={doc_root}, path={path}, safe_path={safe_path}", flush=True)
        print(f"[http10] abspath={os.path.abspath(doc_root)}", flush=True)
        print(f"[http10] exists={os.path.exists(safe_path)}, isfile={os.path.isfile(safe_path) if os.path.exists(safe_path) else False}", flush=True)
        if not os.path.abspath(safe_path).startswith(os.path.abspath(doc_root)):
            conn.sendall(HTTP10_NOT_FOUND.encode())
            return

        if os.path.exists(safe_path) and os.path.isfile(safe_path):
            with open(safe_path, "rb") as f:
                body = f.read()
            ctype, _ = mimetypes.guess_type(safe_path)
            if not ctype:
                ctype = "application/octet-stream"
            header = HTTP10_RESPONSE.format(ctype=ctype, length=len(body))
            conn.sendall(header.encode() + body)
        else:
            conn.sendall(HTTP10_NOT_FOUND.encode())
    except Exception as e:
        print(f"[http10] 错误: {e}", file=sys.stderr)
    finally:
        conn.close()


def main():
    if len(sys.argv) < 3:
        print("用法: python3 http10_backend.py <port> <directory>", file=sys.stderr)
        sys.exit(1)

    port = int(sys.argv[1])
    doc_root = sys.argv[2]

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", port))
    s.listen(5)
    print(f"[http10] 启动于端口 {port}, 根目录: {doc_root}")

    while True:
        conn, addr = s.accept()
        handle_client(conn, addr, doc_root)


if __name__ == "__main__":
    main()
