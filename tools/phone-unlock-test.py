#!/usr/bin/env python3
"""不用手机测电脑端的手机解锁（Wi-Fi / TCP），按 docs/phone-unlock.md 的协议扮演 App。

  sudo hisi-phone pair                                  # 电脑上出配对链接（hisiphone://pair?...）
  phone-unlock-test.py pair 'hisiphone://pair?...'      # 用软件密钥配对，状态存到 --state
  phone-unlock-test.py hello [--shot out.jpg]           # HELLO，解开电脑信息，可选取锁屏截图
  phone-unlock-test.py unlock [--shot out.jpg]          # HELLO → (SHOT) → UNLOCK
  phone-unlock-test.py bad                              # 故意发错签名，看电脑端拒绝
  phone-unlock-test.py lock                             # HELLO → LOCK（锁屏，不用签名）
  phone-unlock-test.py badlock                          # 故意发错的锁屏 mac，看电脑端拒绝
  phone-unlock-test.py away [--grace 30]                # AWAY 走 TCP，电脑端应回 ERR 11（只认蓝牙）

需要 python3-cryptography。私钥是普通文件，只用于测试，不受指纹保护。
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import socket
import sys
import urllib.parse

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

ERR = {1: "手机没配对", 2: "没有图形会话", 3: "签名不对", 4: "挑战过期", 5: "暂停中", 6: "没在配对",
       7: "配对码不对", 8: "没锁屏", 9: "忙", 10: "电脑内部错误", 11: "请求格式不对", 12: "服务没运行"}


def b64(b):
    return base64.b64encode(b).decode()


def unb64(s):
    return base64.b64decode(s, validate=True)


def H(k, m):
    return hmac.new(k, m, hashlib.sha256).digest()


def cat(*parts):
    return b"\0".join(p if isinstance(p, bytes) else p.encode() for p in parts)


def unbox(k, aad, data):
    return AESGCM(k).decrypt(data[:12], data[12:], aad.encode())


class Conn:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=15)
        self.f = self.s.makefile("rb")

    def ask(self, line):
        self.s.sendall(line.encode() + b"\n")
        r = self.f.readline().decode().rstrip("\n")
        if not r:
            sys.exit("电脑断开了连接")
        if r.startswith("ERR "):
            _, code, msg = r.split(" ", 2)
            raise RuntimeError(f"ERR {code}（{ERR.get(int(code), '?')}）：{unb64(msg).decode()}")
        return r.split(" ")


def cmd_pair(a):
    u = urllib.parse.urlparse(a.uri)
    q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query, keep_blank_values=True).items()}
    if u.scheme != "hisiphone" or u.netloc != "pair" or q.get("v") != "1":
        sys.exit("不是配对链接")
    code = base64.urlsafe_b64decode(q["c"] + "=" * (-len(q["c"]) % 4))
    pc_id = q["id"]
    phone_id = os.urandom(16).hex()
    name = a.name.encode()
    key = ec.generate_private_key(ec.SECP256R1())
    pub = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    T = cat("hisi-phone-pair-v1", pc_id, phone_id, name, pub)
    c = Conn(a.host or (q.get("ip") or "127.0.0.1").split(",")[0], int(q.get("p") or 47390))
    print("PING →", " ".join(c.ask("PING")))
    r = c.ask(f"PAIR 1 {phone_id} {b64(name)} {b64(pub)} {b64(H(code, T))}")
    if r[0] != "PAIROK" or not hmac.compare_digest(unb64(r[1]), H(code, cat("hisi-phone-pair-ok-v1", T))):
        sys.exit("mac2 不对：对方不是这台电脑")
    S = H(code, cat("hisi-phone-secret-v1", pc_id, phone_id))
    st = {"pc_id": pc_id, "phone_id": phone_id, "S": b64(S), "host": q.get("h"), "user": q.get("u"),
          "ip": q.get("ip"), "port": int(q.get("p") or 47390), "bt": q.get("bt"),
          "key": key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                   serialization.NoEncryption()).decode()}
    fd = os.open(a.state, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(st, f, indent=1)
    print(f"配对成功：{st['user']}@{st['host']}，手机 ID {phone_id}，状态存到 {a.state}")


def session(a, st):
    c = Conn(a.host or (st.get("ip") or "127.0.0.1").split(",")[0], st["port"])
    pn = os.urandom(16)
    r = c.ask(f"HELLO 1 {st['phone_id']} {b64(pn)}")
    assert r[0] == "CHAL", r
    cn = unb64(r[1])
    ks = H(unb64(st["S"]), cat("hisi-phone-session-v1", cn + pn))
    info = unbox(ks, "hisi-info-v1", unb64(r[2])).decode()
    print("电脑信息：", dict(l.split("=", 1) for l in info.splitlines() if l))
    if a.shot:
        w = str(a.width)
        r = c.ask(f"SHOT {w} {b64(H(ks, cat('hisi-phone-shot-v1', w)))}")
        assert r[0] == "SHOTOK", r
        jpg = unbox(ks, "hisi-shot-v1", unb64(r[1]))
        with open(a.shot, "wb") as f:
            f.write(jpg)
        print(f"截图 {len(jpg)} 字节 → {a.shot}")
    return c, cn, pn, ks


def cmd_hello(a):
    session(a, json.load(open(a.state)))


def cmd_lock(a, bad=False):
    st = json.load(open(a.state))
    c, cn, pn, ks = session(a, st)
    mac = H(ks, b"hisi-phone-lock-v1")
    if bad:
        mac = H(ks, b"hisi-phone-lock-v2")
    print("LOCK →", " ".join(c.ask(f"LOCK {b64(mac)}")))


def cmd_away(a):
    """AWAY 只认蓝牙，走 TCP 应该回 ERR 11；用来确认电脑端把关。"""
    st = json.load(open(a.state))
    c, cn, pn, ks = session(a, st)
    g = str(a.grace)
    print("AWAY →", " ".join(c.ask(f"AWAY {g} {b64(H(ks, cat('hisi-phone-away-v1', g)))}")))


def cmd_unlock(a, bad=False):
    st = json.load(open(a.state))
    c, cn, pn, ks = session(a, st)
    key = serialization.load_pem_private_key(st["key"].encode(), None)
    U = cat("hisi-phone-unlock-v1", st["pc_id"], st["phone_id"], cn + pn)
    if bad:
        U += b"x"
    print("UNLOCK →", " ".join(c.ask(f"UNLOCK {b64(key.sign(U, ec.ECDSA(hashes.SHA256())))}")))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--state", default=os.path.expanduser("~/.cache/phone-unlock-test.json"))
    p.add_argument("--host", help="电脑地址（默认用二维码 / 状态里的第一个 IP）")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("pair")
    s.add_argument("uri")
    s.add_argument("--name", default="phone-unlock-test")
    for n in ("hello", "unlock", "bad", "lock", "badlock", "away"):
        s = sub.add_parser(n)
        s.add_argument("--shot", help="把锁屏截图存到这个文件")
        s.add_argument("--width", type=int, default=720)
        if n == "away":
            s.add_argument("--grace", type=int, default=30)
    a = p.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(a.state)), exist_ok=True)
    try:
        {"pair": cmd_pair, "hello": cmd_hello, "unlock": cmd_unlock,
         "bad": lambda a: cmd_unlock(a, True), "lock": cmd_lock,
         "badlock": lambda a: cmd_lock(a, True), "away": cmd_away}[a.cmd](a)
    except RuntimeError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
