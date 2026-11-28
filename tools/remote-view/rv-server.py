#!/usr/bin/env python3
"""rv-server：L540 远程查看/控制的服务端。

- HTTP 提供 index.html，WebSocket /ws 推送 rv-cap 编出的 H.264，浏览器用 WebCodecs 解码。
- 浏览器发来的鼠标、键盘事件经 /dev/uinput 注入（内核层输入，锁屏界面也收得到）。
  用户需要在 input 组（/dev/uinput 是 root:input 0660）。
- 默认只监听 127.0.0.1，在 Windows 上用 SSH 隧道访问；WebCodecs 只在 localhost/HTTPS 下可用。
- 同一时间只服务一个浏览器窗口，新窗口连上会把旧的踢掉。
- 每次浏览器连上都重新找一遍要采集的 treeland（见 find_display），注销 / 登录之后刷新页面就能接着看。
"""
import argparse
import asyncio
import fcntl
import json
import os
import signal
import socket
import stat
import struct
import sys
import time

from websockets.asyncio.server import serve
from websockets.datastructures import Headers
from websockets.http11 import Response

HERE = os.path.dirname(os.path.abspath(__file__))


def log(*a):
    # SSH 断开后 stderr 可能已经写不进去，日志丢了也不能影响退出流程
    try:
        print(time.strftime('%H:%M:%S'), 'rv-server:', *a, file=sys.stderr, flush=True)
    except (OSError, ValueError):
        pass


# 找要采集的 treeland
#
# 用户会话（桌面、锁屏）的 socket 在用户自己的 XDG_RUNTIME_DIR 下。注销以后屏幕上是登录界面（greeter），
# 由 treeland.service 以 dde 用户运行的 treeland 自己画，它的 socket 在 /run/treeland 下，
# 这个目录是 dde:dde 0700，别的用户连不上。rv-go.ps1 会请用户输入 sudo 密码，
# 用 ACL 只给当前用户开 /run/treeland 的 x 和 socket 的 rw；treeland 重启时目录会重建，ACL 随之失效。

GREETER_DIR = '/run/treeland'
EXIT_NOPERM = 3  # 屏幕在登录界面，但没权限连它的 treeland（rv-go.ps1 看到这个退出码会请求授权）
EXIT_NONE = 4    # 一个能用的 treeland 都没找到


def runtime_dir():
    return os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'


def socket_state(path):
    """'ok'：能连上；'noperm'：没权限；None：不存在或没人监听。"""
    try:
        st = os.stat(path)
    except PermissionError:
        return 'noperm'
    except OSError:
        return None
    if not stat.S_ISSOCK(st.st_mode):
        return None
    # 真连一下：注销后可能留下没人监听的旧 socket；连上马上断开，对合成器没有副作用
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM | socket.SOCK_CLOEXEC)
    s.settimeout(1)
    try:
        s.connect(path)
        return 'ok'
    except PermissionError:
        return 'noperm'
    except OSError:
        return None
    finally:
        s.close()


def find_display():
    """返回 (socket 绝对路径, 说明)；找不到时返回 (None, 'noperm' 或 'none')。"""
    env = os.environ.get('WAYLAND_DISPLAY')
    if env:  # 手动指定了就只用它
        return (env if env.startswith('/') else os.path.join(runtime_dir(), env)), '手动指定（WAYLAND_DISPLAY）'
    for n in ('treeland.socket', 'wayland-0', 'wayland-1'):
        p = os.path.join(runtime_dir(), n)
        if socket_state(p) == 'ok':
            return p, '用户会话（桌面 / 锁屏）'
    noperm = False
    for n in ('wayland-0', 'wayland-1'):
        p = os.path.join(GREETER_DIR, n)
        st = socket_state(p)
        if st == 'ok':
            return p, '登录界面（greeter）'
        noperm = noperm or st == 'noperm'
    return None, ('noperm' if noperm else 'none')


def no_display_msg(why):
    if why == 'noperm':
        u = os.environ.get('USER') or str(os.getuid())
        return (f'屏幕现在是登录界面（greeter），它的 treeland socket 在 {GREETER_DIR} 下，只有 dde 用户能连。'
                f'用 rv-go.ps1 启动会提示输入 sudo 密码自动授权；手动授权：'
                f'sudo setfacl -m u:{u}:x {GREETER_DIR}; sudo setfacl -m u:{u}:rw {GREETER_DIR}/wayland-0')
    return (f'找不到正在运行的 treeland：{runtime_dir()} 和 {GREETER_DIR} 下都没有能连上的 Wayland socket'
            '（正在注销 / 重启？）')


