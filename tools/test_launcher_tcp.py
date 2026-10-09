"""Exercise the rendered launcher through TCP, without desktop automation.

Run in a private working directory. Pass host arguments after --; PSX callers
should include --launcher --hidden-window so a host fallback is hidden too.
Linux callers may run under xvfb-run. No mouse/keyboard/focus APIs are used.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--cwd', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--players', type=int, default=2)
    p.add_argument('args', nargs=argparse.REMAINDER)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(exist_ok=False, parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('LNG_')}
    env.update(LNG_TCP_PORT='0', LNG_TCP_PORT_FILE=str(out/'port.txt'), LNG_TEST_HIDDEN='1')
    args = a.args[1:] if a.args[:1] == ['--'] else a.args
    creation = {'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {'start_new_session': True}
    records = []
    result = {'passed': False, 'transport': 'loopback TCP', 'desktop_input': False}
    def assert_no_visible_windows():
        if os.name != 'nt': return
        import ctypes
        from ctypes import wintypes
        u = ctypes.WinDLL('user32')
        callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        visible = []
        @callback
        def visit(handle, unused):
            pid = wintypes.DWORD()
            u.GetWindowThreadProcessId(handle, ctypes.byref(pid))
            if pid.value == proc.pid and u.IsWindowVisible(handle): visible.append(int(handle))
            return True
        u.EnumWindows(visit, 0)
        assert not visible, ('QA created a visible window', visible)
    with (out/'runtime.log').open('wb') as log:
        proc = subprocess.Popen([str(a.exe.resolve()), *args], cwd=a.cwd, env=env,
            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, **creation)
    def connect():
        s = socket.create_connection(('127.0.0.1', int((out/'port.txt').read_text())), timeout=5)
        return s
    def response(reader):
        assert_no_visible_windows()
        line = reader.readline()
        assert line, 'TCP server closed without a reply'
        value = json.loads(line)
        records.append(value)
        return value
    def command(s, reader, text):
        s.sendall((text+'\n').encode('utf-8'))
        reply = response(reader)
        assert reply.get('ok'), (text, reply)
        return reply
    try:
        deadline = time.monotonic()+45
        while not (out/'port.txt').exists():
            assert proc.poll() is None, f'Launcher exited: {proc.returncode}'
            assert time.monotonic()<deadline, 'TCP listener did not start'
            time.sleep(.1)
        with connect() as s:
            reader = s.makefile('rb')
            # Packet boundaries are not command boundaries.
            s.sendall(b'sta'); time.sleep(.02); s.sendall(b'te\n')
            state = response(reader)
            assert state['hidden'] and state['window_hidden'], state
            assert state['players'] == a.players, state
            command(s, reader, 'size:1120x860')
            command(s, reader, 'wait:3')
            state = command(s, reader, 'state')
            command(s, reader, 'shot:'+str(out/'dashboard.png'))
            assert (out/'dashboard.png').stat().st_size > 1024
            # Press the real Netplay button through ImGui input events.
            command(s, reader, f"click:{state['width']-331},{state['height']-61}")
            command(s, reader, 'wait:3')
            mode = command(s, reader, 'state')
            assert mode['window_hidden'] and mode['netplay'], mode
            assert 'netplay' in mode['view'].lower(), mode
            command(s, reader, 'shot:'+str(out/'netplay.png'))
            s.sendall(b'not-a-command\n')
            assert not response(reader)['ok']
            s.sendall(b'state\nstate\n')
            assert response(reader)['ok'] and response(reader)['ok']
            reader.close()
        # A disconnected client's partial command must not reach a new client.
        with connect() as s:
            s.sendall(b'vie')
        time.sleep(.1)
        with connect() as s:
            reader = s.makefile('rb')
            command(s, reader, 'state')
            command(s, reader, 'quit')
            reader.close()
        assert proc.wait(timeout=10) == 0, proc.returncode
        result.update(passed=True, initial=state, netplay=mode)
    except Exception as e:
        result['error'] = repr(e)
        raise
    finally:
        if proc.poll() is None:
            if os.name == 'nt': proc.terminate()
            else: os.killpg(proc.pid, signal.SIGTERM)
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                if os.name == 'nt': proc.kill()
                else: os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=5)
        result['responses'] = records
        (out/'report.json').write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps({k: v for k, v in result.items() if k != 'responses'}))


if __name__ == '__main__':
    main()
