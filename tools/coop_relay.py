#!/usr/bin/env python3
"""NX2navi co-op relay: forwards datagrams between the players of a room (up to 8), nothing else. Standard library only.

Usage: python tools/coop_relay.py [--host 0.0.0.0] [--port 24680] [--log-addrs] [--log-joins] [--max-rooms 1000]

The game's co-op layer (runtime/coop.c) talks to it over UDP; any player can run one on a machine the others can reach
(forward the UDP port on a router) and put its address into the game's coop_server setting. docs/coop.md describes the
setup. The relay keeps only who is in which room: it stores nothing on disk, and a restart loses the rooms (the players'
games make them again).

A room is named by a secret the players share among themselves; the relay never sees it. It knows the room by a locator
derived from the secret (16 bytes, BLAKE2b), which also keys the tag of every datagram between a client and the relay.
What the players send each other (DATA) is sealed with another key derived from the secret: the relay forwards it and
can neither read nor change nor make it.

The wire format is runtime/coop_proto.h's; tests/coop_vectors.txt holds byte vectors both sides are checked against.
A datagram is a 24-byte header, little-endian, then a payload:

    0  u16 magic 0x434E        2  u8 version (2; the relay accepts that one)   3  u8 type
    4  u32 room (0 until joined)   8  u8 src member 1..8 (0 = relay)   9  u8 dst member (0xFF: every other member)
    10 u16 payload length      12 u32 seq (the sender's datagram counter)
    16 u8[8] tag = HMAC-SHA256(locator, header[0:16] + payload)[:8]

Joining is a cookie exchange, so a spoofed source address cannot make the relay send anything larger than it received:
a CONNECT is answered with a CHALLENGE holding a cookie of the sender's address and the minute; the CONNECT that carries
the cookie joins. Every answer echoes the CONNECT's nonce. JOINED hands the member a rejoin token: a later CONNECT of the
same client that carries it gets the same member id back (from the member's own address and in its own session without
a cookie, from another address or with another session only with one), one without it is a new member. A session that
was in the room before is refused: the players' games bind a member's signed identity to its session.

Limits: per member (datagrams and bytes, a forwarded datagram counted once for each receiver), per network (/24 for
IPv4, /48 for IPv6: challenges, joins, rooms made; at most MAX_NETWORKS of them are tracked), per room (an equal share
of the bytes the relay forwards among the rooms that forwarded to someone in the last ACTIVE seconds), and for the
whole relay (challenges, with a share set aside for members that rejoin with their token, and bytes sent). A join
attempt costs its network one join, whatever the answer; the same attempt sent again (the same client, address and
CONNECT sequence) while the answer would be the same refusal is answered from memory without a charge, so a player
who waits for a room that is not there yet costs one join per attempt, not one per question.
"""
from __future__ import annotations

import argparse
import hashlib
import hmac
import ipaddress
import secrets
import select
import socket
import struct
import sys
import threading
import time

# ---- the wire format

MAGIC = 0x434E
PROTO = 2
HDR_LEN = 24
MAX_DGRAM = 1200
DST_ALL = 0xFF
ALPHABET = '23456789BCDFGHJKMNPRSTVWXYZ'   # no vowels, no 0/1/O/I/L, no Q or E: 27 symbols
SECRET_LEN = 18                            # 85.6 bits
LOCATOR_LEN = 16
MAX_MEMBERS = 8
NAME_MAX = 24
COOKIE_LEN = 16
CONNECT_MIN = 64   # a datagram's smallest size for a CONNECT: the CHALLENGE answering it is smaller
CONNECT_FIXED = 43
ENV_V1, ENV_HDR, ENV_MAC = 1, 13, 16

T_CONNECT, T_CHALLENGE, T_JOINED, T_ERROR, T_ROSTER, T_PING, T_PONG, T_LEAVE, T_KICK, T_DATA = 1, 2, 3, 5, 6, 7, 8, 9, 11, 0x10
INTENT_CREATE, INTENT_JOIN = 1, 2
E_NO_ROOM, E_ROOM_FULL, E_PROTO, E_RATE, E_BAD_TAG, E_SERVER_FULL, E_ROOM_EXISTS, E_REMOVED, E_SESSION_USED = 1, 2, 3, 4, 5, 6, 7, 8, 9
FLAG_AWAY, FLAG_MAKER = 1, 2

# ---- timing (seconds)
SILENT_AWAY = 8       # a member that sent nothing for this long shows as away in the roster
SILENT_DROP = 15      # and is dropped after this; its member id stays reserved for a rejoin
REJOIN_KEEP = 60      # a dropped member's id is kept this long after its last datagram
ROOM_EMPTY_TTL = 600  # a room with nobody in it is deleted after this; one nobody but its maker ever joined, at REJOIN_KEEP
ROSTER_EVERY = 5      # the roster is sent again this often: a lost update repairs itself
ROSTER_AGAIN = (0.25, 0.75)   # and after a change, again this long after it: three chances against loss, as in coop.c's CONNECT bursts
MAX_NETWORKS = 4096   # networks whose challenge or join budget is tracked at once; a budget that is full again is forgotten
SESSIONS_KEPT = 4096  # sessions a room remembers having seen, so that none is taken twice (the oldest forgotten first)
ACTIVE = 2           # a room that forwarded to someone within this many seconds shares the forwarding budget
FAILED_KEPT = 4096   # join attempts whose refusal is remembered, so that the same attempt again costs nothing
FAILED_TTL = 120     # and for this long (a cookie lasts one to two minutes)
MEMBER_CHALLENGES = (2, 8)   # challenges a second, and at once, for CONNECTs with a member's token: from its own network, and from others