# uinput

UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_RELBIT = 0x40045566
UI_SET_ABSBIT = 0x40045567
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502
EV_SYN, EV_KEY, EV_REL, EV_ABS = 0, 1, 2, 3
REL_HWHEEL, REL_WHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES = 6, 8, 11, 12
ABS_X, ABS_Y = 0, 1
ABS_MAX = 32767
BUS_VIRTUAL = 0x06
# 浏览器 MouseEvent.button → BTN_LEFT / BTN_MIDDLE / BTN_RIGHT / BTN_SIDE / BTN_EXTRA
BUTTONS = {0: 0x110, 1: 0x112, 2: 0x111, 3: 0x113, 4: 0x114}

# 浏览器 KeyboardEvent.code → Linux keycode（input-event-codes.h）
CODE2KEY = {
    'Escape': 1, 'Digit1': 2, 'Digit2': 3, 'Digit3': 4, 'Digit4': 5, 'Digit5': 6, 'Digit6': 7,
    'Digit7': 8, 'Digit8': 9, 'Digit9': 10, 'Digit0': 11, 'Minus': 12, 'Equal': 13, 'Backspace': 14,
    'Tab': 15, 'KeyQ': 16, 'KeyW': 17, 'KeyE': 18, 'KeyR': 19, 'KeyT': 20, 'KeyY': 21, 'KeyU': 22,
    'KeyI': 23, 'KeyO': 24, 'KeyP': 25, 'BracketLeft': 26, 'BracketRight': 27, 'Enter': 28,
    'ControlLeft': 29, 'KeyA': 30, 'KeyS': 31, 'KeyD': 32, 'KeyF': 33, 'KeyG': 34, 'KeyH': 35,
    'KeyJ': 36, 'KeyK': 37, 'KeyL': 38, 'Semicolon': 39, 'Quote': 40, 'Backquote': 41,
    'ShiftLeft': 42, 'Backslash': 43, 'KeyZ': 44, 'KeyX': 45, 'KeyC': 46, 'KeyV': 47, 'KeyB': 48,
    'KeyN': 49, 'KeyM': 50, 'Comma': 51, 'Period': 52, 'Slash': 53, 'ShiftRight': 54,
    'NumpadMultiply': 55, 'AltLeft': 56, 'Space': 57, 'CapsLock': 58,
    'F1': 59, 'F2': 60, 'F3': 61, 'F4': 62, 'F5': 63, 'F6': 64, 'F7': 65, 'F8': 66, 'F9': 67,
    'F10': 68, 'NumLock': 69, 'ScrollLock': 70, 'Numpad7': 71, 'Numpad8': 72, 'Numpad9': 73,
    'NumpadSubtract': 74, 'Numpad4': 75, 'Numpad5': 76, 'Numpad6': 77, 'NumpadAdd': 78,
    'Numpad1': 79, 'Numpad2': 80, 'Numpad3': 81, 'Numpad0': 82, 'NumpadDecimal': 83,
    'IntlBackslash': 86, 'F11': 87, 'F12': 88, 'IntlRo': 89, 'Convert': 92, 'KanaMode': 93,
    'NonConvert': 94, 'NumpadEnter': 96, 'ControlRight': 97, 'NumpadDivide': 98,
    'PrintScreen': 99, 'AltRight': 100, 'Home': 102, 'ArrowUp': 103, 'PageUp': 104,
    'ArrowLeft': 105, 'ArrowRight': 106, 'End': 107, 'ArrowDown': 108, 'PageDown': 109,
    'Insert': 110, 'Delete': 111, 'AudioVolumeMute': 113, 'AudioVolumeDown': 114,
    'AudioVolumeUp': 115, 'Power': 116, 'NumpadEqual': 117, 'Pause': 119, 'NumpadComma': 121,
    'Lang1': 122, 'Lang2': 123, 'IntlYen': 124, 'MetaLeft': 125, 'MetaRight': 126,
    'ContextMenu': 127,
    'F13': 183, 'F14': 184, 'F15': 185, 'F16': 186, 'F17': 187, 'F18': 188, 'F19': 189,
    'F20': 190, 'F21': 191, 'F22': 192, 'F23': 193, 'F24': 194,
}


