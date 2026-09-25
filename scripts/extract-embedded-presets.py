#!/usr/bin/env python3
"""Extract factory presets compiled into JUCE plugin binaries (no preset files on disk) as states
Wavelength loads by name.

usage: scripts/extract-embedded-presets.py [plugin ...]      (no argument: try every installed plugin)

For each plugin: save its default state with `wavelength state save`, learn the shape of its XML
state, find same-shaped XML fragments in the bundle (plain, zipped or gzipped), and write each as
Presets/<Plugin>/<Category>/<Name>.wlstate in Wavelength's user folder (on macOS
~/Library/Application Support/Wavelength). `wavelength
presets <plugin>` then lists them. Known to work: TAL-NoiseMaker (256), Thump One (164), Wavetable
(99), Relica 2 (35), Flux Mini 2 (22). Needs only the Python standard library.
"""
import json, os, re, subprocess, sys, struct, tempfile, zlib, copy, hashlib
import xml.etree.ElementTree as ET
def comp_chunk(d):                                   # .vstpreset -> 'Comp' bytes (anything else: as is)
    if d[:4] != b'VST3': return d
    off = struct.unpack('<q', d[40:48])[0]
    for i in range(struct.unpack('<i', d[off+4:off+8])[0]):
        e = off + 8 + 20*i; o, s = struct.unpack('<qq', d[e+4:e+20])
        if d[e:e+4] == b'Comp': return d[o:o+s]
    return b''
def state_xml(c):                                    # -> (xml text, 'vc2' | 'text') or (None, None)
    i = c.find(b'VC2!')                              # copyXmlToBinary; may sit inside a VstW/CcnK/FBCh wrapper
    if i >= 0: return c[i+8:i+8+struct.unpack('<I', c[i+4:i+8])[0]].split(b'\0')[0].decode(), 'vc2'
    i = c.find(b'<?xml')                             # bare XML text (gin-based plugins)
    if i < 0: return None, None
    x = c[i:].split(b'\0')[0]; return x[:x.rfind(b'>')+1].decode('utf-8', 'replace'), 'text'

LAX = re.compile(r'(\s)(\d[\w.-]*|[A-Za-z_][\w.-]*:[\w.-]+)=')   # JUCE allows names ElementTree rejects
lax = lambda t: LAX.sub(lambda m: m.group(1) + 'x__' + m.group(2).replace(':', '__c__') + '=', t)
unlax = lambda t: re.sub(r'(\s)x__([\w.-]+)=', lambda m: m.group(1) + m.group(2).replace('__c__', ':') + '=', t)
def encode(root, kind):
    body = ('<?xml version="1.0" encoding="UTF-8"?>\n' + unlax(ET.tostring(root, encoding='unicode'))).encode()
    return body if kind == 'text' else b'VC2!' + struct.pack('<I', len(body)) + body + b'\0'
def sig(el):                                         # shape: attribute names, child tags, child id/uid/name keys
    s = {'@' + a for a in el.attrib}
    for c in el:
        s.add('<' + c.tag); k = c.get('id') or c.get('uid') or c.get('name')
        if k: s.add('#' + k)
    return s
def blobs_of(bundle):                                # every file in MacOS + Resources, plus embedded zip / gzip members
    out = []
    for sub in ('Contents/MacOS', 'Contents/Resources'):
        for dp, _, fs in os.walk(os.path.join(bundle, sub)):
            out += [open(os.path.join(dp, f), 'rb').read() for f in fs if not os.path.islink(os.path.join(dp, f))]
    for d in list(out):
        for m in re.finditer(rb'PK\x03\x04', d):
            o = m.start(); _, _, meth, _, _, _, csz, _, nl, xl = struct.unpack('<HHHHHIIIHH', d[o+4:o+30]); p = o + 30 + nl + xl
            try: out.append(zlib.decompressobj(-15).decompress(d[p:p + (csz or 50 << 20)], 50 << 20) if meth == 8 else d[p:p+csz])
            except Exception: pass
        for m in re.finditer(rb'\x1f\x8b\x08', d):
            try: out.append(zlib.decompressobj(31).decompress(d[m.start():m.start() + (50 << 20)], 50 << 20))
            except Exception: pass
    return out
