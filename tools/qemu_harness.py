#!/usr/bin/env python3
"""
tools/qemu_harness.py - Robust, Reusable QEMU Test & Emulation Harness
Provides:
  - Ephemeral free port allocation for COM1 serial and QMP sockets
  - Process lifecycle management (clean termination on exit or timeout)
  - Interactive serial event callbacks
  - QMP (QEMU Machine Protocol) client for dynamic USB device hotplugging
  - Automated log collection and regex/pattern assertions
"""

import os
import sys
import time
import socket
import subprocess
import json
import re

WORKSPACE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(WORKSPACE_ROOT, "build")
DEFAULT_BOOT_IMG = os.path.join(BUILD_DIR, "boot.img")


def get_free_port():
    """Bind to an ephemeral port on localhost to find a guaranteed free port."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class QmpClient:
    """Minimal client for QEMU Machine Protocol (QMP) over TCP socket."""
    def __init__(self, port):
        self.port = port
        self.sock = None

    def connect(self, timeout=10):
        start = time.time()
        while time.time() - start < timeout:
            try:
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.connect(("127.0.0.1", self.port))
                # Initial QMP greeting
                self._read_resp()
                # Negotiate QMP capabilities
                self.execute("qmp_capabilities")
                return True
            except (ConnectionRefusedError, OSError):
                time.sleep(0.1)
        return False

    def _read_resp(self):
        resp_data = b""
        self.sock.settimeout(2.0)
        while True:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                resp_data += chunk
                if b"\r\n" in resp_data:
                    lines = resp_data.split(b"\r\n")
                    for line in lines:
                        if line:
                            try:
                                return json.loads(line.decode("utf-8"))
                            except json.JSONDecodeError:
                                pass
            except socket.timeout:
                break
        return {}

    def execute(self, cmd, args=None):
        if not self.sock:
            return {}
        req = {"execute": cmd}
        if args:
            req["arguments"] = args
        self.sock.sendall((json.dumps(req) + "\r\n").encode("utf-8"))
        return self._read_resp()

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None


class QemuHarness:
    """Orchestrates headless QEMU test runs with serial capture and QMP interaction."""
    def __init__(self, boot_img=DEFAULT_BOOT_IMG, memory="1024M", enable_qmp=False, snapshot=True):
        self.boot_img = boot_img
        self.memory = memory
        self.enable_qmp = enable_qmp
        self.snapshot = snapshot
        self.serial_port = get_free_port()
        self.qmp_port = get_free_port() if enable_qmp else None
        self.proc = None
        self.server = None
        self.conn = None
        self.qmp = None
        self.captured_log = ""

    def run(self, extra_args=None, interaction_fn=None, timeout_sec=30):
        extra_args = extra_args or []

        # Setup serial TCP server
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", self.serial_port))
        self.server.listen(1)

        qemu_cmd = ["qemu-system-x86_64"]
        if self.snapshot:
            qemu_cmd.append("-snapshot")
        qemu_cmd.extend([
            "-drive", f"file={self.boot_img},format=raw,if=ide",
            "-device", "qemu-xhci,id=xhci",
            "-serial", f"tcp:127.0.0.1:{self.serial_port}",
            "-display", "none",
            "-m", self.memory
        ])
        if self.enable_qmp:
            qemu_cmd.extend(["-qmp", f"tcp:127.0.0.1:{self.qmp_port},server,nowait"])

        qemu_cmd.extend(extra_args)

        print(f"[*] Launching QEMU (Serial Port {self.serial_port}): {' '.join(qemu_cmd)}")
        self.proc = subprocess.Popen(qemu_cmd, cwd=WORKSPACE_ROOT)

        # Accept serial socket connection
        self.conn, _ = self.server.accept()
        self.conn.setblocking(False)

        # Connect QMP if enabled
        if self.enable_qmp:
            self.qmp = QmpClient(self.qmp_port)
            self.qmp.connect()

        chunks = []
        start_time = time.time()
        state = {}

        try:
            while time.time() - start_time < timeout_sec:
                try:
                    chunk = self.conn.recv(2048)
                    if chunk:
                        text = chunk.decode("utf-8", errors="ignore")
                        chunks.append(text)
                        current_log = "".join(chunks)

                        if interaction_fn:
                            stop = interaction_fn(self.conn, current_log, state, self.qmp)
                            if stop:
                                break
                except (BlockingIOError, OSError):
                    if interaction_fn:
                        current_log = "".join(chunks)
                        stop = interaction_fn(self.conn, current_log, state, self.qmp)
                        if stop:
                            break
                    time.sleep(0.05)
        finally:
            self.cleanup()

        self.captured_log = "".join(chunks)
        return self.captured_log

    def cleanup(self):
        if self.conn:
            try:
                self.conn.close()
            except Exception:
                pass
            self.conn = None

        if self.server:
            try:
                self.server.close()
            except Exception:
                pass
            self.server = None

        if self.qmp:
            self.qmp.close()
            self.qmp = None

        if self.proc:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=2)
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass
            self.proc = None
