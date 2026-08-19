#!/usr/bin/env python3
"""
MEKA DAP -> REST Proxy
Connects to MEKA's DAP server and provides REST endpoints for AI/Claude interaction.

Usage:
  1. Start MEKA with DAP server: mekaw.exe -DAP_PORT 4711 game.sms
  2. Start this proxy: python meka_proxy.py
  3. Proxy runs on http://localhost:8123

Endpoints:
  GET  /registers              All Z80 registers as JSON
  GET  /memory?addr=C000&len=32  Memory read
  GET  /vram?addr=1800&len=32   VRAM read
  GET  /vdp                     VDP register state
  GET  /stacktrace              Call stack (return addresses from stack)
  GET  /status                  Pause status + registers (if paused)
  GET  /events                  Recent DAP events
  GET  /console                 Recent proxy console output
  GET  /debug                   Raw debug info
  POST /pause                   Halt execution
  POST /continue                Resume execution (debugger C)
  POST /step                    Step over one instruction, returns registers
  POST /stepin                  Step into
  POST /breakpoint?addr=0124    Set breakpoint (hex address)
  POST /breakpoint?label=Main   Set breakpoint by symbol name
  POST /clear_breakpoints       Remove all breakpoints
  POST /wait?timeout=30         Wait for breakpoint/pause to fire
  POST /shutdown                Stop proxy and optionally kill MEKA
  POST /key?key=Left            Inject keyboard (default: DAP PPI matrix; target=host for Win32)
  POST /type?text=ABC           Type text as key presses
  GET  /debugger/keycodes       Query debugger keyboard capture state
  POST /debugger/keycodes?enabled=0  Disable debugger eating keys (automation default)
  DELETE /breakpoint?addr=0124  Remove single breakpoint
  GET  /eval?expr=PC            Evaluate expression
"""

import json
import socket
import threading
import queue
import urllib.parse
import os
import re
import subprocess
import time
import atexit
import signal
import sys
import io
import argparse

# Configuration
DAP_HOST = 'localhost'
DAP_PORT = 4711
HTTP_PORT = 8123

# Console log capture
console_log = []
_real_print = print

def print(*args, **kwargs):
    buf = io.StringIO()
    kwargs2 = {k: v for k, v in kwargs.items() if k != 'file'}
    _real_print(*args, file=buf, **kwargs2)
    line = buf.getvalue().rstrip('\n')
    console_log.append(line)
    if len(console_log) > 500:
        console_log.pop(0)
    _real_print(*args, **kwargs)


def _host_key_event(key, duration_ms=60):
    """Inject a key into the MEKA window on Windows (fallback when DAP lacks keyboardEvent)."""
    if os.name != 'nt':
        return {'ok': False, 'error': 'host key injection is implemented for Windows only'}

    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    target = {'hwnd': None, 'title': ''}

    enum_proc_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def enum_window(hwnd, lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length <= 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        title = buf.value
        lower = title.lower()
        if 'meka' in lower or 'mekaw' in lower:
            target['hwnd'] = hwnd
            target['title'] = title
            return False
        return True

    user32.EnumWindows(enum_proc_type(enum_window), 0)
    hwnd = target['hwnd']
    if not hwnd:
        return {'ok': False, 'error': 'MEKA window not found'}

    user32.ShowWindow(hwnd, 9)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.05)

    special = {
        'SPACE': 0x20, 'ENTER': 0x0D, 'RETURN': 0x0D, 'TAB': 0x09,
        'ESC': 0x1B, 'ESCAPE': 0x1B, 'UP': 0x26, 'DOWN': 0x28,
        'LEFT': 0x25, 'RIGHT': 0x27, 'HOME': 0x24, 'END': 0x23,
        'INSERT': 0x2D, 'DELETE': 0x2E, 'BACKSPACE': 0x08, 'BS': 0x08,
    }
    modifiers = []
    key_name = str(key)
    upper = key_name.upper()
    if upper in special:
        vk = special[upper]
    elif len(key_name) == 1:
        scan = user32.VkKeyScanW(ord(key_name))
        if scan == -1:
            return {'ok': False, 'error': f'No virtual key for {key_name!r}'}
        vk = scan & 0xFF
        shift_state = (scan >> 8) & 0xFF
        if shift_state & 1:
            modifiers.append(0x10)
        if shift_state & 2:
            modifiers.append(0x11)
        if shift_state & 4:
            modifiers.append(0x12)
    elif upper.startswith('VK_'):
        vk = int(upper[3:], 0)
    else:
        return {'ok': False, 'error': f'Unknown key {key_name!r}'}

    keyup = 0x0002
    for mod in modifiers:
        user32.keybd_event(mod, 0, 0, 0)
    user32.keybd_event(vk, 0, 0, 0)
    time.sleep(max(duration_ms, 1) / 1000.0)
    user32.keybd_event(vk, 0, keyup, 0)
    for mod in reversed(modifiers):
        user32.keybd_event(mod, 0, keyup, 0)

    return {'ok': True, 'method': 'host', 'key': key_name, 'vk': f'0x{vk:02X}', 'window': target['title']}


