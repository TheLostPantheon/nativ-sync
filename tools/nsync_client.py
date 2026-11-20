#!/usr/bin/env python3
"""nsync_client: a reference client for the Nativ Sync protocol (PROTOCOL.md), for trying a Vita's service from a
computer. The protocol's crypto comes from common/nsync_proto.c itself (built here as a shared library over
Monocypher), so this speaks exactly what the Vita and Nativ do.

    nsync_client.py find
    nsync_client.py pair <vita address> <code>
    nsync_client.py hello|list <vita address>
    nsync_client.py manifest <vita address> <TITLE ID>
    nsync_client.py pull <vita address> <TITLE ID> <folder>     the save's files and slots into folder/files, folder/slots
    nsync_client.py push <vita address> <TITLE ID> <folder>     folder's save onto the Vita (stage, commit)
    nsync_client.py events <vita address> [seconds]

Pairings are kept in $NSYNC_CLIENT_HOME (default ~/.nsync-client): this computer's id and each Vita's key.
"""
import ctypes, hashlib, json, os, secrets, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = os.environ.get('NSYNC_CLIENT_HOME', os.path.expanduser('~/.nsync-client'))
PORT = 1340
PLAIN_MAX = 128 << 10
CHUNK = PLAIN_MAX - 64

def lib():
    out = os.path.join(ROOT, 'build', 'host', 'libnsync' + ('.dylib' if sys.platform == 'darwin' else '.so'))
    srcs = [os.path.join(ROOT, 'common', 'nsync_proto.c'), os.path.join(ROOT, 'third_party', 'monocypher', 'monocypher.c')]
    if not os.path.exists(out) or any(os.path.getmtime(s) > os.path.getmtime(out) for s in srcs):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        subprocess.check_call(['cc', '-O2', '-shared', '-fPIC', '-I' + os.path.join(ROOT, 'third_party', 'monocypher'), '-o', out] + srcs)
    L = ctypes.CDLL(out)
    L.nsync_seal.restype = ctypes.c_size_t
    L.nsync_open.restype = ctypes.c_long
    L.nsync_frame_length.restype = ctypes.c_uint32
    return L
L = None

def buf(n): return ctypes.create_string_buffer(n)

def identity():
    os.makedirs(HOME, exist_ok=True)
    p = os.path.join(HOME, 'id.json')
    if not os.path.exists(p):
        json.dump({'pc_id': secrets.token_hex(8), 'name': socket.gethostname().split('.')[0] or 'Computer'}, open(p, 'w'))
    return json.load(open(p))

def pairings():
    p = os.path.join(HOME, 'pairs.json')
    return json.load(open(p)) if os.path.exists(p) else {}

def save_pairing(addr, console_id, name, key):
    ps = pairings(); ps[addr] = {'console_id': console_id, 'name': name, 'key': key.hex()}
    json.dump(ps, open(os.path.join(HOME, 'pairs.json'), 'w'), indent=1)
    os.chmod(os.path.join(HOME, 'pairs.json'), 0o600)

def find(timeout=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1); s.settimeout(0.3)
    s.sendto(b'NSYNC1?', ('255.255.255.255', PORT)); found = {}; t0 = time.time()
    while time.time() - t0 < timeout:
        try: data, (addr, _) = s.recvfrom(256)
        except socket.timeout: continue
        parts = data.decode(errors='replace').split(' ', 2)
        if parts[0] == 'NSYNC1' and len(parts) == 3: found[addr] = (parts[1], parts[2])
    return found

class Lines:
    def __init__(self, sock): self.s = sock; self.b = b''
    def line(self):
        while b'\n' not in self.b:
            d = self.s.recv(4096)
            if not d: raise ConnectionError('closed')
            self.b += d
        l, self.b = self.b.split(b'\n', 1)
        return l.decode()
    def send(self, t): self.s.sendall(t.encode() + b'\n')

def pair(addr, code):
    global L; L = L or lib()
    me = identity()
    s = socket.create_connection((addr, PORT), timeout=15); c = Lines(s)
    pcn = secrets.token_bytes(16)
    c.send(f'NSYNC1 PAIR {me["pc_id"]} {pcn.hex()} {me["name"]}')
    r = c.line()
    if not r.startswith('NSYNC1 PAIR '): raise RuntimeError(r)
    vn = bytes.fromhex(r.split()[2])
    p = buf(128); L.nsync_pair_start(p, pcn, vn, code.encode()[:6], secrets.token_bytes(32))
    c.send(p.raw[96:128].hex())                       # A (NsyncPair.mine)
    b = bytes.fromhex(c.line()); tag_v = bytes.fromhex(c.line())
    pk, cf, tr = buf(32), buf(32), buf(96)
    if L.nsync_pair_finish(p, 0, b, pk, cf, tr): raise RuntimeError('bad point')
    expect = buf(32); L.nsync_pair_tag(expect, cf, b'vita', tr)
    if expect.raw != tag_v: s.close(); raise RuntimeError('wrong code (or someone in the middle)')
    mine = buf(32); L.nsync_pair_tag(mine, cf, b'pc', tr); c.send(mine.raw.hex())
    r = c.line()
    if not r.startswith('OK '): raise RuntimeError(r)
    _, console_id, name = r.split(' ', 2)
    save_pairing(addr, console_id, name, pk.raw)
    return console_id, name

