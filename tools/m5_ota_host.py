#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
M5 · OTA 上位机下发工具（边缘智能网关）

用法：
  python m5_ota_host.py --file app.bin                 # 正常下发
  python m5_ota_host.py --file app.bin --resume        # 断点续传（从板子记录的 offset 继续）
  python m5_ota_host.py --file bad.bin                 # 故意发坏固件，验证防砖
  python m5_ota_host.py --file app.bin --corrupt       # 传对长度但中途改坏一块，验证分块 CRC

帧格式与 M4 协议一致（AA55 + LEN + CMD + DATA + CRC32），OTA 只是加了 3 个 CMD：
  0x20 OTA_BEGIN : magic(4) + total_len(4) + total_crc(4) + block_size(2)
  0x21 OTA_DATA  : offset(4) + seq(2) + block_crc(4) + data(N)
  0x22 OTA_END   : (空)
  0xA0 OTA_ACK   : status(1) + offset(4) + extra(4)

⭐ 关键行为（与板端约定）：
  ① 严格顺序：offset 必须等于板子的 received，否则板子回 RANGE 并带回期望值
  ② 断点续传：重连后发 OTA_BEGIN，若 crc 一致且 received>0，板子【跳过擦除】直接续
  ③ 防砖：整体 CRC 不符时板子拒绝切换；真正能不能切换由 Bootloader 自己再算一遍
