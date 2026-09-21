#!/usr/bin/env python3
"""Host tool for the direct-framebuffer serial protocol used by the AI home firmware.

Examples:
  miaoink4_serial.py tap 240 300
  miaoink4_serial.py gesture 240 300 120 300
  miaoink4_serial.py key SELECT CLICK
  miaoink4_serial.py frame --out /tmp/home.pbm

The firmware speaks line-oriented UTF-8 at 921600 baud. FRAME? is parsed
until its explicit end marker and its CRC32 is checked before an optional PBM
image is written.  No pyserial/Pillow dependency is required.
"""

from __future__ import annotations

import argparse
import binascii
import errno
import fcntl
import os
import re
import select
import sys
import termios
import time
import struct
from pathlib import Path


DEFAULT_PORT = "/dev/cu.usbmodem21101"
DEFAULT_BAUD = 921600
LOGICAL_WIDTH = 480
LOGICAL_HEIGHT = 800
_BEGIN_RE = re.compile(r"^@@RAW_FRAME_BEGIN\s+(.*)$")
_DATA_RE = re.compile(r"^@@RAW_FRAME_DATA\s+offset=(\d+)\s+data=([0-9a-fA-F]+)$")


def _parse_fields(text: str) -> dict[str, str]:
    return dict(re.findall(r"([A-Za-z_]+)=([^\s]+)", text))


