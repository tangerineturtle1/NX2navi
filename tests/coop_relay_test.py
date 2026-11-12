"""tools/coop_relay.py and the co-op wire format: the secret alphabet, the derived keys, HMAC tags, the byte vectors of
tests/coop_vectors.txt (which runtime/coop_probe.c checks the C codec against), the relay's rooms (create, join, forward,
roster, rejoin with its token, expiry, limits per member, per network and for the relay, bad tags, a full room, a
protocol mismatch, removal by the maker) on a fake clock, hostile clients (a stolen client id, challenge floods, room
floods, fan-out, garbage from a member), the same over real UDP sockets, and, when NX2NAVI_EXE names a built game,
`--probe-coop` against a relay started here (with and without receive loss, and through a relay restart). No game files;
stdlib only.

Run: python tests/coop_relay_test.py            (NX2NAVI_EXE=<game>/ocarina also runs the probe)
     python tests/coop_relay_test.py --write-vectors   rewrites tests/coop_vectors.txt from the Python codec
"""
import hashlib
import hmac
import math
import os
import random
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import coop_relay as cr  # noqa: E402

VECTORS = ROOT / 'tests' / 'coop_vectors.txt'
RELAY_PY = ROOT / 'tools' / 'coop_relay.py'
SECRET = 'H7K3WMBCDFGH234567'

# Sealed player messages (XChaCha20-Poly1305 under the message key of SECRET), made once by runtime/coop_proto.c:
# Python's standard library has no XChaCha20-Poly1305, so this side checks their clear fields and the probe checks that
# they open, and that sealing again gives the same bytes. (name, src, dst, session, counter, kind, message, payload)
SEALED = [
    ('test', 3, 255, 0x0123456789abcdef, 0x00000001, 0xf0, '68656c6c6f',
     '01efcdab896745230101000000d2244219967816dfefab11d765441dbffe22c6b5b160'),
    ('empty', 1, 2, 0xfedcba9876543210, 0xffffffff, 0x03, '-', '011032547698badcfeffffffffe5632ae3fe1043b0359b661003ee21dbb9'),
]


def hx(b):
    return b.hex() if b else '-'


def unhex(s):
    return b'' if s == '-' else bytes.fromhex(s)


# ---- POS (player message kind 2, sealed as every player message): where a player is and what Link plays, for the co-op
# other players in the world (no sender in this build). The relay forwards it unread, so its codec is here and in runtime/coop_proto.c only.
# Little endian: u8 version (2), u32 pseq (the sender's counter), u32 t_ms (the sender's clock), u8 flags (POS_ADULT,
# POS_IN_WORLD), u8 area length (< 48) and the area's name, f32 x y z, f32 yaw (radians), u8 slot count (<= 4), per AS
# slot u8 node name length (< 48), the name, f32 frame. Names are printable ASCII and numbers finite, else the message
# is refused. Version 1 had a u16 pseq.
POS_VERSION = 2
POS_ADULT, POS_IN_WORLD = 1, 2
POS_TEXT_MAX, POS_SLOTS = 47, 4
POS_HEAD = 11


def pack_pos(pseq, t_ms, flags, area, xyz, yaw, slots):
    out = struct.pack('<BIIBB', POS_VERSION, pseq, t_ms, flags, len(area)) + area + struct.pack('<4f', *xyz, yaw)
    out += bytes([len(slots)])
    for node, frame in slots:
        out += bytes([len(node)]) + node + struct.pack('<f', frame)
    return out


def _printable(b):
    return all(0x20 <= c <= 0x7e for c in b)


def parse_pos(b):
    """(pseq, t_ms, flags, area, (x, y, z), yaw, ((node, frame), ...)), or None unless b is exactly one POS of version 2."""
    if len(b) < POS_HEAD or b[0] != POS_VERSION or b[10] > POS_TEXT_MAX or len(b) < POS_HEAD + b[10] + 17:
        return None
    pseq, t_ms, flags, alen = struct.unpack_from('<IIBB', b, 1)
    area, at = b[POS_HEAD:POS_HEAD + alen], POS_HEAD + alen
    x, y, z, yaw = struct.unpack_from('<4f', b, at)
    n, at = b[at + 16], at + 17
    if n > POS_SLOTS or not _printable(area) or not all(map(math.isfinite, (x, y, z, yaw))):
        return None
    slots = []
    for _ in range(n):
        if at >= len(b) or b[at] > POS_TEXT_MAX or at + 1 + b[at] + 4 > len(b):
            return None
        node = b[at + 1:at + 1 + b[at]]
        frame = struct.unpack_from('<f', b, at + 1 + len(node))[0]
        if not _printable(node) or not math.isfinite(frame):
            return None
        slots.append((node, frame))
        at += 1 + len(node) + 4
    return (pseq, t_ms, flags, area, (x, y, z), yaw, tuple(slots)) if at == len(b) else None


def pos_tokens(name, p):
    """A pos vector line's tokens: name, the payload, then the fields (floats exact in f32, so repr reads back)."""
    pseq, t_ms, flags, area, xyz, yaw, slots = parse_pos(p)
    return ('pos', name, hx(p), pseq, t_ms, flags, hx(area), *[repr(v) for v in xyz], repr(yaw),
            *[f'{hx(node)}:{frame!r}' for node, frame in slots])


# ---- the byte vectors

