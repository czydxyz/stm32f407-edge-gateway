#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
M4 上位机测试工具（边缘智能网关）

用途：在没有自研上位机软件之前，用这个脚本验收 M4 的应用层协议是否通。

能做三件事：
  1) --query   连板子的 5000 端口，发 0x03 查询帧，收 0x81 快照帧并解析打印
  2) --relay   发 0x02 设置继电器（例如 --relay 0x0F 打开低 4 路）
  3) --listen  本机监听 5001，接收板子的周期上报帧并解析（同时回 ACK，
               让板子进入稳定的 3s 周期，不再每 9s 重连）

用法示例：
  python m4_host_test.py --query
  python m4_host_test.py --query --watch 3        # 每 3 秒查一次，持续观察
  python m4_host_test.py --relay 0x0F             # 设置继电器低 4 路
  python m4_host_test.py --listen                 # 监听 5001 收上报（推荐先跑这个）
  python m4_host_test.py --listen --badcrc        # 发一帧 CRC 错的帧，验证板子 NAK

帧格式（与 Core/Inc/m4_proto.h 一致）：
  AA 55 | LEN | CMD | DATA | CRC32(大端)
  LEN = 1(CMD) + N(DATA)
  CRC32 覆盖 CMD+DATA，标准 CRC32（初值 0xFFFFFFFF，结果异或），与 zlib.crc32 一致
