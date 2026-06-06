#!/usr/bin/env python3
"""
WebSocket 集成测试 — 使用 Python 标准库实现

测试内容：
1. HTTP 升级握手（101 Switching Protocols）
2. 发送文本帧，接收 echo 回显
3. 发送关闭帧，接收关闭响应
"""

import socket
import base64
import hashlib
import struct
import sys

HOST = "localhost"
PORT = 9999
GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def build_handshake():
    """构建 WebSocket 握手请求"""
    key = base64.b64encode(b"\x00" * 16).decode()
    req = (
        f"GET /ws HTTP/1.1\r\n"
        f"Host: {HOST}:{PORT}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"\r\n"
    )
    return req, key


def parse_response(data):
    """解析 HTTP 101 响应"""
    lines = data.split(b"\r\n")
    status = lines[0].decode()
    headers = {}
    for line in lines[1:]:
        if line == b"":
            break
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.decode().strip().lower()] = v.decode().strip()
    return status, headers


def compute_accept(key):
    """计算 Sec-WebSocket-Accept"""
    concat = key.encode() + GUID
    digest = hashlib.sha1(concat).digest()
    return base64.b64encode(digest).decode()


def build_frame(opcode, payload, masked=True):
    """构建 WebSocket 帧（客户端发送，带掩码）"""
    length = len(payload)
    header = bytearray()
    header.append(0x80 | opcode)  # FIN=1, opcode

    if length <= 125:
        header.append((0x80 if masked else 0x00) | length)
    elif length <= 65535:
        header.append((0x80 if masked else 0x00) | 126)
        header.extend(struct.pack(">H", length))
    else:
        header.append((0x80 if masked else 0x00) | 127)
        header.extend(struct.pack(">Q", length))

    if masked:
        mask = b"\x12\x34\x56\x78"
        masked_payload = bytearray()
        for i, b in enumerate(payload):
            masked_payload.append(b ^ mask[i % 4])
        return bytes(header) + mask + bytes(masked_payload)

    return bytes(header) + payload


def parse_frame(data):
    """解析服务器发来的 WebSocket 帧（无掩码）"""
    if len(data) < 2:
        return None, 0

    b0 = data[0]
    b1 = data[1]
    fin = (b0 >> 7) & 1
    opcode = b0 & 0x0F
    payload_len = b1 & 0x7F
    offset = 2

    if payload_len == 126:
        if len(data) < 4:
            return None, 0
        payload_len = struct.unpack(">H", data[2:4])[0]
        offset = 4
    elif payload_len == 127:
        if len(data) < 10:
            return None, 0
        payload_len = struct.unpack(">Q", data[2:10])[0]
        offset = 10

    # 服务器不掩码
    if len(data) < offset + payload_len:
        return None, 0

    payload = data[offset:offset + payload_len]
    return {"opcode": opcode, "fin": fin, "payload": payload}, offset + payload_len


def recv_all(sock, n):
    """接收至少 n 字节数据"""
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            break
        data += chunk
    return data


def recv_frame(sock):
    """接收一个完整帧（自动读取足够的数据）"""
    data = b""
    while True:
        frame, consumed = parse_frame(data)
        if frame is not None:
            return frame, consumed
        chunk = sock.recv(1024)
        if not chunk:
            return None, 0
        data += chunk


def test_handshake():
    """测试 WebSocket 握手"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((HOST, PORT))

    req, key = build_handshake()
    sock.send(req.encode())

    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(1024)
        if not chunk:
            break
        data += chunk

    status, headers = parse_response(data)
    if "101" not in status:
        print(f"FAIL: 期望 101，实际: {status}")
        sock.close()
        return False

    accept = compute_accept(key)
    if headers.get("sec-websocket-accept") != accept:
        print(f"FAIL: Sec-WebSocket-Accept 不匹配")
        print(f"  期望: {accept}")
        print(f"  实际: {headers.get('sec-websocket-accept')}")
        sock.close()
        return False

    print("PASS: WebSocket 握手 101 + Sec-WebSocket-Accept 正确")
    sock.close()
    return True


def test_echo():
    """测试文本帧 echo"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    sock.connect((HOST, PORT))

    req, key = build_handshake()
    sock.send(req.encode())

    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(1024)
        if not chunk:
            break
        data += chunk

    # 发送文本帧
    msg = b"Hello, Cocoon!"
    frame = build_frame(0x01, msg)
    sock.send(frame)

    # 接收 echo 回显
    frame, _ = recv_frame(sock)
    if frame is None:
        print("FAIL: 未收到响应帧")
        sock.close()
        return False

    if frame["opcode"] != 0x01:
        print(f"FAIL: 期望文本帧(1)，实际操作码: {frame['opcode']}")
        sock.close()
        return False

    if frame["payload"] != msg:
        print(f"FAIL: 回显内容不匹配")
        print(f"  期望: {msg}")
        print(f"  实际: {frame['payload']}")
        sock.close()
        return False

    print("PASS: 文本帧 echo 正确")

    # 发送关闭帧
    close_frame = build_frame(0x08, b"\x03\xe8")  # 1000
    sock.send(close_frame)

    # 接收关闭响应
    frame, _ = recv_frame(sock)
    if frame is None or frame["opcode"] != 0x08:
        print("FAIL: 未收到关闭帧响应")
        sock.close()
        return False

    print("PASS: 关闭帧响应正确")
    sock.close()
    return True


def test_timeout(port=PORT, timeout=2):
    """测试 WebSocket 空闲超时 — 连接后不发帧，等待服务端关闭"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout + 3)
    sock.connect((HOST, port))

    req, key = build_handshake()
    sock.send(req.encode())

    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(1024)
        if not chunk:
            break
        data += chunk

    status, headers = parse_response(data)
    if "101" not in status:
        print(f"FAIL: 握手失败: {status}")
        sock.close()
        return False

    # 等待服务端超时关闭（不发任何帧）
    try:
        frame, _ = recv_frame(sock)
        if frame is not None and frame["opcode"] == 0x08:
            print("PASS: WebSocket 空闲超时后收到服务端关闭帧")
            sock.close()
            return True
        else:
            print(f"FAIL: 超时后未收到关闭帧，opcode={frame['opcode'] if frame else 'None'}")
            sock.close()
            return False
    except socket.timeout:
        print("FAIL: 等待超时关闭时本地 socket 超时")
        sock.close()
        return False


if __name__ == "__main__":
    passed = 0
    failed = 0

    # 支持命令行参数：--timeout-test 只运行超时测试（用于短超时服务器）
    timeout_test_only = len(sys.argv) > 1 and sys.argv[1] == "--timeout-test"

    if not timeout_test_only:
        if test_handshake():
            passed += 1
        else:
            failed += 1

        if test_echo():
            passed += 1
        else:
            failed += 1

    # 超时测试只在 --timeout-test 模式下运行（需要服务器配置短超时）
    if timeout_test_only:
        if test_timeout():
            passed += 1
        else:
            failed += 1

    print(f"\n通过: {passed}, 失败: {failed}")
    sys.exit(0 if failed == 0 else 1)
