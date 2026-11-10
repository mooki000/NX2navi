"""The restorations (runtime/restore.c, restore_engine.c, byml.c, sarc.c) on files of its own: no game files.

Linux: python tests/restore_check.py (clang or cc, and zstd's development files)
Windows: python tests/restore_check.py --toolchain C:/msys64/ucrt64
With --game <built game copy>: also every logic graph of the player's files, read and written back (ainb.c).

Writes fixtures with an encoder of its own (BYML laid out parents first and nothing shared, unlike the engine's
writer): a placement file packed with a zstd dictionary (Pack/ZsDic.pack.zs's role is played by a SARC of two
trained dictionaries), a pack (SARC) of BYML members and raw bytes, the files edits copy from (another placement,
another pack) or check against (an area's other placement file, its static compound), a text archive of message
files, logic graphs (AINB), and files that are not what
they claim. tests/restore_check.c runs the engine's edits of every kind on them, prints each note and checks which
applied, feeds the parser, the engine and the SARC reader and writer every cut length of them and damaged copies,
checks that dicts out of order are refused, and runs restore.c's switches, a start and the probe with the settings,
the log and the RomFS stubbed (every switch off: nothing read, served or logged); this script then decodes what it
wrote with its own decoder and compares the trees with the edits it expects, and checks the packs' members: their
hashes, order, bytes and alignment.
"""
import argparse
import os
import random
import shlex
import shutil
import struct
import subprocess
import sys
from pathlib import Path

try:
    from compression import zstd as _zstd   # Python 3.14

    def compress(data, dic=None):
        return _zstd.compress(data, zstd_dict=dic) if dic else _zstd.compress(data)

    def decompress(data, dics):
        info = _zstd.get_frame_info(data)
        return _zstd.decompress(data, zstd_dict=dics[info.dictionary_id]) if info.dictionary_id else \
            _zstd.decompress(data)

    def train(samples, size):
        d = _zstd.train_dict(samples, size)
        return d, d.dict_id, d.dict_content

    def load_dict(content):
        d = _zstd.ZstdDict(content)
        return d.dict_id, d
except ImportError:
    import zstandard

    def compress(data, dic=None):
        return zstandard.ZstdCompressor(dict_data=dic, write_content_size=True).compress(data)

    def decompress(data, dics):
        info = zstandard.get_frame_parameters(data)
        dctx = zstandard.ZstdDecompressor(dict_data=dics[info.dict_id]) if info.dict_id else zstandard.ZstdDecompressor()
        return dctx.decompress(data)

    def train(samples, size):
        d = zstandard.train_dictionary(size, samples)
        return d, d.dict_id(), d.as_bytes()

    def load_dict(content):
        d = zstandard.ZstdCompressionDict(content)
        return d.dict_id(), d

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'restore-check'


# ---- BYML: values are Python objects; F32/U32/I64/U64/F64/Bin/Hash32 tag the types Python has no own type for
class F32(float): pass
class U32(int): pass
class I64(int): pass
class U64(int): pass
class F64(float): pass
class Bin(bytes): pass
class Hash32(dict): pass   # {int hash: value}


def kind(v):
    if v is None: return 0xff
    if isinstance(v, bool): return 0xd0
    if isinstance(v, F32): return 0xd2
    if isinstance(v, F64): return 0xd6
    if isinstance(v, U32): return 0xd3
    if isinstance(v, I64): return 0xd4
    if isinstance(v, U64): return 0xd5
    if isinstance(v, int): return 0xd1
    if isinstance(v, str): return 0xa0
    if isinstance(v, Bin): return 0xa1
    if isinstance(v, Hash32): return 0x20
    if isinstance(v, dict): return 0xc1
    if isinstance(v, list): return 0xc0
    raise TypeError(type(v))


def byml_encode(root, version=7):
    """Parents before children, every container and 8-byte value written where it is met (nothing shared)."""
    e = '<'
    keys, strs = set(), set()

    def walk(v):
        if isinstance(v, str): strs.add(v)
        elif isinstance(v, Hash32): [walk(x) for x in v.values()]
        elif isinstance(v, dict): keys.update(v), [walk(x) for x in v.values()]
        elif isinstance(v, list): [walk(x) for x in v]
    walk(root)
    keys, strs = sorted(keys, key=lambda s: s.encode()), sorted(strs, key=lambda s: s.encode())
    out = bytearray(16)

    def head(t, n):
        return struct.pack(e + 'I', n << 8 | t)

    def table(items):
        at = len(out)
        blob = b''.join(s.encode() + b'\0' for s in items)
        offs, o = [], 4 + 4 * (len(items) + 1)
        for s in items:
            offs.append(o)
            o += len(s.encode()) + 1
        offs.append(o)
        out.extend(head(0xc2, len(items)) + struct.pack(e + '%dI' % len(offs), *offs) + blob)
        while len(out) % 4: out.append(0)
        return at

    kt = table(keys) if keys else 0
    st = table(strs) if strs else 0

    def align(n):
        while len(out) % n: out.append(0)

    def scalar(v):
        t = kind(v)
        if t == 0xa0: return strs.index(v)
        if t == 0xd0: return int(v)
        if t == 0xd1: return v & 0xffffffff
        if t == 0xd2: return struct.unpack(e + 'I', struct.pack(e + 'f', v))[0]
        if t == 0xd3: return int(v)
        if t == 0xff: return 0
        return None

    def container(v):
        """Writes the container at the end, its children after it; returns its offset."""
        align(4)
        at, t = len(out), kind(v)
        items = list(v.items()) if t == 0x20 else sorted(v.items(), key=lambda kv: kv[0].encode()) if t == 0xc1 \
            else list(enumerate(v))
        if t == 0x20: items.sort(key=lambda kv: kv[0])
        n = len(items)
        if t == 0xc0: size = 4 + (n + 3) // 4 * 4 + 4 * n
        elif t == 0xc1: size = 4 + 8 * n
        else: size = 4 + 8 * n + (n + 3) // 4 * 4
        out.extend(b'\0' * size)
        struct.pack_into(e + 'I', out, at, n << 8 | t)
        for i, (k, x) in enumerate(items):
            xt = kind(x)
            val = scalar(x)
            if val is None:
                if xt in (0xc0, 0xc1, 0x20): val = container(x)
                elif xt == 0xa1:
                    align(4)
                    val = len(out)
                    out.extend(struct.pack(e + 'I', len(x)) + x)
                else:
                    align(8)
                    val = len(out)
                    out.extend(struct.pack(e + {0xd4: 'q', 0xd5: 'Q', 0xd6: 'd'}[xt], x))
            if t == 0xc0:
                out[at + 4 + i] = xt
                struct.pack_into(e + 'I', out, at + 4 + (n + 3) // 4 * 4 + 4 * i, val)
            elif t == 0xc1:
                ki = keys.index(k)
                struct.pack_into(e + 'II', out, at + 4 + 8 * i, ki | xt << 24, val)
            else:
                struct.pack_into(e + 'II', out, at + 4 + 8 * i, k, val)
                out[at + 4 + 8 * n + i] = xt
        return at

    root_at = container(root)
    out[0:2] = b'YB'
    struct.pack_into(e + 'HIII', out, 2, version, kt, st, root_at)
    return bytes(out)


def byml_decode(d):
    assert d[:2] == b'YB', d[:4]
    e = '<'
    _, kt, st, root = struct.unpack_from(e + 'HIII', d, 2)

    def u32(o): return struct.unpack_from(e + 'I', d, o)[0]

    def head(o):
        w = u32(o)
        return w & 0xff, w >> 8

    def table(o):
        if not o: return []
        t, n = head(o)
        assert t == 0xc2
        return [d[o + u32(o + 4 + 4 * i):d.index(b'\0', o + u32(o + 4 + 4 * i))].decode() for i in range(n)]
    keys, strs = table(kt), table(st)
    if keys: assert keys == sorted(keys, key=lambda s: s.encode()), 'key table not sorted'
    if strs: assert strs == sorted(strs, key=lambda s: s.encode()), 'string table not sorted'

    def val(t, v):
        if t == 0xa0: return strs[v]
        if t == 0xa1: return Bin(d[v + 4:v + 4 + u32(v)])
        if t in (0xc0, 0xc1, 0x20):
            assert v % 4 == 0
            return node(v, t)
        if t == 0xd0: return bool(v)
        if t == 0xd1: return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == 0xd2: return F32(struct.unpack('<f', struct.pack('<I', v))[0])
        if t == 0xd3: return U32(v)
        if t in (0xd4, 0xd5, 0xd6):
            assert v % 8 == 0, '64-bit value not 8-aligned'
            return {0xd4: I64, 0xd5: U64, 0xd6: F64}[t](struct.unpack_from(e + {0xd4: 'q', 0xd5: 'Q', 0xd6: 'd'}[t], d, v)[0])
        if t == 0xff: return None
        raise ValueError(hex(t))

    def node(o, want):
        t, n = head(o)
        assert t == want
        if t == 0xc0:
            vo = o + 4 + (n + 3) // 4 * 4
            return [val(d[o + 4 + i], u32(vo + 4 * i)) for i in range(n)]
        if t == 0xc1:
            res, last = {}, -1
            for i in range(n):
                w = u32(o + 4 + 8 * i)
                ki, ty = w & 0xffffff, w >> 24
                assert ki > last, 'dict not sorted by key'
                last = ki
                res[keys[ki]] = val(ty, u32(o + 8 + 8 * i))
            return res
        res = Hash32()
        for i in range(n):
            res[u32(o + 4 + 8 * i)] = val(d[o + 4 + 8 * n + i], u32(o + 8 + 8 * i))
        assert list(res) == sorted(res), 'hash dict not sorted'
        return res
    return node(root, head(root)[0]) if root else None


def same(a, b):
    """Tree equality with types: a float and an int of equal value differ."""
    if kind(a) != kind(b): return False
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, float):
        return struct.pack('<f' if isinstance(a, F32) else '<d', a) == struct.pack('<f' if isinstance(b, F32) else '<d', b)
    return a == b


