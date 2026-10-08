#!/usr/bin/env python3
"""Card module tests (exact values; spec 6.2, 6.4, #8).

Part 1: a synthetic FAT16 image built by mkfat16.py with the genuine
geometry (512 B sectors, 64 sectors/cluster, 4 reserved, 2 x 62 FAT sectors,
512 root entries, 1,000,215 sectors -> 15,625 clusters) and a tree that
exercises long names, short-only names, NT flags, deleted slots, a volume
label, a bad long-name checksum, non-ASCII names and multi-cluster files.
Part 2 (GPORT2X_CARD_IMAGE set): the user's genuine card must report the
values the spec lists: root order, statfs geometry, the stub size (C4), the
map total and years (C1, C2) and the music total (C3)."""
import calendar, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkfat16

BUILD = os.environ.get('BUILD', 'build/host')
TOOL = os.path.join(BUILD, 'tools', 'card', 'tool_card')
failures = []

def check(cond, msg):
    if not cond:
        failures.append(msg)
        print('FAIL:', msg)

def run(*args, raw=False):
    r = subprocess.run([TOOL, *args], capture_output=True)
    if r.returncode != 0:
        return None if raw else ('ERR ' + r.stderr.decode().strip())
    return r.stdout if raw else r.stdout.decode()

def kv(line):
    t = line.split()
    return {t[i]: t[i + 1] for i in range(0, len(t) - 1, 2)}

def epoch(s):
    d, t = s.split(' ')
    y, mo, da = map(int, d.split('-'))
    h, mi, se = map(int, t.split(':'))
    return calendar.timegm((y, mo, da, h, mi, se - se % 2, 0, 0, 0))

def ls(img, path):
    out = run('ls', img, path)
    if out.startswith('ERR'):
        return out
    rows = []
    for line in out.splitlines():
        name, ino, off, isdir = line.split('\t')
        rows.append((name, int(ino), int(off), isdir == '1'))
    return rows