def normalize_secret(text):
    """The canonical 18-character room secret of text (any case, '-' and spaces ignored), or None when it is not one."""
    out = []
    for ch in text:
        if ch in '- ':
            continue
        o = ord(ch)
        if 97 <= o <= 122:
            ch = chr(o - 32)
        out.append(ch)
    s = ''.join(out)
    return s if len(s) == SECRET_LEN and all(c in ALPHABET for c in s) else None


def format_secret(s):
    return '-'.join(s[i:i + 6] for i in range(0, SECRET_LEN, 6))


def random_secret():
    return ''.join(secrets.choice(ALPHABET) for _ in range(SECRET_LEN))


def room_locator(secret):
    """The relay's name of the room, and the key of the tags: BLAKE2b-128 keyed with the secret."""
    return hashlib.blake2b(b'nx2navi-coop/2 room', key=secret.encode('ascii'), digest_size=LOCATOR_LEN).digest()


def message_key(secret):
    """The key that seals the players' messages (the relay never computes it; the tests and vectors do)."""
    return hashlib.blake2b(b'nx2navi-coop/2 messages', key=secret.encode('ascii'), digest_size=32).digest()


def make_tag(key, hdr16, payload):
    return hmac.new(key, hdr16 + payload, hashlib.sha256).digest()[:8]


class Dgram:
    """A parsed datagram: the header fields, its first 16 bytes (what the tag covers) and the payload."""
    __slots__ = ('ver', 'type', 'room', 'src', 'dst', 'seq', 'tag', 'payload', 'hdr16')

    def __init__(self, ver, type_, room, src, dst, seq, tag, payload, hdr16):
        self.ver, self.type, self.room, self.src, self.dst = ver, type_, room, src, dst
        self.seq, self.tag, self.payload, self.hdr16 = seq, tag, payload, hdr16

    def verify(self, key):
        return hmac.compare_digest(make_tag(key, self.hdr16, self.payload), self.tag)


def pack(type_, room, src, dst, seq, payload=b'', key=None, ver=PROTO):
    """One datagram. key None: the tag is zero (the answer to a client of another protocol version)."""
    hdr = struct.pack('<HBBIBBHI', MAGIC, ver, type_, room, src, dst, len(payload), seq & 0xFFFFFFFF)
    return hdr + (make_tag(key, hdr, payload) if key else bytes(8)) + payload


def parse(data):
    """The Dgram in data, or None when it is not one: wrong size, magic, or a length that does not match."""
    if len(data) < HDR_LEN or len(data) > MAX_DGRAM:
        return None
    magic, ver, type_, room, src, dst, length, seq = struct.unpack_from('<HBBIBBHI', data)
    if magic != MAGIC or length != len(data) - HDR_LEN:
        return None
    return Dgram(ver, type_, room, src, dst, seq, data[16:24], data[HDR_LEN:], data[:16])


def control_char(c):
    """A character a log or a console may act on: C0, DEL, C1 and the bidi controls (as coop_proto.c's)."""
    o = ord(c)
    return o < 32 or 127 <= o < 160 or o in (0x061C, 0x200E, 0x200F) or 0x202A <= o <= 0x202E or 0x2066 <= o <= 0x2069


def clean_name(raw):
    """A display name from the bytes a client sent: at most NAME_MAX bytes of valid UTF-8 without control characters
    (each of those, and each invalid byte, becomes '?')."""
    text = raw.decode('utf-8', errors='replace')
    text = ''.join('?' if c == '\ufffd' or control_char(c) else c for c in text)
    out = b''
    for c in text:
        enc = c.encode('utf-8')
        if len(out) + len(enc) > NAME_MAX:
            break
        out += enc
    return out


def pack_roster(members):
    """members: (id, flags, session, name bytes) in id order."""
    out = bytes([len(members)])
    for mid, flags, session, name in members:
        out += struct.pack('<BBQB', mid, flags, session, len(name)) + name
    return out


def parse_roster(payload, offset=0):
    """((id, flags, session, name bytes), ...) from payload[offset:], or None when it is cut short or malformed."""
    if len(payload) <= offset:
        return None
    n, at, out = payload[offset], offset + 1, []
    if n > MAX_MEMBERS:
        return None
    for _ in range(n):
        if at + 11 > len(payload):
            return None
        mid, flags, session, nlen = struct.unpack_from('<BBQB', payload, at)
        at += 11
        if nlen > NAME_MAX or at + nlen > len(payload):
            return None
        out.append((mid, flags, session, payload[at:at + nlen]))
        at += nlen
    return tuple(out)