def build_vectors():
    """The lines of tests/coop_vectors.txt. HMAC and BLAKE2b values come from the standard library, everything else from
    the codec in tools/coop_relay.py (and SEALED from runtime/coop_proto.c); runtime/coop_proto.c is checked against the
    file, not against this function."""
    out = ['# Byte vectors of the co-op wire format, protocol 2. Written by `python tests/coop_relay_test.py --write-vectors`,',
           '# read by tests/coop_relay_test.py and by runtime/coop_probe.c (--probe-coop); each side checks its codec against them.',
           '# Hex is lower case, "-" is empty. Fields are tokens; a name is hex of its UTF-8 bytes.',
           '#',
           '# hmac <name> <key> <message> <HMAC-SHA256>                  RFC 4231 cases and a locator as the key',
           '# secret <canonical|-> <text, the rest of the line>          normalisation: the secret a text means, - for none',
           '# keys <secret> <locator> <message key>                      BLAKE2b keyed with the secret (coop_proto.h)',
           '# dgram <name> <secret|-> <ver> <type> <room> <src> <dst> <seq> <payload> <datagram>   parse, verify the tag with the',
           '#                                                            secret\'s locator (- : a zero tag), pack again',
           '# badtag <name> <secret> <datagram>                          parses, and the tag does not verify',
           '# connect|challenge|roster|joined|error|kick ...             one message payload: parse, check the fields, pack again',
           '# sealed <name> <secret> <src> <dst> <session> <counter> <kind> <message> <payload>   opens to kind and message, seals',
           '#                                                            to the same bytes',
           '# pos <name> <payload> <pseq> <t_ms> <flags> <area> <x> <y> <z> <yaw> <node:frame>...   a POS (the message after its',
           '#                                                            kind byte): parse, check the fields, pack again',
           '# reject <kind> <name> <bytes>                               parse must fail']

    def line(*tokens):
        out.append(' '.join(str(t) for t in tokens))

    loc = cr.room_locator(SECRET)
    for name, key, msg in (
            ('rfc4231-1', bytes([0x0b] * 20), b'Hi There'),
            ('rfc4231-2', b'Jefe', b'what do ya want for nothing?'),
            ('rfc4231-6', bytes([0xaa] * 131), b'Test Using Larger Than Block-Size Key - Hash Key First'),
            ('locator', loc, bytes(range(26))),
            ('empty', cr.room_locator('23BCDF' * 3), b'')):
        line('hmac', name, hx(key), hx(msg), hmac.new(key, msg, hashlib.sha256).hexdigest())
    for canon, text in ((SECRET, 'h7k3wm-bcdfgh-234567'), (SECRET, ' H7K 3WM BCD FGH 234 567 '), (SECRET, SECRET),
                        ('-', 'H7K3WMBCDFGH23456'), ('-', 'H7K3WMBCDFGH2345678'), ('-', 'H7K3WMBCDFGH23456E'),
                        ('-', 'H7K3WMBCDFGH23456Q'), ('-', 'H7K3WMBCDFGH234560'), ('-', 'H7K3WM.BCDFGH234567'), ('-', '')):
        line('secret', canon, text)
    for s in (SECRET, '23BCDF' * 3, 'Z' * 18):
        line('keys', s, hx(cr.room_locator(s)), hx(cr.message_key(s)))

    cookie = bytes(range(0xA0, 0xB0))
    token = bytes(range(0x30, 0x40))
    cid, session, nonce = 0x0123456789ABCDEF, 0x1122334455667788, 0x99AABBCCDDEEFF00
    roster = cr.pack_roster([(1, cr.FLAG_MAKER, session, b'Alice'), (2, cr.FLAG_AWAY, 7, 'Zoë'.encode())])
    sealed = unhex(SEALED[0][7])
    dgrams = [
        ('connect-create', SECRET, cr.T_CONNECT, 0, 0, 0, 7, cr.pack_connect(cr.INTENT_CREATE, 0, loc, cid, session, nonce, b'Alice')),
        ('connect-join', SECRET, cr.T_CONNECT, 0, 0, 0, 7, cr.pack_connect(cr.INTENT_JOIN, 0, loc, cid, session, nonce, b'Bob', cookie, token)),
        ('challenge', SECRET, cr.T_CHALLENGE, 0, 0, 0, 1, cr.pack_challenge(nonce, cookie)),
        ('joined', SECRET, cr.T_JOINED, 0xCAFE0001, 0, 2, 2, cr.pack_joined(2, nonce, token, roster)),
        ('error', SECRET, cr.T_ERROR, 0, 0, 0, 3, cr.pack_error(cr.E_NO_ROOM, nonce, 'no such room')),
        ('roster', SECRET, cr.T_ROSTER, 0xCAFE0001, 0, 1, 4, roster),
        ('ping', SECRET, cr.T_PING, 0xCAFE0001, 3, 0, 99, struct.pack('<I', 0xDEADBEEF)),
        ('pong', SECRET, cr.T_PONG, 0xCAFE0001, 0, 3, 100, struct.pack('<I', 0xDEADBEEF)),
        ('leave', SECRET, cr.T_LEAVE, 0xCAFE0001, 3, 0, 101, b''),
        ('kick', SECRET, cr.T_KICK, 0xCAFE0001, 1, 0, 102, cr.pack_kick(2, 7)),
        ('data-sealed', SECRET, cr.T_DATA, 0xCAFE0001, 3, cr.DST_ALL, 103, sealed),
        ('data-max', SECRET, cr.T_DATA, 1, 8, cr.DST_ALL, 0, bytes(i * 7 & 0xFF for i in range(cr.MAX_DGRAM - cr.HDR_LEN))),
    ]
    for name, s, type_, room, src, dst, seq, payload in dgrams:
        d = cr.pack(type_, room, src, dst, seq, payload, cr.room_locator(s) if s else None)
        line('dgram', name, s or '-', cr.PROTO, type_, room, src, dst, seq, hx(payload), d.hex())
    d = cr.pack(cr.T_DATA, 0xCAFE0001, 3, cr.DST_ALL, 103, sealed, loc)
    line('badtag', 'flipped-tag', SECRET, (d[:16] + bytes([d[16] ^ 1]) + d[17:]).hex())
    line('badtag', 'wrong-secret', '23BCDF' * 3, d.hex())
    line('dgram', 'other-version', '-', 1, cr.T_PING, 0, 0, 0, 5, hx(b'\0\0\0\0'), cr.pack(cr.T_PING, 0, 0, 0, 5, b'\0\0\0\0', None, ver=1).hex())

    line('connect', 'create', cr.INTENT_CREATE, 0, hx(loc), f'{cid:016x}', f'{session:016x}', f'{nonce:016x}', hx(b'Alice'),
         hx(bytes(16)), hx(bytes(16)), hx(cr.pack_connect(cr.INTENT_CREATE, 0, loc, cid, session, nonce, b'Alice')))
    line('connect', 'join', cr.INTENT_JOIN, 0, hx(loc), f'{cid:016x}', f'{session:016x}', f'{nonce:016x}', hx('Zoë'.encode()),
         hx(cookie), hx(token), hx(cr.pack_connect(cr.INTENT_JOIN, 0, loc, cid, session, nonce, 'Zoë'.encode(), cookie, token)))
    long_name = b'x' * cr.NAME_MAX
    line('connect', 'long-name', cr.INTENT_JOIN, 1, hx(loc), f'{1:016x}', f'{2:016x}', f'{3:016x}', hx(long_name), hx(cookie),
         hx(token), hx(cr.pack_connect(cr.INTENT_JOIN, 1, loc, 1, 2, 3, long_name, cookie, token)))
    line('challenge', 'one', hx(cr.pack_challenge(nonce, cookie)), f'{nonce:016x}', hx(cookie))
    line('roster', 'two', hx(roster), f'1:{cr.FLAG_MAKER}:{session:016x}:{b"Alice".hex()}', f'2:1:{7:016x}:{"Zoë".encode().hex()}')
    line('roster', 'empty', hx(cr.pack_roster([])))
    full = cr.pack_roster([(i, 0, i, f'P{i}'.encode()) for i in range(1, 9)])
    line('roster', 'full', hx(full), *[f'{i}:0:{i:016x}:{f"P{i}".encode().hex()}' for i in range(1, 9)])
    line('joined', 'two', hx(cr.pack_joined(2, nonce, token, roster)), 2, f'{nonce:016x}', hx(token),
         f'1:{cr.FLAG_MAKER}:{session:016x}:{b"Alice".hex()}', f'2:1:{7:016x}:{"Zoë".encode().hex()}')
    line('error', 'no-room', hx(cr.pack_error(cr.E_NO_ROOM, nonce, 'no such room')), cr.E_NO_ROOM, f'{nonce:016x}', hx(b'no such room'))
    line('error', 'bare', hx(cr.pack_error(cr.E_SERVER_FULL, 0, '')), cr.E_SERVER_FULL, f'{0:016x}', '-')
    line('kick', 'two', hx(cr.pack_kick(2, session)), 2, f'{session:016x}')
    for name, src, dst, sess, ctr, kind, msg, payload in SEALED:
        line('sealed', name, SECRET, src, dst, f'{sess:016x}', ctr, kind, msg, payload)
    walk = pack_pos(7, 123456, POS_IN_WORLD, b'HyruleField', (1234.5, -56.25, 789.125), 1.5,
                    [(b'Walk', 12.0), (b'Lower_Walk', 3.5)])
    line(*pos_tokens('walk', walk))
    line(*pos_tokens('adult-no-slots', pack_pos(0xFFFFFFFF, 0xFFFFFFFF, POS_ADULT | POS_IN_WORLD, b'ForestTemple',
                                                (0.0, 0.0, -0.5), -3.0, [])))
    line(*pos_tokens('loading', pack_pos(1, 2, 0, b'', (0.0, 0.0, 0.0), 0.0, [])))
    line(*pos_tokens('long-names', pack_pos(9, 9, POS_IN_WORLD, b'a' * POS_TEXT_MAX, (1.0, 2.0, 3.0), 0.25,
                                            [(b'n' * POS_TEXT_MAX, float(i)) for i in range(POS_SLOTS)])))

    zeros = bytes(16)
    ping = cr.pack(cr.T_PING, 0, 0, 0, 1, b'abcd')
    bad = [
        ('dgram', 'short', bytes(23)),
        ('dgram', 'magic', b'\x00\x00' + ping[2:]),
        ('dgram', 'length-long', ping[:10] + struct.pack('<H', 5) + ping[12:]),
        ('dgram', 'trailing', ping + b'x'),
        ('connect', 'name-too-long', struct.pack('<BB16sQQQB', 2, 0, loc, 1, 2, 3, 25) + bytes(25) + zeros + zeros),
        ('connect', 'no-token', struct.pack('<BB16sQQQB', 2, 0, loc, 1, 2, 3, 3) + b'Bob' + zeros + bytes(15)),
        ('connect', 'cut', struct.pack('<BB16sQ', 2, 0, loc, 1)),
        ('challenge', 'long', bytes(25)),
        ('challenge', 'short', bytes(23)),
        ('roster', 'count-9', bytes([9]) + bytes(100)),
        ('roster', 'cut-name', bytes([1, 1, 0]) + bytes(8) + bytes([9]) + b'abc'),
        ('roster', 'cut-entry', bytes([2, 1, 0]) + bytes(8) + bytes([1]) + b'A' + bytes([2, 0])),
        ('joined', 'cut', bytes([1]) + bytes(20)),
        ('error', 'cut-text', bytes([1]) + bytes(8) + bytes([9]) + b'abc'),
        ('error', 'cut', bytes([1]) + bytes(8)),
        ('kick', 'long', bytes(10)),
        ('sealed', 'short', bytes([1]) + bytes(28)),
        ('sealed', 'version', bytes([2]) + bytes(40)),
        ('pos', 'version-1', bytes([1]) + walk[1:]),
        ('pos', 'cut', walk[:-1]),
        ('pos', 'trailing', walk + b'x'),
        ('pos', 'area-48', struct.pack('<BIIBB', POS_VERSION, 0, 0, 0, 48) + bytes(48) + bytes(17)),
        ('pos', 'slots-5', pack_pos(0, 0, 0, b'', (0.0, 0.0, 0.0), 0.0, [])[:-1] + bytes([5]) + bytes(25)),
        ('pos', 'node-48', pack_pos(0, 0, 0, b'', (0.0, 0.0, 0.0), 0.0, [])[:-1] + bytes([1, 48]) + bytes(52)),
        ('pos', 'area-newline', pack_pos(1, 2, POS_IN_WORLD, b'Hyrule\nField', (0.0, 0.0, 0.0), 0.0, [])),
        ('pos', 'area-utf8', pack_pos(1, 2, POS_IN_WORLD, 'Zoë'.encode(), (0.0, 0.0, 0.0), 0.0, [])),
        ('pos', 'node-nul', pack_pos(1, 2, POS_IN_WORLD, b'HyruleField', (0.0, 0.0, 0.0), 0.0, [(b'Wa\0k', 1.0)])),
        ('pos', 'x-nan', pack_pos(1, 2, POS_IN_WORLD, b'HyruleField', (float('nan'), 0.0, 0.0), 0.0, [])),
        ('pos', 'yaw-inf', pack_pos(1, 2, POS_IN_WORLD, b'HyruleField', (0.0, 0.0, 0.0), float('inf'), [])),
        ('pos', 'frame-inf', pack_pos(1, 2, POS_IN_WORLD, b'HyruleField', (0.0, 0.0, 0.0), 0.0, [(b'Walk', float('-inf'))])),
        ('pos', 'empty', b''),
    ]
    for kind, name, data in bad:
        line('reject', kind, name, hx(data))
    return out


def write_vectors():
    VECTORS.write_text('\n'.join(build_vectors()) + '\n', encoding='utf-8')


def read_vectors():
    """[(kind, tokens)] of the data lines; a secret line keeps its text as one token after the canonical secret."""
    rows = []
    for raw in VECTORS.read_text(encoding='utf-8').splitlines():
        if not raw.strip() or raw.startswith('#'):
            continue
        if raw.startswith('secret '):
            parts = raw.split(' ', 2)
            rows.append(('secret', [parts[1], parts[2] if len(parts) > 2 else '']))
        else:
            parts = raw.split()
            rows.append((parts[0], parts[1:]))
    return rows


def roster_entries(tokens):
    return tuple((int(e.split(':')[0]), int(e.split(':')[1]), int(e.split(':')[2], 16), bytes.fromhex(e.split(':')[3]))
                 for e in tokens)