def fragments(data, tag):                            # (offset, element) for each well-formed <tag ...> element
    t = tag.encode()
    for m in re.finditer(b'<' + re.escape(t) + rb'[\s/>]', data):
        s = m.start(); gt = data.find(b'>', s); ends, pos = [], gt
        if gt < 0: continue
        if data[gt-1:gt] == b'/': ends = [gt + 1]
        else:
            for _ in range(8):                       # nested same-name elements: try successive closing tags
                e = data.find(b'</' + t + b'>', pos)
                if e < 0 or e - s > 2 << 20: break
                ends.append(e + len(t) + 3); pos = e + 1
        for end in ends:
            if b'\0' in data[s:end]: break
            try: yield s, ET.fromstring(lax(data[s:end].decode('utf-8'))); break
            except Exception: pass
def scan(xml, bundle, min_share=0.6):
    """learn the default state's shape, find same-shaped elements in the bundle (outermost element that has >= 2)"""
    root = ET.fromstring(lax(xml)); blobs = blobs_of(bundle); order, q, seen = [], [root], set()
    while q: el = q.pop(0); order.append(el); q += list(el)
    for el in order:
        ref = sig(el)
        if el.tag in seen or len(ref) < 3: continue
        seen.add(el.tag); found = {}
        for bi, b in enumerate(blobs):
            for off, f in fragments(b, el.tag) if b'<' + el.tag.encode() in b else ():
                s = sig(f); common = len(ref & s)    # most of the fragment's keys are known to the default state
                if common >= 2 and common / len(s) >= min_share:
                    found.setdefault(hashlib.md5(ET.tostring(f)).digest(), (bi, off, f))
        if len(found) >= 2:
            hits = sorted(found.values(), key=lambda h: h[:2])
            return root, el, [f for *_, f in hits], [(blobs[bi], off) for bi, off, _ in hits]
    return root, None, [], []
def binarydata_name(d, off):
    """JUCE BinaryData original filename of the resource containing file offset `off` (Mach-O, unstripped locals)"""
    heads = [struct.unpack('>5I', d[8+20*i:28+20*i])[2] for i in range(struct.unpack('>I', d[4:8])[0])] if d[:4] == b'\xca\xfe\xba\xbe' else [0]
    base = max([h for h in heads if h <= off] or [0])
    if struct.unpack('<I', d[base:base+4])[0] != 0xfeedfacf: return None
    p, segs, symtab = base + 32, [], None
    for _ in range(struct.unpack('<I', d[base+16:base+20])[0]):
        cmd, size = struct.unpack('<II', d[p:p+8])
        if cmd == 0x19: segs.append(struct.unpack('<4Q', d[p+24:p+56]))   # vmaddr vmsize fileoff filesize
        if cmd == 0x2: symtab = struct.unpack('<4I', d[p+8:p+24])        # symoff nsyms stroff strsize
        p += size
    v2f = lambda a: next((base + fo + a - va for va, _, fo, fs in segs if va <= a < va + fs), None)
    if not symtab: return None
    res, names = {}, None
    for i in range(symtab[1]):
        strx, _, _, _, val = struct.unpack('<IBBHQ', d[base+symtab[0]+16*i:base+symtab[0]+16*i+16])
        s0 = base + symtab[2] + strx; n = d[s0:d.index(b'\0', s0)]
        m = re.match(rb'__ZN10BinaryDataL\d+temp_binary_data_(\d+)E$', n)
        if m: res[int(m.group(1))] = v2f(val)
        elif n == b'__ZN10BinaryData17originalFilenamesE': names = v2f(val)
    starts = [(o, k) for k, o in res.items() if o is not None and o <= off]
    if names is None or not starts: return None
    k = max(starts)[1]; ptr = v2f(struct.unpack('<Q', d[names+8*k:names+8*k+8])[0] & 0xFFFFFFFFF)  # chained-fixup target
    return d[ptr:d.index(b'\0', ptr)].decode('utf-8', 'replace').rsplit('.', 1)[0] if ptr else None