def pack_connect(intent, flags, locator, client_id, session, nonce, name, cookie=bytes(COOKIE_LEN), token=bytes(COOKIE_LEN)):
    """The CONNECT payload, zero-padded so that the datagram is CONNECT_MIN bytes at least."""
    body = struct.pack('<BB16sQQQB', intent, flags, locator, client_id, session, nonce, len(name)) + name + cookie + token
    return body + bytes(max(0, CONNECT_MIN - HDR_LEN - len(body)))


def parse_connect(payload):
    """(intent, flags, locator, client_id, session, nonce, name bytes, cookie, token) or None."""
    if len(payload) < CONNECT_FIXED:
        return None
    intent, flags, locator, cid, session, nonce, nlen = struct.unpack_from('<BB16sQQQB', payload)
    if nlen > NAME_MAX or CONNECT_FIXED + nlen + 2 * COOKIE_LEN > len(payload):
        return None
    at = CONNECT_FIXED + nlen
    return (intent, flags, locator, cid, session, nonce, payload[CONNECT_FIXED:at], payload[at:at + COOKIE_LEN],
            payload[at + COOKIE_LEN:at + 2 * COOKIE_LEN])


def pack_challenge(nonce, cookie):
    return struct.pack('<Q', nonce) + cookie


def parse_challenge(payload):
    if len(payload) != 8 + COOKIE_LEN:
        return None
    return struct.unpack_from('<Q', payload)[0], payload[8:]


def pack_joined(member, nonce, token, roster):
    return struct.pack('<BQ', member, nonce) + token + roster


def parse_joined(payload):
    """(member, nonce, token, roster) or None."""
    if len(payload) < 9 + COOKIE_LEN:
        return None
    member, nonce = struct.unpack_from('<BQ', payload)
    roster = parse_roster(payload, 9 + COOKIE_LEN)
    return None if roster is None else (member, nonce, payload[9:9 + COOKIE_LEN], roster)


def pack_error(reason, nonce, text):
    t = text.encode('utf-8')[:60]
    return struct.pack('<BQB', reason, nonce, len(t)) + t


def parse_error(payload):
    """(reason, nonce, text) or None."""
    if len(payload) < 10 or 10 + payload[9] > len(payload):
        return None
    reason, nonce, tlen = struct.unpack_from('<BQB', payload)
    return reason, nonce, payload[10:10 + tlen].decode('utf-8', errors='replace')


def pack_kick(member, session):
    return struct.pack('<BQ', member, session)


def parse_kick(payload):
    return struct.unpack('<BQ', payload) if len(payload) == 9 else None


def prefix_of(addr):
    """The network an address belongs to for the limits: its /24 for IPv4 (and IPv4 in IPv6), its /48 for IPv6 (the
    smallest block a provider usually hands one customer, who may hold every /64 in it)."""
    try:
        ip = ipaddress.ip_address(addr[0].split('%')[0])
    except ValueError:
        return addr[0]
    if ip.version == 6 and ip.ipv4_mapped:
        ip = ip.ipv4_mapped
    bits = 24 if ip.version == 4 else 48
    return str(ipaddress.ip_network(f'{ip}/{bits}', strict=False))


# ---- the relay

class Bucket:
    """A token bucket: rate tokens per second up to burst."""
    __slots__ = ('rate', 'burst', 'tokens', 'stamp')

    def __init__(self, rate, burst, now):
        self.rate, self.burst, self.tokens, self.stamp = rate, burst, burst, now

    def refill(self, now):
        self.tokens = min(self.burst, self.tokens + max(0.0, now - self.stamp) * self.rate)
        self.stamp = max(self.stamp, now)

    def take(self, n, now):
        self.refill(now)
        if self.tokens < n:
            return False
        self.tokens -= n
        return True

    def full(self, now):
        """Refilled to its burst: the same as a new bucket, so it need not be kept."""
        self.refill(now)
        return self.tokens >= self.burst


class Member:
    __slots__ = ('id', 'cid', 'session', 'name', 'addr', 'last_seen', 'away', 'live', 'connect_seq', 'pkts', 'bytes',
                 'token', 'maker', 'home_challenges', 'away_challenges', 'home_rejoins')

    def __init__(self, mid, cid, session, name, addr, now, connect_seq, pkts, bytes_, maker):
        self.id, self.cid, self.session, self.name, self.addr = mid, cid, session, name, addr
        self.last_seen, self.away, self.live, self.connect_seq = now, False, True, connect_seq
        self.pkts, self.bytes = pkts, bytes_
        self.token = secrets.token_bytes(COOKIE_LEN)
        self.maker = maker
        # its own challenges, apart from its packets: anyone who saw its join has its token, and a flood of CONNECTs with
        # it from another network takes neither the member's messages nor the challenge of its own rejoin from its network
        self.home_challenges, self.away_challenges = Bucket(*MEMBER_CHALLENGES, now), Bucket(*MEMBER_CHALLENGES, now)
        self.home_rejoins = None   # its share of the challenges set aside for rejoins from its own network (Relay.take_rejoin)