class CodecTest(unittest.TestCase):
    def test_the_alphabet_has_27_symbols_and_no_lookalikes(self):
        self.assertEqual(len(cr.ALPHABET), 27)
        self.assertEqual(len(set(cr.ALPHABET)), 27)
        for c in '01OILEQAU':
            self.assertNotIn(c, cr.ALPHABET)

    def test_a_secret_has_more_than_80_bits(self):
        import math
        self.assertGreater(cr.SECRET_LEN * math.log2(len(cr.ALPHABET)), 85)

    def test_normalize(self):
        self.assertEqual(cr.normalize_secret('h7k3wm-bcdfgh-234567'), SECRET)
        for bad in ('', SECRET[:-1], SECRET + 'X', SECRET[:-1] + '0', SECRET[:-1] + 'E', SECRET[:-1] + '\u017f'):
            self.assertIsNone(cr.normalize_secret(bad), bad)
        self.assertEqual(cr.format_secret(SECRET), 'H7K3WM-BCDFGH-234567')

    def test_random_secrets_are_valid(self):
        for _ in range(200):
            self.assertIsNotNone(cr.normalize_secret(cr.random_secret()))

    def test_keys_are_the_documented_blake2b(self):
        self.assertEqual(cr.room_locator(SECRET), hashlib.blake2b(b'nx2navi-coop/2 room', key=SECRET.encode(), digest_size=16).digest())
        self.assertEqual(cr.message_key(SECRET), hashlib.blake2b(b'nx2navi-coop/2 messages', key=SECRET.encode(), digest_size=32).digest())
        self.assertNotEqual(cr.room_locator(SECRET), cr.message_key(SECRET)[:16])

    def test_tag_covers_header_and_payload(self):
        key = cr.room_locator(SECRET)
        d = cr.pack(cr.T_DATA, 5, 1, 2, 3, b'payload', key)
        dg = cr.parse(d)
        self.assertTrue(dg.verify(key))
        for i in range(len(d)):
            if 16 <= i < 24 or i in (10, 11):
                continue   # the tag itself; the length (covered by parse)
            p = cr.parse(d[:i] + bytes([d[i] ^ 0x40]) + d[i + 1:])
            if p is not None:
                self.assertFalse(p.verify(key), f'byte {i}')
        self.assertFalse(dg.verify(cr.room_locator('23BCDF' * 3)))

    def test_clean_name(self):
        self.assertEqual(cr.clean_name(b'Alice'), b'Alice')
        self.assertEqual(cr.clean_name(b'A\x01B\x7fC'), b'A?B?C')
        self.assertEqual(cr.clean_name(b'ab\xffcd'), b'ab?cd')
        self.assertEqual(cr.clean_name('a\u0085b\u202ec\u2066d'.encode()), b'a?b?c?d')   # C1 and bidi controls
        self.assertEqual(len(cr.clean_name(('\u00e9' * 30).encode())), 24)
        self.assertEqual(cr.clean_name(b'x' * 30), b'x' * 24)

    def test_vectors(self):
        """The file matches what this codec writes now, and every line checks out."""
        self.assertEqual(VECTORS.read_text(encoding='utf-8'), '\n'.join(build_vectors()) + '\n',
                         'tests/coop_vectors.txt is stale: python tests/coop_relay_test.py --write-vectors')
        count = 0
        for kind, t in read_vectors():
            count += 1
            if kind == 'hmac':
                self.assertEqual(hmac.new(unhex(t[1]), unhex(t[2]), hashlib.sha256).hexdigest(), t[3], t[0])
            elif kind == 'secret':
                self.assertEqual(cr.normalize_secret(t[1]), None if t[0] == '-' else t[0], t[1])
            elif kind == 'keys':
                self.assertEqual((cr.room_locator(t[0]).hex(), cr.message_key(t[0]).hex()), (t[1], t[2]))
            elif kind == 'dgram':
                name, s, ver, type_, room, src, dst, seq, payload, data = t
                d = unhex(data)
                p = cr.parse(d)
                self.assertEqual((p.ver, p.type, p.room, p.src, p.dst, p.seq, p.payload),
                                 (int(ver), int(type_), int(room), int(src), int(dst), int(seq), unhex(payload)), name)
                key = cr.room_locator(s) if s != '-' else None
                self.assertTrue(p.verify(key) if key else p.tag == bytes(8), name)
                self.assertEqual(cr.pack(p.type, p.room, p.src, p.dst, p.seq, p.payload, key, ver=p.ver), d, name)
            elif kind == 'badtag':
                p = cr.parse(unhex(t[2]))
                self.assertIsNotNone(p, t[0])
                self.assertFalse(p.verify(cr.room_locator(t[1])), t[0])
            elif kind == 'connect':
                name, intent, flags, loc, cid, sess, nonce, nm, cookie, token, payload = t
                c = cr.parse_connect(unhex(payload))
                self.assertEqual(c, (int(intent), int(flags), unhex(loc), int(cid, 16), int(sess, 16), int(nonce, 16), unhex(nm),
                                     unhex(cookie), unhex(token)), name)
                self.assertEqual(cr.pack_connect(*c), unhex(payload), name)
            elif kind == 'challenge':
                self.assertEqual(cr.parse_challenge(unhex(t[1])), (int(t[2], 16), unhex(t[3])), t[0])
            elif kind == 'roster':
                payload, want = unhex(t[1]), roster_entries(t[2:])
                self.assertEqual(cr.parse_roster(payload), want, t[0])
                self.assertEqual(cr.pack_roster(want), payload, t[0])
            elif kind == 'joined':
                payload = unhex(t[1])
                self.assertEqual(cr.parse_joined(payload), (int(t[2]), int(t[3], 16), unhex(t[4]), roster_entries(t[5:])), t[0])
            elif kind == 'error':
                self.assertEqual(cr.parse_error(unhex(t[1])), (int(t[2]), int(t[3], 16), unhex(t[4]).decode()), t[0])
            elif kind == 'kick':
                self.assertEqual(cr.parse_kick(unhex(t[1])), (int(t[2]), int(t[3], 16)), t[0])
            elif kind == 'sealed':
                name, s, src, dst, sess, ctr, kind_byte, msg, payload = t
                p = unhex(payload)   # the clear fields; the probe opens it
                self.assertEqual((p[0], struct.unpack_from('<QI', p, 1), len(p)),
                                 (cr.ENV_V1, (int(sess, 16), int(ctr)), cr.ENV_HDR + cr.ENV_MAC + 1 + len(unhex(msg))), name)
            elif kind == 'pos':
                name, payload = t[0], unhex(t[1])
                pseq, t_ms, flags, area, x, y, z, yaw = t[2:10]
                want = (int(pseq), int(t_ms), int(flags), unhex(area), (float(x), float(y), float(z)), float(yaw),
                        tuple((unhex(s.split(':')[0]), float(s.split(':')[1])) for s in t[10:]))
                self.assertEqual(parse_pos(payload), want, name)
                self.assertEqual(pack_pos(*want[:6], list(want[6])), payload, name)
            elif kind == 'reject':
                what, data = t[0], unhex(t[2])
                parsers = {'dgram': cr.parse, 'connect': cr.parse_connect, 'challenge': cr.parse_challenge, 'roster': cr.parse_roster,
                           'joined': cr.parse_joined, 'error': cr.parse_error, 'kick': cr.parse_kick,
                           'sealed': lambda b: None if len(b) < cr.ENV_HDR + cr.ENV_MAC + 1 or b[0] != cr.ENV_V1 else b,
                           'pos': parse_pos}
                self.assertIsNone(parsers[what](data), f'{what} {t[1]}')
            else:
                self.fail(f'unknown vector line {kind}')
        self.assertGreater(count, 60)


# ---- the relay on a fake clock

class Client:
    """A fake client: what coop.c does, one datagram at a time, against Relay.handle."""
    _next_port = 40000

    def __init__(self, name='Alice', cid=None, ip='10.0.0.1', secret=None):
        Client._next_port += 1
        self.addr = (ip, Client._next_port)
        self.name = name.encode()
        self.cid = cid if cid is not None else random.getrandbits(64)
        self.seq = 0
        self.room = 0
        self.id = 0
        self.secret = secret or cr.random_secret()
        self.token = bytes(16)
        self.session = random.getrandbits(64)
        self.nonce = 0
        self.roster = ()

    @property
    def key(self):
        return cr.room_locator(self.secret)

    def connect(self, intent, cookie=bytes(16), seq=None, token=None):
        payload = cr.pack_connect(intent, 0, self.key, self.cid, self.session, self.nonce, self.name, cookie,
                                  self.token if token is None else token)
        return cr.pack(cr.T_CONNECT, 0, 0, 0, self.seq if seq is None else seq, payload, self.key)

    def join(self, relay, now, intent=cr.INTENT_JOIN, secret=None, seq=None):
        """The whole handshake; (JOINED fields or ERROR (reason, nonce, text), the other datagrams the relay sent)."""
        if secret:
            self.secret = secret
        self.seq += 1
        self.nonce = random.getrandbits(64)
        out = relay.handle(self.connect(intent, seq=seq), self.addr, now)
        assert out and out[0][0] == self.addr, out   # a challenge, or (a member with its token) JOINED at once
        ch = cr.parse(out[0][1])
        if ch.type == cr.T_CHALLENGE:
            assert len(out[0][1]) < 64 and ch.verify(self.key)
            nonce, cookie = cr.parse_challenge(ch.payload)
            assert nonce == self.nonce
            out = relay.handle(self.connect(intent, cookie, seq=seq), self.addr, now)
        mine = [cr.parse(d) for to, d in out if to == self.addr]
        rest = [(to, cr.parse(d)) for to, d in out if to != self.addr]
        first = mine[0]
        assert first.verify(self.key)
        if first.type == cr.T_ERROR:
            e = cr.parse_error(first.payload)
            assert e[1] == self.nonce
            return e, rest
        assert first.type == cr.T_JOINED, first.type
        member, nonce, token, roster = cr.parse_joined(first.payload)
        assert nonce == self.nonce
        self.room, self.id, self.token, self.roster = first.room, member, token, roster
        return (self.id, roster), rest

    def send(self, relay, type_, payload=b'', dst=0, now=0, key='own', room=None, src=None):
        self.seq += 1
        key = self.key if key == 'own' else key
        d = cr.pack(type_, self.room if room is None else room, self.id if src is None else src, dst, self.seq, payload, key)
        return relay.handle(d, self.addr, now), d


