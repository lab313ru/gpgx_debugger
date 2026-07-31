"""Run the whole Pirates of Dark Water extraction and rebuild the gallery.

Everything lands in <assets>/ and the browsable index is <assets>/index.html.
Stages run in order because the later ones consume the earlier ones' JSON:
scenes.json is what lets the block renderer pick each screen's real palettes.

    python pdw_all.py                     # uses the defaults below
    python pdw_all.py --rom X.gen --out D
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_ROM = Path(r"F:\Projects\sega\Pirates of Dark Water, The (UE) [f1].gen")
DEFAULT_OUT = Path(r"F:\Projects\sega\assets")


def run(script: str, *args: str) -> bool:
    cmd = [sys.executable, str(HERE / script), *args]
    print(f"\n--- {script} " + " ".join(a for a in args if not a.startswith("-")))
    result = subprocess.run(cmd, cwd=HERE)
    if result.returncode:
        print(f"    {script} failed ({result.returncode})")
    return result.returncode == 0


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", type=Path, default=DEFAULT_ROM)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    if not args.rom.is_file():
        sys.exit(f"ROM not found: {args.rom}")
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    (out / "screens").mkdir(exist_ok=True)
    rom = str(args.rom)

    ok = True
    ok &= run("pdw_scenes.py", rom, "--json", str(out / "scenes.json"))
    ok &= run("pdw_portraits.py", rom, str(out / "portraits"))
    ok &= run("pdw_sprites.py", rom, "--min", "1", "--json", str(out / "sprites.json"))
    # Needs scenes.json, so it comes after.
    ok &= run("pdw_screens.py", rom, str(out / "blocks"),
              "--scenes", str(out / "scenes.json"))
    for hero in (0, 1, 2):
        ok &= run("pdw_screen.py", rom,
                  str(out / "screens" / f"heroselect_{hero}.png"), "--hero", str(hero))
        ok &= run("pdw_screen.py", rom,
                  str(out / "screens" / f"selectitem_{hero}.png"),
                  "--hero", str(hero), "--items", "--marker", "0")
    ok &= run("pdw_gallery.py", str(out))

    print(f"\n{'done' if ok else 'finished with errors'} -> {out / 'index.html'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