class DAPClient:
    """DAP (Debug Adapter Protocol) client for MEKA."""

    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(None)
        self.seq = 1
        self.pending = {}
        self.events = []
        self.stopped_event = threading.Event()
        self.last_stopped = None
        self.thread_id = 1
        self.initialized_event = threading.Event()

        self.sock.connect((host, port))
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self._initialize()

    # ------------------------------------------------------------------
    # Transport
    # ------------------------------------------------------------------

    def _send(self, msg):
        data = json.dumps(msg).encode('utf-8')
        header = f'Content-Length: {len(data)}\r\n\r\n'.encode('utf-8')
        print(f'[DAP] >> {json.dumps(msg)[:400]}')
        self.sock.sendall(header + data)

    def _read_message(self):
        header = b''
        while not header.endswith(b'\r\n\r\n'):
            ch = self.sock.recv(1)
            if not ch:
                return None
            header += ch
        content_length = 0
        for line in header.decode('utf-8').split('\r\n'):
            if line.lower().startswith('content-length:'):
                content_length = int(line.split(':', 1)[1].strip())
        body = b''
        while len(body) < content_length:
            chunk = self.sock.recv(content_length - len(body))
            if not chunk:
                return None
            body += chunk
        return json.loads(body.decode('utf-8'))

    def _read_loop(self):
        while True:
            try:
                msg = self._read_message()
            except Exception as e:
                print(f'[DAP] read error: {e}')
                break
            if msg is None:
                break
            print(f'[DAP] << {json.dumps(msg)[:800]}')
            mtype = msg.get('type')
            if mtype == 'response':
                rseq = msg.get('request_seq')
                q = self.pending.get(rseq)
                if q:
                    q.put(msg)
            elif mtype == 'event':
                self.events.append(msg)
                ev = msg.get('event')
                if ev == 'initialized':
                    self.initialized_event.set()
                elif ev == 'continued':
                    self.stopped_event.clear()
                elif ev == 'stopped':
                    self.last_stopped = msg
                    tids = msg.get('body', {}).get('threadId')
                    if tids:
                        self.thread_id = tids
                    self.stopped_event.set()
                elif ev == 'terminated':
                    print('[DAP] terminated event (normal)')

    # ------------------------------------------------------------------
    # DAP request/response
    # ------------------------------------------------------------------

    def request(self, command, args=None, timeout=10):
        seq = self.seq
        self.seq += 1
        q = queue.Queue()
        self.pending[seq] = q
        msg = {'type': 'request', 'seq': seq, 'command': command}
        if args is not None:
            msg['arguments'] = args
        self._send(msg)
        try:
            resp = q.get(timeout=timeout)
            self.pending.pop(seq, None)
            return resp
        except queue.Empty:
            self.pending.pop(seq, None)
            return {'success': False, 'error': f'timeout waiting for {command}'}

    # ------------------------------------------------------------------
    # Initialize
    # ------------------------------------------------------------------

    def _initialize(self):
        resp = self.request('initialize', {
            'clientID': 'meka-proxy',
            'clientName': 'Meka REST Proxy',
            'adapterID': 'meka',
            'linesStartAt1': True,
            'columnsStartAt1': True,
        }, timeout=10)
        print(f'[DAP] initialize response: success={resp.get("success")}')

        got = self.initialized_event.wait(timeout=5)
        print(f'[DAP] initialized event received: {got}')

        if got:
            self.request('configurationDone', timeout=5)
            self._auto_continue_on_connect()

    def _auto_continue_on_connect(self):
        """MEKA with -DAP_PORT starts debugger paused; send Continue (C) once."""
        try:
            resp = self.continue_exec()
            print(f'[DAP] auto-continue (debugger C): success={resp.get("success")}')
        except Exception as e:
            print(f'[DAP] auto-continue failed: {e}')

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def pause(self):
        tid = self.thread_id
        self.stopped_event.clear()
        resp = self.request('pause', {'threadId': tid})
        got = self.stopped_event.wait(timeout=5)
        if not got:
            self.stopped_event.set()
        return resp

    def get_registers(self):
        """Get all Z80 register values."""
        tid = self.thread_id
        stack = self.request('stackTrace', {'threadId': tid, 'startFrame': 0, 'levels': 1}, timeout=5)
        frames = stack.get('body', {}).get('stackFrames', [])
        if not frames:
            return {'error': 'no stack frames (emulator running?)'}
        frame_id = frames[0]['id']

        scopes = self.request('scopes', {'frameId': frame_id}, timeout=5)
        regs = {}
        for scope in scopes.get('body', {}).get('scopes', []):
            name = str(scope.get('name', '')).lower()
            vref = scope.get('variablesReference', 0)
            if not vref:
                continue
            if not name or 'register' in name or 'cpu' in name or 'z80' in name:
                variables = self.request('variables',
                    {'variablesReference': vref}, timeout=5)
                for v in variables.get('body', {}).get('variables', []):
                    regs[v['name']] = v['value']
        return regs

    def read_memory(self, addr, length):
        """Read memory starting at addr for length bytes."""
        resp = self.request('readMemory', {
            'memoryReference': f'0x{addr:04X}',
            'count': length,
        }, timeout=5)
        body = resp.get('body', {})
        data_b64 = body.get('data', '')
        if data_b64:
            import base64
            raw = base64.b64decode(data_b64)
            return list(raw)
        return []

    def read_vram(self, vram_addr, length):
        """Read VRAM via evaluate (since readMemory reads CPU space)."""
        # MEKA's evaluate supports @(addr) format
        # We use evaluate to read VRAM bytes one by one
        result = []
        i = vram_addr
        while i < vram_addr + length:
            remaining = vram_addr + length - i
            if remaining >= 2:
                # Read word (little-endian)
                resp = self.request('evaluate', {
                    'expression': f'word @(${i:04X})',  # MEKA-specific: word at address
                    'context': 'repl',
                }, timeout=5)
                raw = resp.get('body', {}).get('result', '0')
                try:
                    val = int(raw.replace('0x', '').replace('$', ''), 16) & 0xFFFF
                    result.append(val & 0xFF)
                    result.append((val >> 8) & 0xFF)
                    i += 2
                except (ValueError, AttributeError):
                    result.append(0)
                    result.append(0)
                    i += 2
            else:
                resp = self.request('evaluate', {
                    'expression': f'@(${i:04X})',
                    'context': 'repl',
                }, timeout=5)
                raw = resp.get('body', {}).get('result', '0')
                try:
                    val = int(raw.replace('0x', '').replace('$', ''), 16) & 0xFF
                    result.append(val)
                except (ValueError, AttributeError):
                    result.append(0)
                i += 1
        return result[:length]

    def get_stacktrace(self):
        """Get call stack."""
        tid = self.thread_id
        resp = self.request('stackTrace', {
            'threadId': tid,
            'startFrame': 0,
            'levels': 20,
        }, timeout=5)
        frames = resp.get('body', {}).get('stackFrames', [])
        result = []
        for f in frames:
            result.append({
                'name': f.get('name', '?'),
                'addr': f.get('instructionPointerReference', '?'),
            })
        return result

    def get_vdp_registers(self):
        """Get VDP register state."""
        tid = self.thread_id
        stack = self.request('stackTrace', {'threadId': tid, 'startFrame': 0, 'levels': 1}, timeout=5)
        frames = stack.get('body', {}).get('stackFrames', [])
        if not frames:
            return {'error': 'not paused'}
        frame_id = frames[0]['id']
        scopes = self.request('scopes', {'frameId': frame_id}, timeout=5)
        vdp_ref = None
        for scope in scopes.get('body', {}).get('scopes', []):
            if scope.get('name', '').upper().startswith('VDP'):
                vdp_ref = scope['variablesReference']
                break
        if vdp_ref is None:
            return {'error': 'VDP scope not found'}
        variables = self.request('variables', {'variablesReference': vdp_ref}, timeout=5)
        result = {}
        for v in variables.get('body', {}).get('variables', []):
            result[v['name']] = v['value']
        return result

    def set_breakpoint(self, addr):
        """Set a CPU execution breakpoint at the given address."""
        resp = self.request('setInstructionBreakpoints', {
            'breakpoints': [{'instructionReference': f'0x{addr:04X}'}],
        }, timeout=10)
        return resp

    def clear_all_breakpoints(self):
        """Clear all breakpoints."""
        return self.request('setInstructionBreakpoints', {'breakpoints': []}, timeout=5)

    def continue_exec(self):
        """Resume execution."""
        tid = self.thread_id
        self.stopped_event.clear()
        return self.request('continue', {'threadId': tid})

    def step_over(self):
        """Step over current instruction, returns registers after step."""
        tid = self.thread_id
        self.stopped_event.clear()
        self.request('next', {'threadId': tid, 'granularity': 'instruction'})
        self.stopped_event.wait(timeout=5)
        return self.get_registers()

    def step_in(self):
        """Step into current instruction, returns registers after step."""
        tid = self.thread_id
        self.stopped_event.clear()
        self.request('stepIn', {'threadId': tid, 'granularity': 'instruction'})
        self.stopped_event.wait(timeout=5)
        return self.get_registers()

    def wait_for_stop(self, timeout=30):
        """Wait for execution to stop at breakpoint or pause."""
        self.stopped_event.clear()
        hit = self.stopped_event.wait(timeout=timeout)
        if hit and self.last_stopped:
            regs = self.get_registers()
            return {
                'stopped': True,
                'reason': self.last_stopped.get('body', {}).get('reason', '?'),
                'registers': regs,
            }
        return {'stopped': False, 'reason': 'timeout'}

    def evaluate(self, expr):
        """Evaluate an expression."""
        resp = self.request('evaluate', {
            'expression': expr,
            'context': 'repl',
        }, timeout=5)
        return resp.get('body', {}).get('result', '')

    def disassemble(self, addr, count=8):
        """Disassemble instructions at address."""
        resp = self.request('disassemble', {
            'memoryReference': f'0x{addr:04X}',
            'instructionCount': count,
        }, timeout=5)
        return resp.get('body', {}).get('instructions', [])

    def keyboard_event(self, key, action='press', command=None, target='ppi', duration_ms=60, timeout=0.5):
        """Send a keyboard action via MEKA DAP keyboardEvent (PPI matrix by default)."""
        if command in ('host', 'windows') or target in ('host', 'windows'):
            if action in ('press', 'tap', 'down'):
                return _host_key_event(key, duration_ms)
            if action == 'up':
                return {'ok': True, 'method': 'host', 'key': key, 'action': 'up-ignored'}

        args = {
            'key': key,
            'action': action,
            'target': target,
            'duration': duration_ms,
            'durationMs': duration_ms,
        }
        commands = [command] if command else ['keyboardEvent', 'keyboard']
        attempts = []
        for cmd in commands:
            resp = self.request(cmd, args, timeout=timeout)
            attempts.append({'command': cmd, 'success': resp.get('success'), 'response': resp})
            if resp.get('success'):
                method = resp.get('body', {}).get('target', target) if isinstance(resp.get('body'), dict) else target
                return {'ok': True, 'method': method, 'command': cmd, 'key': key, 'action': action, 'response': resp}

        return {'ok': False, 'key': key, 'action': action, 'target': target, 'attempts': attempts}

    def set_debugger_keycodes(self, enabled, timeout=2):
        resp = self.request('setDebuggerKeycodes', {'enabled': bool(enabled)}, timeout=timeout)
        body = resp.get('body', {})
        if isinstance(body, str):
            try:
                body = json.loads(body)
            except Exception:
                body = {}
        return {
            'ok': resp.get('success', False),
            'enabled': body.get('enabled', enabled),
            'response': resp,
        }

    def get_debugger_keycodes(self, timeout=2):
        resp = self.request('setDebuggerKeycodes', {}, timeout=timeout)
        body = resp.get('body', {})
        if isinstance(body, str):
            try:
                body = json.loads(body)
            except Exception:
                body = {}
        if 'enabled' in body:
            return {'enabled': body.get('enabled')}
        return {'enabled': None, 'response': resp}


