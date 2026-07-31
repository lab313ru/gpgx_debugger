"""Build a browsable index of everything extracted from the Pirates ROM.

Writes assets/index.html next to the PNGs the other tools produce. Image
sources stay as relative paths so the page works straight off the filesystem;
the manifests are inlined as JSON because fetch() is blocked under file://.

Run the extractors first:
    pdw_portraits.py <rom> assets/portraits
    pdw_screens.py   <rom> assets/blocks
    pdw_sprites.py   <rom> --json assets/sprites.json
    pdw_scenes.py    <rom> --json assets/scenes.json

Then:  python pdw_gallery.py <assets-dir>
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

CSS = """
:root{--bg:#14161a;--panel:#1c1f26;--line:#2b3039;--fg:#dfe3ea;--dim:#8b93a3;--acc:#e08b4c}
@media (prefers-color-scheme:light){:root{--bg:#f6f7f9;--panel:#fff;--line:#dfe3ea;--fg:#1a1d23;--dim:#666e7d;--acc:#b4651f}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
     font:14px/1.5 ui-sans-serif,system-ui,'Segoe UI',sans-serif}
header{padding:20px 24px;border-bottom:1px solid var(--line);position:sticky;top:0;
       background:var(--bg);z-index:10}
h1{margin:0 0 4px;font-size:19px;font-weight:600}
.sub{color:var(--dim);font-size:13px}
nav{margin-top:12px;display:flex;gap:6px;flex-wrap:wrap}
nav button{background:var(--panel);color:var(--fg);border:1px solid var(--line);
           border-radius:6px;padding:5px 12px;cursor:pointer;font-size:13px}
nav button.on{border-color:var(--acc);color:var(--acc)}
main{padding:20px 24px 60px}
section{display:none} section.on{display:block}
.grid{display:grid;gap:14px;grid-template-columns:repeat(auto-fill,minmax(180px,1fr))}
.card{background:var(--panel);border:1px solid var(--line);border-radius:8px;
      padding:10px;overflow:hidden}
.card img{width:100%;height:auto;display:block;image-rendering:pixelated;
          background:#0b0d10;border-radius:4px}
.card .t{margin-top:8px;font-weight:600;font-size:13px}
.card .m{color:var(--dim);font-size:11px;font-family:ui-monospace,Consolas,monospace;
         white-space:pre-line}
.wide{grid-template-columns:repeat(auto-fill,minmax(340px,1fr))}
table{border-collapse:collapse;width:100%;font-size:12.5px}
th,td{text-align:left;padding:6px 10px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--dim);font-weight:600}
code{font-family:ui-monospace,Consolas,monospace;color:var(--acc)}
.scene{background:var(--panel);border:1px solid var(--line);border-radius:8px;
       padding:14px 16px;margin-bottom:14px}
.scene h3{margin:0 0 10px;font-size:14px}
.tools{margin-bottom:14px;color:var(--dim);font-size:12.5px}
.tools input{width:70px;background:var(--panel);color:var(--fg);
             border:1px solid var(--line);border-radius:4px;padding:3px 6px}
svg{background:#0b0d10;border-radius:4px;display:block;width:100%;height:auto}
"""

JS = """
const tabs=[...document.querySelectorAll('nav button')];
tabs.forEach(b=>b.onclick=()=>{
  tabs.forEach(x=>x.classList.toggle('on',x===b));
  document.querySelectorAll('section').forEach(s=>
    s.classList.toggle('on',s.id===b.dataset.tab));
});
const zoom=document.getElementById('zoom');
if(zoom) zoom.oninput=()=>document.querySelectorAll('.grid').forEach(
  g=>g.style.gridTemplateColumns=`repeat(auto-fill,minmax(${zoom.value}px,1fr))`);
"""


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def portraits_html(root):
    m = load(root / "portraits" / "manifest.json")
    if not m:
        return "<p class='sub'>Run pdw_portraits.py first.</p>"
    cards = []
    for p in m["portraits"]:
        cards.append(
            f"<div class='card'><img src='portraits/{p['name']}.png' alt=''>"
            f"<div class='t'>{esc(p['name'])}</div>"
            f"<div class='m'>tiles {p['tiles_at']}\npalette {p['palette_at']}\n"
            f"{p['size'][0]}x{p['size'][1]} px</div></div>")
    return f"<div class='grid'>{''.join(cards)}</div>"


def screens_html(root):
    files = sorted((root / "screens").glob("*.png")) if (root / "screens").is_dir() else []
    if not files:
        return "<p class='sub'>Run pdw_screen.py first.</p>"
    cards = "".join(
        f"<div class='card'><img src='screens/{f.name}' alt=''>"
        f"<div class='t'>{esc(f.stem)}</div>"
        f"<div class='m'>320x224, plane A</div></div>" for f in files)
    note = ("<p class='tools'>Rebuilt from the ROM the way the screen's own setup "
            "code does it &mdash; tiles, nametable, palettes, font and 9-patch frames. "
            "The selected item uses the burning frame set.</p>")
    return note + f"<div class='grid wide'>{cards}</div>"


def blocks_html(root):
    m = load(root / "blocks" / "manifest.json")
    if not m:
        return "<p class='sub'>Run pdw_screens.py first.</p>"
    cards = []
    for b in sorted(m["blocks"], key=lambda x: x["kind"] != "container"):
        at = b["at"].replace("0x", "")
        if b["kind"] == "container":
            img = f"blocks/{at}_map.png"
            meta = f"container\n{b['map'][0]}x{b['map'][1]} cells, {b['tiles']} tiles"
        else:
            img = f"blocks/{at}_tiles.png"
            meta = f"tiles only\n{b['tiles']} tiles, {b['unpacked']} bytes"
        cards.append(
            f"<div class='card'><img src='{img}' alt='' loading='lazy'>"
            f"<div class='t'>{esc(b['at'])}</div><div class='m'>{esc(meta)}</div></div>")
    note = ("<p class='tools'>Container renders use the menu palettes, so screens "
            "from elsewhere in the game will have the wrong colours until their "
            "scene's palettes are wired in.</p>")
    return note + f"<div class='grid wide'>{''.join(cards)}</div>"


def sprites_html(root):
    data = load(root / "sprites.json")
    if not data:
        return "<p class='sub'>Run pdw_sprites.py --json first.</p>"
    cards = []
    for s in sorted(data, key=lambda d: -len(d["pieces"]))[:120]:
        e = s["extent"]
        w, h = max(e["w"], 1), max(e["h"], 1)
        rects = "".join(
            f"<rect x='{p['x'] - e['x']}' y='{p['y'] - e['y']}' "
            f"width='{p['w'] * 8}' height='{p['h'] * 8}' fill='none' "
            f"stroke='#e08b4c' stroke-width='1'/>"
            f"<text x='{p['x'] - e['x'] + 2}' y='{p['y'] - e['y'] + 8}' "
            f"font-size='6' fill='#8b93a3'>{p['tile']:X}</text>"
            for p in s["pieces"])
        cards.append(
            f"<div class='card'><svg viewBox='0 0 {w} {h}' "
            f"preserveAspectRatio='xMidYMid meet'>{rects}</svg>"
            f"<div class='t'>{esc(s['at'])}</div>"
            f"<div class='m'>{len(s['pieces'])} pieces\n{w}x{h} px</div></div>")
    note = ("<p class='tools'>Piece layout only — the tiles come from whatever the "
            "scene uploaded, so these are outlines with tile offsets, not pixels.</p>")
    return note + f"<div class='grid'>{''.join(cards)}</div>"


def scenes_html(root):
    data = load(root / "scenes.json")
    if not data:
        return "<p class='sub'>Run pdw_scenes.py --json first.</p>"
    out = []
    for s in data:
        rows = []
        for u in s["upload"]:
            rows.append(f"<tr><td>upload</td><td><code>{esc(u['src'])}</code></td>"
                        f"<td>VRAM <code>{esc(u['vram'])}</code>, {u['tiles']} tiles</td></tr>")
        for t in s["tilemap"]:
            rows.append(f"<tr><td>tilemap</td><td><code>{esc(t['src'])}</code></td>"
                        f"<td>VDP <code>{esc(t['vdp'])}</code>, {t['cols']}x{t['rows']}, "
                        f"base <code>{esc(t['base'])}</code></td></tr>")
        if s["unpack"]:
            rows.append("<tr><td>unpack</td><td colspan='2'>"
                        + ", ".join(f"<code>{esc(x)}</code>" for x in s["unpack"])
                        + "</td></tr>")
        for p in s["palette"]:
            rows.append(f"<tr><td>palette</td><td>line {p['line']}</td>"
                        f"<td><code>{esc(p['src'])}</code></td></tr>")
        if s["portrait"]:
            rows.append(f"<tr><td>portraits</td><td colspan='2'>{esc(s['portrait'])}</td></tr>")
        for d in s["desc_list"]:
            rows.append(f"<tr><td>desc list</td><td colspan='2'><code>{esc(d)}</code></td></tr>")
        out.append(f"<div class='scene'><h3>scene {s['index']} &mdash; entry "
                   f"<code>{esc(s['entry'])}</code></h3>"
                   f"<table>{''.join(rows) or '<tr><td>nothing found</td></tr>'}</table></div>")
    note = ("<p class='tools'>Recovered by reading call arguments, not by following "
            "control flow &mdash; strong leads, worth confirming in the listing.</p>")
    return note + "".join(out)


def load(path):
    try:
        return json.loads(Path(path).read_text())
    except Exception:
        return None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    tabs = [("screens", "Screens", screens_html(root)),
            ("portraits", "Portraits", portraits_html(root)),
            ("blocks", "Blocks", blocks_html(root)),
            ("sprites", "Sprites", sprites_html(root)),
            ("scenes", "Scenes", scenes_html(root))]

    nav = "".join(f"<button data-tab='{k}'{' class=on' if i == 0 else ''}>{n}</button>"
                  for i, (k, n, _) in enumerate(tabs))
    secs = "".join(f"<section id='{k}'{' class=on' if i == 0 else ''}>{h}</section>"
                   for i, (k, _, h) in enumerate(tabs))

    html = f"""<!doctype html><meta charset=utf-8>
<title>Pirates of Dark Water &mdash; extracted assets</title>
<meta name=viewport content='width=device-width,initial-scale=1'>
<style>{CSS}</style>
<header>
  <h1>The Pirates of Dark Water &mdash; extracted assets</h1>
  <div class=sub>Everything below comes from the ROM alone. See RE_NOTES.md.</div>
  <nav>{nav}</nav>
  <div class=tools style='margin-top:10px'>tile size
    <input id=zoom type=range min=90 max=420 value=180></div>
</header>
<main>{secs}</main>
<script>{JS}</script>
"""
    out = root / "index.html"
    out.write_text(html, encoding="utf-8")
    print(f"-> {out}   ({len(html) // 1024} KB)")


if __name__ == "__main__":
    main()