class UInput:
    def __init__(self, name, product, keys=(), rels=(), abs_axes=()):
        self.fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        try:
            for ev, bit, codes in ((EV_KEY, UI_SET_KEYBIT, keys), (EV_REL, UI_SET_RELBIT, rels),
                                   (EV_ABS, UI_SET_ABSBIT, abs_axes)):
                if codes:
                    fcntl.ioctl(self.fd, UI_SET_EVBIT, ev)
                    for c in codes:
                        fcntl.ioctl(self.fd, bit, c)
            absmax = [0] * 64
            for a in abs_axes:
                absmax[a] = ABS_MAX
            # struct uinput_user_dev：name[80] + input_id(4×u16) + ff_effects_max + absmax/absmin/absfuzz/absflat[64]
            dev = struct.pack('<80s4HI64i64i64i64i', name.encode(), BUS_VIRTUAL, 0x1209, product, 1, 0,
                              *absmax, *([0] * 64), *([0] * 64), *([0] * 64))
            os.write(self.fd, dev)
            fcntl.ioctl(self.fd, UI_DEV_CREATE)
        except OSError:
            os.close(self.fd)
            raise

    def emit(self, events):
        # struct input_event（aarch64）：timeval(2×long) + type u16 + code u16 + value s32；时间戳由内核填
        data = b''.join(struct.pack('<qqHHi', 0, 0, t, c, v) for t, c, v in events)
        os.write(self.fd, data + struct.pack('<qqHHi', 0, 0, EV_SYN, 0, 0))

    def close(self):
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        finally:
            os.close(self.fd)


class Input:
    def __init__(self):
        self.kbd = self.ptr = None
        try:
            self.kbd = UInput('rv-keyboard', 0x5201, keys=range(1, 249))
            self.ptr = UInput('rv-pointer', 0x5202, keys=BUTTONS.values(),
                              rels=(REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES),
                              abs_axes=(ABS_X, ABS_Y))
        except OSError as e:
            log(f'建 uinput 设备失败（{e}），只能看不能控制。用户要在 input 组。')
            self.close()
        self.wheel = [0, 0]

    @property
    def ok(self):
        return self.kbd is not None and self.ptr is not None

    def handle(self, m, keys_down, btns_down, wheel):
        if not self.ok:
            return
        t = m.get('t')
        if t == 'm':
            x = min(max(float(m['x']), 0.0), 1.0)
            y = min(max(float(m['y']), 0.0), 1.0)
            self.ptr.emit([(EV_ABS, ABS_X, round(x * ABS_MAX)), (EV_ABS, ABS_Y, round(y * ABS_MAX))])
        elif t == 'b':
            btn = BUTTONS.get(m.get('b'))
            if btn is None:
                return
            down = 1 if m.get('d') else 0
            (btns_down.add if down else btns_down.discard)(btn)
            self.ptr.emit([(EV_KEY, btn, down)])
        elif t == 'w':
            ev = []
            # 高精度滚轮 120 = 一格；同时补发传统 REL_WHEEL，和真实鼠标一致
            for i, (hi_code, lo_code, val) in enumerate(((REL_WHEEL_HI_RES, REL_WHEEL, m.get('v', 0)),
                                                         (REL_HWHEEL_HI_RES, REL_HWHEEL, m.get('h', 0)))):
                val = max(-1200, min(1200, int(val)))
                if not val:
                    continue
                ev.append((EV_REL, hi_code, val))
                wheel[i] += val
                notches = int(wheel[i] / 120)
                if notches:
                    ev.append((EV_REL, lo_code, notches))
                    wheel[i] -= notches * 120
            if ev:
                self.ptr.emit(ev)
        elif t == 'k':
            key = CODE2KEY.get(m.get('c'))
            if key is None:
                return
            down = 1 if m.get('d') else 0
            (keys_down.add if down else keys_down.discard)(key)
            self.kbd.emit([(EV_KEY, key, down)])
        elif t == 'release':
            self.release_all(keys_down, btns_down)

    def release_all(self, keys_down, btns_down):
        if not self.ok:
            keys_down.clear()
            btns_down.clear()
            return
        if keys_down:
            self.kbd.emit([(EV_KEY, k, 0) for k in keys_down])
        if btns_down:
            self.ptr.emit([(EV_KEY, b, 0) for b in btns_down])
        keys_down.clear()
        btns_down.clear()

    def close(self):
        for d in (self.kbd, self.ptr):
            if d is not None:
                d.close()
        self.kbd = self.ptr = None