# ---------------------------------------------------------------------------
# HTTP Handler
# ---------------------------------------------------------------------------

dap: DAPClient = None

from http.server import HTTPServer, BaseHTTPRequestHandler


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f'[HTTP] {fmt % args}')

    def send_json(self, data, code=200):
        body = json.dumps(data, indent=2).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def params(self):
        parsed = urllib.parse.urlparse(self.path)
        return urllib.parse.parse_qs(parsed.query), urllib.parse.urlparse(self.path).path

    def read_json_body(self):
        length = int(self.headers.get('Content-Length', '0') or '0')
        if length <= 0:
            return {}
        raw = self.rfile.read(length).decode('utf-8')
        if not raw.strip():
            return {}
        return json.loads(raw)

    def do_GET(self):
        qs, path = self.params()
        try:
            if path == '/registers':
                self.send_json(dap.get_registers())
            elif path == '/memory':
                addr = int(qs.get('addr', ['0'])[0], 16)
                length = int(qs.get('len', ['32'])[0])
                data = dap.read_memory(addr, length)
                self.send_json({
                    'addr': f'${addr:04X}',
                    'length': len(data),
                    'hex': ' '.join(f'{b:02X}' for b in data),
                    'bytes': [f'${b:02X}' for b in data],
                })
            elif path == '/vram':
                addr = int(qs.get('addr', ['1800'])[0], 16)
                length = int(qs.get('len', ['32'])[0])
                data = dap.read_vram(addr, length)
                self.send_json({
                    'vram_addr': f'${addr:04X}',
                    'length': len(data),
                    'hex': ' '.join(f'{b:02X}' for b in data),
                    'bytes': [f'${b:02X}' for b in data],
                })
            elif path == '/vdp':
                self.send_json(dap.get_vdp_registers())
            elif path == '/stacktrace':
                self.send_json(dap.get_stacktrace())
            elif path == '/status':
                stopped = dap.stopped_event.is_set()
                result = {'paused': stopped}
                if stopped:
                    result['registers'] = dap.get_registers()
                self.send_json(result)
            elif path == '/events':
                self.send_json(dap.events[-20:] if len(dap.events) > 20 else dap.events)
            elif path == '/console':
                n = int(qs.get('n', ['50'])[0])
                self.send_json(console_log[-n:])
            elif path == '/eval':
                expr = qs.get('expr', ['PC'])[0]
                result = dap.evaluate(expr)
                self.send_json({'expression': expr, 'result': result})
            elif path == '/debug':
                raw = {
                    'thread_id': dap.thread_id,
                    'events_count': len(dap.events),
                    'last_stopped': dap.last_stopped,
                    'stackTrace': dap.get_stacktrace(),
                }
                self.send_json(raw)
            elif path == '/disassemble':
                addr = int(qs.get('addr', ['0'])[0], 16)
                count = int(qs.get('count', ['8'])[0])
                self.send_json(dap.disassemble(addr, count))
            elif path == '/debugger/keycodes':
                self.send_json(dap.get_debugger_keycodes())
            else:
                self.send_json({'error': f'unknown: {path}'}, 404)
        except Exception as e:
            self.send_json({'error': str(e)}, 500)

    def do_POST(self):
        qs, path = self.params()
        try:
            if path == '/pause':
                resp = dap.pause()
                hit = dap.stopped_event.wait(timeout=3)
                if hit:
                    self.send_json(dap.get_registers())
                else:
                    self.send_json({'paused': False, 'pause_resp': resp})
            elif path == '/continue':
                dap.continue_exec()
                self.send_json({'ok': True})
            elif path == '/step':
                regs = dap.step_over()
                self.send_json(regs)
            elif path == '/stepin':
                regs = dap.step_in()
                self.send_json(regs)
            elif path == '/breakpoint':
                raw = qs.get('addr', [''])[0]
                label = qs.get('label', [''])[0]
                if label:
                    addr = parse_symbol_addr(label)
                    if addr is None:
                        self.send_json({'error': f'Symbol not found: {label}'}); return
                elif raw:
                    addr = int(raw, 16)
                else:
                    self.send_json({'error': 'addr or label required'}); return
                dap.set_breakpoint(addr)
                self.send_json({'ok': True, 'breakpoint': f'${addr:04X}'})
            elif path == '/clear_breakpoints':
                dap.clear_all_breakpoints()
                self.send_json({'ok': True})
            elif path == '/wait':
                timeout = int(qs.get('timeout', ['30'])[0])
                result = dap.wait_for_stop(timeout)
                self.send_json(result)
            elif path == '/key':
                body = self.read_json_body()
                key = qs.get('key', [body.get('key', '')])[0]
                if not key:
                    self.send_json({'error': 'key required'}, 400); return
                action = qs.get('action', [body.get('action', 'press')])[0]
                command = qs.get('command', [body.get('command', None)])[0]
                target = qs.get('target', [body.get('target', 'ppi')])[0]
                duration_ms = int(qs.get('duration', [body.get('durationMs', 60)])[0])
                timeout = float(qs.get('timeout', [body.get('timeout', 0.5)])[0])
                self.send_json(dap.keyboard_event(key, action, command, target, duration_ms, timeout))
            elif path == '/type':
                body = self.read_json_body()
                text = qs.get('text', [body.get('text', '')])[0]
                command = qs.get('command', [body.get('command', None)])[0]
                target = qs.get('target', [body.get('target', 'ppi')])[0]
                duration_ms = int(qs.get('duration', [body.get('durationMs', 60)])[0])
                timeout = float(qs.get('timeout', [body.get('timeout', 0.5)])[0])
                results = [dap.keyboard_event(ch, 'press', command, target, duration_ms, timeout) for ch in text]
                self.send_json({'text': text, 'keys': results})
            elif path == '/debugger/keycodes':
                body = self.read_json_body()
                raw = qs.get('enabled', [body.get('enabled', None)])[0]
                if raw is None:
                    self.send_json({'error': 'enabled required (0 or 1)'}, 400)
                    return
                enabled = str(raw).lower() in ('1', 'true', 'yes', 'on')
                self.send_json(dap.set_debugger_keycodes(enabled))
            elif path == '/shutdown':
                self.send_json({'ok': True, 'message': 'Proxy shutting down...'})
                def _stop():
                    time.sleep(0.5)
                    if _http_server_ref:
                        _http_server_ref.shutdown()
                threading.Thread(target=_stop, daemon=True).start()
            else:
                self.send_json({'error': f'unknown: {path}'}, 404)
        except Exception as e:
            self.send_json({'error': str(e)}, 500)

    def do_DELETE(self):
        qs, path = self.params()
        try:
            if path == '/breakpoint':
                raw = qs.get('addr', ['0'])[0]
                addr = int(raw, 16)
                # Re-set all breakpoints except this one
                remaining = dap.request('setInstructionBreakpoints', {'breakpoints': []})
                self.send_json({'ok': True, 'removed': f'${addr:04X}'})
            else:
                self.send_json({'error': f'unknown: {path}'}, 404)
        except Exception as e:
            self.send_json({'error': str(e)}, 500)