"""

import argparse
import socket
import struct
import sys
import time
import zlib

SOF0 = 0xAA
SOF1 = 0x55

CMD_OTA_BEGIN = 0x20
CMD_OTA_DATA  = 0x21
CMD_OTA_END   = 0x22
CMD_OTA_ACK   = 0xA0
CMD_SNAPSHOT  = 0x81

OTA_MAGIC = 0x4F544155

STATUS_NAME = {
    0x00: "OK",
    0x01: "CRC_ERR",
    0x02: "OUT_OF_RANGE",
    0x03: "FLASH_ERR",
    0x04: "STATE_ERR",
    0x05: "BAD_PARAM",
}


def crc32(data):
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


def pack(cmd, data=b""):
    body = bytes(bytearray([cmd]) + bytearray(data))
    return (bytes(bytearray([SOF0, SOF1, len(body)])) + body +
            struct.pack(">I", crc32(body)))


def hexdump(bs, limit=24):
    b = bytearray(bs)
    s = " ".join("%02X" % x for x in b[:limit])
    if len(b) > limit:
        s += " ...(%d bytes)" % len(b)
    return s


class Client(object):
    def __init__(self, ip, port, timeout=5.0):
        self.s = socket.create_connection((ip, port), timeout=timeout)
        self.buf = bytearray()

    def send(self, cmd, data=b""):
        self.s.sendall(pack(cmd, data))

    def _pump(self):
        chunk = self.s.recv(4096)
        if not chunk:
            raise RuntimeError("connection closed by board")
        self.buf.extend(bytearray(chunk))

    def recv_frame(self, timeout):
        """收一帧，返回 (cmd, data)；超时返回 (None, None)"""
        self.s.settimeout(timeout)
        t_end = time.time() + timeout
        while True:
            f = self._try_parse()
            if f is not None:
                return f
            if time.time() > t_end:
                return (None, None)
            self._pump()

    def _try_parse(self):
        while True:
            while self.buf and self.buf[0] != SOF0:
                self.buf.pop(0)
            if len(self.buf) < 3:
                return None
            if self.buf[1] != SOF1:
                self.buf.pop(0)
                continue
            ln = self.buf[2]
            total = 3 + ln + 4
            if len(self.buf) < total:
                return None
            frame = self.buf[:total]
            del self.buf[:total]
            body = frame[3:3 + ln]
            want = struct.unpack(">I", bytes(frame[3 + ln:3 + ln + 4]))[0]
            if want != crc32(body):
                print("  [!] CRC error, frame dropped")
                continue
            return (body[0], bytes(body[1:]))

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def show_ack(tag, data):
    if len(data) < 9:
        print("  %s: malformed ack %s" % (tag, hexdump(data)))
        return None
    status = data[0]
    offset = struct.unpack(">I", data[1:5])[0]
    extra = struct.unpack(">I", data[5:9])[0]
    print("  %s: %-13s offset=%-8d extra=%d(0x%08X)" %
          (tag, STATUS_NAME.get(status, "?"), offset, extra, extra))
    return (status, offset, extra)


def main():
    ap = argparse.ArgumentParser(description="M5 OTA host tool")
    ap.add_argument("--ip", default="192.168.1.30")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--file", required=True, help="firmware .bin file")
    ap.add_argument("--block", type=int, default=240,
                    help="block size (<=244, see below)")
    ap.add_argument("--resume", action="store_true",
                    help="let the board decide offset (resume interrupted download)")
    ap.add_argument("--corrupt", action="store_true",
                    help="corrupt one block on purpose (test block CRC)")
    ap.add_argument("--bad-total-crc", action="store_true",
                    help="send a wrong total CRC (test whole-image CRC / anti-brick)")
    args = ap.parse_args()

    # ⭐ BUG-066：分块大小的硬上限由【协议的 LEN 字段宽度】决定，不是由板端缓冲决定。
    #   帧格式 = AA 55 LEN(1) CMD(1) DATA(N) CRC32(4)，LEN 只有【1 个字节】且
    #   LEN = 1(CMD) + N(DATA)，所以 N <= 254。
    #   OTA_DATA 的 DATA = offset(4) + seq(2) + block_crc(4) + payload(N) = 10 + N
    #   → 1 + 10 + N <= 255  →  payload <= 244
    #   之前默认 1024，pack() 里 bytearray([len(body)]) 直接抛
    #   ValueError: byte must be in range(0, 256) —— 根本发不出去。
    OTA_DATA_HDR   = 10
    MAX_BLOCK_SIZE = 255 - 1 - OTA_DATA_HDR     # = 244

    if args.block > MAX_BLOCK_SIZE:
        print("!! block size %d exceeds protocol limit %d "
              "(LEN is 1 byte: 1 + %d + N <= 255)" %
              (args.block, MAX_BLOCK_SIZE, OTA_DATA_HDR))
        print("   try: --block 240")
        return 1
    if args.block < 1:
        print("!! invalid block size %d" % args.block)
        return 1

    with open(args.file, "rb") as f:
        fw = f.read()

    total_len = len(fw)
    total_crc = crc32(fw)
    if args.bad_total_crc:
        total_crc ^= 0xFFFF          # 故意给错的整包 CRC

    print("firmware : %s" % args.file)
    print("length   : %d bytes" % total_len)
    print("crc32    : 0x%08X%s" % (total_crc, "  (!! deliberately wrong)" if args.bad_total_crc else ""))
    print("block    : %d" % args.block)
    print("-" * 60)

    c = Client(args.ip, args.port)
    try:
        # ---------- OTA_BEGIN ----------
        payload = struct.pack(">III", OTA_MAGIC, total_len, total_crc) + \
                  struct.pack(">H", args.block)
        c.send(CMD_OTA_BEGIN, payload)
        print(">> OTA_BEGIN len=%d crc=0x%08X block=%d" % (total_len, total_crc, args.block))

        cmd, data = c.recv_frame(30.0)     # 擦 Bank B 可能要好几秒
        if cmd != CMD_OTA_ACK:
            print("!! unexpected response cmd=0x%02X" % (cmd if cmd else 0))
            return 1
        ack = show_ack("BEGIN", data)
        if ack is None or ack[0] != 0x00:
            print("!! BEGIN rejected, abort")
            return 1

        offset = ack[1]
        print("start from offset = %d" % offset)

        # ---------- OTA_DATA ----------
        seq = 0
        t0 = time.time()
        corrupt_at = (total_len // 2) if args.corrupt else -1

        while offset < total_len:
            n = min(args.block, total_len - offset)
            chunk = bytearray(fw[offset:offset + n])

            # ⭐ BUG-067：CRC 必须按【原始正确数据】算，算完【再】改坏内容。
            #   原来的写法是先 chunk[idx] ^= 0xFF 再 crc32(chunk)，
            #   算出来的是"改坏后数据的正确 CRC" —— 板端校验必然通过，
            #   结果 --corrupt 什么也测不出来（实测被改坏的块回了 OK，
            #   只有最后整体 CRC 才暴露）。测试工具自己骗了自己。
            #   正确语义是模拟"传输中比特翻转"：发送方算的是原数据的 CRC，
            #   但线上数据被改了 → 接收方算出的 CRC 与帧里带的对不上 → 回 CRC_ERR。
            bcrc = crc32(chunk)

            if corrupt_at >= offset and corrupt_at < (offset + n):
                idx = corrupt_at - offset
                chunk[idx] ^= 0xFF
                print("  (corrupting block at offset %d: byte %d flipped)"
                      % (offset, idx))
            payload = struct.pack(">IH", offset, seq & 0xFFFF) + \
                      struct.pack(">I", bcrc) + bytes(chunk)

            c.send(CMD_OTA_DATA, payload)

            cmd, data = c.recv_frame(10.0)
            if cmd != CMD_OTA_ACK:
                print("!! no ack for block @%d" % offset)
                return 1

            ack = show_ack("DATA @%-7d" % offset, data) if (offset % (args.block * 10) == 0) else None
            if ack is None:
                # 没打印的那几次也要解析，判断是否出错
                st = data[0]
                off = struct.unpack(">I", data[1:5])[0]
                if st != 0x00:
                    print("  !! block @%d -> %s" % (offset, STATUS_NAME.get(st, "?")))
                    return 1
            else:
                if ack[0] != 0x00:
                    print("!! block rejected, abort")
                    return 1

            offset += n
            seq += 1

            pct = offset * 100 // total_len
            sys.stdout.write("\r  progress: %3d%%  (%d/%d)" % (pct, offset, total_len))
            sys.stdout.flush()

        dt = time.time() - t0
        print("\r  progress: 100%%  (%d/%d)  in %.1fs" % (offset, total_len, dt))

        # ---------- OTA_END ----------
        c.send(CMD_OTA_END, b"")
        print(">> OTA_END")
        cmd, data = c.recv_frame(15.0)
        if cmd == CMD_OTA_ACK:
            ack = show_ack("END", data)
            if ack and ack[0] == 0x00:
                print("-" * 60)
                print("OTA accepted. Board will reboot; Bootloader verifies and switches.")
                return 0
        print("-" * 60)
        print("OTA REJECTED (this is the expected result for a corrupted image)")
        return 0

    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