def parsed(out):
    return [(to, cr.parse(d)) for to, d in out]


class RelayTest(unittest.TestCase):
    def setUp(self):
        self.relay = cr.Relay()
        self.log = []
        self.relay.log_fn = self.log.append

    def room_of(self, n, now=0, relay=None):
        """n clients in one room: (clients, secret)."""
        relay = relay or self.relay
        a = Client('A')
        a.join(relay, now, cr.INTENT_CREATE)
        clients = [a]
        for i in range(1, n):
            c = Client(f'P{i}', ip=f'10.0.{i}.1')
            c.join(relay, now, cr.INTENT_JOIN, a.secret)
            clients.append(c)
        return clients, a.secret

    # -- joining

    def test_create_answers_a_challenge_then_a_room(self):
        a = Client('Alice')
        a.seq, a.nonce = 1, 77
        first = a.connect(cr.INTENT_CREATE)
        self.assertGreaterEqual(len(first), cr.CONNECT_MIN)
        out = self.relay.handle(first, a.addr, 0)
        ch = cr.parse(out[0][1])
        self.assertEqual((len(out), ch.type, ch.room, ch.src), (1, cr.T_CHALLENGE, 0, 0))
        self.assertLess(len(out[0][1]), len(first))
        self.assertEqual(cr.parse_challenge(ch.payload)[0], 77)   # the nonce comes back
        self.assertEqual(self.relay.rooms, {})   # nothing exists before the cookie came back
        info, rest = a.join(self.relay, 0, cr.INTENT_CREATE)
        self.assertEqual((info[0], info[1][0][0], info[1][0][1], info[1][0][3]), (1, 1, cr.FLAG_MAKER, b'Alice'))
        self.assertEqual(rest, [])
        self.assertEqual(len(self.relay.rooms), 1)

    def test_the_relay_never_sees_the_secret(self):
        a = Client('Alice')
        sent = []
        real = self.relay.handle
        self.relay.handle = lambda d, addr, now: (sent.append(d), real(d, addr, now))[1]
        a.join(self.relay, 0, cr.INTENT_CREATE)
        b = Client('Bob', secret=a.secret, ip='10.0.9.1')
        b.join(self.relay, 0, cr.INTENT_JOIN)
        blob = b''.join(sent)
        for leak in (a.secret.encode(), cr.message_key(a.secret)[:8]):
            self.assertNotIn(leak, blob)
        self.assertIn(cr.room_locator(a.secret), blob)
        self.assertEqual(list(self.relay.locators), [cr.room_locator(a.secret)])

    def test_a_short_connect_gets_no_answer(self):
        a = Client()
        d = a.connect(cr.INTENT_CREATE)
        self.assertEqual(self.relay.handle(d[:cr.CONNECT_MIN - 1], a.addr, 0), [])

    def test_a_cookie_belongs_to_its_address_and_expires(self):
        a, b = Client('A'), Client('B')
        a.seq = b.seq = 1
        ch = cr.parse(self.relay.handle(a.connect(cr.INTENT_CREATE), a.addr, 0)[0][1])
        cookie = cr.parse_challenge(ch.payload)[1]
        self.assertEqual(cr.parse(self.relay.handle(b.connect(cr.INTENT_CREATE, cookie), b.addr, 1)[0][1]).type, cr.T_CHALLENGE)
        self.assertEqual(cr.parse(self.relay.handle(a.connect(cr.INTENT_CREATE, cookie), a.addr, 125)[0][1]).type, cr.T_CHALLENGE)
        self.assertEqual(cr.parse(self.relay.handle(a.connect(cr.INTENT_CREATE, cookie), a.addr, 59)[0][1]).type, cr.T_JOINED)

    def test_join_and_the_roster_of_the_others(self):
        a = Client('Alice')
        a.join(self.relay, 0, cr.INTENT_CREATE)
        b = Client('Bob', ip='10.0.9.1')
        info, rest = b.join(self.relay, 1, cr.INTENT_JOIN, a.secret)
        self.assertEqual(info[0], 2)
        self.assertEqual(info[1], ((1, cr.FLAG_MAKER, a.session, b'Alice'), (2, 0, b.session, b'Bob')))
        (to, dg), = rest
        self.assertEqual((to, dg.type, dg.dst), (a.addr, cr.T_ROSTER, 1))
        self.assertTrue(dg.verify(a.key))
        self.assertEqual(cr.parse_roster(dg.payload), info[1])

    def test_no_such_room_and_a_bad_tag(self):
        a = Client()
        info, _ = a.join(self.relay, 0, cr.INTENT_JOIN)
        self.assertEqual(info[0], cr.E_NO_ROOM)
        b = Client()
        b.seq, b.nonce = 1, 5
        ch = cr.parse(self.relay.handle(b.connect(cr.INTENT_JOIN), b.addr, 0)[0][1])
        d = bytearray(b.connect(cr.INTENT_JOIN, cr.parse_challenge(ch.payload)[1]))
        d[17] ^= 1
        (to, dg), = parsed(self.relay.handle(bytes(d), b.addr, 0))
        self.assertEqual(cr.parse_error(dg.payload)[:2], (cr.E_BAD_TAG, 5))
        self.assertTrue(dg.verify(b.key))

    def test_a_create_of_an_open_room_is_refused_not_joined(self):
        (a,), secret = self.room_of(1)
        x = Client('X', ip='10.7.0.1')
        info, _ = x.join(self.relay, 1, cr.INTENT_CREATE, secret)
        self.assertEqual(info[0], cr.E_ROOM_EXISTS)
        self.assertEqual(len(self.relay.rooms[a.room].live()), 1)

    def test_a_connect_sent_again_is_answered_again(self):
        a = Client('A')
        info, _ = a.join(self.relay, 0, cr.INTENT_CREATE)
        d = a.connect(cr.INTENT_CREATE, self.relay.cookie(a.addr, 0))
        second = cr.parse(self.relay.handle(d, a.addr, 0.1)[0][1])
        self.assertEqual((second.type, second.room), (cr.T_JOINED, a.room))
        self.assertEqual(len(self.relay.rooms[a.room].live()), 1)

    def test_room_full_at_eight(self):
        clients, secret = self.room_of(8)
        self.assertEqual([c.id for c in clients], list(range(1, 9)))
        info, _ = Client('X', ip='10.8.0.1').join(self.relay, 0, cr.INTENT_JOIN, secret)
        self.assertEqual(info[0], cr.E_ROOM_FULL)

    def test_server_full(self):
        relay = cr.Relay(max_rooms=2)
        for i in range(2):
            Client(ip=f'10.{i}.0.1').join(relay, 0, cr.INTENT_CREATE)
        info, _ = Client(ip='10.9.0.1').join(relay, 0, cr.INTENT_CREATE)
        self.assertEqual(info[0], cr.E_SERVER_FULL)

    def test_protocol_mismatch(self):
        a = Client()
        d = cr.pack(cr.T_CONNECT, 0, 0, 0, 1, bytes(60), ver=1)
        (to, dg), = parsed(self.relay.handle(d, a.addr, 0))
        self.assertEqual((dg.type, dg.ver, dg.tag, cr.parse_error(dg.payload)[0]), (cr.T_ERROR, cr.PROTO, bytes(8), cr.E_PROTO))
        self.assertLessEqual(len(self.relay.handle(d, a.addr, 0)[0][1]), len(d))
        self.assertEqual(self.relay.handle(cr.pack(cr.T_PING, 0, 0, 0, 1, b'', ver=1), a.addr, 0), [])

    def test_names_are_cleaned(self):
        a = Client()
        a.name = b'\x01evil\xff' + '\u00e9'.encode() * 8
        info, _ = a.join(self.relay, 0, cr.INTENT_CREATE)
        name = info[1][0][3]
        self.assertTrue(name.startswith(b'?evil?') and len(name) <= cr.NAME_MAX)

    # -- forwarding

    def test_data_goes_to_the_others_unchanged(self):
        (a, b, c), _ = self.room_of(3)
        out, d = a.send(self.relay, cr.T_DATA, b'\x01sealed', dst=cr.DST_ALL, now=1)
        self.assertEqual(sorted(to for to, _ in out), sorted([b.addr, c.addr]))
        self.assertTrue(all(data == d for _, data in out))
        out, d = a.send(self.relay, cr.T_DATA, b'\x01to b', dst=b.id, now=1)
        self.assertEqual(out, [(b.addr, d)])
        self.assertEqual(a.send(self.relay, cr.T_DATA, b'x', dst=a.id, now=1)[0], [])
        self.assertEqual(a.send(self.relay, cr.T_DATA, b'x', dst=7, now=1)[0], [])

    def test_data_from_the_wrong_place_is_dropped(self):
        (a, b), _ = self.room_of(2)
        d = cr.pack(cr.T_DATA, a.room, a.id, cr.DST_ALL, 5, b'\x01x', a.key)
        self.assertEqual(self.relay.handle(d, b.addr, 1), [])
        self.assertEqual(self.relay.handle(d, ('10.9.9.9', 5), 1), [])
        bad = cr.pack(cr.T_DATA, a.room, a.id, cr.DST_ALL, 5, b'\x01x', cr.room_locator('23BCDF' * 3))
        self.assertEqual(self.relay.handle(bad, a.addr, 1), [])
        self.assertEqual(a.send(self.relay, cr.T_DATA, b'\x01x', dst=cr.DST_ALL, now=1, src=b.id)[0], [])
        self.assertEqual(a.send(self.relay, cr.T_DATA, b'\x01x', dst=cr.DST_ALL, now=1, key=None)[0], [])
        self.assertEqual(len(a.send(self.relay, cr.T_DATA, b'\x01x', dst=cr.DST_ALL, now=1)[0]), 1)

    def test_ping_pong(self):
        (a,), _ = self.room_of(1)
        out, _ = a.send(self.relay, cr.T_PING, struct.pack('<I', 0x01020304), now=1)
        (to, pong), = parsed(out)
        self.assertEqual((to, pong.type, pong.dst, pong.payload), (a.addr, cr.T_PONG, a.id, struct.pack('<I', 0x01020304)))
        self.assertTrue(pong.verify(a.key))

    def test_leave_frees_the_id_and_updates_the_others(self):
        (a, b, c), secret = self.room_of(3)
        got = parsed(b.send(self.relay, cr.T_LEAVE, now=1)[0])
        self.assertEqual(sorted(to for to, _ in got), sorted([a.addr, c.addr]))
        self.assertEqual([m[0] for m in cr.parse_roster(got[0][1].payload)], [1, 3])
        self.assertEqual(Client('D', ip='10.4.0.1').join(self.relay, 2, cr.INTENT_JOIN, secret)[0][0], 2)

    # -- the maker and its removals

    def test_only_the_maker_removes_and_the_removed_cannot_come_back_as_itself(self):
        (a, b, c), secret = self.room_of(3)
        self.assertEqual(c.send(self.relay, cr.T_KICK, cr.pack_kick(b.id, b.session), now=1)[0], [])   # not the maker
        self.assertEqual(a.send(self.relay, cr.T_KICK, cr.pack_kick(b.id, b.session ^ 1), now=1)[0], [])   # another session
        got = parsed(a.send(self.relay, cr.T_KICK, cr.pack_kick(b.id, b.session), now=1)[0])
        to_b = [dg for to, dg in got if to == b.addr]
        self.assertEqual(cr.parse_error(to_b[0].payload)[0], cr.E_REMOVED)
        self.assertNotIn(b.id, [m[0] for m in cr.parse_roster([dg for to, dg in got if to == a.addr][0].payload)])
        info, _ = b.join(self.relay, 2, cr.INTENT_JOIN, secret)   # the same client id and address
        self.assertEqual(info[0], cr.E_REMOVED)

    def test_the_maker_mark_follows_the_maker_and_goes_with_it(self):
        (a, b), secret = self.room_of(2)
        roster = cr.parse_roster(parsed(a.send(self.relay, cr.T_LEAVE, now=1)[0])[0][1].payload)
        self.assertEqual([(m[0], m[1]) for m in roster], [(2, 0)])   # nobody is the maker now
        self.assertEqual(b.send(self.relay, cr.T_KICK, cr.pack_kick(2, b.session), now=1)[0], [])

    # -- a stolen client id (F7)

    def test_a_client_id_without_its_token_does_not_take_the_member(self):
        (a, b), secret = self.room_of(2)
        thief = Client('A', cid=a.cid, ip='10.66.0.1', secret=secret)
        info, _ = thief.join(self.relay, 1, cr.INTENT_JOIN)
        self.assertEqual(info[0], 3)                     # a new member, not member 1
        room = self.relay.rooms[a.room]
        self.assertEqual((room.members[1].addr, room.members[1].maker, room.members[3].maker), (a.addr, True, False))
        out, _ = a.send(self.relay, cr.T_PING, b'\0\0\0\0', now=2)
        self.assertEqual(out[0][0], a.addr)               # the maker's traffic still goes to the maker

    def test_a_member_rejoins_with_its_token_from_its_address_without_a_cookie_and_from_another_after_one(self):
        (a, b), secret = self.room_of(2)
        self.relay.tick(cr.SILENT_DROP + 1)
        a.seq += 1
        a.nonce = 9
        out = parsed(self.relay.handle(a.connect(cr.INTENT_JOIN), a.addr, cr.SILENT_DROP + 2))
        mine = [dg for to, dg in out if to == a.addr]
        self.assertEqual(mine[0].type, cr.T_JOINED)        # its own address: the token proves it, no challenge
        self.assertEqual(cr.parse_joined(mine[0].payload)[0], 1)
        # from another address: a challenge first, so a token cannot aim the room's traffic at an address that never answered
        a.addr = ('10.0.0.1', a.addr[1] + 1000)
        a.seq += 1
        a.nonce = 10
        out = parsed(self.relay.handle(a.connect(cr.INTENT_JOIN), a.addr, cr.SILENT_DROP + 3))
        self.assertEqual([dg.type for to, dg in out], [cr.T_CHALLENGE])
        self.assertEqual(self.relay.rooms[a.room].members[1].addr[1], a.addr[1] - 1000)   # not moved yet
        info, _ = a.join(self.relay, cr.SILENT_DROP + 3)
        self.assertEqual(info[0], 1)                        # with the cookie: the same member, at its new address

    def test_a_rejoin_that_changes_the_session_needs_the_cookie_even_from_the_members_address(self):
        (a, b), secret = self.room_of(2)
        # someone on the path saw the member's CONNECT (client id, token, address): a spoofed one with a new session
        a.seq += 1
        a.nonce = 11
        old = a.session
        a.session = random.getrandbits(64)
        out = parsed(self.relay.handle(a.connect(cr.INTENT_JOIN), a.addr, 1))
        self.assertEqual([dg.type for to, dg in out], [cr.T_CHALLENGE])
        self.assertEqual(self.relay.rooms[a.room].members[1].session, old)   # the member's session stays
        info, _ = a.join(self.relay, 2)                     # with the cookie (the member itself rejoining): taken
        self.assertEqual(info[0], 1)
        self.assertEqual(self.relay.rooms[a.room].members[1].session, a.session)

    def test_a_session_used_in_the_room_is_refused(self):
        (a, b), secret = self.room_of(2)
        b.send(self.relay, cr.T_LEAVE, now=1)
        m = Client('M', ip='10.7.0.1')
        m.session = b.session        # the session of a member that left, which the rosters showed
        e, _ = m.join(self.relay, 2, cr.INTENT_JOIN, secret)
        self.assertEqual(len(e), 3, f'a newcomer took a session used in the room before: {e}')   # an ERROR, not JOINED
        m.session = random.getrandbits(64)
        info, _ = m.join(self.relay, 3, cr.INTENT_JOIN, secret)
        self.assertEqual(len(info), 2)                      # with a session of its own it joins

    # -- time

    def test_silence_marks_away_then_drops_and_the_id_is_kept_for_a_rejoin(self):
        (a, b), secret = self.room_of(2)
        self.relay.tick(1)
        self.assertEqual(self.relay.tick(4), [])
        out = parsed(self.relay.tick(cr.SILENT_AWAY + 1))
        self.assertEqual({m[0]: m[1] & cr.FLAG_AWAY for m in cr.parse_roster(out[0][1].payload)}, {1: cr.FLAG_AWAY, 2: cr.FLAG_AWAY})
        a.send(self.relay, cr.T_PING, b'\0\0\0\0', now=cr.SILENT_AWAY + 2)
        out = parsed(self.relay.tick(cr.SILENT_DROP + 1))
        self.assertEqual([m[0] for m in cr.parse_roster(out[0][1].payload)], [1])
        info, rest = b.join(self.relay, 30, cr.INTENT_JOIN)   # its token: its id again
        self.assertEqual(info[0], 2)
        self.assertEqual(Client('C', ip='10.5.0.1').join(self.relay, 31, cr.INTENT_JOIN, secret)[0][0], 3)

    def test_a_dropped_id_is_free_again_after_a_minute(self):
        (a, b), secret = self.room_of(2)
        self.relay.tick(cr.SILENT_DROP + 1)
        self.relay.tick(cr.REJOIN_KEEP + 5)
        self.assertEqual(self.relay.rooms, {} if False else self.relay.rooms)
        info, _ = Client('C', ip='10.5.0.1').join(self.relay, cr.REJOIN_KEEP + 6, cr.INTENT_JOIN, secret)
        self.assertEqual(info[0], 1)

    def test_an_empty_room_closes_after_ten_minutes(self):
        (a, b), secret = self.room_of(2)
        a.send(self.relay, cr.T_LEAVE, now=1)
        b.send(self.relay, cr.T_LEAVE, now=1)
        self.relay.tick(cr.ROOM_EMPTY_TTL - 1)
        self.assertEqual(len(self.relay.rooms), 1)
        self.relay.tick(cr.ROOM_EMPTY_TTL + 2)
        self.assertEqual(self.relay.rooms, {})
        self.assertEqual(Client(ip='10.6.0.1').join(self.relay, cr.ROOM_EMPTY_TTL + 3, cr.INTENT_JOIN, secret)[0][0], cr.E_NO_ROOM)

    def test_a_room_its_maker_left_alone_closes_at_once(self):
        (a,), secret = self.room_of(1)
        a.send(self.relay, cr.T_LEAVE, now=1)
        self.assertEqual(self.relay.rooms, {})
        (c,), _ = self.room_of(1, now=2)
        self.relay.tick(cr.SILENT_DROP + 3)   # dropped, never shared: gone a minute later, not ten
        self.relay.tick(cr.SILENT_DROP + 3 + cr.REJOIN_KEEP + 1)
        self.assertEqual(self.relay.rooms, {})

    def test_the_roster_is_repeated_and_a_change_goes_out_three_times(self):
        (a, b), secret = self.room_of(2)
        self.assertEqual(sorted(to for to, _ in self.relay.tick(cr.ROSTER_EVERY + 1)), sorted([a.addr, b.addr]))
        c = Client('C', ip='10.3.0.1')
        c.join(self.relay, 7, cr.INTENT_JOIN, secret)
        self.assertEqual(self.relay.tick(7.1), [])
        for delay in cr.ROSTER_AGAIN:
            out = parsed(self.relay.tick(7 + delay + 0.01))
            self.assertEqual(sorted(to for to, _ in out), sorted([a.addr, b.addr, c.addr]), delay)

    # -- limits

    def test_a_member_over_its_packet_rate_is_dropped(self):
        relay = cr.Relay(pkt_rate=200, pkt_burst=20)
        (a,), _ = self.room_of(1, relay=relay)
        answered = sum(1 for _ in range(100) if a.send(relay, cr.T_PING, b'\0\0\0\0', now=1)[0])
        self.assertEqual((answered, relay.stats['rate_limited']), (20, 80))

    def test_fan_out_is_charged_to_the_sender(self):
        relay = cr.Relay(byte_rate=10000, byte_burst=20000)
        clients, _ = self.room_of(8, relay=relay)
        big = bytes(1000)
        sent = sum(1 for _ in range(20) if clients[0].send(relay, cr.T_DATA, big, dst=cr.DST_ALL, now=2)[0])
        self.assertLessEqual(sent, 3)   # 7 copies of 1024 bytes each: 20 KB lasts for two or three
        self.assertGreaterEqual(sent, 2)

    def test_the_relay_limits_what_it_sends_in_all(self):
        relay = cr.Relay(egress_rate=20000)
        clients, _ = self.room_of(8, relay=relay)
        copies = 0
        for c in clients:
            for _ in range(5):
                copies += len(c.send(relay, cr.T_DATA, bytes(1000), dst=cr.DST_ALL, now=2)[0])
        self.assertLessEqual(copies * 1024, 20000 + 7 * 1024)
        self.assertGreater(relay.stats['egress_limited'], 0)

    def test_joins_per_network_are_limited(self):
        relay = cr.Relay(join_burst=3, join_rate=1 / 60)
        results = [Client(f'P{i}', ip=f'10.0.0.{i + 1}').join(relay, 0, cr.INTENT_CREATE)[0][0] for i in range(5)]
        self.assertEqual(results, [1, 1, 1, cr.E_RATE, cr.E_RATE])   # one /24
        self.assertEqual(Client('other', ip='10.0.1.1').join(relay, 0, cr.INTENT_CREATE)[0][0], 1)

    def ask(self, relay, c, now, intent=cr.INTENT_JOIN):
        """One question of a session that waits for a room (coop.c): the same attempt's CONNECT, and at a challenge the
        CONNECT with the cookie at once; the answer to it."""
        out = parsed(relay.handle(c.connect(intent, c.cookie), c.addr, now))
        if out and out[0][1].type == cr.T_CHALLENGE:
            c.cookie = cr.parse_challenge(out[0][1].payload)[1]
            out = parsed(relay.handle(c.connect(intent, c.cookie), c.addr, now))
        return [dg for to, dg in out if to == c.addr][0]

    def test_a_missing_room_costs_one_join_and_the_same_attempt_again_costs_nothing(self):
        relay = cr.Relay(join_burst=3, join_rate=1 / 600)
        secret = cr.random_secret()
        b = Client('B', ip='10.5.0.1', secret=secret)
        b.seq, b.nonce, b.cookie = 1, 5, bytes(16)
        for t in range(40):   # the same attempt asked again and again, as a session that waits for the room
            e = cr.parse_error(self.ask(relay, b, t).payload)
            self.assertEqual(e[0], cr.E_NO_ROOM, f'question {t}: {e}')
        self.assertEqual(relay.stats['repeats'], 39)
        # two of the network's three joins are left: another attempt, and the room's maker on the same network
        c = Client('C', ip='10.5.0.2', secret=secret)
        self.assertEqual(c.join(relay, 41, cr.INTENT_JOIN)[0][0], cr.E_NO_ROOM)
        self.assertEqual(Client('A', ip='10.5.0.3', secret=secret).join(relay, 42, cr.INTENT_CREATE)[0][0], 1)

    def test_two_players_behind_one_address_wait_for_a_room_without_running_out_of_joins(self):
        relay = cr.Relay()   # the limits a relay runs with: 20 joins, and one more every 3 s, per network
        secret = cr.random_secret()
        waiting = [Client('B', ip='192.0.2.7', secret=secret), Client('C', ip='192.0.2.7', secret=secret)]
        for c in waiting:
            c.seq, c.nonce, c.cookie = 1, random.getrandbits(64), bytes(16)
        refused = []
        for t in range(0, 600, 10):   # ten minutes, each asking every 10 s (coop.c's pace for a room that is not there)
            for i, c in enumerate(waiting):
                dg = self.ask(relay, c, t + i)
                self.assertEqual(dg.type, cr.T_ERROR)
                refused.append(cr.parse_error(dg.payload)[0])
            relay.tick(t + 5)
        self.assertEqual(set(refused), {cr.E_NO_ROOM}, f'{refused.count(cr.E_RATE)} of {len(refused)} questions refused for the budget')
        a = Client('A', ip='192.0.2.7', secret=secret)   # the maker, behind the same address, makes the room
        self.assertEqual(a.join(relay, 600, cr.INTENT_CREATE)[0][0], 1)
        for i, c in enumerate(waiting):
            self.assertEqual(self.ask(relay, c, 610 + i).type, cr.T_JOINED)

    def test_a_flood_of_challenges_does_not_keep_a_member_from_rejoining(self):
        relay = cr.Relay(challenge_rate=100)
        (a, b), secret = self.room_of(2, relay=relay)
        relay.tick(cr.SILENT_DROP + 1)   # a went silent and was dropped; its id waits for it
        for i in range(2000):   # spoofed CONNECTs from many networks take the relay's challenges for this second
            ip = f'10.{i >> 8 & 255}.{i & 255}.9'
            relay.handle(Client(ip=ip).connect(cr.INTENT_CREATE), (ip, 5), cr.SILENT_DROP + 2)
        a.addr = (a.addr[0], a.addr[1] + 1000)   # back from a new port (a new NAT mapping): a cookie first
        a.seq, a.nonce = a.seq + 1, 12
        out = parsed(relay.handle(a.connect(cr.INTENT_JOIN), a.addr, cr.SILENT_DROP + 2))
        self.assertEqual([dg.type for to, dg in out], [cr.T_CHALLENGE])
        flood = parsed(relay.handle(Client(ip='10.200.0.1').connect(cr.INTENT_CREATE), ('10.200.0.1', 5), cr.SILENT_DROP + 2))
        self.assertEqual(flood, [])   # the flood's own share is spent

    def test_a_flood_of_rejoins_with_a_members_token_takes_neither_its_packets_nor_its_own_rejoin(self):
        """Someone who saw a member's join (its client id and token travel in the clear) sends 250 CONNECTs a second with
        them and no cookie from another network, for ten seconds. The member's own messages still pass, its rejoin in a
        new session from its own network still gets a challenge, and the flood gets a few challenges, not hundreds."""
        (a, b), secret = self.room_of(2)
        flood = Client(ip='10.66.0.9', cid=a.cid, secret=secret)
        flood.token = a.token
        challenged = passed = sent = 0
        for i in range(2500):
            now = 1 + i / 250
            flood.seq, flood.nonce = flood.seq + 1, i
            challenged += len(self.relay.handle(flood.connect(cr.INTENT_JOIN), flood.addr, now))
            if i % 25 == 0 and i >= 2000:   # the member's messages in the last two seconds, ten a second
                out, _ = a.send(self.relay, cr.T_DATA, b'\x01sealed', dst=cr.DST_ALL, now=now)
                sent, passed = sent + 1, passed + any(to == b.addr for to, d in out)
        a.session = random.getrandbits(64)   # the member joins again in a new session, from its own network
        a.seq, a.nonce = a.seq + 1, 77
        out = parsed(self.relay.handle(a.connect(cr.INTENT_JOIN), a.addr, 11))
        self.assertEqual((passed, [dg.type for to, dg in out]), (sent, [cr.T_CHALLENGE]))
        self.assertLessEqual(challenged, 8 + 2 * 11)

    def test_the_rejoins_of_rooms_someone_made_to_flood_with_leave_other_rooms_their_share(self):
        """Someone makes four rooms of eight members and, every half second for four seconds, sends a CONNECT with each of
        the 32 tokens from the member's own network and one from another, in new sessions: 64 at once, as many as each
        member's own challenges allow, against a relay that sets 40 a second aside for rejoins. A member of another room
        who comes back from a new port right after the last of them still gets its challenge: each room has its own share
        of those set aside."""
        relay = cr.Relay(rejoin_challenge_rate=40)
        (a, b), secret = self.room_of(2, relay=relay)
        flooders = []
        for r in range(4):
            maker = Client(f'M{r}', ip=f'10.66.{r}.1')
            maker.join(relay, 0, cr.INTENT_CREATE)
            flooders.append(maker)
            for i in range(1, 8):
                c = Client(f'M{r}{i}', ip=f'10.66.{r}.{i + 1}', secret=maker.secret)
                c.join(relay, 0, cr.INTENT_JOIN)
                flooders.append(c)
        challenged = 0
        for step in range(9):
            now = 1 + step / 2
            for m in flooders:
                for addr in (m.addr, ('10.77.0.9', m.addr[1])):   # its own network, and another
                    m.seq, m.nonce, m.session = m.seq + 1, random.getrandbits(64), random.getrandbits(64)
                    challenged += len(relay.handle(m.connect(cr.INTENT_JOIN), addr, now))
        a.addr = (a.addr[0], a.addr[1] + 1000)   # back from a new port (a new NAT mapping): a cookie first
        a.seq, a.nonce = a.seq + 1, 99
        out = parsed(relay.handle(a.connect(cr.INTENT_JOIN), a.addr, 5))
        self.assertEqual(([dg.type for to, dg in out], challenged >= 9 * 8), ([cr.T_CHALLENGE], True))

    def full_relay(self, members, rooms=1000):
        """A relay with rooms open rooms: ours, with members members, each from its own /24, and rooms - 1 others, each
        with its maker in it, eight to a /24 (the per-network limit). (relay, our members)"""
        relay = cr.Relay()
        a = Client('A', ip='10.0.0.1')
        a.join(relay, 0, cr.INTENT_CREATE)
        ours = [a]
        for i in range(1, members):
            c = Client(f'P{i}', ip=f'10.0.{i}.1')
            c.join(relay, 0, cr.INTENT_JOIN, a.secret)
            ours.append(c)
        for i in range(rooms - 1):
            net = 100 + i // 8
            Client(f'R{i}', ip=f'10.{net >> 8}.{net & 255}.{i % 8 + 1}').join(relay, 0, cr.INTENT_CREATE)
        self.assertEqual(len(relay.rooms), rooms)
        return relay, ours

    def test_a_flood_of_rejoins_with_a_members_token_on_a_full_relay_keeps_neither_its_messages_nor_its_own_rejoin_out(self):
        """The flood of test_a_flood_of_rejoins_with_a_members_token_takes_neither_its_packets_nor_its_own_rejoin (250
        CONNECTs a second with the member's seen token from another network, for ten seconds) on a relay that holds its
        full 1000 rooms: the member's messages pass, and its rejoin in a new session from its own network gets its
        challenge."""
        relay, (a, b) = self.full_relay(2)
        flood = Client(ip='10.66.0.9', cid=a.cid, secret=a.secret)
        flood.token = a.token
        passed = sent = 0
        for i in range(2500):
            now = 1 + i / 250
            flood.seq, flood.nonce = flood.seq + 1, i
            relay.handle(flood.connect(cr.INTENT_JOIN), flood.addr, now)
            if i % 25 == 0 and i >= 2000:
                out, _ = a.send(relay, cr.T_DATA, b'\x01sealed', dst=cr.DST_ALL, now=now)
                sent, passed = sent + 1, passed + any(to == b.addr for to, d in out)
        a.session = random.getrandbits(64)
        a.seq, a.nonce = a.seq + 1, 77
        out = parsed(relay.handle(a.connect(cr.INTENT_JOIN), a.addr, 11))
        self.assertEqual((passed, [dg.type for to, dg in out]), (sent, [cr.T_CHALLENGE]))

    def test_a_paced_flood_with_every_token_of_a_room_keeps_no_members_own_rejoin_out_also_on_a_full_relay(self):
        """Someone who saw the CONNECTs of all eight members of a room sends CONNECTs with each token, in new sessions, from
        another network at the 2 a second each member's own challenges for other networks allow; and one member of the
        room sends CONNECTs with its own token from its own network at the 2 a second its own challenges allow. For a
        minute, on a relay with one room and on one with its full 1000. Member A joins again in a new session from its
        own network as coop.c does (a burst of four CONNECTs 250 ms apart, then waits of 1, 2, 4, 8 and 15 s) and answers
        the first challenge: its first CONNECT gets one."""
        for rooms in (1, 1000):
            relay, ours = self.full_relay(8, rooms)
            a, b = ours[0], ours[1]
            events = []   # (time, what, index): 0 a seen token from another network, 1 b's own from its network, 2 a
            for k, m in enumerate(ours):
                f = Client(ip=f'10.66.{k}.9', cid=m.cid, secret=a.secret)
                f.token = m.token
                ours[k] = (m, f)
                events += [(0.01 + k * 1e-3, 0, k)] * 8 + [(0.5 + k / 16 + j / 2, 0, k) for j in range(120)]
            own = Client(ip=b.addr[0], cid=b.cid, secret=a.secret)   # b's own network, another port
            own.token = b.token
            events += [(0.02, 1, 0)] * 8 + [(0.53 + j / 2, 1, 0) for j in range(120)]
            at, wait = 1.37, 1.0
            while at < 60:
                events += [(at + k * 0.25, 2, 0) for k in range(4)]
                at += 0.75 + wait
                wait = min(wait * 2, 15)
            events.sort()
            a.session = random.getrandbits(64)
            first = None
            for i, (now, what, k) in enumerate(events):
                if what == 0:
                    f = ours[k][1]
                    f.seq, f.nonce, f.session = f.seq + 1, i, random.getrandbits(64)
                    relay.handle(f.connect(cr.INTENT_JOIN), f.addr, now)
                elif what == 1:
                    own.seq, own.nonce, own.session = own.seq + 1, i, random.getrandbits(64)
                    relay.handle(own.connect(cr.INTENT_JOIN), own.addr, now)
                elif first is None:
                    a.seq, a.nonce = a.seq + 1, 1000 + i
                    if [dg.type for to, dg in parsed(relay.handle(a.connect(cr.INTENT_JOIN), a.addr, now))] == [cr.T_CHALLENGE]:
                        first = now
            self.assertEqual((rooms, first), (rooms, 1.37))

    def test_challenges_are_limited_per_network_so_one_cannot_lock_out_the_rest(self):
        relay = cr.Relay(challenge_rate=100, prefix_challenge_rate=10, prefix_challenge_burst=10)
        flood = sum(len(relay.handle(Client(ip=f'10.1.0.{i}').connect(cr.INTENT_CREATE), (f'10.1.0.{i}', 5), 0)) for i in range(200))
        self.assertEqual(flood, 10)   # one /24 gets its share, no more
        other = Client(ip='10.2.0.1')
        other.seq = 1
        self.assertEqual(len(relay.handle(other.connect(cr.INTENT_CREATE), other.addr, 0)), 1)

    def test_ipv6_addresses_of_one_48_share_their_limits(self):
        self.assertEqual(cr.prefix_of(('2001:db8:1:2::1', 5)), cr.prefix_of(('2001:db8:1:2:ffff::9', 6)))
        self.assertEqual(cr.prefix_of(('2001:db8:1:2::1', 5)), cr.prefix_of(('2001:db8:1:ff03::1', 5)))
        self.assertNotEqual(cr.prefix_of(('2001:db8:1:2::1', 5)), cr.prefix_of(('2001:db8:2:2::1', 5)))
        self.assertEqual(cr.prefix_of(('::ffff:10.0.0.7', 5)), cr.prefix_of(('10.0.0.9', 6)))
        relay = cr.Relay(prefix_challenge_rate=5, prefix_challenge_burst=5)
        n = sum(len(relay.handle(Client(ip=f'2001:db8::{i:x}').connect(cr.INTENT_CREATE), (f'2001:db8::{i:x}', 7, 0, 0), 0))
                for i in range(1, 50))
        self.assertEqual(n, 5)

    def test_rooms_per_network_are_limited(self):
        relay = cr.Relay(rooms_per_prefix=3, join_burst=100)
        results = [Client(f'P{i}', ip=f'10.0.0.{i + 1}').join(relay, 0, cr.INTENT_CREATE)[0][0] for i in range(5)]
        self.assertEqual(results, [1, 1, 1, cr.E_RATE, cr.E_RATE])
        self.assertEqual(Client(ip='10.0.1.1').join(relay, 0, cr.INTENT_CREATE)[0][0], 1)

    def test_one_ipv6_48_opens_no_more_rooms_than_one_network(self):
        relay = cr.Relay(rooms_per_prefix=3, join_burst=100)
        results = [Client(f'P{i}', ip=f'2001:db8:7:{i:x}::1').join(relay, 0, cr.INTENT_CREATE)[0][0] for i in range(5)]
        self.assertEqual(results, [1, 1, 1, cr.E_RATE, cr.E_RATE])   # five /64s of one /48

    def test_a_flood_from_many_networks_keeps_the_relay_state_small(self):
        relay = cr.Relay(challenge_rate=1000)
        for i in range(20000):   # spoofed CONNECTs, each from another /24, over two seconds
            ip = f'10.{i >> 8 & 255}.{i & 255}.1'
            c = Client(ip=ip)
            relay.handle(c.connect(cr.INTENT_CREATE), (ip, 5), i / 10000)
        self.assertLessEqual(len(relay.challenge_buckets) + len(relay.join_buckets), 4096)
        relay.tick(2.5)
        other = Client(ip='192.0.2.1')   # and a new player still gets a challenge a moment later
        self.assertEqual(len(relay.handle(other.connect(cr.INTENT_CREATE), other.addr, 4)), 1)

    def test_one_room_cannot_take_the_forwarding_budget_of_another(self):
        relay = cr.Relay(egress_rate=40000, pkt_rate=10000, pkt_burst=10000, byte_rate=1 << 22, byte_burst=1 << 22)
        busy, _ = self.room_of(8, relay=relay)
        q1, q2 = Client('Q1', ip='10.9.0.1'), Client('Q2', ip='10.9.1.1')
        q1.join(relay, 0, cr.INTENT_CREATE)
        q2.join(relay, 0, cr.INTENT_JOIN, q1.secret)
        relay.tick(1)
        got = 0
        for t in range(100):   # one second: the busy room sends small datagrams as fast as it can, the quiet one 200 bytes every 100 ms
            now = 2 + t / 100
            for c in busy:
                for _ in range(50):
                    c.send(relay, cr.T_DATA, b'x', dst=busy[0].id if c is not busy[0] else busy[1].id, now=now)
            if t % 10 == 0:
                got += len(q1.send(relay, cr.T_DATA, bytes(200), dst=q2.id, now=now)[0])
            if t % 25 == 0:
                relay.tick(now)
        self.assertEqual(got, 10)

    def test_idle_rooms_do_not_shrink_the_forwarding_share_of_a_busy_one(self):
        relay = cr.Relay(egress_rate=80000, max_rooms=2000)
        for i in range(300):   # rooms of one member each that send nothing, each from a network of its own
            Client(f'I{i}', ip=f'10.{100 + i // 250}.{i % 250}.1').join(relay, 0, cr.INTENT_CREATE)
        q1, q2 = Client('Q1', ip='10.9.0.1'), Client('Q2', ip='10.9.1.1')
        q1.join(relay, 0, cr.INTENT_CREATE)
        q2.join(relay, 0, cr.INTENT_JOIN, q1.secret)
        relay.tick(1)
        got = 0
        for t in range(100):   # one second of 200-byte datagrams every 10 ms: 20 KB/s, a quarter of the relay's budget
            now = 2 + t / 100
            got += len(q1.send(relay, cr.T_DATA, bytes(200), dst=q2.id, now=now)[0])
            if t % 25 == 0:
                relay.tick(now)
        self.assertGreaterEqual(got, 95, f'{got} of 100 forwarded beside 300 idle rooms')

    def test_rooms_that_forward_to_nobody_do_not_shrink_the_share_of_a_busy_one(self):
        relay = cr.Relay(egress_rate=80000, max_rooms=2000)
        lonely = []
        for i in range(300):   # rooms of one member each that send DATA nobody receives, each from a network of its own
            c = Client(f'L{i}', ip=f'10.{100 + i // 250}.{i % 250}.1')
            c.join(relay, 0, cr.INTENT_CREATE)
            lonely.append(c)
        q1, q2 = Client('Q1', ip='10.9.0.1'), Client('Q2', ip='10.9.1.1')
        q1.join(relay, 0, cr.INTENT_CREATE)
        q2.join(relay, 0, cr.INTENT_JOIN, q1.secret)
        relay.tick(1)
        got = 0
        for t in range(100):   # one second: each lonely room sends every second, the busy one 200 bytes every 10 ms
            now = 2 + t / 100
            for c in lonely[t::100]:
                for _ in range(3):
                    c.send(relay, cr.T_DATA, bytes(100), dst=cr.DST_ALL, now=now)
            got += len(q1.send(relay, cr.T_DATA, bytes(200), dst=q2.id, now=now)[0])
            if t % 25 == 0:
                relay.tick(now)
        self.assertGreaterEqual(got, 95, f'{got} of 100 forwarded beside 300 rooms that send to nobody')

    def test_garbage_is_ignored(self):
        rnd = random.Random(7)
        for _ in range(3000):
            data = bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(0, 80)))
            self.assertEqual(self.relay.handle(data, ('10.0.0.9', 1), 0), [])
        base = Client().connect(cr.INTENT_CREATE)
        for _ in range(3000):   # a CONNECT with bytes changed: never an exception
            d = bytearray(base)
            for _ in range(rnd.randrange(1, 4)):
                d[rnd.randrange(len(d))] = rnd.getrandbits(8)
            self.relay.handle(bytes(d), ('10.0.0.9', 1), 0)
            self.relay.handle(bytes(d[:rnd.randrange(len(d))]), ('10.0.0.9', 1), 0)

    def test_garbage_from_a_member_is_ignored(self):
        """DATA, LEAVE, KICK, PING with random payloads and fields, from a member's own address with the room's tag."""
        (a, b, c), _ = self.room_of(3)
        rnd = random.Random(11)
        for i in range(3000):
            type_ = rnd.choice([cr.T_DATA, cr.T_KICK, cr.T_PING, cr.T_PONG, cr.T_ROSTER, 0x77])
            payload = bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(0, 40)))
            a.send(self.relay, type_, payload, dst=rnd.choice([0, 1, 2, 3, 9, cr.DST_ALL]), now=1 + i / 1000)
        self.assertEqual(sorted(m.id for m in self.relay.rooms[a.room].live()), [1, 2, 3])   # a random KICK hit no session

    def test_log_has_no_secret_locator_or_address(self):
        a = Client('Alice')
        a.join(self.relay, 0, cr.INTENT_CREATE)
        text = '\n'.join(self.log)
        self.assertIn('Alice joined as member 1', text)
        for leak in (a.secret, a.secret[:6], cr.room_locator(a.secret).hex()[:8], '10.0.0.1'):
            self.assertNotIn(leak, text)
        relay = cr.Relay(log=self.log.append, log_addrs=True)
        Client('Bob').join(relay, 0, cr.INTENT_CREATE)
        self.assertIn('10.0.0.1', self.log[-1])


