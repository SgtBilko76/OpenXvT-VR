#!/usr/bin/env python3
"""Stage the X-Wing Revival 3D cockpits used by the VR build.

OpenXvT VR draws 3D cockpits from X-Wing Revival (free download:
https://www.raptor007.com/xwing/; models from the X-Wing Alliance Upgrade
project). This copies only the cockpit models and the textures they use from
an unpacked X-Wing Revival folder into <out>/Models and <out>/Textures;
tools/quest/build-native.sh bundles them as APK assets (XWingRevival/).

Usage: stage_revival_cockpits.py <X-Wing Revival folder> <out dir>
"""
import pathlib
import re
import shutil
import sys

COCKPITS = [
    "x-wing_cockpit_hq.obj",
    "y-wing_cockpit_hq.obj",
    "a-wing_cockpit.obj",
    "b-wing_cockpit.obj",
    "tie-fighter_cockpit_vr.obj",
    "tie-interceptor_cockpit_vr.obj",
    "tie-bomber_cockpit_vr.obj",
    "gunboat_cockpit.obj",
]


def find_ci(folder: pathlib.Path, rel: str):
    path = folder
    for part in rel.replace("\\", "/").split("/"):
        if not path.is_dir():
            return None
        match = [p for p in path.iterdir() if p.name.lower() == part.lower()]
        if not match:
            return None
        path = match[0]
    return path


def main() -> int:
    src = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2])
    models, textures = src / "Models", src / "Textures"
    if not models.is_dir() or not textures.is_dir():
        print(f"{src} has no Models/ and Textures/ folders", file=sys.stderr)
        return 1
    (out / "Models").mkdir(parents=True, exist_ok=True)
    (out / "Textures").mkdir(parents=True, exist_ok=True)
    wanted_textures = set()
    copied = 0
    for name in COCKPITS:
        obj = models / name
        if not obj.is_file():
            print(f"missing {obj}", file=sys.stderr)
            return 1
        shutil.copy2(obj, out / "Models" / name)
        copied += 1
        text = obj.read_text(errors="ignore")
        for mtl in re.findall(r"^\s*mtllib\s+(\S+)", text, re.M):
            mtl_path = find_ci(models, mtl)
            if not mtl_path:
                continue
            shutil.copy2(mtl_path, out / "Models" / mtl_path.name)
            copied += 1
            for kind, rel in re.findall(r"^\s*(map_Kd|map_glow)\s+(\S+)", mtl_path.read_text(errors="ignore"), re.M):
                if not rel.lower().startswith("special/"):
                    wanted_textures.add(rel.replace("\\", "/"))
    missing = 0
    for rel in sorted(wanted_textures):
        tex = find_ci(textures, rel)
        if not tex:
            missing += 1
            continue
        dst = out / "Textures" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(tex, dst)
        copied += 1
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"staged {copied} files ({size / 1e6:.1f} MB) into {out}; {missing} textures missing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