def own_name(f):
    for k, v in f.attrib.items():
        if re.search('name|preset', k, re.I) and v: return os.path.splitext(os.path.basename(v))[0]
def build(root, target, frag, kind):                 # default tree with `target` replaced by the fragment
    if target is root: return encode(copy.deepcopy(frag), kind)
    path, el = [], target
    parents = {c: p for p in root.iter() for c in p}
    while el is not root: path.insert(0, list(parents[el]).index(el)); el = parents[el]
    new = copy.deepcopy(root); p = new
    for i in path[:-1]: p = p[i]
    p[path[-1]] = copy.deepcopy(frag); return encode(new, kind)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WAVELENGTH = os.path.join(ROOT, 'build', 'wavelength')
def data_dir():                                      # Wavelength's user folder (platform::dataDir)
    if sys.platform == 'darwin': return os.path.expanduser('~/Library/Application Support/Wavelength')
    if os.name == 'nt': return os.path.join(os.environ.get('APPDATA', ''), 'Wavelength')
    return os.path.join(os.environ.get('XDG_CONFIG_HOME') or os.path.expanduser('~/.config'), 'wavelength')
LIBRARY = os.path.join(data_dir(), 'Presets')

def category_and_name(n, frag):
    """split a preset label into (category, name): "Bass--Deep Sub", "KB Piano House TAL", tags"""
    if '--' in n: c, _, rest = n.partition('--'); return c.strip(), rest.strip()
    m = re.match(r'^([A-Z]{2,3}) (.+)$', n)
    if m: return m.group(1), n                        # TAL: "LD 77 ..." keeps its prefix in the name
    tags = frag.get('tags', '').split()
    return (tags[0] if tags else 'Factory'), n

def extract(plugin):
    info = json.loads(subprocess.run([WAVELENGTH, 'plugins', '--json'], capture_output=True, text=True).stdout)['plugins']
    hit = [p for p in info if p['name'].lower() == plugin.lower() or p['id'] == plugin]
    hit = sorted(hit, key=lambda p: p['format'] != 'vst3')   # the VST3 when a plugin ships both
    if not hit or not hit[0].get('bundle'): return print(f'{plugin}: not installed')
    p = hit[0]
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'default' + ('.vstpreset' if p['format'] == 'vst3' else '.clap-preset'))
        r = subprocess.run([WAVELENGTH, 'state', 'save', f"{p['format']}:{p['name']}", '--out', out], capture_output=True, text=True)
        if r.returncode: return print(f"{p['name']}: cannot save its default state")
        data = open(out, 'rb').read()
        if data[:4] == b'clap': data = data[8 + struct.unpack('>I', data[4:8])[0]:]
        xml, kind = state_xml(comp_chunk(data))
    if xml is None: return print(f"{p['name']}: state is not XML, skipped")
    try: root, target, frags, locs = scan(xml, p['bundle'])
    except Exception as e: return print(f"{p['name']}: scan failed ({e})")
    if not frags: return print(f"{p['name']}: no embedded presets")
    files = [binarydata_name(b, o) for b, o in locs]
    names = files if all(files) and len(set(files)) == len(files) else [own_name(f) or '#%d' % i for i, f in enumerate(frags)]
    dest = os.path.join(LIBRARY, p['name'])
    count = 0
    for f, n in zip(frags, names):
        cat, name = category_and_name(n, f)
        d = os.path.join(dest, re.sub(r'[/:]', '_', cat))
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, re.sub(r'[/:]', '_', name)[:80] + '.wlstate'), 'wb').write(build(root, target, f, kind))
        count += 1
    print(f"{p['name']}: {count} presets -> {dest}")

if __name__ == '__main__':
    names = sys.argv[1:]
    if not names:
        info = json.loads(subprocess.run([WAVELENGTH, 'plugins', '--json'], capture_output=True, text=True).stdout)['plugins']
        names = sorted({p['name'] for p in info if p['format'] in ('clap', 'vst3')})
    for n in names: extract(n)