# HTTP / WebSocket

def local_origin(origin):
    # 只接受本机页面发起的 WebSocket，防止别的网站借浏览器连 localhost 注入输入
    if origin is None:
        return True
    for host in ('localhost', '127.0.0.1', '[::1]'):
        if origin == f'http://{host}' or origin.startswith(f'http://{host}:'):
            return True
    return False


class Server:
    def __init__(self, args):
        self.args = args
        self.input = Input()
        self.current = None
        self.stop = None
        self.exit_code = 0
        # 按下的键 / 鼠标键属于每个连接，旧连接收尾不能把新连接按着的键释放掉
        self.keys_down = set()
        self.btns_down = set()
        self.wheel = [0, 0]
        with open(os.path.join(HERE, 'index.html'), 'rb') as f:
            self.index = f.read()

    def quit(self, code=0):
        self.exit_code = code
        if self.stop is not None and not self.stop.done():
            self.stop.set_result(None)

    def cap_cmd(self):
        a = self.args
        cmd = [os.path.join(HERE, 'rv-cap'), '-r', str(a.fps), '-q', str(a.crf), '-t', str(a.threads)]
        if a.size:
            w, _, h = a.size.partition('x')
            if w:
                cmd += ['-w', w]
            if h:
                cmd += ['-h', h]
        if a.no_cursor:
            cmd.append('-n')
        if a.cpu:
            cmd.append('-C')
        return cmd

    def process_request(self, connection, request):
        path = request.path.split('?')[0]
        if path == '/ws':
            if not local_origin(request.headers.get('Origin')):
                log('拒绝非本机来源的连接：', request.headers.get('Origin'))
                return connection.respond(403, 'Forbidden\n')
            return None
        if path in ('/', '/index.html'):
            return Response(200, 'OK', Headers([
                ('Content-Type', 'text/html; charset=utf-8'),
                ('Content-Length', str(len(self.index))),
                ('Cache-Control', 'no-store'),
            ]), self.index)
        if path == '/favicon.ico':
            return Response(204, 'No Content', Headers([('Content-Length', '0')]), b'')
        return connection.respond(404, 'Not Found\n')

    async def handler(self, ws):
        old, self.current = self.current, ws
        if old is not None:
            log('新窗口连上，断开旧窗口')
            await old.close(4000, 'replaced')
        peer = ws.remote_address
        display, why = find_display()
        if display is None:
            log('浏览器连上', peer, '，但', no_display_msg(why))
            if why == 'noperm':
                # 用户会话没了、回到了登录界面。退出让 rv-go.ps1 请求授权后重启服务端，浏览器会自己重连
                msg = '设备回到了登录界面，需要重新授权：到 rv-go 窗口输入设备的 sudo 密码，授权后这里会自动连上'
            else:
                msg = '设备上暂时找不到 treeland（正在注销 / 重启？），稍后自动重试'
            await ws.send(json.dumps({'type': 'notice', 'msg': msg, 'sticky': True}))
            await ws.close(4002, why)
            if self.current is ws:
                self.current = None
            if why == 'noperm':
                self.quit(EXIT_NOPERM)
            return
        env = dict(os.environ, WAYLAND_DISPLAY=display)
        log('浏览器连上', peer, f'，采集{why} {display}，启动', ' '.join(self.cap_cmd()))
        proc = await asyncio.create_subprocess_exec(*self.cap_cmd(), stdin=asyncio.subprocess.PIPE,
                                                    stdout=asyncio.subprocess.PIPE, env=env)
        if not self.input.ok:
            await ws.send(json.dumps({'type': 'notice', 'msg': '服务端建不了 uinput 设备，只能看不能控制'}))

        async def pump():
            try:
                while True:
                    hdr = await proc.stdout.readexactly(8)
                    typ, flags, _, length = struct.unpack('>BBHI', hdr)
                    data = await proc.stdout.readexactly(length)
                    if typ == 3:
                        # rv-cap 正在等亮屏 / 放弃：都作为 sticky 提示发给页面。
                        # 每个 rv-cap 进程对每种状态只发一次，不会刷屏；亮屏后 type 1 config
                        # 到达时页面会 showMsg('') 清掉。之前只转 flags==0 把"等亮屏"吞了。
                        await ws.send(json.dumps({'type': 'notice', 'msg': data.decode().strip(), 'sticky': True}))
                    elif typ == 1:
                        await ws.send(data.decode())
                    elif typ == 2:
                        # send() 在缓冲区满时会等，rv-cap 随之阻塞在写管道上，自然限速
                        await ws.send(bytes([flags]) + data)
            except asyncio.IncompleteReadError:
                code = await proc.wait()
                log(f'rv-cap 退出（{code}）')
                await ws.close(4001, f'rv-cap exit {code}')

        pump_task = asyncio.create_task(pump())
        try:
            async for msg in ws:
                if not isinstance(msg, str):
                    continue
                try:
                    m = json.loads(msg)
                except ValueError:
                    continue
                if m.get('t') == 'kf':
                    if proc.returncode is None:
                        proc.stdin.write(b'k')
                else:
                    try:
                        self.input.handle(m, self.keys_down, self.btns_down, self.wheel)
                    except (OSError, ValueError, KeyError, TypeError) as e:
                        log('注入输入失败：', e)
        finally:
            self.input.release_all(self.keys_down, self.btns_down)
            pump_task.cancel()
            if proc.returncode is None:
                proc.stdin.close()
                try:
                    await asyncio.wait_for(proc.wait(), 3)
                except asyncio.TimeoutError:
                    proc.kill()
                    await proc.wait()
            if self.current is ws:
                self.current = None
            log('浏览器断开', peer)

    async def run(self):
        a = self.args
        loop = asyncio.get_running_loop()
        stop = self.stop = loop.create_future()
        for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            loop.add_signal_handler(s, self.quit)
        # ssh 没分配终端时断线收不到 SIGHUP，只会看到 stdin（管道/套接字）EOF，这时也退出
        mode = os.fstat(0).st_mode
        if stat.S_ISFIFO(mode) or stat.S_ISSOCK(mode):
            def on_stdin():
                try:
                    data = os.read(0, 4096)
                except OSError:
                    data = b''
                if not data:
                    loop.remove_reader(0)
                    self.quit()
                    log('标准输入关闭（SSH 断开？），退出')
            loop.add_reader(0, on_stdin)
        async with serve(self.handler, a.listen, a.port, process_request=self.process_request,
                         compression=None, max_size=1 << 16, ping_interval=20):
            display, why = find_display()
            log(f'监听 {a.listen}:{a.port}，' + ('可以控制' if self.input.ok else '只能查看') +
                f'；当前画面：{why} {display}')
            log(f'Windows 上执行：ssh -N -L {a.port}:127.0.0.1:{a.port} <用户>@<本机 IP>，'
                f'然后用 Chrome/Edge 打开 http://localhost:{a.port}')
            if a.listen not in ('127.0.0.1', 'localhost', '::1'):
                log('警告：监听的不是本机回环地址，同网段的人都能连上并控制这台机器')
            await stop
        self.input.close()
        return self.exit_code


