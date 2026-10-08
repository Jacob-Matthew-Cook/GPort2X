#!/usr/bin/env python3
"""Builds a synthetic FAT16 image for the card tests (no real card data).

build(path, spec) writes the image. spec:
  total_sectors, spc, reserved, root_entries, fats, spf, label (optional),
  entries: [entry, ...] in the exact on-disk order wanted, where entry is
    {"name": str, "dir": bool, "mtime": "YYYY-MM-DD HH:MM:SS", "ctime": ...,
     "data": bytes | "size": int (pattern-filled), "short": "NAME.EXT" (use
     only a short slot), "nt": int (NT flags), "attr": int, "entries": [...],
     "deleted": bool (write it as a deleted slot), "badsum": bool (write LFN
     slots with a wrong checksum)}
Returns a dict of path -> (sector, index, first_cluster) of each short slot.
"""
import struct, sys, json, os

def fat_datetime(s):
    if not s:
        return 0, 0
    d, t = s.split(' ')
    y, mo, da = map(int, d.split('-'))
    h, mi, se = map(int, t.split(':'))
    return (h << 11) | (mi << 5) | (se // 2), ((y - 1980) << 9) | (mo << 5) | da

def short_alias(name, used):
    base, ext = (name.rsplit('.', 1) + [''])[:2] if '.' in name else (name, '')
    def clean(s):
        return ''.join(ch for ch in s.upper() if (ch.isalnum() and ch.isascii()) or ch in '_-')[:8] or 'FILE'
    b, e = clean(base)[:6], clean(ext)[:3]
    for i in range(1, 100):
        cand = f'{b}~{i}'
        full = cand.ljust(8) + e.ljust(3)
        if full not in used:
            used.add(full)
            return full.encode('ascii')
    raise RuntimeError('too many aliases')

def needs_lfn(name):
    if name != name.upper() or ' ' in name:
        return True
    base, ext = (name.rsplit('.', 1) + [''])[:2] if '.' in name else (name, '')
    if len(base) > 8 or len(ext) > 3 or '.' in base:
        return True
    return not all(ch.isalnum() or ch in '_-' for ch in base + ext)

def checksum(short11):
    s = 0
    for b in short11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s

class Builder:
    def __init__(self, spec):
        self.bps = 512
        self.spc = spec.get('spc', 64)
        self.reserved = spec.get('reserved', 4)
        self.fats = spec.get('fats', 2)
        self.root_entries = spec.get('root_entries', 512)
        self.spf = spec.get('spf', 62)
        self.total = spec['total_sectors']
        self.root_sector = self.reserved + self.fats * self.spf
        self.root_sectors = self.root_entries * 32 // self.bps
        self.data_sector = self.root_sector + self.root_sectors
        self.clusters = (self.total - self.data_sector) // self.spc
        self.cluster_bytes = self.bps * self.spc
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0xFFF8
        self.fat[1] = 0xFFFF
        self.next_free = 2
        self.writes = []  # (byte offset, bytes)
        self.positions = {}
        self.spec = spec

    def alloc(self, nbytes):
        n = max(1, (nbytes + self.cluster_bytes - 1) // self.cluster_bytes)
        first = self.next_free
        for i in range(n):
            cl = first + i
            self.fat[cl] = cl + 1 if i < n - 1 else 0xFFFF
        self.next_free += n
        return first, n

    def cluster_off(self, cl):
        return (self.data_sector + (cl - 2) * self.spc) * self.bps

    def file_data(self, e):
        if 'data' in e:
            d = e['data']
            return d if isinstance(d, bytes) else d.encode('utf-8')
        size = e.get('size', 0)
        # deterministic pattern: byte i = (i * 7 + len(name)) & 0xFF
        k = len(e['name'])
        return bytes(((i * 7 + k) & 0xFF) for i in range(size))

    def slots_for(self, e, used, parent_path):
        name = e['name']
        attr = e.get('attr', 0x10 if e.get('dir') else 0x20)
        if 'short' in e:
            sn = e['short']
            b, x = (sn.split('.') + [''])[:2]
            short11 = (b.ljust(8) + x.ljust(3)).encode('ascii')
            used.add(short11.decode())
            lfn = None
        elif needs_lfn(name):
            short11 = short_alias(name, used)
            lfn = name
        else:
            b, x = (name.split('.') + [''])[:2]
            short11 = (b.ljust(8) + x.ljust(3)).encode('ascii')
            used.add(short11.decode())
            lfn = None
        slots = []
        if lfn:
            units = lfn.encode('utf-16le')
            units += b'\x00\x00'
            while len(units) % 26:
                units += b'\xff\xff'
            n = len(units) // 26
            cs = checksum(short11) ^ (0x55 if e.get('badsum') else 0)
            for i in range(n - 1, -1, -1):
                chunk = units[i * 26:(i + 1) * 26]
                seq = (i + 1) | (0x40 if i == n - 1 else 0)
                slots.append(bytes([seq]) + chunk[0:10] + bytes([0x0F, 0, cs]) + chunk[10:22] + b'\x00\x00' + chunk[22:26])
        t, d = fat_datetime(e.get('mtime', '2005-10-08 00:07:00'))
        ct, cd = fat_datetime(e.get('ctime', e.get('mtime', '2005-10-08 00:07:00')))
        return slots, short11, attr, (t, d, ct, cd)

    def write_dir(self, entries, parent_path, is_root, self_cluster=0, parent_cluster=0):
        used = set()
        raw = b''
        pending = []  # (slot index of short entry, entry dict, data bytes)
        if is_root and self.spec.get('label'):
            lab = self.spec['label'].ljust(11).encode('ascii')
            t, d = fat_datetime(self.spec.get('label_mtime', '2007-01-03 15:46:12'))
            raw += lab + bytes([0x08, 0, 0]) + struct.pack('<HHHHHHHI', 0, 0, 0, 0, t, d, 0, 0)
        if not is_root:
            t, d = fat_datetime('2005-10-08 00:07:00')
            for nm, cl in (('.', self_cluster), ('..', parent_cluster)):
                raw += nm.ljust(11).encode() + bytes([0x10, 0, 0]) + struct.pack('<HHHHHHHI', t, d, d, 0, t, d, cl, 0)
        for e in entries:
            slots, short11, attr, (t, d, ct, cd) = self.slots_for(e, used, parent_path)
            if e.get('dir'):
                first, _ = self.alloc(self.cluster_bytes)  # one cluster for the directory itself (grown below)
                data = None
            else:
                data = self.file_data(e)
                first = 0
                if data:
                    first, _ = self.alloc(len(data))
            for s in slots:
                raw += s
            idx = len(raw) // 32
            size = 0 if e.get('dir') else len(data)
            short = bytearray(short11 + bytes([attr, e.get('nt', 0), 0]) + struct.pack('<HHHHHHHI', ct, cd, d, 0, t, d, first & 0xFFFF, size))
            if e.get('deleted'):
                short[0] = 0xE5
            raw += bytes(short)
            path = (parent_path + '/' + e['name']) if parent_path else e['name']
            pending.append((idx, e, data, first, path))
        return raw, pending

    def place_dir(self, raw, pending, base_off, base_sector, path_prefix):
        self.writes.append((base_off, raw))
        for idx, e, data, first, path in pending:
            sector = base_sector + (idx * 32) // self.bps
            self.positions[path] = (sector, idx % 16, first)
            if e.get('dir'):
                sub_raw, sub_pending = self.write_dir(e.get('entries', []), path, False, first,
                                                      0 if base_sector == self.root_sector else self.dir_cluster_of[path_prefix])
                if len(sub_raw) > self.cluster_bytes:
                    raise RuntimeError('directory larger than one cluster not supported by the builder')
                self.dir_cluster_of[path] = first
                self.place_dir(sub_raw, sub_pending, self.cluster_off(first), self.cluster_off(first) // self.bps, path)
            elif data:
                self.writes.append((self.cluster_off(first), data))

    def build(self, path):
        self.dir_cluster_of = {'': 0}
        raw, pending = self.write_dir(self.spec['entries'], '', True)
        if len(raw) > self.root_entries * 32:
            raise RuntimeError('root directory overflow')
        self.place_dir(raw, pending, self.root_sector * self.bps, self.root_sector, '')
        # boot sector
        bs = bytearray(512)
        bs[0:3] = b'\xEB\x3C\x90'
        bs[3:11] = b'MSDOS5.0'
        struct.pack_into('<HBHBHHBHHHII', bs, 11, self.bps, self.spc, self.reserved, self.fats, self.root_entries,
                         0 if self.total > 0xFFFF else self.total, 0xF8, self.spf, 63, 255, 0,
                         self.total if self.total > 0xFFFF else 0)
        bs[36] = 0x80
        bs[38] = 0x29
        struct.pack_into('<I', bs, 39, 0x12345678)
        bs[43:54] = b'NO NAME    '
        bs[54:62] = b'FAT16   '
        bs[510] = 0x55
        bs[511] = 0xAA
        fat = b''.join(struct.pack('<H', v) for v in self.fat)
        with open(path, 'wb') as f:
            f.truncate(self.total * self.bps)
            f.seek(0)
            f.write(bs)
            for i in range(self.fats):
                f.seek((self.reserved + i * self.spf) * self.bps)
                f.write(fat)
            for off, data in self.writes:
                f.seek(off)
                f.write(data)
        return self.positions

def build(path, spec):
    return Builder(spec).build(path)

if __name__ == '__main__':
    spec = json.load(open(sys.argv[2]))
    print(json.dumps(build(sys.argv[1], spec)))