class SerialLink:
    def __init__(self, port: str, baud: int, timeout: float) -> None:
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.fd = -1
        self._old_attrs = None
        self._line_buffer = bytearray()

    def __enter__(self) -> "SerialLink":
        try:
            self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as exc:
            raise RuntimeError(
                f"无法打开串口 {self.port}: {exc}. "
                "如果设备显示为 USB 存储，请先退出 U 盘模式或按复位键。"
            ) from exc
        try:
            self._old_attrs = termios.tcgetattr(self.fd)
            attrs = termios.tcgetattr(self.fd)
            attrs[0] = 0  # iflag
            attrs[1] = 0  # oflag
            attrs[2] = termios.CLOCAL | termios.CREAD | getattr(termios, "CS8", 0)  # cflag
            attrs[3] = 0  # lflag
            speed = getattr(termios, f"B{self.baud}", None)
            if speed is None:
                # Darwin does not expose every high baud constant through
                # Python's termios module.  IOSSIOSPEED accepts the integer
                # rate used by USB Serial/JTAG after the raw attributes are
                # installed.  Keep a harmless fallback in the termios speed
                # fields for
                # platforms where the ioctl is unavailable.
                speed = getattr(termios, "B115200", termios.B9600)
            attrs[4] = speed
            attrs[5] = speed
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            if getattr(termios, f"B{self.baud}", None) is None:
                # IOSSIOSPEED is _IOW('T', 2, speed_t) in Darwin's IOSSI.h.
                try:
                    fcntl.ioctl(self.fd, 0x80045402, struct.pack("I", self.baud))
                except OSError as exc:
                    # A pseudo-terminal used by a host-side unit test has no
                    # Darwin speed ioctl; real /dev/cu.* nodes do.
                    if exc.errno != errno.ENOTTY:
                        raise
            # Darwin asserts DTR/RTS when the node is opened. On ESP32-S3 USB
            # Serial/JTAG those lines drive EN and GPIO0, so leaving them
            # asserted holds the target in ROM download mode and the product
            # firmware never runs. Release both so the application keeps
            # running while we talk to it.
            self._release_modem_lines()
        except Exception:
            self.close()
            raise
        # Opening the ESP32-S3 USB Serial/JTAG node can reset the target.
        # Drain boot chatter so the first command is not joined to an old line.
        self.drain(1.5)
        return self

    # Darwin's modem-control ioctls have no Python wrapper, so the raw values
    # are spelled out here (sys/ioctl.h / ttycom.h).
    _TIOCM_DTR = 0x002
    _TIOCM_RTS = 0x004
    _TIOCMBIC = 0x8004746B  # clear the given modem bits

    def _release_modem_lines(self) -> None:
        """Deassert DTR/RTS so the target is not held in download mode."""
        try:
            fcntl.ioctl(self.fd, self._TIOCMBIC,
                        struct.pack("I", self._TIOCM_DTR | self._TIOCM_RTS))
        except OSError as exc:
            # Pseudo-terminals used by host-side tests have no modem lines.
            if exc.errno != errno.ENOTTY:
                raise

    def close(self) -> None:
        if self.fd >= 0:
            if self._old_attrs is not None:
                try:
                    termios.tcsetattr(self.fd, termios.TCSANOW, self._old_attrs)
                except OSError:
                    pass
            os.close(self.fd)
            self.fd = -1

    def __exit__(self, _type, _value, _traceback) -> None:
        self.close()

    def drain(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            wait = max(0.0, min(0.05, deadline - time.monotonic()))
            ready, _, _ = select.select([self.fd], [], [], wait)
            if not ready:
                continue
            try:
                os.read(self.fd, 4096)
            except BlockingIOError:
                pass

    def write_line(self, command: str) -> None:
        payload = (command.rstrip("\r\n") + "\n").encode("utf-8")
        offset = 0
        deadline = time.monotonic() + self.timeout
        while offset < len(payload):
            try:
                written = os.write(self.fd, payload[offset:])
                if written == 0:
                    raise TimeoutError("串口写入返回 0 字节")
                offset += written
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise TimeoutError("串口写入超时")
                time.sleep(0.001)

    def read_line(self, timeout: float | None = None) -> str | None:
        limit = self.timeout if timeout is None else timeout
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            newline = self._line_buffer.find(b"\n")
            if newline >= 0:
                raw = bytes(self._line_buffer[:newline])
                del self._line_buffer[: newline + 1]
                return raw.rstrip(b"\r").decode("utf-8", errors="replace")
            wait = max(0.0, min(0.05, deadline - time.monotonic()))
            ready, _, _ = select.select([self.fd], [], [], wait)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            if chunk:
                self._line_buffer.extend(chunk)
            else:
                time.sleep(0.005)
        return None


def _print_reply(link: SerialLink, command: str, timeout: float) -> int:
    link.write_line(command)
    deadline = time.monotonic() + timeout
    got_reply = False
    while time.monotonic() < deadline:
        line = link.read_line(max(0.01, deadline - time.monotonic()))
        if line is None:
            break
        if line.startswith("@@"):
            print(line)
            got_reply = True
            if line.startswith(("@@INPUT_ACK", "@@INPUT_ERROR", "@@STATE", "@@INPUT_HELP", "@@FONT_ACK", "@@CAPSULE_ACK", "@@MCP_REPLY", "@@REMINDER_STATE", "@@DIAG_")):
                break
    if not got_reply:
        print("(未收到协议回复；普通 UI 命令可能只触发动作，不回 ACK)", file=sys.stderr)
        return 1
    return 0


def _validate_xy(x: int, y: int, label: str = "坐标") -> None:
    if not 0 <= x < LOGICAL_WIDTH or not 0 <= y < LOGICAL_HEIGHT:
        raise ValueError(f"{label}超出逻辑竖屏范围 x=0..479, y=0..799")


def _send_gesture(link: SerialLink, x0: int, y0: int, x1: int, y1: int,
                   steps: int, interval: float, timeout: float) -> int:
    """Replay one physical-like gesture while keeping one USB session open.

    Opening a USB Serial/JTAG node may reset an ESP32-S3, so DOWN/MOVE/UP
    must not be split across three independent process invocations.  This
    helper deliberately waits for each ACK before sending the next point.
    """
    if steps < 0 or steps > 64:
        raise ValueError("gesture 的 steps 必须在 0..64")
    if interval < 0 or interval > 10:
        raise ValueError("gesture 的 interval 必须在 0..10 秒")
    commands = [f"TOUCH DOWN {x0} {y0}"]
    for step in range(1, steps + 1):
        x = x0 + (x1 - x0) * step // (steps + 1)
        y = y0 + (y1 - y0) * step // (steps + 1)
        commands.append(f"TOUCH MOVE {x} {y}")
    commands.append(f"TOUCH UP {x1} {y1}")
    for index, command in enumerate(commands):
        result = _print_reply(link, command, timeout)
        if result != 0:
            return result
        if index + 1 < len(commands) and interval:
            time.sleep(interval)
    return 0


def _write_pbm(path: Path, width: int, height: int, payload: bytes) -> None:
    stride = (width + 7) // 8
    if len(payload) != stride * height:
        raise ValueError(f"帧长度 {len(payload)} != {stride}*{height}")
    # Firmware framebuffer uses 0=black, 1=white; PBM uses 1=black.
    inverted = bytes((value ^ 0xFF) for value in payload)
    path.write_bytes(f"P4\n{width} {height}\n".encode("ascii") + inverted)


def _read_frame(link: SerialLink, panel: bool, timeout: float, out: Path | None) -> int:
    link.write_line("FRAME_PANEL?" if panel else "FRAME?")
    deadline = time.monotonic() + timeout
    header: dict[str, str] | None = None
    payload: bytearray | None = None
    coverage: bytearray | None = None
    while time.monotonic() < deadline:
        line = link.read_line(max(0.01, deadline - time.monotonic()))
        if line is None:
            break
        match = _BEGIN_RE.match(line)
        if match:
            header = _parse_fields(match.group(1))
            try:
                payload = bytearray(int(header["bytes"]))
            except (KeyError, ValueError) as exc:
                raise RuntimeError(f"帧头无效: {line}") from exc
            if len(payload) <= 0 or len(payload) > 800 * 480 // 8:
                raise RuntimeError(f"帧长度异常: {len(payload)}")
            try:
                width = int(header["w"])
                height = int(header["h"])
                stride = int(header["stride"])
            except (KeyError, ValueError) as exc:
                raise RuntimeError(f"帧尺寸字段无效: {line}") from exc
            if (width, height) not in ((LOGICAL_WIDTH, LOGICAL_HEIGHT), (800, 480)):
                raise RuntimeError(f"帧尺寸异常: {width}x{height}")
            if stride != (width + 7) // 8 or len(payload) != stride * height:
                raise RuntimeError(f"帧步长/长度异常: {width}x{height} stride={stride} bytes={len(payload)}")
            coverage = bytearray(len(payload))
            continue
        if header is None or payload is None:
            if line.startswith("@@RAW_FRAME_ERROR"):
                print(line, file=sys.stderr)
                return 1
            continue
        data_match = _DATA_RE.match(line)
        if data_match:
            offset = int(data_match.group(1))
            chunk = bytes.fromhex(data_match.group(2))
            if offset < 0 or offset + len(chunk) > len(payload):
                raise RuntimeError(f"帧数据越界: {line}")
            payload[offset : offset + len(chunk)] = chunk
            coverage[offset : offset + len(chunk)] = b"\x01" * len(chunk)
            continue
        if line == "@@RAW_FRAME_END":
            if coverage is None or not all(coverage):
                raise RuntimeError("帧数据不完整")
            expected = int(header.get("crc", "0"), 16)
            actual = binascii.crc32(payload) & 0xFFFFFFFF
            width = int(header["w"])
            height = int(header["h"])
            print(
                f"frame kind={header.get('kind', '?')} seq={header.get('seq', '?')} "
                f"{width}x{height} bytes={len(payload)} "
                f"crc={'OK' if actual == expected else 'MISMATCH'} "
                f"(device={expected:08x} host={actual:08x})"
            )
            if actual != expected:
                return 2
            if out is not None:
                if out.suffix.lower() == ".pbm":
                    _write_pbm(out, width, height, bytes(payload))
                else:
                    out.write_bytes(payload)
                print(f"saved {out}")
            return 0
    raise TimeoutError("等待完整 RAW_FRAME 超时")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--timeout", type=float, default=5.0)
    sub = parser.add_subparsers(dest="op", required=True)

    for name in ("tap", "click", "down", "move", "up"):
        command = sub.add_parser(name)
        command.add_argument("x", type=int)
        command.add_argument("y", type=int)

    swipe = sub.add_parser("swipe")
    swipe.add_argument("values", nargs="+", type=int, metavar="N")
    gesture = sub.add_parser("gesture")
    gesture.add_argument("x0", type=int)
    gesture.add_argument("y0", type=int)
    gesture.add_argument("x1", type=int)
    gesture.add_argument("y1", type=int)
    gesture.add_argument("--steps", type=int, default=2,
                         help="中间 MOVE 点数量（默认 2）")
    gesture.add_argument("--interval", type=float, default=0.02,
                         help="协议 ACK 间隔秒数（默认 0.02）")
    sub.add_parser("back")
    sub.add_parser("state")
    sub.add_parser("help")
    for name in ("button", "key"):
        command = sub.add_parser(name)
        command.add_argument("id")
        command.add_argument("action")
    frame = sub.add_parser("frame")
    frame.add_argument("--panel", action="store_true")
    frame.add_argument("--out", type=Path)
    raw = sub.add_parser("command")
    raw.add_argument("text", nargs=argparse.REMAINDER)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        with SerialLink(args.port, args.baud, args.timeout) as link:
            if args.op == "frame":
                return _read_frame(link, args.panel, max(args.timeout, 8.0), args.out)
            if args.op == "tap":
                _validate_xy(args.x, args.y)
                command = f"TOUCH TAP {args.x} {args.y}"
            elif args.op == "click":
                _validate_xy(args.x, args.y)
                command = f"TOUCH CLICK {args.x} {args.y}"
            elif args.op in ("down", "move", "up"):
                _validate_xy(args.x, args.y)
                command = f"TOUCH {args.op.upper()} {args.x} {args.y}"
            elif args.op == "swipe":
                if len(args.values) not in (1, 4):
                    raise ValueError("swipe 需要 1 个 dx，或 4 个起止坐标")
                if len(args.values) == 1:
                    if not -LOGICAL_WIDTH <= args.values[0] <= LOGICAL_WIDTH:
                        raise ValueError("swipe 的 dx 必须在 -480..480")
                else:
                    _validate_xy(args.values[0], args.values[1], "swipe 起点")
                    _validate_xy(args.values[2], args.values[3], "swipe 终点")
                command = "TOUCH SWIPE " + " ".join(str(value) for value in args.values)
            elif args.op == "gesture":
                _validate_xy(args.x0, args.y0, "gesture 起点")
                _validate_xy(args.x1, args.y1, "gesture 终点")
                return _send_gesture(link, args.x0, args.y0, args.x1, args.y1,
                                      args.steps, args.interval, args.timeout)
            elif args.op == "back":
                command = "TOUCH BACK"
            elif args.op == "state":
                command = "STATE?"
            elif args.op == "help":
                command = "INPUT HELP?"
            elif args.op in ("button", "key"):
                command = f"{args.op.upper()} {args.id} {args.action}"
            else:
                if not args.text:
                    raise ValueError("command 需要一条命令文本")
                command = " ".join(args.text)
            return _print_reply(link, command, args.timeout)
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