"""

import argparse
import socket
import struct
import sys
import time
import zlib

try:
    input_func = raw_input  # Python 2
except NameError:
    input_func = input

SOF0 = 0xAA
SOF1 = 0x55

CMD_REPORT   = 0x01
CMD_SET_RELAY= 0x02
CMD_QUERY    = 0x03
CMD_SNAPSHOT = 0x81
CMD_ACK      = 0x82
CMD_NAK      = 0xFF

CMD_NAME = {
    0x01: "REPORT", 0x02: "SET_RELAY", 0x03: "QUERY",
    0x20: "OTA_BEGIN", 0x21: "OTA_DATA", 0x22: "OTA_END", 0xA0: "OTA_ACK",
    0x81: "SNAPSHOT", 0x82: "ACK", 0xFF: "NAK",
}

NAK_NAME = {
    0x01: "BAD_CRC", 0x02: "BAD_LEN",
    0x03: "UNSUPPORTED", 0x04: "BAD_PARAM",
}


def crc32(data):
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


def pack(cmd, data=b""):
    """打包一帧"""
    body = bytes(bytearray([cmd]) + bytearray(data))
    c = crc32(body)
    return (bytes(bytearray([SOF0, SOF1, len(body)])) + body +
            struct.pack(">I", c))


def hexdump(bs):
    return " ".join("%02X" % b for b in bytearray(bs))


class Parser(object):
    """与板子端同构的解析状态机（用于收帧）"""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, chunk):
        """喂字节，返回解析出的帧列表 [(cmd, data), ...]"""
        self.buf.extend(bytearray(chunk))
        frames = []
        while True:
            # 找帧头
            while self.buf and self.buf[0] != SOF0:
                self.buf.pop(0)
            if len(self.buf) < 3:
                break
            if self.buf[1] != SOF1:
                self.buf.pop(0)
                continue
            ln = self.buf[2]
            total = 3 + ln + 4
            if len(self.buf) < total:
                break
            frame = self.buf[:total]
            del self.buf[:total]
            body = frame[3:3 + ln]
            want = struct.unpack(">I", bytes(frame[3 + ln:3 + ln + 4]))[0]
            got = crc32(body)
            if want != got:
                print("  [!] CRC error (want %08X got %08X), frame dropped"
                      % (want, got))
                continue
            frames.append((body[0], bytes(body[1:])))
        return frames


def decode_snapshot(data):
    """解析 0x81 快照帧的 DATA（M5 起为 39 字节，大端）

    35 -> 39：M5 在末尾加了 4 字节【上行序号 seq】（偏移 35，大端）。
    它跨链路、跨重连单调递增，上位机靠它去重。
    """
    if len(data) < 39:
        return {"error": "snapshot len %d < 39" % len(data)}
    if len(data) < 35:
        return {"error": "snapshot len %d < 35" % len(data)}

    def s16(off):
        return struct.unpack(">h", data[off:off + 2])[0]

    def u16(off):
        return struct.unpack(">H", data[off:off + 2])[0]

    def u32(off):
        return struct.unpack(">I", data[off:off + 4])[0]

    t = s16(0)
    h = s16(2)
    di = data[4]
    rl = data[5]
    tgt = data[6]
    flags = data[7]
    poll_total = u32(8)
    poll_fail = u32(12)
    can1_rx = u32(16)
    can2_rx = u32(20)
    can1_drop = u16(24)
    can2_drop = u16(26)
    age = u16(28)
    uptime = u32(30)
    cmd_bad = data[34]
    up_seq = u32(35) if len(data) >= 39 else 0

    ok = poll_total - poll_fail if poll_total >= poll_fail else 0
    rate = (ok * 100.0 / poll_total) if poll_total else 0.0

    return {
        "temp": t / 10.0,
        "humi": h / 10.0,
        "di": di,
        "relay": rl,
        "target": tgt,
        "dirty": bool(flags & 0x01),
        "eth_link": bool(flags & 0x02),
        "slave_online": (bool(flags & 0x04), bool(flags & 0x08)),
        "can_online": (bool(flags & 0x10), bool(flags & 0x20)),
        "poll_total": poll_total,
        "poll_fail": poll_fail,
        "rate": rate,
        "can1_rx": can1_rx,
        "can2_rx": can2_rx,
        "can1_drop": can1_drop,
        "can2_drop": can2_drop,
        "age_s": age,
        "uptime_s": uptime,
        "cmd_bad": cmd_bad,
        "up_seq": up_seq,
    }


def print_snapshot(d):
    if "error" in d:
        print("  snapshot error:", d["error"])
        return
    stale = ""
    if d["age_s"] == 0xFFFF:
        stale = "  (never updated!)"
    elif d["age_s"] > 5:
        stale = "  (STALE %ds)" % d["age_s"]
    print("  T=%.1fC  H=%.1f%%  age=%ds%s" % (d["temp"], d["humi"], d["age_s"], stale))
    print("  DI=0x%02X  RL=0x%02X  target=0x%02X  dirty=%d"
          % (d["di"], d["relay"], d["target"], d["dirty"]))
    print("  slaves online=%s,%s   eth=%s   can online=%s,%s"
          % (d["slave_online"][0], d["slave_online"][1],
             d["eth_link"], d["can_online"][0], d["can_online"][1]))
    print("  poll %d/%d ok (%.1f%%)   CAN rx=%d/%d drop=%d/%d"
          % (d["poll_total"] - d["poll_fail"], d["poll_total"], d["rate"],
             d["can1_rx"], d["can2_rx"], d["can1_drop"], d["can2_drop"]))
    print("  uptime=%ds  cmd_bad=%d  seq=%d" % (d["uptime_s"], d["cmd_bad"], d["up_seq"]))


class Dedup(object):
    """⭐ 上行序号去重（M5 主备切换的关键）

    为什么必须有：切换链路的瞬间，主链路发出的 seq=100 可能 ACK 丢了，
    系统切到备份后【用同一个 seq 重发】。上位机若不去重，同一条数据会入库两次。
    对温度这种无所谓，但对累计量/事件型数据就是数据事故。

    ⚠️ 必须用"有符号差值"比较，不能用 seq > last：
       uint32 回绕后新的 seq（比如 1）会小于 last（4294967290），
       朴素比较会把整整一轮新数据误判成旧数据丢掉。
    """
    def __init__(self):
        self.last = None
        self.dup = 0
        self.ok = 0

    def accept(self, seq):
        if self.last is None:
            self.last = seq
            self.ok += 1
            return True
        d = (seq - self.last) & 0xFFFFFFFF
        if d == 0 or d > 0x80000000:     # 重复 or 比 last 更旧
            self.dup += 1
            return False
        self.last = seq
        self.ok += 1
        return True


def do_query(ip, port, watch, badcrc):
    parser = Parser()
    period = watch if watch else 0
    while True:
        try:
            s = socket.create_connection((ip, port), timeout=3)
        except Exception as e:
            print("connect %s:%d failed: %s" % (ip, port, e))
            if not period:
                return 1
            time.sleep(period)
            continue

        print("connected to %s:%d" % (ip, port))
        try:
            if badcrc:
                # 故意构造一帧 CRC 错的帧，验证板子会回 NAK(BAD_CRC)
                f = bytearray(pack(CMD_QUERY))
                f[-1] ^= 0xFF
                print(">> (bad crc) %s" % hexdump(bytes(f)))
                s.sendall(bytes(f))
            else:
                f = pack(CMD_QUERY)
                print(">> %s" % hexdump(f))
                s.sendall(f)

            try:
                rx = s.recv(1024)
            except socket.timeout:
                print("  (no response, timeout)")
                rx = b""
            if rx:
                print("<< %s" % hexdump(rx))
                for cmd, data in parser.feed(rx):
                    print("  frame CMD=0x%02X (%s) len=%d"
                          % (cmd, CMD_NAME.get(cmd, "?"), len(data)))
                    if cmd == CMD_SNAPSHOT:
                        print_snapshot(decode_snapshot(data))
                    elif cmd == CMD_NAK:
                        print("  NAK: %s" % NAK_NAME.get(data[0], "?"))
                    elif cmd == CMD_ACK:
                        print("  ACK ok")
        finally:
            s.close()

        if not period:
            return 0
        time.sleep(period)


def do_relay(ip, port, bitmap):
    try:
        s = socket.create_connection((ip, port), timeout=3)
    except Exception as e:
        print("connect failed: %s" % e)
        return 1
    f = pack(CMD_SET_RELAY, bytes(bytearray([bitmap])))
    print(">> %s  (set relay 0x%02X)" % (hexdump(f), bitmap))
    s.sendall(f)
    try:
        rx = s.recv(256)
        print("<< %s" % hexdump(rx))
        for cmd, data in Parser().feed(rx):
            print("  CMD=0x%02X (%s)" % (cmd, CMD_NAME.get(cmd, "?")))
            if cmd == CMD_NAK:
                print("  NAK: %s" % NAK_NAME.get(data[0], "?"))
    except socket.timeout:
        print("  (no response)")
    s.close()
    return 0


def do_listen(port, ack, seconds):
    """监听端口，接收板子周期上报"""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(2)
    srv.settimeout(1.0)
    print("listening on 0.0.0.0:%d  (waiting for board report, Ctrl+C to stop)" % port)

    parser = Parser()
    dedup = Dedup()
    conn = None
    t_end = time.time() + seconds if seconds else None
    n = 0

    try:
        while True:
            if t_end and time.time() > t_end:
                break
            if conn is None:
                try:
                    conn, addr = srv.accept()
                    conn.settimeout(1.0)
                    parser = Parser()
                    print("board connected from %s:%d" % (addr[0], addr[1]))
                except socket.timeout:
                    continue
            try:
                rx = conn.recv(1024)
            except socket.timeout:
                continue
            except Exception:
                conn = None
                continue
            if not rx:
                print("board disconnected")
                conn = None
                continue

            n += 1
            print("[%d] << %s" % (n, hexdump(rx)))
            for cmd, data in parser.feed(rx):
                if cmd == CMD_SNAPSHOT or cmd == CMD_REPORT:
                    d = decode_snapshot(data)
                    if "error" not in d:
                        fresh = dedup.accept(d["up_seq"])
                        if not fresh:
                            print("  (dup seq=%d, dropped)" % d["up_seq"])
                            continue
                    print("  SNAPSHOT:")
                    print_snapshot(d)
                else:
                    print("  CMD=0x%02X (%s)" % (cmd, CMD_NAME.get(cmd, "?")))
            if ack:
                # 回一帧 ACK：板子收到后进入稳定 3s 周期（否则 9s 超时重连）
                f = pack(CMD_ACK, b"\x00")
                try:
                    conn.sendall(f)
                except Exception:
                    conn = None
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        srv.close()
    return 0


def main():
    ap = argparse.ArgumentParser(description="M4 gateway host test tool")
    ap.add_argument("--ip", default="192.168.1.30", help="board IP (default 192.168.1.30)")
    ap.add_argument("--port", type=int, default=5000, help="board cmd port (default 5000)")
    ap.add_argument("--query", action="store_true", help="send QUERY and print snapshot")
    ap.add_argument("--watch", type=float, default=0, help="repeat query every N seconds")
    ap.add_argument("--badcrc", action="store_true", help="send a frame with wrong CRC")
    ap.add_argument("--relay", type=lambda x: int(x, 0), help="set relay bitmap, e.g. 0x0F")
    ap.add_argument("--listen", action="store_true", help="listen for board report")
    ap.add_argument("--listen-port", type=int, default=5001, help="listen port (default 5001)")
    ap.add_argument("--ack", action="store_true", help="reply ACK when listening")
    ap.add_argument("--seconds", type=float, default=0, help="stop after N seconds")
    args = ap.parse_args()

    if args.listen:
        return do_listen(args.listen_port, args.ack, args.seconds)
    if args.relay is not None:
        return do_relay(args.ip, args.port, args.relay & 0xFF)
    if args.query or args.watch:
        return do_query(args.ip, args.port, args.watch, args.badcrc)

    ap.print_help()
    print("\n提示：没有参数时默认不做任何事。常用组合：")
    print("  python m4_host_test.py --listen --ack        # 先看板子上报")
    print("  python m4_host_test.py --query --watch 3     # 主动查询，每 3s 一次")
    return 0


if __name__ == "__main__":
    sys.exit(main())