class Session:
    def __init__(self, addr):
        global L; L = L or lib()
        me = identity(); pr = pairings().get(addr)
        if not pr: raise RuntimeError(f'not paired with {addr}')
        self.s = socket.create_connection((addr, PORT), timeout=30); c = Lines(self.s)
        pcn = secrets.token_bytes(16)
        c.send(f'NSYNC1 HELLO {me["pc_id"]} {pcn.hex()}')
        r = c.line()
        if not r.startswith('NSYNC1 HELLO '): raise RuntimeError(r)
        vn = bytes.fromhex(r.split()[2])
        self.ses = buf(80); L.nsync_session(self.ses, bytes.fromhex(pr['key']), pcn, vn, 0)
        self.out = buf(PLAIN_MAX + 64); self.inb = buf(PLAIN_MAX + 64); self.plain = buf(PLAIN_MAX + 64)
        self.hello = self.json(f'HELLO {me["name"]}')
    def _recv(self, n):
        b = b''
        while len(b) < n:
            d = self.s.recv(n - len(b))
            if not d: raise ConnectionError('closed')
            b += d
        return b
    def request(self, line, payload=b''):
        msg = line.encode() + (b'\n' + payload if payload else b'')
        n = L.nsync_seal(self.ses, self.out, msg, len(msg)); self.s.sendall(self.out.raw[:n])
        head = self._recv(4); blen = int.from_bytes(head, 'big'); body = self._recv(blen)
        ctypes.memmove(self.inb, body, blen)
        pn = L.nsync_open(self.ses, self.plain, self.inb, blen)
        if pn < 0: raise RuntimeError('a reply frame did not open')
        r = self.plain.raw[:pn]
        status, _, rest = r.partition(b'\n')
        if status != b'OK': raise RuntimeError(status.decode(errors='replace'))
        return rest
    def json(self, line, payload=b''): return json.loads(self.request(line, payload) or b'{}')

# ---- saves on this computer: folder/files/<path>, folder/slots/<n> (0x34C bytes) ----
def blake(b): return hashlib.blake2b(b, digest_size=32).hexdigest()
def local_manifest(folder):
    files, slots = [], []
    fdir = os.path.join(folder, 'files'); sdir = os.path.join(folder, 'slots')
    for dp, _, fs in os.walk(fdir):
        for f in fs:
            p = os.path.join(dp, f); rel = os.path.relpath(p, fdir).replace(os.sep, '/'); data = open(p, 'rb').read()
            files.append((rel, len(data), blake(data)))
    if os.path.isdir(sdir):
        for f in os.listdir(sdir):
            if f.isdigit(): slots.append((int(f), blake(open(os.path.join(sdir, f), 'rb').read())))
    files.sort(key=lambda x: x[0].encode()); slots.sort()
    text = ''.join(f'F {p}\t{n}\t{h}\n' for p, n, h in files) + ''.join(f'S {n}\t{h}\n' for n, h in slots)
    return text, blake(text.encode()), files, slots

def pull(ses, tid, folder):
    m = ses.json(f'MANIFEST {tid}')
    for f in m['files']:
        data = b''
        while len(data) < f['size']: data += ses.request(f'GET {tid} {f["path"]} {len(data)} {min(CHUNK, f["size"] - len(data))}')
        assert blake(data) == f['hash'], f['path']
        p = os.path.join(folder, 'files', *f['path'].split('/')); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, 'wb').write(data)
    for s in m['slots']:
        data = ses.request(f'GET {tid} slot/{s["slot"]} 0 {0x34C}')
        assert blake(data) == s['hash'], s['slot']
        p = os.path.join(folder, 'slots', str(s['slot'])); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, 'wb').write(data)
    return m

def push(ses, tid, folder):
    text, h, files, slots = local_manifest(folder)
    there = ses.json(f'MANIFEST {tid}')
    have = {f['path']: f['hash'] for f in there['files']}; have_s = {s['slot']: s['hash'] for s in there['slots']}
    sent = 0
    for p, n, fh in files:
        if have.get(p) == fh: continue
        data = open(os.path.join(folder, 'files', *p.split('/')), 'rb').read()
        for off in range(0, max(len(data), 1), CHUNK): ses.request(f'PUT {tid} {p} {off} {len(data)}', data[off:off + CHUNK]); sent += 1
    for n, sh in slots:
        if have_s.get(n) == sh: continue
        ses.request(f'PUT {tid} slot/{n} 0 {0x34C}', open(os.path.join(folder, 'slots', str(n)), 'rb').read()); sent += 1
    r = ses.json(f'COMMIT {tid} {h}', text.encode())
    return h, sent, r

def main(a):
    if not a: print(__doc__); return
    cmd = a[0]
    if cmd == 'find': print(json.dumps(find(), indent=1)); return
    if cmd == 'pair': print('paired:', pair(a[1], a[2])); return
    ses = Session(a[1])
    if cmd == 'hello': print(json.dumps(ses.hello, indent=1))
    elif cmd == 'list': print(json.dumps(ses.json('LIST'), indent=1, ensure_ascii=False))
    elif cmd == 'manifest': print(json.dumps(ses.json(f'MANIFEST {a[2]}'), indent=1, ensure_ascii=False))
    elif cmd == 'pull': m = pull(ses, a[2], a[3]); print('pulled', len(m['files']), 'files,', len(m['slots']), 'slots, hash', m['hash'])
    elif cmd == 'push': print('pushed: hash %s, %d frames, %s' % push(ses, a[2], a[3]))
    elif cmd == 'events':
        t_end = time.time() + float(a[2] if len(a) > 2 else 30)
        while time.time() < t_end: print(json.dumps(ses.json('EVENTS 5000')))
    else: print(__doc__)

if __name__ == '__main__':
    main(sys.argv[1:])