class Room:
    __slots__ = ('id', 'locator', 'members', 'empty_since', 'next_roster', 'again', 'banned', 'prefix', 'shared', 'egress',
                 'sessions', 'forwarded_at', 'rejoins')

    def __init__(self, rid, locator, prefix, now):
        self.id, self.locator, self.prefix = rid, locator, prefix
        self.members = {}   # member id -> Member, a dropped one (live False) keeps its id for a rejoin
        self.empty_since = now
        self.next_roster = now + ROSTER_EVERY
        self.again = []     # when the roster goes out once more after a change
        self.banned = set()  # client ids and addresses the maker removed
        self.shared = False  # someone besides its maker joined it once
        self.egress = None   # its share of the bytes the relay forwards (Relay.share_egress)
        self.sessions = {}   # sessions its members used (insertion order: the oldest first), never taken again
        self.forwarded_at = None   # when it last forwarded: a room that did recently shares the budget
        self.rejoins = None  # its share of the challenges set aside for its members' rejoins from other networks (Relay.take_rejoin)

    def use_session(self, session):
        self.sessions[session] = True
        if len(self.sessions) > SESSIONS_KEPT:
            del self.sessions[next(iter(self.sessions))]

    @property
    def key(self):
        return self.locator

    @property
    def label(self):
        return f'{self.id:08x}'

    def live(self):
        return [m for m in self.members.values() if m.live]


