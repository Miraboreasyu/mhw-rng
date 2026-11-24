"""Release archive packager for MIRA (mhw_rng).

Produces a clean game-loadable release zip containing only runtime files
(root patched SharpPluginLoader `winmm.dll` + `nativePC/plugins/...`).
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys
import zipfile

EXPECTED_PATCHED_WINMM_SHA256 = (
    "64DAAE7CC00EC2ADA0113EDEE1B95A551383968212E4C2E85EFFFFB8402CED34"
)

# (archive_rel_path, repo_rel_path)
RELEASE_FILES: tuple[tuple[str, str], ...] = (
    ("winmm.dll", "patches/spl/winmm.dll"),
    ("nativePC/plugins/mhw_rng.dll", "build/mhw_rng.dll"),
    ("nativePC/plugins/mhw_rng.ini", "mhw_rng.ini"),
    ("nativePC/plugins/mhw_rng/rng_crit.dll", "build/mhw_rng/rng_crit.dll"),
    ("nativePC/plugins/mhw_rng/rng_endemic.dll", "build/mhw_rng/rng_endemic.dll"),
    ("nativePC/plugins/mhw_rng/rng_inspector.dll", "build/mhw_rng/rng_inspector.dll"),
    ("nativePC/plugins/mhw_rng/rng_size.dll", "build/mhw_rng/rng_size.dll"),
    ("nativePC/plugins/mhw_rng/rng_steam.dll", "build/mhw_rng/rng_steam.dll"),
    ("nativePC/plugins/mhw_rng/rng_thk.dll", "build/mhw_rng/rng_thk.dll"),
    (
        "nativePC/plugins/mhw_rng/thk_presets/fatalis_p1_nasty.txt",
        "build/mhw_rng/thk_presets/fatalis_p1_nasty.txt",
    ),
)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest().upper()


def verify_inputs(repo_root: Path) -> list[tuple[str, Path, str]]:
    resolved: list[tuple[str, Path, str]] = []
    missing: list[Path] = []
    for arc_path, rel_src in RELEASE_FILES:
        src = repo_root / rel_src
        if not src.is_file():
            missing.append(src)
            continue
        digest = sha256_file(src)
        if arc_path == "winmm.dll" and digest != EXPECTED_PATCHED_WINMM_SHA256:
            raise ValueError(
                f"Patched winmm.dll SHA256 mismatch at {src}: "
                f"expected {EXPECTED_PATCHED_WINMM_SHA256}, got {digest}"
            )
        resolved.append((arc_path, src, digest))

    if missing:
        names = ", ".join(str(p.relative_to(repo_root)) for p in missing)
        raise FileNotFoundError(
            f"Missing release payload files ({names}). Run build.bat first."
        )
    return resolved


def write_zip(out_path: Path, items: list[tuple[str, Path, str]]) -> str:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(
        out_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as zf:
        for arc_path, src_path, _ in items:
            zf.write(src_path, arcname=arc_path)

    with zipfile.ZipFile(out_path, "r") as zf:
        bad = zf.testzip()
        if bad is not None:
            raise RuntimeError(f"Zip CRC check failed at member: {bad}")
    return sha256_file(out_path)


def main() -> int:
    parser = argparse.ArgumentParser(description="Package MIRA release zip.")
    parser.add_argument(
        "--check",
        action="store_true",
        help="Verify all payload files and checksums without writing a zip.",
    )
    parser.add_argument(
        "--out",
        type=Path,
        help="Output zip path (required unless --check is specified).",
    )
    args = parser.parse_args()

    if not args.check and args.out is None:
        parser.error("Specify either --check or --out <zip_path>.")

    repo_root = Path(__file__).resolve().parent.parent
    items = verify_inputs(repo_root)

    for arc_path, src_path, digest in items:
        size = src_path.stat().st_size
        print(f"  [OK] {arc_path:<58} {size:>8} B  {digest[:16]}...")

    if args.check:
        print(f"Verified {len(items)}/{len(RELEASE_FILES)} release payload entries.")
        return 0

    out_path = args.out.resolve()
    zip_sha = write_zip(out_path, items)
    print(f"Created {out_path} ({out_path.stat().st_size} bytes)")
    print(f"ZIP SHA256: {zip_sha}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