# ---- over real sockets

class UdpTest(unittest.TestCase):
    def setUp(self):
        self.relay = cr.Relay()
        self.thread, self.stop, self.port = cr.run_in_thread(self.relay)
        self.addCleanup(self.thread.join, 2)
        self.addCleanup(self.stop.set)

    def sock(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(2)
        s.bind(('127.0.0.1', 0))
        self.addCleanup(s.close)
        return s

    def exchange(self, s, data):
        s.sendto(data, ('127.0.0.1', self.port))
        return cr.parse(s.recvfrom(2048)[0])

    def handshake(self, s, intent, secret, name):
        loc, cid, sess = cr.room_locator(secret), random.getrandbits(64), random.getrandbits(64)
        ch = self.exchange(s, cr.pack(cr.T_CONNECT, 0, 0, 0, 1, cr.pack_connect(intent, 0, loc, cid, sess, 1, name), loc))
        self.assertEqual(ch.type, cr.T_CHALLENGE)
        cookie = cr.parse_challenge(ch.payload)[1]
        return self.exchange(s, cr.pack(cr.T_CONNECT, 0, 0, 0, 1, cr.pack_connect(intent, 0, loc, cid, sess, 1, name, cookie), loc))

    def test_two_clients_over_udp(self):
        a, b = self.sock(), self.sock()
        secret = cr.random_secret()
        key = cr.room_locator(secret)
        ja = self.handshake(a, cr.INTENT_CREATE, secret, b'A')
        self.assertEqual((ja.type, ja.payload[0]), (cr.T_JOINED, 1))
        jb = self.handshake(b, cr.INTENT_JOIN, secret, b'B')
        self.assertEqual((jb.type, jb.payload[0]), (cr.T_JOINED, 2))
        self.assertEqual(cr.parse(a.recvfrom(2048)[0]).type, cr.T_ROSTER)
        d = cr.pack(cr.T_DATA, ja.room, 1, cr.DST_ALL, 10, b'\x01to everyone', key)
        a.sendto(d, ('127.0.0.1', self.port))
        self.assertEqual(b.recvfrom(2048)[0], d)
        pong = self.exchange(a, cr.pack(cr.T_PING, ja.room, 1, 0, 12, b'\x01\x02\x03\x04', key))
        self.assertEqual((pong.type, pong.payload), (cr.T_PONG, b'\x01\x02\x03\x04'))

    def test_the_command_line_prints_where_it_listens(self):
        p = subprocess.Popen([sys.executable, str(RELAY_PY), '--host', '127.0.0.1', '--port', '0'], stdout=subprocess.PIPE, text=True)
        self.addCleanup(p.stdout.close)
        self.addCleanup(p.wait, 5)
        self.addCleanup(p.kill)
        line = p.stdout.readline()
        self.assertIn('listening on 127.0.0.1:', line)
        port = int(line.split('listening on 127.0.0.1:')[1].split()[0])
        s = self.sock()
        loc = cr.room_locator(cr.random_secret())
        s.sendto(cr.pack(cr.T_CONNECT, 0, 0, 0, 1, cr.pack_connect(cr.INTENT_CREATE, 0, loc, 1, 2, 3, b'A'), loc), ('127.0.0.1', port))
        self.assertEqual(cr.parse(s.recvfrom(2048)[0]).type, cr.T_CHALLENGE)


# ---- the C client: --probe-coop against a relay started here

EXE = os.environ.get('NX2NAVI_EXE')


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Relay_proc:
    """tools/coop_relay.py as a process on a fixed port, so that it can be stopped and started again. Its log (with
    where each network's joins went) is kept across restarts, for a failure's message."""

    def __init__(self, port):
        self.port, self.p, self.log = port, None, []

    def _read(self, out):
        try:
            for line in out:
                self.log.append(line.rstrip())
        except (OSError, ValueError):
            pass   # closed by stop()

    def start(self):
        self.p = subprocess.Popen([sys.executable, str(RELAY_PY), '--host', '127.0.0.1', '--port', str(self.port), '--log-joins', '--log-addrs'],
                                  stdout=subprocess.PIPE, text=True)
        line = self.p.stdout.readline()
        assert 'listening' in line
        self.log.append(line.rstrip())
        self.reader = threading.Thread(target=self._read, args=(self.p.stdout,), daemon=True)
        self.reader.start()

    def stop(self):
        if self.p:
            self.p.kill()
            self.p.wait()
            self.reader.join(5)
            self.p.stdout.close()
            self.p = None
            self.log.append('(the relay stopped)')


@unittest.skipUnless(EXE and Path(EXE).is_file(), 'set NX2NAVI_EXE to a built game executable to run --probe-coop')
class ProbeTest(unittest.TestCase):
    """The probe starts two sessions in one process and joins them through the relay at NX2NAVI_COOP_RELAY. Between
    its phases it asks for a relay restart on its output and waits for a line on its input."""

    def run_probe(self, drop):
        port = free_udp_port()
        relay = Relay_proc(port)
        relay.start()
        self.addCleanup(relay.stop)
        env = dict(os.environ, NX2NAVI_COOP_RELAY=f'127.0.0.1:{port}', NX2NAVI_COOP_DROP=str(drop),
                   NX2NAVI_COOP_VECTORS=str(VECTORS), NX2NAVI_COOP_RESTART='stdin')
        p = subprocess.Popen([EXE, '--probe-coop'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, env=env)
        lines = []
        deadline = time.monotonic() + 240
        for line in p.stdout:
            lines.append(line.rstrip())
            if 'coop-probe: restart the relay' in line:
                relay.stop()
                time.sleep(0.3)
                relay.start()
                p.stdin.write('restarted\n')
                p.stdin.flush()
            if time.monotonic() > deadline:
                p.kill()
                break
        p.stdin.close()
        p.stdout.close()
        code = p.wait()
        text = '\n'.join(lines)
        if os.environ.get('COOP_TEST_VERBOSE'):
            print(text)
        relay_log = "\n--- the relay's log ---\n" + '\n'.join(relay.log[-300:])
        self.assertEqual(code, 0, text + relay_log)
        self.assertIn('coop-probe: passed', text, relay_log)
        self.assertIn('restart', text)

    def test_probe_without_loss(self):
        self.run_probe(0)

    def test_probe_with_30_percent_loss(self):
        self.run_probe(30)


if __name__ == '__main__':
    if '--write-vectors' in sys.argv:
        write_vectors()
        print(f'wrote {VECTORS}')
    else:
        unittest.main()