class Relay:
    """The relay's state machine. handle() takes one received datagram and tick() the passage of time; both return the
    datagrams to send as (address, bytes). now is a monotonic clock in seconds that the caller supplies, so a test
    can run the timeouts without waiting. An address is the tuple socket.recvfrom gives."""

    def __init__(self, max_rooms=1000, log=None, log_addrs=False, secret=None,
                 pkt_rate=200, pkt_burst=400, byte_rate=65536, byte_burst=131072, join_rate=1 / 3, join_burst=20,
                 challenge_rate=5000, prefix_challenge_rate=20, prefix_challenge_burst=40, rooms_per_prefix=8,
                 egress_rate=8 << 20, rejoin_challenge_rate=1000, log_joins=False):
        self.max_rooms = max_rooms
        self.log_fn = log
        self.log_addrs = log_addrs
        self.log_joins = log_joins
        self.secret = secret or secrets.token_bytes(32)
        self.limits = (pkt_rate, pkt_burst, byte_rate, byte_burst)
        self.join_limit = (join_rate, join_burst)
        self.challenge_rate = challenge_rate
        self.rejoin_challenge_rate = rejoin_challenge_rate
        self.prefix_challenge = (prefix_challenge_rate, prefix_challenge_burst)
        self.rooms_per_prefix = rooms_per_prefix
        self.egress_rate = egress_rate
        self.rooms = {}        # room id -> Room
        self.locators = {}     # locator -> Room
        self.by_addr = {}      # address -> (Room, Member)
        self.join_buckets = {}       # network prefix -> Bucket
        self.challenge_buckets = {}  # network prefix -> Bucket
        self.prefix_rooms = {}       # network prefix -> rooms it made that are open
        self.failed = {}       # (client id, address, CONNECT seq, locator) -> (reason, when): refusals of an attempt (oldest first)
        self.challenges = None
        self.next_prune = 0
        self.egress = None
        self.rate_said = -10.0   # when the log last told of a join refused for the network's budget
        self.seq = 0
        self.stats = {k: 0 for k in ('rx', 'malformed', 'bad_tag', 'no_member', 'wrong_addr', 'rate_limited',
                                     'challenges', 'joins', 'rejoins', 'leaves', 'drops', 'forwarded', 'rooms_made',
                                     'kicks', 'egress_limited', 'repeats', 'join_charges')}
    # -- helpers

    def log(self, text):
        if self.log_fn:
            self.log_fn(text)

    def where(self, addr):
        return f' ({addr[0]}:{addr[1]})' if self.log_addrs else ''

    def next_seq(self):
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        return self.seq

    def cookie(self, addr, minute):
        return hmac.new(self.secret, f'{addr[0]}|{addr[1]}|{minute}'.encode(), hashlib.sha256).digest()[:COOKIE_LEN]

    def cookie_ok(self, addr, cookie, now):
        minute = int(now // 60)
        return any(hmac.compare_digest(cookie, self.cookie(addr, m)) for m in (minute, minute - 1))

    def to_member(self, room, member, type_, payload=b''):
        return member.addr, pack(type_, room.id, 0, member.id, self.next_seq(), payload, room.key)

    def error(self, addr, reason, nonce, text, key):
        return [(addr, pack(T_ERROR, 0, 0, 0, self.next_seq(), pack_error(reason, nonce, text), key))]

    def roster_payload(self, room):
        return pack_roster([(m.id, (FLAG_AWAY if m.away else 0) | (FLAG_MAKER if m.maker else 0), m.session, m.name)
                            for m in sorted(room.live(), key=lambda m: m.id)])

    def roster_to_all(self, room, skip=None, changed_at=None):
        """The roster to every live member but skip. changed_at (the time): the roster changed, so it goes out again soon
        (ROSTER_AGAIN) for a member that lost this one."""
        if changed_at is not None:
            room.again = [changed_at + t for t in ROSTER_AGAIN]
        payload = self.roster_payload(room)
        return [self.to_member(room, m, T_ROSTER, payload) for m in room.live() if m is not skip]

    def joined(self, room, member, nonce):
        payload = pack_joined(member.id, nonce, member.token, self.roster_payload(room))
        return [self.to_member(room, member, T_JOINED, payload)]

    def new_member_limits(self, now):
        pkt_rate, pkt_burst, byte_rate, byte_burst = self.limits
        return Bucket(pkt_rate, pkt_burst, now), Bucket(byte_rate, byte_burst, now)

    def forget(self, room, member):
        """Takes member out of the address index; the caller drops it from the room."""
        if self.by_addr.get(member.addr[:2], (None, None))[1] is member:
            del self.by_addr[member.addr[:2]]

    def prune(self, now):
        """Forgets the networks whose budgets are full again (as good as new): the tables hold only networks that sent
        something in the last seconds. And the refusals older than FAILED_TTL."""
        for table in (self.join_buckets, self.challenge_buckets):
            for k in [k for k, b in table.items() if b.full(now)]:
                del table[k]
        for k in [k for k, (_, when) in self.failed.items() if now - when >= FAILED_TTL]:
            del self.failed[k]

    def note_join(self, net, bucket, what):
        """With --log-joins, where a network's join budget went (the network itself only with --log-addrs)."""
        if self.log_joins:
            who = net if self.log_addrs else 'a network'
            self.log(f'joins of {who}: {what}, {bucket.tokens:.1f} left' if bucket else f'joins of {who}: {what}')

    def bucket(self, table, key, rate, burst, now):
        """The budget of network key, or None when MAX_NETWORKS networks are tracked and none of them is done."""
        b = table.get(key)
        if b is None:
            if len(self.join_buckets) + len(self.challenge_buckets) >= MAX_NETWORKS:
                self.prune(now)
                if len(self.join_buckets) + len(self.challenge_buckets) >= MAX_NETWORKS:
                    return None
            b = table[key] = Bucket(rate, burst, now)
        return b

    def take_challenge(self, addr, now, member=None, room=None):
        """A challenge (or an answer to another protocol version) for addr: the relay's budget first (a flood it refuses
        leaves no state behind), then its network's. A CONNECT that carries a member's valid token (a rejoin) takes from
        that member's own challenges for its network or for the others (MEMBER_CHALLENGES), never from its packets, and
        then from the share set aside for rejoins (take_rejoin), which a flood without a token never reaches."""
        if member is not None:
            home = prefix_of(addr) == prefix_of(member.addr)
            mine = member.home_challenges if home else member.away_challenges
            return mine.take(1, now) and self.take_rejoin(room, member, home, now)
        if self.challenges is None:
            self.challenges = Bucket(self.challenge_rate, self.challenge_rate, now)
        self.challenges.refill(now)
        if self.challenges.tokens < 1:
            return False
        mine = self.bucket(self.challenge_buckets, prefix_of(addr), *self.prefix_challenge, now)
        return mine is not None and mine.take(1, now) and self.challenges.take(1, now)

    def take_rejoin(self, room, member, home, now):
        """A challenge from the share set aside for rejoins: rejoin_challenge_rate a second split equally among the open
        rooms, for CONNECTs from other networks than the member's (a member that moves, or someone with a token it saw),
        and as much again, split among the rooms and then among the eight member ids of each, for CONNECTs from the
        member's own network. Each holds MAX_MEMBERS at once at least. So the tokens of rooms someone made to flood with
        take only those rooms' shares, and nobody without the member's own network (a stranger with a token it saw, or
        another member of its room) takes the share of the member's own rejoin."""
        share = self.rejoin_challenge_rate / max(1, len(self.rooms))
        if home:
            share /= MAX_MEMBERS
        burst = max(share, MAX_MEMBERS)
        b = member.home_rejoins if home else room.rejoins
        if b is None:
            b = Bucket(share, burst, now)
            if home:
                member.home_rejoins = b
            else:
                room.rejoins = b
        else:
            b.refill(now)
            b.rate, b.burst = share, burst
            b.tokens = min(b.tokens, burst)
        return b.take(1, now)

    def share_egress(self, now):
        """Each room's share of the forwarding budget: equal among the rooms that forwarded to someone in the last ACTIVE
        seconds, so a busy room cannot take what another needs and rooms that send nothing (or to nobody) take nothing."""
        active = sum(1 for r in self.rooms.values() if r.forwarded_at is not None and now - r.forwarded_at < ACTIVE)
        share = self.egress_rate / max(1, active)
        for room in self.rooms.values():
            if room.egress is None:
                room.egress = Bucket(share, share, now)
            else:
                room.egress.refill(now)
                room.egress.rate, room.egress.burst = share, share
                room.egress.tokens = min(room.egress.tokens, share)

    def take_egress(self, room, n, now):
        if self.egress is None:
            self.egress = Bucket(self.egress_rate, self.egress_rate, now)
        idle = room.forwarded_at is None or now - room.forwarded_at >= ACTIVE
        room.forwarded_at = now
        if room.egress is None or idle:   # it counts among the rooms that share from now on
            self.share_egress(now)
        return room.egress.take(n, now) and self.egress.take(n, now)

    def delete_room(self, room, why):
        self.rooms.pop(room.id, None)
        self.locators.pop(room.locator, None)
        left = self.prefix_rooms.get(room.prefix, 1) - 1
        if left > 0:
            self.prefix_rooms[room.prefix] = left
        else:
            self.prefix_rooms.pop(room.prefix, None)
        for m in list(room.members.values()):
            self.forget(room, m)
        self.log(f'room {room.label} closed, {why} ({len(self.rooms)} rooms)')

    # -- datagrams

    def handle(self, data, addr, now):
        self.stats['rx'] += 1
        dg = parse(data)
        if dg is None:
            self.stats['malformed'] += 1
            return []
        if dg.ver != PROTO:
            # a client of another protocol major: tell it so, if the answer is no larger than what it sent (a spoofed
            # source gains nothing) and the network's challenge budget allows
            out = self.error(addr, E_PROTO, 0, f'relay speaks protocol {PROTO}', None)
            return out if len(out[0][1]) <= len(data) and self.take_challenge(addr, now) else []
        if dg.type == T_CONNECT:
            return self.connect(dg, data, addr, now)
        room = self.rooms.get(dg.room)
        member = room.members.get(dg.src) if room else None
        if member is None or not member.live and dg.type != T_LEAVE:
            self.stats['no_member'] += 1
            return []
        if member.addr != addr:
            self.stats['wrong_addr'] += 1
            return []
        if not (member.pkts.take(1, now) and member.bytes.take(len(data), now)):
            self.stats['rate_limited'] += 1
            return []
        if not dg.verify(room.key):
            self.stats['bad_tag'] += 1
            return []
        out = []
        if not member.live:   # dropped, its id reserved: a LEAVE gives the id up, anything else is ignored
            del room.members[member.id]
            return out
        member.last_seen = now
        if member.away:
            member.away = False
            out += self.roster_to_all(room, changed_at=now)
        if dg.type == T_PING and len(dg.payload) >= 4:
            out.append(self.to_member(room, member, T_PONG, dg.payload[:4]))
        elif dg.type == T_LEAVE:
            out += self.leave(room, member, now, 'left')
        elif dg.type == T_KICK:
            out += self.kick(room, member, dg, now)
        elif dg.type == T_DATA:
            out += self.forward(room, member, dg, data, now)
        return out

    def forward(self, room, member, dg, data, now):
        if dg.dst == DST_ALL:
            targets = [m for m in room.live() if m is not member]
        else:
            target = room.members.get(dg.dst)
            targets = [target] if target and target.live and target is not member else []
        # each copy counts against the sender (its byte budget already paid one) and against the relay's
        if not targets:   # to nobody: nothing to forward, and the room does not count among those that share the budget
            return []
        extra = len(data) * max(0, len(targets) - 1)
        if extra and not member.bytes.take(extra, now):
            self.stats['rate_limited'] += 1
            return []
        if not self.take_egress(room, len(data) * len(targets), now):
            self.stats['egress_limited'] += 1
            return []
        self.stats['forwarded'] += len(targets)
        return [(m.addr, data) for m in targets]

    def kick(self, room, member, dg, now):
        k = parse_kick(dg.payload)
        if k is None or not member.maker:
            return []
        target = room.members.get(k[0])
        if target is None or target is member or target.session != k[1]:
            return []   # an earlier session of that id, or nobody: a KICK sent again later removes no one else
        room.banned.update((target.cid, target.addr))
        self.stats['kicks'] += 1
        out = []
        if target.live:
            out.append((target.addr, pack(T_ERROR, room.id, 0, target.id, self.next_seq(),
                                          pack_error(E_REMOVED, 0, 'removed by the room\'s maker'), room.key)))
        return out + self.leave(room, target, now, 'was removed by the maker')

    def connect(self, dg, data, addr, now):
        c = parse_connect(dg.payload)
        if c is None:
            self.stats['malformed'] += 1
            return []
        intent, _flags, locator, cid, session, nonce, raw_name, cookie, token = c
        if intent not in (INTENT_CREATE, INTENT_JOIN):
            self.stats['malformed'] += 1
            return []
        key = locator   # the tag proves the sender knows the locator, which an observer of a CONNECT does too
        room = self.locators.get(locator)
        rejoin = None
        if room is not None and any(token):   # a member of the room again: its token is its proof
            rejoin = next((m for m in room.members.values() if m.cid == cid and hmac.compare_digest(m.token, token)), None)
        # no cookie needed only from the member's own address and in its own session: from another address the token
        # would aim the room's traffic at an address that never showed it receives there, and with another session
        # someone who saw the member's CONNECT on the way would take its place
        if (rejoin is None or rejoin.addr != addr or rejoin.session != session) and not self.cookie_ok(addr, cookie, now):
            # not yet proven to receive at its address: only a challenge, and only if that is no larger than the request
            if len(data) < CONNECT_MIN or not self.take_challenge(addr, now, rejoin, room):
                return []
            self.stats['challenges'] += 1
            return [(addr, pack(T_CHALLENGE, 0, 0, 0, self.next_seq(), pack_challenge(nonce, self.cookie(addr, int(now // 60))), key))]
        if not dg.verify(key):
            self.stats['bad_tag'] += 1
            return self.error(addr, E_BAD_TAG, nonce, 'bad tag', key)
        known = self.by_addr.get(addr[:2])
        if known and known[1].cid == cid and known[1].connect_seq == dg.seq and known[1].live and known[0] is room:
            known[1].last_seen = now   # a CONNECT sent again because its answer was lost: the answer again
            return self.joined(known[0], known[1], nonce)
        net = prefix_of(addr)
        refusal = self.refusal(intent, room, rejoin, cid, addr, session, net)
        attempt = (cid, addr[:2], dg.seq, locator)
        if refusal is not None and self.failed.get(attempt, (None,))[0] == refusal[0]:
            # the same attempt again (an answer was lost, or it asks again for a room that is not there yet): the same
            # answer, without a charge, so that waiting does not use up the network's joins
            self.stats['repeats'] += 1
            return self.error(addr, refusal[0], nonce, refusal[1], key)
        bucket = self.bucket(self.join_buckets, net, *self.join_limit, now)
        if bucket is None or not bucket.take(1, now):
            self.stats['rate_limited'] += 1
            if self.log_joins or now - self.rate_said >= 10:   # a line every 10 s at most: a flood does not fill the log
                self.rate_said = now
                self.log(f'a join refused: too many joins from {net if self.log_addrs else "one network"} ({self.stats["rate_limited"]} so far)')
            return self.error(addr, E_RATE, nonce, 'too many joins, try again later', key)
        self.stats['join_charges'] += 1
        if refusal is not None:
            self.failed.pop(attempt, None)
            self.failed[attempt] = (refusal[0], now)
            if len(self.failed) > FAILED_KEPT:
                del self.failed[next(iter(self.failed))]
            self.note_join(net, bucket, f'charged 1 for a refused attempt ({refusal[1]})')
            return self.error(addr, refusal[0], nonce, refusal[1], key)
        self.note_join(net, bucket, 'charged 1 for a ' + ('rejoin' if rejoin else 'join' if room else 'new room'))
        name = clean_name(raw_name) or b'Player'
        out = []
        if room is None:
            room = self.make_room(locator, net, now)
        room.use_session(session)
        if known and known[0] is not room:
            out += self.leave(known[0], known[1], now, 'moved to another room')   # same address, new room: it left the old
        member = rejoin
        if member is None:
            mid = next((i for i in range(1, MAX_MEMBERS + 1) if i not in room.members), None)
            if mid is None:   # every id is taken: a dropped member's id goes to the newcomer, the longest gone first
                gone = sorted((m for m in room.members.values() if not m.live), key=lambda m: m.last_seen)
                if not gone:
                    return out + self.error(addr, E_ROOM_FULL, nonce, 'room full', key)
                mid = gone[0].id
                del room.members[mid]
            pkts, byts = self.new_member_limits(now)
            member = Member(mid, cid, session, name, addr, now, dg.seq, pkts, byts, maker=not room.members and intent == INTENT_CREATE)
            room.members[mid] = member
            if not member.maker:
                room.shared = True
            self.stats['joins'] += 1
            self.log(f'room {room.label}: {name.decode()} joined as member {mid} ({len(room.live())}/{MAX_MEMBERS}){self.where(addr)}')
        else:   # the same member again (after the relay was silent, from a new address, under a new name or session)
            if member.addr != addr:
                self.forget(room, member)
            if not member.live:
                self.log(f'room {room.label}: {name.decode()} rejoined as member {member.id}{self.where(addr)}')
                self.stats['rejoins'] += 1
            member.addr, member.name, member.connect_seq, member.session = addr, name, dg.seq, session
            member.last_seen, member.away, member.live = now, False, True
        old = self.by_addr.get(addr[:2])
        if old and old[1] is not member:   # another member of this room on the same address: that one is gone
            self.forget(old[0], old[1])
            old[0].members.pop(old[1].id, None)
        self.by_addr[addr[:2]] = (room, member)
        room.empty_since = None
        return out + self.joined(room, member, nonce) + self.roster_to_all(room, skip=member, changed_at=now)

    def refusal(self, intent, room, rejoin, cid, addr, session, net):
        """(reason, text) when this attempt is refused, None when it joins or makes the room; nothing is charged here."""
        if intent == INTENT_JOIN and room is None:
            return E_NO_ROOM, 'no such room'
        if intent == INTENT_CREATE and room is not None and rejoin is None:
            return E_ROOM_EXISTS, 'a room with this secret is open'
        if room is None:
            if len(self.rooms) >= self.max_rooms:
                return E_SERVER_FULL, 'server full'
            if self.prefix_rooms.get(net, 0) >= self.rooms_per_prefix:
                return E_RATE, 'too many rooms from this network'
            return None
        if cid in room.banned or addr in room.banned:
            return E_REMOVED, 'removed by the room\'s maker'
        if session in room.sessions and (rejoin is None or session != rejoin.session):
            # a session is the member's identity to the others (their games bind its signature to it): one that was in
            # the room before is someone else's, replayed
            return E_SESSION_USED, 'a session used in this room before'
        return None

    def make_room(self, locator, prefix, now):
        rid = 0
        while not rid or rid in self.rooms:
            rid = secrets.randbits(32)
        room = Room(rid, locator, prefix, now)
        self.rooms[rid] = room
        self.locators[locator] = room
        self.prefix_rooms[prefix] = self.prefix_rooms.get(prefix, 0) + 1
        self.share_egress(now)
        self.stats['rooms_made'] += 1
        self.log(f'room {room.label} created ({len(self.rooms)} rooms)')
        return room

    def leave(self, room, member, now, why):
        self.forget(room, member)
        room.members.pop(member.id, None)
        self.stats['leaves'] += 1
        self.log(f'room {room.label}: {member.name.decode()} (member {member.id}) {why} ({len(room.live())}/{MAX_MEMBERS})')
        if not room.live():
            room.empty_since = now
            if not room.shared:   # nobody but its maker was ever in it: nothing to keep it for
                self.delete_room(room, 'its maker left before anyone joined')
                return []
        return self.roster_to_all(room, changed_at=now)

    # -- time

    def tick(self, now):
        out = []
        for room in list(self.rooms.values()):
            changed = False
            for m in list(room.members.values()):
                silent = now - m.last_seen
                if m.live and silent >= SILENT_DROP:
                    m.live = False
                    self.forget(room, m)
                    self.stats['drops'] += 1
                    self.log(f'room {room.label}: {m.name.decode()} (member {m.id}) dropped, silent {SILENT_DROP} s '
                             f'({len(room.live())}/{MAX_MEMBERS})')
                    changed = True
                elif m.live and not m.away and silent >= SILENT_AWAY:
                    m.away = True
                    changed = True
                elif not m.live and silent >= REJOIN_KEEP:
                    del room.members[m.id]
            if not room.live() and room.empty_since is None:
                room.empty_since = now
            ttl = ROOM_EMPTY_TTL if room.shared else REJOIN_KEEP
            if room.empty_since is not None and now - room.empty_since >= ttl:
                self.delete_room(room, f'empty for {ttl} s')
                continue
            again = any(t <= now for t in room.again)
            if again:
                room.again = [t for t in room.again if t > now]
            if changed or again or now >= room.next_roster:
                out += self.roster_to_all(room, changed_at=now if changed else None)
                room.next_roster = now + ROSTER_EVERY
        if now >= self.next_prune:
            self.next_prune = now + 1
            self.prune(now)
            self.share_egress(now)
        return out


# ---- the socket loop

def open_socket(host, port):
    family, _, _, _, sockaddr = socket.getaddrinfo(host, port, type=socket.SOCK_DGRAM, flags=socket.AI_PASSIVE)[0]
    s = socket.socket(family, socket.SOCK_DGRAM)
    if family == socket.AF_INET6:
        s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    s.bind(sockaddr)
    s.setblocking(False)
    return s


def serve(relay, sock, stop=None, clock=time.monotonic):
    """Runs relay on sock until stop (a threading.Event) is set."""

    def send_all(datagrams):
        for to, out in datagrams:
            try:
                sock.sendto(out, to)
            except OSError:
                pass   # a datagram that cannot go is a lost datagram

    last_tick = clock()
    while not (stop and stop.is_set()):
        if select.select([sock], [], [], 0.1)[0]:
            for _ in range(512):
                try:
                    data, addr = sock.recvfrom(2048)
                except OSError:   # nothing more to read (or an ICMP error reported on Windows)
                    break
                send_all(relay.handle(data, addr, clock()))
        now = clock()
        if now - last_tick >= 0.25:
            last_tick = now
            send_all(relay.tick(now))


def run_in_thread(relay, host='127.0.0.1', port=0):
    """Starts serve() on a new socket in a thread (tests): (thread, stop event, bound port)."""
    sock = open_socket(host, port)
    stop = threading.Event()
    t = threading.Thread(target=lambda: (serve(relay, sock, stop), sock.close()), daemon=True)
    t.start()
    return t, stop, sock.getsockname()[1]


def main():
    ap = argparse.ArgumentParser(description='NX2navi co-op relay (see docs/coop.md)')
    ap.add_argument('--host', default='0.0.0.0', help='address to listen on (default 0.0.0.0; :: for IPv6 and IPv4)')
    ap.add_argument('--port', type=int, default=24680, help='UDP port (default 24680; 0 picks a free one)')
    ap.add_argument('--log-addrs', action='store_true', help="write the players' addresses to the log (off by default)")
    ap.add_argument('--max-rooms', type=int, default=1000, help='rooms at most (default 1000)')
    ap.add_argument('--rooms-per-network', type=int, default=8, help='open rooms one /24 or /48 may make (default 8)')
    ap.add_argument('--log-joins', action='store_true', help="write where each network's join budget goes to the log")
    a = ap.parse_args()

    def log(text):
        print(time.strftime('%H:%M:%S ') + text, flush=True)

    relay = Relay(max_rooms=a.max_rooms, log=log, log_addrs=a.log_addrs, rooms_per_prefix=a.rooms_per_network, log_joins=a.log_joins)
    sock = open_socket(a.host, a.port)
    log(f'coop_relay: listening on {a.host}:{sock.getsockname()[1]}')
    try:
        serve(relay, sock)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