# ---- SARC
def sarc_encode(members, aligns):
    """members: [(name, bytes)] in node order (sorted by the name hash, as the game's are); aligns: data alignment of
    each member."""
    def h(name):
        x = 0
        for c in name.encode(): x = (x * 0x65 + (c if c < 0x80 else c - 0x100)) & 0xffffffff
        return x
    members = sorted(members, key=lambda m: h(m[0]))
    names = bytearray()
    offs = []
    for n, _ in members:
        offs.append(len(names) // 4)
        names += n.encode() + b'\0'
        while len(names) % 4: names.append(0)
    head = 0x14 + 12 + 16 * len(members) + 8 + len(names)
    data_at = (head + 0xff) & ~0xff
    data = bytearray()
    nodes = b''
    for (n, b), off in zip(members, offs):
        a = aligns[n]
        while (data_at + len(data)) % a: data.append(0)
        begin = len(data)
        data += b
        nodes += struct.pack('<IIII', h(n), 0x01000000 | off, begin, len(data))
    out = bytearray(b'SARC' + struct.pack('<HHIIHH', 0x14, 0xfeff, 0, data_at, 0x100, 0))
    out += b'SFAT' + struct.pack('<HHI', 12, len(members), 0x65) + nodes
    out += b'SFNT' + struct.pack('<HH', 8, 0) + names
    out += b'\0' * (data_at - len(out)) + data
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def sarc_decode(d):
    """{name: (absolute offset, bytes)}; checks the size field and that the nodes are sorted by their names' hashes."""
    assert d[:4] == b'SARC'
    head, bom, size, data_at = struct.unpack_from('<HHII', d, 4)
    assert size == len(d), 'SARC size field'
    count, mult = struct.unpack_from('<HI', d, head + 6)
    names = head + 12 + 16 * count + 8
    res, last = {}, -1
    for i in range(count):
        h, attr, lo, hi = struct.unpack_from('<IIII', d, head + 12 + 16 * i)
        o = names + (attr & 0xffffff) * 4
        name = d[o:d.index(b'\0', o)].decode()
        x = 0
        for c in name.encode(): x = (x * mult + (c if c < 0x80 else c - 0x100)) & 0xffffffff
        assert h == x, 'SARC node hash of %s' % name
        assert h > last, 'SARC nodes not sorted by hash'
        last = h
        assert o < data_at, 'SARC name past the data offset'
        res[name] = (data_at + lo, d[data_at + lo:data_at + hi])
    return res


# ---- MSBT: a message is a list of parts, str for text, (group, type, bytes) for a control tag, (group, type) for a
# closing tag; the 0 unit that ends it is added
def msbt_message(parts, wide=False):
    out = bytearray()
    for p in parts:
        if isinstance(p, str):
            out += p.encode('utf-16-le' if wide else 'utf-8')
        elif len(p) == 3:
            out += (b'\x0e\x00' if wide else b'\x0e') + struct.pack('<HHH', p[0], p[1], len(p[2])) + p[2]
        else:
            out += (b'\x0f\x00' if wide else b'\x0f') + struct.pack('<HH', p[0], p[1])
    return bytes(out + (b'\0\0' if wide else b'\0'))


def msbt_hash(b):
    h = 0xcbf29ce484222325
    for x in b: h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h


def msbt_encode(messages, wide=False, before=b'', after=b''):
    """messages: [(label, bytes)], the message index being the order; before / after: the data of an ATR1 section
    between LBL1 and TXT2 and of a TSY1 section after TXT2 (none when empty). Labels sit in 7 hash buckets."""
    buckets = [[] for _ in range(7)]
    for i, (label, _) in enumerate(messages):
        h = 0
        for c in label.encode(): h = (h * 0x492 + c) & 0xffffffff
        buckets[h % 7].append((label, i))
    lbl, blobs = bytearray(struct.pack('<I', 7)), bytearray()
    for b in buckets:
        lbl += struct.pack('<II', len(b), 4 + 8 * 7 + len(blobs))
        for label, i in b: blobs += bytes([len(label)]) + label.encode() + struct.pack('<I', i)
    lbl += blobs
    txt = bytearray(struct.pack('<I', len(messages)))
    at = 4 + 4 * len(messages)
    for _, m in messages:
        txt += struct.pack('<I', at)
        at += len(m)
    for _, m in messages: txt += m
    secs = [(b'LBL1', lbl)] + ([(b'ATR1', before)] if before else []) + [(b'TXT2', txt)] + \
        ([(b'TSY1', after)] if after else [])
    body = bytearray()
    for name, data in secs:
        body += name + struct.pack('<I', len(data)) + b'\0' * 8 + data
        while len(body) % 16: body.append(0xab)
    head = b'MsgStdBn\xff\xfe\0\0' + bytes([1 if wide else 0, 3]) + struct.pack('<HHI', len(secs), 0, 0x20 + len(body))
    return head + b'\0' * (0x20 - len(head)) + bytes(body)


def msbt_decode(d):
    """(encoding byte, {label: message bytes}, {section name: data}); checks the size field and the AB padding."""
    assert d[:10] == b'MsgStdBn\xff\xfe' and struct.unpack_from('<I', d, 0x12)[0] == len(d), 'MSBT header'
    secs, o = {}, 0x20
    for _ in range(struct.unpack_from('<H', d, 0xe)[0]):
        size = struct.unpack_from('<I', d, o + 4)[0]
        secs[d[o:o + 4].decode()] = d[o + 16:o + 16 + size]
        o += 16 + size
        pad = (16 - o % 16) % 16
        assert d[o:o + pad] == b'\xab' * pad, 'MSBT section padding'
        o += pad
    assert o == len(d), 'MSBT data past the last section'
    lbl, txt = secs['LBL1'], secs['TXT2']
    n = struct.unpack_from('<I', txt, 0)[0]
    offs = list(struct.unpack_from('<%dI' % n, txt, 4)) + [len(txt)]
    labels = {}
    for b in range(struct.unpack_from('<I', lbl, 0)[0]):
        cnt, off = struct.unpack_from('<II', lbl, 4 + 8 * b)
        for _ in range(cnt):
            ln = lbl[off]
            i = struct.unpack_from('<I', lbl, off + 1 + ln)[0]
            labels[lbl[off + 1:off + 1 + ln].decode()] = txt[offs[i]:offs[i + 1]]
            off += 1 + ln + 4
    return d[0xc], labels, secs


# ---- fixtures and what the edits should make of them
def placement():
    return {
        'Actors': [
            {'Gyaml': 'Obj_A', 'Hash': U64(1001), 'Translate': [F32(1.0), F32(2.0), F32(3.0)],
             'Phive': {'Placement': {'ID': U64(1001)}}, 'SRTHash': U32(77)},
            {'Gyaml': 'Obj_B', 'Hash': U64(1002), 'Translate': [F32(4.0), F32(5.0), F32(6.0)],
             'Rotate': [F32(0.0), F32(1.5), F32(0.0)], 'Bakeable': True, 'SRTHash': U32(78),
             'Phive': {'Placement': {'ID': U64(1002)}},
             'Links': [{'Dst': U64(1001), 'Gyaml': 'Reference', 'Name': 'Reference', 'Src': U64(1002)}]},
            {'Gyaml': 'Obj_A', 'Hash': U64(1003), 'Translate': [F32(7.0), F32(8.0), F32(9.0)],
             'Scale': [F32(1.0), F32(2.0), F32(3.0)], 'Dynamic': {'Count': 5, 'Speed': F32(0.5)}},
            {'Gyaml': 'Obj_C', 'Hash': U64(18446744073709551557), 'Translate': [F32(-1.5), F32(0.25), F32(1e6)]},
        ],
        'FilePath': 'Work/Banc/Scene/Test.bcett.json',
        'Misc': {
            'Bool': False, 'Int': -7, 'UInt': U32(4000000000), 'Float': F32(-0.125), 'Int64': I64(-(1 << 40)),
            'UInt64': U64((1 << 64) - 2), 'Double': F64(3.141592653589793), 'Null': None, 'Bin': Bin(b'\x01\x02\x03'),
            'Empty': {}, 'EmptyList': [], 'Nested': [[U64(5), U64(5)], [U64(5), U64(5)], {'X': F32(1.0)}],
            'Hashes': Hash32({0x9000: 'b', 0x10: 'a', 0xffffffff: [1, 2]}), 'Unicode': 'Ж мод',
        },
    }


def placement_expected():
    t = placement()
    a = t['Actors']
    a[1]['Rotate'][1] = F32(3.0)
    a[1]['Bakeable'] = False
    a[2]['Scale'][1] = F32(5.0)
    a[2]['Dynamic']['Count'] = 9
    t['Misc']['Double'] = F64(2.5)
    return t


def source():
    """Another area's placement: the object RESTORE_ADD_FROM copies."""
    return {'Actors': [
        {'Gyaml': 'Obj_S', 'Hash': U64(5001), 'Translate': [F32(1.0), F32(1.0), F32(1.0)], 'Dynamic': {'Room': 24},
         'Phive': {'Placement': {'ID': U64(5001)}}, 'SRTHash': U32(9),
         'Links': [{'Dst': U64(5002), 'Gyaml': 'Reference', 'Name': 'Reference', 'Src': U64(5001)}]},
        {'Gyaml': 'Obj_T', 'Hash': U64(5002), 'Translate': [F32(0.0), F32(0.0), F32(0.0)]},
    ], 'FilePath': 'Work/Banc/Scene/Other.bcett.json'}


def placement_kinds_expected():
    t = placement()
    a = t['Actors']
    copy = {k: v for k, v in a[1].items() if k not in ('Links', 'SRTHash')}
    copy['Hash'] = U64(2002)
    copy['Phive'] = {'Placement': {'ID': U64(2002)}}
    copy['Translate'] = [F32(10.0), F32(20.0), F32(30.0)]
    copy['Rotate'] = [F32(0.0), F32(3.0), F32(0.0)]
    a[2]['Name'] = 'Added'
    a[2]['Dynamic']['Extra'] = F32(2.5)
    t['FilePath'] = 'Work/Other.json'
    t['Misc']['Flag'] = True
    t['Misc']['Aaa'] = 3
    del a[0]                                 # Obj_A 1001 removed
    a.append(copy)
    a.append({'Gyaml': 'Obj_S', 'Hash': U64(6001), 'Translate': [F32(9.0), F32(9.0), F32(9.0)],
              'Rotate': [F32(0.0), F32(0.5), F32(0.0)], 'Dynamic': {'Room': 17},
              'Phive': {'Placement': {'ID': U64(6001)}}})
    return t


def pack_members():
    one = byml_encode({'MoveSpeed': F32(0.15), 'Other': 3})
    three = byml_encode({'Params': {'Speed': F32(1.5), 'Count': 4, 'Flag': True}})
    raw = bytes(random.Random(5).randrange(256) for _ in range(300))
    return [('A/one.bgyml', one), ('B/two.bin', raw), ('C/three.bgyml', three)], \
        {'A/one.bgyml': 8, 'B/two.bin': 0x100, 'C/three.bgyml': 0x80}


NAVI = [(3, 0, b'\x0f\x01\x00'), (4, 0, b'\x0c\x00Attention_00\x00')]   # leading tags: window, Navi's voice


def text_messages():
    """The two MSBT members of the text archive: {member: [(label, message bytes)]}. tests/restore_check.c names
    these messages by their msbt_hash, which main() checks against the constants it holds."""
    navi = [('Enemy_A', NAVI + ['That thing looks like you!\nShould we call it ', (0, 3, b'\0\0'), 'Dark One',
                                (0, 3, b'\xff\xff'), '?']),
            ('Enemy_B', [(4, 0, b'\x05\x00Happy\x00'), 'Another enemy.']),
            ('Enemy_C', [(3, 0, b'\x0f\x01\x00'), 'Third.']),
            ('Plain', ['No tags at all.'])]
    wide = [('W0', [(4, 0, b'\x02\x00Hi\x00'), 'Wide text']), ('W1', ['Second'])]
    return {'Msg/Navi.msbt': [(l, msbt_message(p)) for l, p in navi],
            'Msg/Wide.msbt': [(l, msbt_message(p, True)) for l, p in wide]}


def text_expected():
    """What tests/restore_check.c's edits make of them: each changed message keeps its leading tags."""
    m = {k: dict(v) for k, v in text_messages().items()}
    m['Msg/Navi.msbt']['Enemy_A'] = msbt_message(NAVI + [(0, 3, b'\0\0'), 'Dark One', (0, 3, b'\xff\xff'),
                                                         '\nConquer yourself!'])
    m['Msg/Navi.msbt']['Enemy_C'] = msbt_message([(3, 0, b'\x0f\x01\x00'), '自分自身に', (1, 2, b'\x0a\x0b'), '打ち勝つ',
                                                  (1, 2), '！'])
    m['Msg/Navi.msbt']['Plain'] = msbt_message([])
    m['Msg/Wide.msbt']['W0'] = msbt_message([(4, 0, b'\x02\x00Hi\x00'), 'Wide ', (0, 3, b'\0\0'), 'ж',
                                             (0, 3, b'\xff\xff'), ' \U0001F600\nline'], True)
    return m


# The hashes tests/restore_check.c declares (msbt_hash of the game's message, as a restoration names it)
TEXT_HASHES = {('Msg/Navi.msbt', 'Enemy_A'): 0xa07a3f64afb3b6e0, ('Msg/Navi.msbt', 'Enemy_B'): 0x410786bdd54ba4d3,
               ('Msg/Navi.msbt', 'Enemy_C'): 0x25f7f232d73044d2, ('Msg/Navi.msbt', 'Plain'): 0xe801dbf933a05e5d,
               ('Msg/Wide.msbt', 'W0'): 0x7ab759bfc5845b1b}

# ---- AINB logic graphs: a node is a dict: type (0 application-defined, else the built-in code), name, flags (bit 0
# query), props [(k, name, flags, value)], ins [(k, name, source, flags, value)] with source None (a value of its
# own), (node, output index) or ('multi', [(node, output index)]), outs [(k, name, used)], plugs {category:
# [(node, name, extra bytes)]}, query [ranks]. Types k: int, bool, float, string, vec3, pointer (a pointer's value is
# (class name, u32)).
AINB_PSZ, AINB_ISZ, AINB_OSZ = [12, 12, 12, 12, 20, 12], [16, 16, 16, 16, 24, 20], [4, 4, 4, 4, 4, 8]
OWN, DEFAULT, BOUND = 0x00800000, 0x01000000, 0x80000000


def ainb_encode(g):
    """A graph laid out as the game's are (ainb.c's header comment). g: name, cmds [(name, node)], nodes, bb
    [(slot, name, value bytes)], att [(name, param bytes of 0x34)], exb, mod, action, link, repl, x6c (bytes of those
    sections)."""
    pool = bytearray()
    where = {}

    def s(text):
        if text not in where:
            where[text] = len(pool)
            pool.extend(text.encode() + b'\0')
        return where[text]

    def val(k, v):
        if k == 0: return struct.pack('<i', v)
        if k == 1: return struct.pack('<I', int(v))
        if k == 2: return struct.pack('<f', v)
        if k == 3: return struct.pack('<I', s(v))
        if k == 4: return struct.pack('<3f', *v)

    bb = bytearray()
    entries, values, first = bytearray(), bytearray(), 0
    for slot in range(7):   # string, int, u32, float, bool, vec3, pointer
        mine = [x for x in g.get('bb', []) if x[0] == slot]
        bb += struct.pack('<HHHH', len(mine), first, len(values), 0)
        for _, name, v in mine:
            entries += struct.pack('<II', s(name) | 0x800000, 0)
            values += v
        first += len(mine)
    bb += entries + values
    nodes = g['nodes']
    props, ins, outs = [[] for _ in range(6)], [[] for _ in range(6)], [[] for _ in range(6)]
    multi, query = [], []
    blocks = []
    for n in nodes:
        head = bytearray()
        for k in range(6):
            mine = [p for p in n.get('props', []) if p[0] == k]
            head += struct.pack('<II', len(props[k]), len(mine))
            for _, name, fl, v in mine:
                props[k].append(struct.pack('<III', s(name), s(v[0]), fl) if k == 5 else
                                struct.pack('<II', s(name), fl) + val(k, v))
        for k in range(6):
            mine_in = [p for p in n.get('ins', []) if p[0] == k]
            mine_out = [p for p in n.get('outs', []) if p[0] == k]
            head += struct.pack('<IIII', len(ins[k]), len(mine_in), len(outs[k]), len(mine_out))
            for _, name, src, fl, v in mine_in:
                if src is None: link = (-1, 0)
                elif src[0] == 'multi':
                    link = (-100 - len(multi), len(src[1]))
                    multi.extend(struct.pack('<HHI', a, b, 0) for a, b in src[1])
                else: link = src
                if k == 5: ins[k].append(struct.pack('<IIhhII', s(name), s(v[0]), link[0], link[1], fl, v[1]))
                else: ins[k].append(struct.pack('<IhhI', s(name), link[0], link[1], fl) + val(k, v))
            for _, name, used in mine_out:
                o = struct.pack('<I', s(name) | (0x80000000 if used else 0))
                outs[k].append(o + (struct.pack('<I', s('game::Data')) if k == 5 else b''))
        plugs = n.get('plugs', {})
        total = sum(len(v) for v in plugs.values())
        acc = 0
        for c in range(10):
            head += struct.pack('<BB', len(plugs.get(c, [])), acc)
            acc += len(plugs.get(c, []))
        blocks.append((head, [(c, p) for c in range(10) for p in plugs.get(c, [])], total))
        n['_query_base'] = len(query)
        query.extend(n.get('query', []))
    # layout
    out = bytearray(0x74)
    cmd_at = 0x74
    elems_at = cmd_at + 0x18 * len(g['cmds'])
    bb_at = elems_at + 0x3c * len(nodes)
    at = bb_at + len(bb)
    param_at = []
    body = bytearray()
    for head, plugs, total in blocks:
        param_at.append(at + len(body))
        here = at + len(body)
        data = bytearray()
        ptrs = bytearray()
        for c, (node, name, extra) in plugs:
            ptrs += struct.pack('<I', here + 0xa4 + 4 * total + len(data))
            data += struct.pack('<II', node, s(name)) + extra
        body += head + ptrs + data
    at += len(body)
    att = g.get('att', [])
    attid_at = att_at = at
    at += 0x10 * len(att)
    attp_at = []
    for _ in att:
        attp_at.append(at)
        at += 0x64
    prop_at = at
    prop_body = bytearray()
    starts = []
    for k in range(6):
        starts.append(prop_at + 0x18 + len(prop_body))
        prop_body += b''.join(props[k])
    io_at = prop_at + 0x18 + len(prop_body)
    io_body = bytearray()
    io_starts = []
    for k in range(6):
        io_starts.append(io_at + 0x30 + len(io_body))
        io_body += b''.join(ins[k])
        io_starts.append(io_at + 0x30 + len(io_body))
        io_body += b''.join(outs[k])
    multi_at = io_at + 0x30 + len(io_body)
    jump_at = multi_at + 8 * len(multi)
    query_at = jump_at
    exb_at = query_at + 4 * len(query)
    mod_at = exb_at + len(g.get('exb', b''))
    action_at = mod_at + len(g['mod'])
    link_at = action_at + len(g['action'])
    repl_at = link_at + len(g['link'])
    x6c_at = repl_at + len(g['repl'])
    enum_at = x6c_at + len(g['x6c'])
    str_at = enum_at + 4
    s(g['name'])
    for name, _ in g['cmds']: s(name)
    for n in nodes: s(n.get('name', ''))
    for name, _ in att: s(name)
    # header
    struct.pack_into('<4sIIIIIII', out, 0, b'AIB ', 0x408, s(g['name']), len(g['cmds']), len(nodes),
                     sum(n.get('flags', 0) & 1 for n in nodes), len(att), 0)
    struct.pack_into('<17I', out, 0x20, bb_at, str_at, enum_at, prop_at, jump_at, io_at, multi_at, att_at, attid_at,
                     exb_at if g.get('exb') else 0, repl_at, query_at, jump_at, 0, 0, mod_at, s('AI'))
    struct.pack_into('<IIII', out, 0x64, 0, action_at, x6c_at, link_at)
    for name, node in g['cmds']:
        out += struct.pack('<I', s(name)) + bytes(range(16)) + struct.pack('<HH', node, 0)
    for i, n in enumerate(nodes):
        name = n.get('name', '')
        out += struct.pack('<HHHBBIIII', n['type'], i, 0, n.get('flags', 0), 0, s(name),
                           murmur3(name.encode()) if n['type'] == 0 else 0, 0, param_at[i])
        out += struct.pack('<HHHHIHHI', 0, 0, sum(len(src[1]) for _, _, src, _, _ in n.get('ins', [])
                                                 if src and src[0] == 'multi'), 0, 0, n['_query_base'],
                           len(n.get('query', [])), 0)
        out += bytes((i * 7 + j) & 0xff for j in range(16))
    out += bb + body
    for (name, param), pa in zip(att, attp_at):
        out += struct.pack('<IIII', s(name), pa, 0, murmur3(name.encode()))
    for (name, param), pa in zip(att, attp_at):
        out += param + b''.join(struct.pack('<II', 0, pa + 0x64) for _ in range(6))
    out += b''.join(struct.pack('<I', x) for x in starts) + prop_body
    out += b''.join(struct.pack('<I', x) for x in io_starts) + io_body
    out += b''.join(multi) + b''.join(struct.pack('<HH', q, 0) for q in query)
    out += g.get('exb', b'') + g['mod'] + g['action'] + g['link'] + g['repl'] + g['x6c'] + b'\0' * 4 + pool
    assert len(out) == str_at + len(pool)
    return bytes(out)


def murmur3(data):
    c1, c2, h, n = 0xcc9e2d51, 0x1b873593, 0, len(data)
    rot = lambda x, r: ((x << r) | (x >> (32 - r))) & 0xffffffff
    for i in range(0, n - n % 4, 4):
        k = rot(struct.unpack_from('<I', data, i)[0] * c1 & 0xffffffff, 15) * c2 & 0xffffffff
        h = (rot(h ^ k, 13) * 5 + 0xe6546b64) & 0xffffffff
    k = int.from_bytes(data[n - n % 4:], 'little')
    if n % 4: h ^= rot(k * c1 & 0xffffffff, 15) * c2 & 0xffffffff
    h ^= n
    for sh, m in ((16, 0x85ebca6b), (13, 0xc2b2ae35), (16, 1)):
        h ^= h >> sh
        h = h * m & 0xffffffff
    return h


def ainb_decode(d):
    """{name, nodes (as ainb_encode takes them, plus guid; plugs as (node, name)), sections: {name: bytes} of the
    sections moved as they are}; checks the layout as ainb.c reads it."""
    u = lambda o: struct.unpack_from('<I', d, o)[0]
    assert d[:4] == b'AIB ' and u(4) == 0x408
    sp = u(0x24)
    st = lambda o: d[sp + o:d.index(b'\0', sp + o)].decode()
    ncmd, nel, nq, natt = u(0xc), u(0x10), u(0x14), u(0x18)
    elems_at = 0x74 + 0x18 * ncmd
    assert u(0x20) == elems_at + 0x3c * nel and u(0x50) == u(0x30)
    pt, io = u(0x2c), u(0x34)
    pa = [u(pt + 4 * k) for k in range(6)] + [io]
    ia = [u(io + 4 * k) for k in range(12)] + [u(0x38)]
    assert pa[0] == pt + 0x18 and ia[0] == io + 0x30

    def val(k, o):
        if k == 0: return struct.unpack_from('<i', d, o)[0]
        if k == 1: return bool(u(o))
        if k == 2: return round(struct.unpack_from('<f', d, o)[0], 5)
        if k == 3: return st(u(o))
        if k == 4: return tuple(round(x, 5) for x in struct.unpack_from('<3f', d, o))

    nodes = []
    for i in range(nel):
        e = elems_at + 0x3c * i
        t, idx, natt_e, fl = struct.unpack_from('<HHHB', d, e)
        assert idx == i
        p = u(e + 0x14)
        n = dict(type=t, name=st(u(e + 8)), flags=fl, guid=d[e + 0x2c:e + 0x3c], props=[], ins=[], outs=[], plugs={})
        assert u(e + 0xc) == (murmur3(n['name'].encode()) if t == 0 else 0)
        for k in range(6):
            b, c = struct.unpack_from('<II', d, p + 8 * k)
            for j in range(c):
                o = pa[k] + (b + j) * AINB_PSZ[k]
                n['props'].append((k, st(u(o)), u(o + 8), (st(u(o + 4)),)) if k == 5 else
                                  (k, st(u(o)), u(o + 4), val(k, o + 8)))
        for k in range(6):
            ib, ic, ob, oc = struct.unpack_from('<IIII', d, p + 0x30 + 16 * k)
            for j in range(ic):
                o = ia[2 * k] + (ib + j) * AINB_ISZ[k]
                so = 8 if k == 5 else 4
                a, b = struct.unpack_from('<hh', d, o + so)
                src = None if a < 0 and a > -100 else (a, b) if a >= 0 else \
                    ('multi', [struct.unpack_from('<HH', d, u(0x38) + 8 * (-100 - a + q)) for q in range(b)])
                v = (st(u(o + 4)), u(o + 16)) if k == 5 else val(k, o + 12)
                n['ins'].append((k, st(u(o)), src, u(o + so + 4), v))
            for j in range(oc):
                o = ia[2 * k + 1] + (ob + j) * AINB_OSZ[k]
                n['outs'].append((k, st(u(o) & 0x7fffffff), bool(u(o) >> 31)))
        raw = d[p + 0x90:p + 0xa4]
        for c in range(10):
            cnt, base = raw[2 * c], raw[2 * c + 1]
            if cnt:
                n['plugs'][c] = [(u(q), st(u(q + 4))) for q in (u(p + 0xa4 + 4 * (base + j)) for j in range(cnt))]
        assert struct.unpack_from('<H', d, e + 0x1c)[0] == sum(len(src[1]) for _, _, src, _, _ in n['ins']
                                                              if src and src[0] == 'multi'), 'multi-input count'
        qb, qc = struct.unpack_from('<HH', d, e + 0x24)
        n['query'] = [struct.unpack_from('<H', d, u(0x4c) + 4 * (qb + j))[0] for j in range(qc)]
        nodes.append(n)
    assert sum(n['flags'] & 1 for n in nodes) == nq
    secs = {}
    order = [(0x38, 'multi'), (0x30, 'jump'), (0x4c, 'query'), (0x44, 'exb'), (0x5c, 'mod'), (0x68, 'action'),
             (0x70, 'link'), (0x48, 'repl'), (0x6c, 'x6c'), (0x28, 'enum'), (0x24, 'str')]
    present = [(u(f), n) for f, n in order if u(f)]
    for (a, n), (b, _) in zip(present, present[1:]):
        if n not in ('query',): secs[n] = d[a:b]
    first_param = u(elems_at + 0x14) if nel else min(u(0x3c), u(0x40))
    secs['bb'] = d[u(0x20):first_param]
    secs['att'] = [(st(u(u(0x3c) + 0x10 * j)), d[u(u(0x3c) + 0x10 * j + 4):u(u(0x3c) + 0x10 * j + 4) + 0x34])
                   for j in range(natt)]
    for j in range(natt):
        ap = u(u(0x3c) + 0x10 * j + 4)
        assert all(u(ap + 0x34 + 8 * k + 4) == ap + 0x64 for k in range(6)), 'attachment end offsets'
    return dict(name=st(u(8)), cmds=[(st(u(0x74 + 0x18 * i)), struct.unpack_from('<H', d, 0x74 + 0x18 * i + 0x14)[0])
                                     for i in range(ncmd)], nodes=nodes, sections=secs)


def graph_fixture():
    """The graph tests/restore_check.c edits (AI/Test.module.ainb), and the one it copies a node from."""
    g = dict(name='Test.module', cmds=[('Root', 0)], bb=[(1, 'Count', struct.pack('<i', 0)),
                                                       (3, 'Wait', struct.pack('<f', 2.5))], nodes=[
        dict(type=3, name='', plugs={2: [(1, '', b''), (3, '', b'')]}),
        dict(type=7, name='', props=[(0, 'CalculateTiming', 0, 0)], ins=[(1, 'Input', (2, 0), 0, False)],
             plugs={0: [(2, 'Input', b'\0' * 8)], 2: [(3, 'True', b''), (4, 'False', b'')]}, query=[0]),
        dict(type=0, name='QueryTestEqualS32', flags=1, ins=[(0, 'S32', None, BOUND | 0, 0), (0, 'S32Base', None, OWN, 5)],
             outs=[(1, 'Bool', True)]),
        dict(type=0, name='ExecuteTestSetS32', props=[(0, 'Mode', 0, 1), (1, 'ExecuteOnce', 0, True),
                                                      (3, 'BlackboardKey', 0, 'Count')],
             ins=[(0, 'S32', None, OWN | DEFAULT, 0)]),
        dict(type=0, name='QueryTestLength', flags=1, ins=[(4, 'Vector', None, OWN, (1.0, 2.0, 3.0))],
             outs=[(2, 'Length', False)]),
        dict(type=0, name='QueryTestLessF32', flags=1, ins=[(2, 'F32', None, OWN | DEFAULT, 0.0),
                                                           (2, 'F32Base', None, OWN, 0.5)], outs=[(1, 'Bool', False)]),
        dict(type=0, name='QueryTestPlayer', flags=1, outs=[(4, 'Velocity', False), (5, 'Data', False)]),
        dict(type=0, name='ExecuteTestMulti', ins=[(1, 'BoolMulti', ('multi', [(2, 0), (5, 0)]), 0, False),
                                                   (5, 'Target', None, OWN, ('game::Data', 0))], query=[0, 2]),
        dict(type=0, name='QueryTestEqualS32', flags=1, ins=[(0, 'S32', None, OWN, 1), (0, 'S32Base', None, OWN, 7)],
             outs=[(1, 'Bool', False)]),
    ], att=[('TestAttachment', b'\0' * 0x34)], exb=b'EXB ' + bytes(range(40)), mod=struct.pack('<IIII', 1, 0, 0, 1),
        action=struct.pack('<I', 0), link=struct.pack('<II', 0x1234, 0x5678), repl=bytes.fromhex('00000000ffffffff'),
        x6c=b'\0' * 4)
    tmpl = dict(name='Other.module', cmds=[], nodes=[
        dict(type=0, name='QueryTestFar', flags=1, props=[(2, 'Distance', 0, 4.0), (3, 'Kind', 0, 'Near')],
             ins=[(1, 'Enabled', (1, 0), 0, True)], outs=[(0, 'Value', False)]),
        dict(type=0, name='QueryTestEqualS32', flags=1, ins=[(0, 'S32', None, BOUND | 0, 0)], outs=[(1, 'Bool', True)]),
        dict(type=3, name='', plugs={2: [(0, '', b'')]}),
    ], mod=struct.pack('<I', 0), action=struct.pack('<I', 0), link=b'\0' * 8, repl=bytes.fromhex('00000000ffffffff'),
        x6c=b'\0' * 4)
    return g, tmpl


def graph_expected(g):
    """What tests/restore_check.c's edits make of graph_fixture()'s graph, decoded as ainb_decode does."""
    import copy
    n = copy.deepcopy(ainb_decode(ainb_encode(g))['nodes'])
    n[3]['props'][0] = (0, 'Mode', 0, 3)
    n[3]['props'][2] = (3, 'BlackboardKey', 0, 'Total')
    n[3]['ins'][0] = (0, 'S32', (11, 0), 0, 0)
    n[3]['query'] = [7]
    n[2]['ins'][1] = (0, 'S32Base', None, OWN, 9)
    n[6]['outs'][0] = (4, 'Velocity', True)
    length = copy.deepcopy(n[4])
    length['ins'][0] = (4, 'Vector', (6, 0), 0, (1.0, 2.0, 3.0))
    length['outs'][0] = (2, 'Length', True)
    length['query'] = [3]
    less = copy.deepcopy(n[5])
    less['ins'] = [(2, 'F32', (9, 0), 0, 0.0), (2, 'F32Base', None, OWN, 0.3)]
    less['outs'] = [(1, 'Bool', True)]
    less['query'] = [5]
    n[1]['ins'][0] = (1, 'Input', ('multi', [(10, 0), (8, 0)]), 0, False)
    n[1]['plugs'][0] = [(10, 'Input')]
    n[1]['query'] = [6, 4]
    n[7]['ins'][0] = (1, 'BoolMulti', ('multi', [(2, 0), (5, 0), (8, 0)]), 0, False)
    n[7]['query'] = [0, 2, 4]
    n[8]['outs'][0] = (1, 'Bool', True)
    far = dict(type=0, name='QueryTestFar', flags=1, props=[(2, 'Distance', 0, 4.0), (3, 'Kind', 0, 'Near')],
               ins=[(1, 'Enabled', None, OWN, True)], outs=[(0, 'Value', True)], plugs={}, query=[])
    for x in (length, less, far): x['plugs'], x['guid'] = {}, None
    return n + [length, less, far]


def game_graphs(game):
    """Every distinct logic graph of a built game copy's romfs (romfs/index.json, romfs.bin): loose ones and members
    of packs, unpacked as the game unpacks them."""
    import hashlib
    import json
    index = json.loads((game / 'romfs' / 'index.json').read_text())['files']
    at = {f['path']: (f['offset'], f['size']) for f in index}
    with open(game / 'romfs' / 'romfs.bin', 'rb') as rom:
        def read(path):
            rom.seek(at[path][0])
            return rom.read(at[path][1])
        dics = {}
        for _, (_, d) in sarc_decode(decompress(read('Pack/ZsDic.pack.zs'), {})).items():
            i, obj = load_dict(d)
            dics[i] = obj
        unpack = lambda d: decompress(d, dics) if d[:4] == b'\x28\xb5\x2f\xfd' else d
        seen = set()
        for path in sorted(at):
            if path.endswith('.ainb') or path.endswith('.ainb.zs'):
                found = [unpack(read(path))]
            elif path.endswith('.pack.zs'):
                try:
                    found = [b for n, (_, b) in sarc_decode(unpack(read(path))).items() if n.endswith('.ainb')]
                except (AssertionError, struct.error, ValueError):
                    continue
            else:
                continue
            for d in found:
                h = hashlib.sha1(d).digest()
                if h not in seen:
                    seen.add(h)
                    yield d


def graph_roundtrip(exe, env, game, check):
    """Each graph of the game written back unchanged must come out byte for byte the same; with a copy of one of its
    nodes added, every node, value, connection and section it had must be as it was (decoded by this script)."""
    p = subprocess.Popen([str(exe), '--graphs'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
    total = same = grown = nocopy = 0
    for d in game_graphs(game):
        total += 1
        p.stdin.write(struct.pack('<I', len(d)) + d)
        p.stdin.flush()
        head = p.stdout.read(5)
        if len(head) != 5:
            check(False, 'graph round trip: the checker stopped')
            break
        same += head[0]
        gn = struct.unpack_from('<I', head, 1)[0]
        g = p.stdout.read(gn)
        if not gn:
            nocopy += 1
            continue
        try:
            a, b = ainb_decode(d), ainb_decode(g)
            ok = b['nodes'][:-1] == a['nodes'] and all(b['sections'].get(k) == v for k, v in a['sections'].items()
                                                       if k != 'str') and (a['name'], a['cmds']) == (b['name'], b['cmds'])
            new = b['nodes'][-1]
            t = [n for n in a['nodes'] if n['name'] == new['name'] and n['type'] == new['type']]
            ok = ok and any(dict(n, guid=None, plugs={}, query=[], outs=[(k, nm, False) for k, nm, u in n['outs']],
                                 ins=[(k, nm, None, f if s is None else OWN, v) for k, nm, s, f, v in n['ins']])
                            == dict(new, guid=None) for n in t)
        except (AssertionError, struct.error, KeyError, ValueError, IndexError):
            ok = False
        grown += ok
    p.stdin.close()
    p.wait()
    print('graph round trip: %d graphs, %d written back byte for byte the same, %d of %d with a node added read '
          'back with all they had (%d have no node that can be copied)' % (total, same, grown, total - nocopy, nocopy))
    check(total and same == total and grown == total - nocopy, 'graph round trip: not every graph came back')


def build(cc, flags, env):
    exe = OUT / ('restore-check.exe' if os.name == 'nt' else 'restore-check')
    srcs = [ROOT / 'tests' / 'restore_check.c'] + [ROOT / 'runtime' / n
                                                   for n in ('restore.c', 'restore_engine.c', 'byml.c', 'sarc.c', 'msbt.c',
                                                             'ainb.c')]
    subprocess.run([cc, '-std=gnu11', '-O1', '-g', '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined' if os.name != 'nt' else '-O1',
                    '-I', str(ROOT / 'runtime'), *map(str, srcs), '-o', str(exe), *flags, '-lm'],
                   env=env, check=True)
    return exe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--toolchain', type=Path, help='an MSYS2 environment (Windows)')
    ap.add_argument('--game', type=Path, help='a built game copy: also read every logic graph of its romfs and write '
                                              'it back (unchanged, and with a node added)')
    a = ap.parse_args()
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    env = dict(os.environ)
    if a.toolchain:
        env['PATH'] = str(a.toolchain / 'bin') + os.pathsep + env.get('PATH', '')
        cc, flags = str(a.toolchain / 'bin' / 'clang.exe'), ['-L', str(a.toolchain / 'lib'), '-lzstd']
    else:
        cc = os.environ.get('CC') or shutil.which('clang') or 'cc'
        try:
            flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libzstd'], text=True))
        except (OSError, subprocess.CalledProcessError):
            flags = ['-lzstd']
    exe = build(cc, flags, env)

    # Two trained dictionaries in a SARC, as Pack/ZsDic.pack.zs holds the game's; the placement is packed with the
    # second, so the engine has to pick it by its id.
    rnd = random.Random(1)
    samples = [byml_encode({'Actors': [{'Gyaml': 'Obj_%d' % rnd.randrange(50), 'Hash': U64(rnd.getrandbits(64)),
                                        'Translate': [F32(rnd.random()) for _ in range(3)]} for _ in range(8)]})
               for _ in range(300)]
    d1, id1, c1 = train(samples[:150], 4096)
    d2, id2, c2 = train(samples[150:], 4096)
    assert id1 != id2 and id1 and id2
    zsdic = sarc_encode([('zs.zsdic', c1), ('bcett.byml.zsdic', c2)], {'zs.zsdic': 4, 'bcett.byml.zsdic': 4})
    (OUT / 'zsdic.sarc').write_bytes(zsdic)
    place = byml_encode(placement())
    (OUT / 'place.byml.zs').write_bytes(compress(place, d2))
    members, aligns = pack_members()
    pack = sarc_encode(members, aligns)
    (OUT / 'pack.zs').write_bytes(compress(pack))
    (OUT / 'junk.bin').write_bytes(b'YB\x07\x00' + b'\xff' * 60)
    (OUT / 'otherdic.zs').write_bytes(compress(place, train(samples[:100], 2048)[0]))
    (OUT / 'src.byml').write_bytes(byml_encode(source()))
    (OUT / 'area_Dynamic.bcett.byml.zs').write_bytes(compress(byml_encode(
        {'Actors': [{'Gyaml': 'Obj_D', 'Hash': U64(7001), 'Translate': [F32(0.0), F32(0.0), F32(0.0)]}]})))
    (OUT / 'compound.bphsc').write_bytes(b'static compound ' + struct.pack('<Q', 1003) + b' rest')
    x_member = byml_encode({'Liftable': True, 'Pos': [F32(0.0), F32(0.5), F32(0.0)]})
    (OUT / 'pack2.zs').write_bytes(compress(sarc_encode([('X/x.bgyml', x_member)], {'X/x.bgyml': 0x40})))
    # A text archive: two message files (UTF-8 with sections around the texts, UTF-16) and a BYML member.
    tm = text_messages()
    navi_msbt = msbt_encode(tm['Msg/Navi.msbt'], before=b'\x01\x02\x03\x04\x05', after=b'style')
    wide_msbt = msbt_encode(tm['Msg/Wide.msbt'], wide=True)
    text_members = [('Msg/Navi.msbt', navi_msbt), ('Msg/Wide.msbt', wide_msbt), ('A/one.bgyml', members[0][1])]
    text_aligns = {'Msg/Navi.msbt': 0x10, 'Msg/Wide.msbt': 0x10, 'A/one.bgyml': 8}
    (OUT / 'text.zs').write_bytes(compress(sarc_encode(text_members, text_aligns)))
    # Logic graphs: one in a pack beside a BYML member, the same loose, and the one nodes are copied from.
    gfix, tfix = graph_fixture()
    graph = ainb_encode(gfix)
    graph_members = [('AI/Test.module.ainb', graph), ('A/one.bgyml', members[0][1])]
    (OUT / 'graph.zs').write_bytes(compress(sarc_encode(graph_members, {'AI/Test.module.ainb': 8, 'A/one.bgyml': 8})))
    (OUT / 'graph.ainb').write_bytes(graph)
    (OUT / 'tmpl.zs').write_bytes(compress(sarc_encode([('AI/Other.module.ainb', ainb_encode(tfix))],
                                                       {'AI/Other.module.ainb': 8})))
    hashes = {(m, l): msbt_hash(b) for m, lst in tm.items() for l, b in lst}
    assert all(hashes[k] == v for k, v in TEXT_HASHES.items()), 'TEXT_HASHES are not the fixture messages\' hashes'

    r = subprocess.run([str(exe), str(OUT)], env=env, capture_output=True, text=True, timeout=120)
    print(r.stdout, end='')
    print(r.stderr, end='', file=sys.stderr)
    fails = [r.returncode != 0 and 'restore_check.c failed (exit %d)' % r.returncode]

    def check(cond, what):
        if not cond: fails.append(what)

    dics = {id1: d1, id2: d2}
    out = (OUT / 'place.out.zs').read_bytes()
    check(out[:4] == b'\x28\xb5\x2f\xfd', 'placement: not a zstd frame')
    check(same(byml_decode(decompress(out, dics)), placement_expected()),
          'placement: the edited tree is not the expected one')

    p = decompress((OUT / 'pack.out.zs').read_bytes(), {})
    m = sarc_decode(p)
    check(set(m) == {n for n, _ in members}, 'pack: members changed')
    check(same(byml_decode(m['A/one.bgyml'][1]), {'MoveSpeed': F32(0.15 * 15.71 / 9.68), 'Other': 3}), 'pack: A')
    check(same(byml_decode(m['C/three.bgyml'][1]), {'Params': {'Speed': F32(1.5), 'Count': 7, 'Flag': True}}),
          'pack: C')
    check(m['B/two.bin'][1] == dict(members)['B/two.bin'], 'pack: B lost its bytes')
    for n, (off, _) in m.items():
        check(off % aligns[n] == 0, 'pack: %s at %#x lost its alignment %#x' % (n, off, aligns[n]))

    g = sarc_decode((OUT / 'grown.sarc').read_bytes())
    check(g['A/one.bgyml'][1] == bytes(range(256)) * 3, 'grown pack: A has not the new bytes')
    for n in ('B/two.bin', 'C/three.bgyml'):
        check(g[n][1] == dict(members)[n], 'grown pack: %s lost its bytes' % n)
        check(g[n][0] % aligns[n] == 0, 'grown pack: %s at %#x lost its alignment %#x' % (n, g[n][0], aligns[n]))

    # the kinds that remove, copy, set text and add keys
    check(same(byml_decode(decompress((OUT / 'place2.out.zs').read_bytes(), dics)), placement_kinds_expected()),
          'placement, other kinds: the edited tree is not the expected one')
    m2 = sarc_decode(decompress((OUT / 'pack2.out.zs').read_bytes(), {}))
    old = dict(members)
    check(set(m2) == set(old) | {'D/new.bin', 'E/copy.bgyml', 'F/other.bgyml'}, 'pack, members added: the member set')
    for n in ('A/one.bgyml', 'B/two.bin'):
        check(m2[n][1] == old[n], 'pack, members added: %s lost its bytes' % n)
    for n in old:
        check(m2[n][0] % aligns[n] == 0, 'pack, members added: %s lost its alignment' % n)
    check(m2['D/new.bin'][1] == b'new\0\x01\x02\x03', 'pack, members added: the declared bytes')
    check(same(byml_decode(m2['E/copy.bgyml'][1]), {'Params': {'Speed': F32(1.5), 'Count': 8, 'Flag': True}}),
          'pack, members added: the copy from the same pack, then set')
    check(m2['F/other.bgyml'][1] == x_member, 'pack, members added: the copy from another pack')
    check(same(byml_decode(m2['C/three.bgyml'][1]),
               {'Params': {'Speed': F32(1.5), 'Count': 4, 'Flag': True, 'Name': 'liftable'}}), 'pack, members added: C')
    check(all(m2[n][0] % 8 == 0 for n in ('D/new.bin', 'E/copy.bgyml', 'F/other.bgyml')),
          'pack, members added: a new member not 8-aligned')
    check(m2['F/other.bgyml'][0] % 0x40 == 0, 'pack, members added: the copy lost its source alignment')
    g2 = sarc_decode((OUT / 'grown2.sarc').read_bytes())
    check(struct.unpack_from('<I', (OUT / 'grown2.sarc').read_bytes(), 12)[0] > struct.unpack_from('<I', pack, 12)[0],
          'long names: the data did not move (the test does not reach that path)')
    for n in old:
        check(g2[n][1] == old[n] and g2[n][0] % aligns[n] == 0, 'long names: %s lost its bytes or alignment' % n)
    longs = [n for n in g2 if n.startswith('Long/')]
    check(len(longs) == 6, 'long names: %d of 6 added' % len(longs))
    for n in longs:
        i = int(n.split('/')[1])
        check(g2[n][1] == b'new\0\x01\x02\x03' and g2[n][0] % (0x100 if i % 2 else 8) == 0, 'long names: ' + n)

    # messages replaced
    tx = sarc_decode(decompress((OUT / 'text.out.zs').read_bytes(), {}))
    check(set(tx) == {n for n, _ in text_members}, 'text archive: members changed')
    check(tx['A/one.bgyml'][1] == members[0][1], 'text archive: the BYML member lost its bytes')
    for n, (off, _) in tx.items():
        check(off % text_aligns[n] == 0, 'text archive: %s lost its alignment' % n)
    want = text_expected()
    for n, wide in (('Msg/Navi.msbt', 0), ('Msg/Wide.msbt', 1)):
        try:
            enc, labels, secs = msbt_decode(tx[n][1])
        except AssertionError as e:
            check(False, 'text archive: %s: %s' % (n, e))
            continue
        check(enc == wide, 'text archive: %s changed its encoding' % n)
        check(labels == want[n], 'text archive: %s: the messages are not the expected ones: %s' % (
            n, sorted(l for l in labels if labels[l] != want[n].get(l))))
        _, _, old_secs = msbt_decode(navi_msbt if n == 'Msg/Navi.msbt' else wide_msbt)
        for s in old_secs:
            check(s == 'TXT2' or secs.get(s) == old_secs[s], 'text archive: %s: section %s changed' % (n, s))

    # logic graphs
    before = ainb_decode(graph)
    gx = sarc_decode(decompress((OUT / 'graph.out.zs').read_bytes(), {}))
    check(set(gx) == {'AI/Test.module.ainb', 'A/one.bgyml'} and gx['A/one.bgyml'][1] == members[0][1],
          'graph pack: the other member changed')
    check(all(off % 8 == 0 for off, _ in gx.values()), 'graph pack: a member lost its alignment')
    try:
        after = ainb_decode(gx['AI/Test.module.ainb'][1])
        want = graph_expected(graph_fixture()[0])
        got = after['nodes']
        check(len(got) == len(want), 'graph: %d nodes, not %d' % (len(got), len(want)))
        for i, (x, y) in enumerate(zip(got, want)):
            if y['guid'] is None:
                x = dict(x, guid=None)
            check(x == y, 'graph: node %d is %r, not %r' % (i, x, y))
        check(len({n['guid'] for n in got}) == len(got), 'graph: two nodes share a GUID')
        for name, sec in before['sections'].items():
            grows = name in ('str', 'multi')   # new strings and multi-input lists go at their ends
            check(after['sections'][name][:len(sec)] == sec if grows else after['sections'].get(name) == sec,
                  'graph: section %s changed' % name)
        check((after['name'], after['cmds']) == (before['name'], before['cmds']), 'graph: name or commands changed')
        loose_after = ainb_decode((OUT / 'graph.out.ainb').read_bytes())
        lw = [dict(n) for n in before['nodes']]
        lw[8] = dict(lw[8], ins=[(0, 'S32', None, OWN, -4), (0, 'S32Base', None, OWN, 7)])
        check(loose_after['nodes'] == lw, 'loose graph: not the expected nodes')
    except (AssertionError, struct.error, KeyError, ValueError) as e:
        check(False, 'graph: the edited graph does not decode: %r' % e)

    if a.game:
        graph_roundtrip(exe, env, a.game, check)

    fails = [f for f in fails if f]
    for f in fails:
        print('FAIL', f)
    print('restore-check: %s' % ('FAILED' if fails else 'passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
