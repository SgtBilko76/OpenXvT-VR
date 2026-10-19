#!/usr/bin/env python3
"""One-time import of the Quest scaffolding from the OpenXWA-VR port.

Copies the Android/Gradle app, the OpenXR headers and the SDL Java glue from
~/Projekte/OpenXWA-VR and renames the package/activity for OpenXvT. Re-running
overwrites the copied files (tools/quest/*.sh and src/xvt_vr are maintained by
hand and are not touched).
"""
import pathlib
import shutil

SRC = pathlib.Path.home() / "Projekte/OpenXWA-VR"
DST = pathlib.Path(__file__).resolve().parents[2]

RENAMES = [
    ("com.bilko.openxwavr", "com.bilko.openxvtvr"),
    ("com/bilko/openxwavr", "com/bilko/openxvtvr"),
    ("OpenXWAActivity", "OpenXvTActivity"),
    ("OpenXWA-VR", "OpenXvT-VR"),
    ("OpenXWA VR", "OpenXvT VR"),
    ("OpenXWA", "OpenXvT"),
    ("X-Wing Alliance", "X-Wing vs. TIE Fighter"),
]

TEXT_SUFFIXES = {".gradle", ".xml", ".java", ".properties", ".h", ""}


def rename(text: str) -> str:
    for old, new in RENAMES:
        text = text.replace(old, new)
    return text


def copy_tree(rel: str) -> int:
    count = 0
    for src in (SRC / rel).rglob("*"):
        if src.is_dir() or "/build/" in str(src) or "/.gradle/" in str(src) or src.name == "local.properties":
            continue
        out = DST / rename(str(src.relative_to(SRC)))
        out.parent.mkdir(parents=True, exist_ok=True)
        if src.suffix in TEXT_SUFFIXES and src.name not in ("gradlew",) and not src.name.endswith(".jar"):
            try:
                out.write_text(rename(src.read_text()))
            except UnicodeDecodeError:
                shutil.copy2(src, out)
        else:
            shutil.copy2(src, out)
        count += 1
    return count


def main() -> None:
    n = copy_tree("android")
    n += copy_tree("third_party/openxr")
    shutil.copy2(SRC / "android/gradlew", DST / "android/gradlew")
    print(f"copied {n} files")


if __name__ == "__main__":
    main()