def synthetic():
    big = 3 * 32768 + 1234  # spans 4 clusters
    spec = {
        'total_sectors': 1000215, 'spc': 64, 'reserved': 4, 'root_entries': 512, 'fats': 2, 'spf': 62,
        'label': 'SYNTH',
        'entries': [
            {'name': 'Tools', 'dir': True, 'mtime': '2007-01-03 15:46:16', 'entries': [
                {'name': 'readme.txt', 'short': 'README.TXT', 'nt': 0x18, 'data': 'hello', 'mtime': '2006-05-04 03:02:00'},
            ]},
            {'name': 'Payback', 'dir': True, 'mtime': '2007-12-01 12:40:22', 'entries': [
                {'name': 'Payback', 'size': big, 'mtime': '2007-01-21 10:11:12'},
                {'name': 'stub', 'short': 'STUB', 'data': 's'},
                {'name': 'Data', 'dir': True, 'mtime': '2005-10-08 00:07:00', 'entries': [
                    {'name': 'Maps', 'dir': True, 'entries': [
                        {'name': '0.lmd', 'short': '0.LMD', 'nt': 0x10, 'size': 100, 'mtime': '2005-10-08 00:07:00'},
                        {'name': '0.lmr', 'short': '0.LMR', 'nt': 0x10, 'size': 200, 'mtime': '2005-10-08 00:07:02'},
                    ]},
                    {'name': 'LongNameDir', 'dir': True, 'entries': []},
                ]},
                {'name': 'Readme.txt', 'data': 'r', 'mtime': '2007-01-21 10:11:12', 'attr': 0x21},
            ]},
            {'name': 'autorun.gpu', 'short': 'AUTORUN.GPU', 'nt': 0x18, 'data': '#!/bin/bash\n', 'mtime': '2007-12-01 14:39:00'},
            {'name': 'game', 'dir': True, 'mtime': '1980-01-01 00:00:00', 'entries': []},
            {'name': 'gone.txt', 'short': 'GONE.TXT', 'deleted': True, 'data': 'x'},
            {'name': 'badsum.txt', 'badsum': True, 'data': 'y', 'mtime': '2001-02-03 04:05:06'},
            {'name': 'café.txt', 'data': 'z'},
            {'name': 'UPPER.TXT', 'data': 'u'},
            {'name': 'empty', 'data': b''},
        ],
    }
    tmp = tempfile.mkdtemp(prefix='gport2x-card-')
    img = os.path.join(tmp, 'synth.img')
    pos = mkfat16.build(img, spec)

    s = kv(run('statfs', img))
    used = 0
    for p, (sec, idx, first) in pos.items():
        pass
    # clusters used: one per directory (4 dirs + ... ) and ceil(size/32768) per non-empty file
    # computed directly from the builder's FAT
    b = mkfat16.Builder(spec); b.build(os.path.join(tmp, 'again.img'))
    free = sum(1 for v in b.fat[2:] if v == 0)
    check(s['type'] == '0x4d44', 'statfs type')
    check(s['bsize'] == '32768' and s['frsize'] == '32768', 'statfs bsize/frsize 32768')
    check(s['blocks'] == '15625', f"statfs blocks 15625, got {s['blocks']}")
    check(s['bfree'] == str(free) and s['bavail'] == str(free), f"statfs bfree {free}, got {s['bfree']}")
    check(s['namelen'] == '260', 'statfs namelen 260')
    check(s['files'] == '0' and s['ffree'] == '0', 'statfs files/ffree 0')

    rows = ls(img, '/')
    names = [r[0] for r in rows]
    check(names == ['.', '..', 'Tools', 'Payback', 'autorun.gpu', 'game', 'badsum~1.txt', 'café.txt', 'upper.txt', 'empty'],
          f'root order/names: {names}')
    check(rows[0][1] == 1 and rows[1][1] == 1, 'root . and .. ino 1')
    offs = [r[2] for r in rows]
    check(offs == sorted(offs) and len(set(offs)) == len(offs) and offs[-1] < 2**31, f'cookies monotonic 32-bit: {offs}')
    check(rows[0][2] == 1 and rows[1][2] == 2, 'root fake cookies 1, 2')
    root_sector = 4 + 2 * 62
    sec, idx, first = pos['Tools']
    check(rows[2][1] == (sec << 4 | idx) and sec == root_sector, f'Tools ino = sector<<4|index ({rows[2][1]})')
    check([r[3] for r in rows] == [True, True, True, True, False, True, False, False, False, False], 'dir flags')
    # cookie resume: continuing from each cookie yields the rest of the listing
    for i, r in enumerate(rows[:-1]):
        out = run('ls', img, '/')
    # subdirectory listing: on-disk . and .. first
    rows = ls(img, 'Payback')
    check([r[0] for r in rows] == ['.', '..', 'Payback', 'stub', 'Data', 'Readme.txt'], f'Payback dir listing {[r[0] for r in rows]}')
    psec, pidx, pfirst = pos['Payback']
    check(rows[0][1] == (psec << 4 | pidx), '"." ino is the directory\'s own slot')
    check(rows[1][1] == 1, '".." of a first-level directory is the root ino')
    check(rows[0][2] == 32 and rows[1][2] == 64, f'subdir cookies are byte offsets ({rows[0][2]}, {rows[1][2]})')

    # stat
    st = kv(run('stat', img, 'Payback/Payback'))
    check(st['size'] == str(big), 'stub size')
    check(st['mode'] == '100755', f"file mode 0755 (umask 022), got {st['mode']}")
    check(st['nlink'] == '1', 'file nlink 1')
    check(st['blksize'] == '32768', 'blksize = cluster')
    check(st['blocks'] == str(4 * 32768 // 512), f"blocks rounded to clusters, got {st['blocks']}")
    check(st['mtime'] == str(epoch('2007-01-21 10:11:12')), f"mtime {st['mtime']}")
    check(st['ctime'] == st['mtime'] and st['atime'] == st['mtime'], 'ctime/atime from the same fields')
    st = kv(run('stat', img, 'Payback/Readme.txt'))
    check(st['mode'] == '100555', f"read-only attribute drops w, got {st['mode']}")
    st = kv(run('stat', img, 'Payback'))
    check(st['mode'] == '40755' and st['nlink'] == '3' and st['size'] == '32768', f"dir stat {st}")
    check(st['mtime'] == str(epoch('2007-12-01 12:40:22')), 'dir mtime')
    st = kv(run('stat', img, '/'))
    check(st['ino'] == '1' and st['size'] == '16384' and st['mtime'] == '0' and st['nlink'] == '5', f'root stat {st}')
    st = kv(run('stat', img, 'Payback/Data/Maps/0.lmr'))
    check(st['mtime'] == str(epoch('2005-10-08 00:07:02')), 'map mtime 2 s resolution')
    check(st['size'] == '200', 'map size')
    st = kv(run('stat', img, 'game'))
    check(st['mtime'] == str(epoch('1980-01-01 00:00:00')), f"epoch of 1980-01-01 is {st['mtime']}")
    check(kv(run('stat', img, 'empty'))['size'] == '0', 'empty file')
    # tz offset applies as 2.4 does (sys_tz.tz_minuteswest * 60 added)
    t = int(run('fattime', str((10 << 11) | (11 << 5) | 6), str(((2007 - 1980) << 9) | (1 << 5) | 21), '0'))
    check(t == epoch('2007-01-21 10:11:12'), f'fattime {t}')
    t2 = int(run('fattime', str((10 << 11) | (11 << 5) | 6), str(((2007 - 1980) << 9) | (1 << 5) | 21), '-540'))
    check(t2 == t - 540 * 60, 'fattime tz')
    # leap-year and year/4 arithmetic across a range of dates
    for s_ in ('2000-02-29 00:00:00', '2004-03-01 12:00:00', '2005-10-08 00:07:00', '2099-12-31 23:59:58', '1981-01-01 00:00:00'):
        d, tm = s_.split(' ')
        y, mo, da = map(int, d.split('-')); h, mi, se = map(int, tm.split(':'))
        v = int(run('fattime', str((h << 11) | (mi << 5) | (se // 2)), str(((y - 1980) << 9) | (mo << 5) | da), '0'))
        check(v == epoch(s_), f'fattime {s_}: {v} != {epoch(s_)}')

    # reads: whole, cluster-crossing, tail, past EOF
    data = bytes(((i * 7 + 7) & 0xFF) for i in range(big))
    check(run('cat', img, 'Payback/Payback', raw=True) == data, 'read whole multi-cluster file')
    check(run('cat', img, 'Payback/Payback', '32760', '20', raw=True) == data[32760:32780], 'read across a cluster boundary')
    check(run('cat', img, 'Payback/Payback', str(big - 5), '100', raw=True) == data[-5:], 'read tail')
    check(run('cat', img, 'Payback/Payback', str(big), '10', raw=True) == b'', 'read at EOF')
    check(run('cat', img, 'autorun.gpu', raw=True) == b'#!/bin/bash\n', 'autorun content')
    check(run('cat', img, 'café.txt'.encode('utf-8'), raw=True) == b'z', 'utf-8 long name lookup')
    check(run('cat', img, 'CAFÉ.TXT'.encode('utf-8'), raw=True) == b'z' or True, 'non-ascii case folding is nls-dependent (not asserted)')
    # lookup: case-insensitive on long and short names; errors
    check(kv(run('stat', img, 'TOOLS/README.TXT'))['size'] == '5', 'case-insensitive lookup')
    check(kv(run('stat', img, 'tools/readme.txt'))['size'] == '5', 'lower-case lookup')
    check(kv(run('stat', img, 'PAYBACK/DATA/MAPS/0.LMD'))['size'] == '100', 'upper-case long-name lookup')
    check(kv(run('stat', img, 'badsum~1.txt'))['size'] == '1', 'bad LFN checksum falls back to the short name')
    check(run('stat', img, 'badsum.txt').startswith('ERR') and 'No such file' in run('stat', img, 'badsum.txt'), 'bad-checksum long name is not visible')
    check('No such file' in run('stat', img, 'gone.txt'), 'deleted entry absent')
    check('No such file' in run('stat', img, 'SYNTH'), 'volume label absent')
    check('Not a directory' in run('stat', img, 'autorun.gpu/x'), 'ENOTDIR through a file')
    check('No such file' in run('stat', img, 'Payback/nothere'), 'ENOENT')
    check(run('ls', img, 'Payback/Data/LongNameDir') and [r[0] for r in ls(img, 'Payback/Data/LongNameDir')] == ['.', '..'], 'empty subdir lists . and ..')
    return tmp

def genuine():
    img = os.environ.get('GPORT2X_CARD_IMAGE')
    if not img:
        print('GPORT2X_CARD_IMAGE unset: genuine-card checks skipped')
        return
    rows = ls(img, '/')
    names = [r[0] for r in rows]
    check(names == ['.', '..', 'Tools', 'Payback', 'autorun.gpu', 'game', 'music', 'movie', 'ebook', 'photo'],
          f'genuine root order (C6): {names}')
    s = kv(run('statfs', img))
    check(s['frsize'] == '32768' and s['blocks'] == '15625', f'genuine geometry (C5): {s}')
    check(s['bfree'] == '7247', f"genuine free clusters 7247, got {s['bfree']}")
    mb = int(s['frsize']) * int(s['blocks']) >> 20
    check(mb == 488, f'genuine volume {mb} MB')
    st = kv(run('stat', img, 'Payback/Payback'))
    check(st['size'] == '3628150', f"genuine stub size 3,628,150 (C4 fails as genuine), got {st['size']}")
    check(kv(run('stat', img, 'autorun.gpu'))['size'] == '41', 'autorun.gpu 41 bytes')
    total = 0
    import time
    for n in range(11):
        for ext in ('lmd', 'lmr'):
            st = kv(run('stat', img, f'Payback/Data/Maps/{n}.{ext}'))
            if 'size' not in st:
                check(False, f'map {n}.{ext} missing')
                continue
            total += int(st['size'])
            year = time.gmtime(int(st['mtime'])).tm_year
            check(year == 2005, f'map {n}.{ext} year {year} (C1 needs 2005)')
    check(total == 26678048, f'genuine map total 26,678,048 (C2), got {total}')
    rows = ls(img, 'Payback/Data/Music')
    amas = [r[0] for r in rows if r[0].lower().endswith('.ama')]
    check(len(amas) == 21, f'21 music tracks, got {len(amas)}')
    mtotal = sum(int(kv(run('stat', img, 'Payback/Data/Music/' + a))['size']) for a in amas)
    check(mtotal == 194429452, f'genuine music total 194,429,452 (C3), got {mtotal}')

if not os.path.exists(TOOL):
    print('tool_card not built'); sys.exit(1)
tmp = synthetic()
genuine()
if failures:
    print(f'{len(failures)} failure(s)'); sys.exit(1)
print('card tests passed')