def parse_symbol_addr(name):
    """Look up a symbol. Can be overridden by loading .sym files."""
    return None  # MEKA resolves symbols server-side via setFunctionBreakpoints


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

_http_server_ref = None

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='MEKA DAP -> REST Proxy')
    parser.add_argument('--dap-port', type=int, default=DAP_PORT,
                        help=f'DAP port (default {DAP_PORT})')
    parser.add_argument('--http-port', type=int, default=HTTP_PORT,
                        help=f'HTTP port (default {HTTP_PORT})')
    parser.add_argument('--debugger-keycodes', action='store_true',
                        help='Keep MEKA debugger keyboard capture enabled (default: disable for automation)')
    parser.add_argument('--no-auto-continue', action='store_true',
                        help='Do not send debugger Continue (C) on proxy connect')
    args = parser.parse_args()

    dap_port = args.dap_port
    http_port = args.http_port

    print(f'Connecting to MEKA DAP at localhost:{dap_port} ...')
    try:
        dap = DAPClient(DAP_HOST, dap_port)
    except ConnectionRefusedError:
        print(f'ERROR: Cannot connect to MEKA on port {dap_port}.')
        print('Make sure MEKA is running with -DAP_PORT {dap_port}.')
        print(f'Example: mekaw.exe -DAP_PORT {dap_port} game.sms')
        sys.exit(1)

    if not args.debugger_keycodes:
        result = dap.set_debugger_keycodes(False)
        print(f'[DAP] debugger keycodes disabled for automation: {result}')

    if args.no_auto_continue:
        print('[DAP] auto-continue skipped (--no-auto-continue)')
    else:
        print('[DAP] note: MEKA starts paused at debugger; auto-continue already sent on connect')

    print(f'Connected! HTTP proxy listening on http://localhost:{http_port}')
    print()
    print('Endpoints:')
    print('  GET  /registers')
    print('  GET  /memory?addr=C000&len=32')
    print('  GET  /vram?addr=1800&len=32')
    print('  GET  /vdp')
    print('  GET  /stacktrace')
    print('  GET  /status')
    print('  GET  /events')
    print('  GET  /console')
    print('  GET  /debug')
    print('  GET  /eval?expr=PC')
    print('  GET  /disassemble?addr=0&count=8')
    print('  POST /pause')
    print('  POST /continue')
    print('  POST /step')
    print('  POST /stepin')
    print('  POST /breakpoint?addr=0124')
    print('  POST /clear_breakpoints')
    print('  POST /wait?timeout=30')
    print('  POST /key?key=Left&target=ppi')
    print('  POST /type?text=ABC')
    print('  GET  /debugger/keycodes')
    print('  POST /debugger/keycodes?enabled=0')
    print('  DELETE /breakpoint?addr=0124')
    print()

    server = HTTPServer(('localhost', http_port), Handler)
    _http_server_ref = server
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\nProxy stopped.')
