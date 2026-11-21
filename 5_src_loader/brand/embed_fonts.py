import base64, re, urllib.request, pathlib

brand = pathlib.Path(r'F:\Game Dev\Rob Run\5_src_loader\brand')
css = (brand / 'fonts.css').read_text(encoding='utf-8', errors='replace')
blocks = re.findall(r'@font-face\s*\{(.*?)\}', css, re.S)
picked = {}
for b in blocks:
    fam = re.search(r"font-family:\s*'([^']+)'", b)
    wt = re.search(r'font-weight:\s*(\d+)', b)
    ur = re.search(r'unicode-range:\s*([^;]+);', b)
    url = re.search(r'url\((https://[^)]+\.woff2)\)', b)
    if not (fam and wt and url and ur):
        continue
    if 'U+0000-00FF' not in ur.group(1):
        continue
    picked[(fam.group(1), wt.group(1))] = url.group(1)

out_dir = brand / 'woff2'
out_dir.mkdir(exist_ok=True)
parts = []
for (fam, wt), url in sorted(picked.items()):
    name = f"{fam.replace(' ', '')}-{wt}.woff2"
    data = urllib.request.urlopen(url, timeout=60).read()
    (out_dir / name).write_bytes(data)
    b64 = base64.b64encode(data).decode()
    parts.append(
        "@font-face{font-family:'%s';font-style:normal;font-weight:%s;font-display:swap;"
        "src:url(data:font/woff2;base64,%s) format('woff2')}" % (fam, wt, b64)
    )
    print(f"{fam} {wt}: {len(data)} bytes")

(brand / 'fonts.embedded.css').write_text('\n'.join(parts), encoding='ascii')
print('total css bytes', sum(len(p) for p in parts))
