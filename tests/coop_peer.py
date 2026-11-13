"""A second co-op player without a second game, for tests on one PC (docs/coop.md, "Checks").

It speaks the stand-in for Steam (runtime/coop_local.c: datagrams between UDP ports on 127.0.0.1) and the room's
messages (runtime/coop.c, runtime/coop_game.c) to a game started with NX2NAVI_COOP_LOCAL=<host port> and
NX2NAVI_COOP_LOCAL_MAKE=1:

    python tests/coop_peer.py --host 27601 [--port 27602] [--seconds 120] [--flags HASH[,HASH...]]

It joins the game's room, says HELLO, and once the game's Link is in the world stands two metres beside him (so the
game shows its name there), says a chat line, asks for the game's shared progress and, with --flags, sends those flags
(hex hashes of on/off flags) as set. It prints what the game sends and ends with "peer: ok" when it was let in, saw
the game's Link in the world and got its progress; python's exit code says the same. Standard library only.
"""
import argparse
import socket
import struct
import sys
import time

L_JOIN, L_MEMBERS, L_DATA, L_LEAVE = 1, 2, 3, 4
K_HELLO, K_WELCOME, K_REFUSED, K_ROSTER, K_POSE, K_CHAT = 1, 2, 3, 4, 5, 6
K_FLAGS, K_ASK = 16, 17
PROTOCOL, GAME_BUILD = 1, 0xE0F3CCFA


def pose_bytes(number, pos, scene):
    s = scene.encode()
    return struct.pack('<I3f3ffBB', number, *pos, 0.0, 0.0, 0.0, 0.0, 0, len(s)) + s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', type=int, required=True, help="the game's NX2NAVI_COOP_LOCAL port")
    ap.add_argument('--port', type=int, default=27602)
    ap.add_argument('--seconds', type=float, default=120)
    ap.add_argument('--flags', default='', help='hex hashes of on/off flags to send as set, comma separated')
    a = ap.parse_args()
    flags = [int(h, 16) for h in a.flags.split(',') if h]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', a.port))
    sock.settimeout(0.02)
    host = ('127.0.0.1', a.host)

    def send(kind, body=b''):
        sock.sendto(bytes([L_DATA, kind]) + body, host)

    start = time.monotonic()
    in_lobby = welcomed = False
    last_join = last_hello = last_pose = last_ask = 0.0
    number = 0
    host_pose = None        # (scene, x, y, z) as the game last said
    schema = None
    got_pairs = 0
    chatted = flags_sent = False
    roster = 0
    while time.monotonic() - start < a.seconds:
        now = time.monotonic()
        if now - last_join >= 1.0:
            sock.sendto(bytes([L_JOIN]), host)
            last_join = now
        if in_lobby and not welcomed and now - last_hello >= 1.0:
            send(K_HELLO, struct.pack('<HI', PROTOCOL, GAME_BUILD))
            last_hello = now
        if welcomed and now - last_pose >= 0.05:
            number += 1
            if host_pose and host_pose[0]:
                send(K_POSE, pose_bytes(number, (host_pose[1] + 2.0, host_pose[2], host_pose[3] + 2.0), host_pose[0]))
            else:
                send(K_POSE, pose_bytes(number, (0.0, 0.0, 0.0), ''))
            last_pose = now
        if welcomed and host_pose and host_pose[0] and not chatted:
            send(K_CHAT, 'hello from the test peer'.encode())
            chatted = True
            print('peer: sent a chat line')
        if welcomed and schema is not None and now - last_ask >= 5.0:
            send(K_ASK, struct.pack('<I', schema))
            last_ask = now
        if welcomed and schema is not None and flags and not flags_sent:
            send(K_FLAGS, struct.pack('<I', schema) + b''.join(struct.pack('<II', h, 1) for h in flags))
            flags_sent = True
            print(f'peer: sent {len(flags)} flags as set')
        try:
            d, who = sock.recvfrom(2048)
        except (socket.timeout, ConnectionResetError):   # nothing yet, or the game's port is not open yet
            continue
        if who != host or not d:
            continue
        if d[0] == L_MEMBERS:
            if not in_lobby:
                print(f'peer: in the lobby with {(len(d) - 1) // 8} members')
            in_lobby = True
        elif d[0] == L_DATA and len(d) >= 2:
            kind, b = d[1], d[2:]
            if kind == K_WELCOME:
                welcomed = True
                print('peer: the host let it in')
            elif kind == K_REFUSED:
                print(f'peer: REFUSED, reason {b[0] if b else "?"}')
                return 1
            elif kind == K_ROSTER and b:
                if b[0] != roster:
                    roster = b[0]
                    print(f'peer: the host lists {roster} players: {[struct.unpack_from("<Q", b, 1 + 8 * i)[0] for i in range(roster)]}')
            elif kind == K_POSE and len(b) >= 34:
                n, x, y, z, vx, vy, vz, yaw, fl, sl = struct.unpack_from('<I3f3ffBB', b)
                scene = b[34:34 + sl].decode(errors='replace')
                if host_pose is None or host_pose[0] != scene:
                    print(f'peer: the game\'s Link: {scene or "(not in the world)"} at {x:.1f} {y:.1f} {z:.1f}, yaw {yaw:.2f}')
                host_pose = (scene, x, y, z)
            elif kind == K_CHAT:
                print(f'peer: chat from the game: {b.decode(errors="replace")}')
            elif kind == K_FLAGS and len(b) >= 4:
                if schema is None:
                    schema = struct.unpack_from('<I', b)[0]
                    print('peer: the game shares progress')
                got_pairs += (len(b) - 4) // 8
            elif kind == K_ASK and len(b) == 4:
                if schema is None:
                    schema = struct.unpack_from('<I', b)[0]
                    print('peer: the game asked for the room\'s progress')
    sock.sendto(bytes([L_LEAVE]), host)
    seen = bool(host_pose and host_pose[0])
    print(f'peer: welcomed {welcomed}, saw the game\'s Link in the world {seen}, got {got_pairs} values of its progress')
    ok = welcomed and seen and got_pairs > 0
    print('peer: ok' if ok else 'peer: FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