def main():
    p = argparse.ArgumentParser(description='L540 远程查看/控制服务端')
    p.add_argument('--listen', default='127.0.0.1', help='监听地址（默认 127.0.0.1，经 SSH 隧道访问）')
    p.add_argument('--port', type=int, default=8765)
    p.add_argument('--size', default='', help='编码尺寸，如 1440x960、1440x（按比例）；默认与屏幕物理分辨率相同')
    p.add_argument('--fps', type=int, default=30, help='最大帧率（画面不变时不出帧）')
    p.add_argument('--crf', type=int, default=23, help='x264 质量，越小越清晰、码率越高')
    p.add_argument('--threads', type=int, default=4)
    p.add_argument('--no-cursor', action='store_true', help='画面里不叠加远端光标')
    p.add_argument('--cpu', action='store_true', help='缩放和颜色转换不用 GPU，全在 CPU 上做（GPU 不可用时会自动退回）')
    p.add_argument('--probe', action='store_true',
                   help=f'只检查现在能采集哪个 treeland 就退出：0 能采集，{EXIT_NOPERM} 在登录界面但没权限，{EXIT_NONE} 找不到')
    args = p.parse_args()
    display, why = find_display()
    if display is None:
        print(no_display_msg(why), file=sys.stdout if args.probe else sys.stderr)
        sys.exit(EXIT_NOPERM if why == 'noperm' else EXIT_NONE)
    if args.probe:
        print(f'{why}：{display}')
        return
    if not os.path.exists(os.path.join(HERE, 'rv-cap')):
        sys.exit('找不到 rv-cap，先在本目录 make')
    sys.exit(asyncio.run(Server(args).run()))


if __name__ == '__main__':
    main()
