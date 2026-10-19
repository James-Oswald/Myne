"""Embed the pinned KaTeX browser distribution in a generated C include."""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
from pathlib import Path
import tarfile
import urllib.request


VERSION = "0.16.22"
PACKAGE_SHA256 = "e9e0d167db3175481cbadaff38e8d90b130f6a3ddb451a47e43c577fd511f365"
ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache" / f"katex-{VERSION}.tgz"
URL = f"https://registry.npmjs.org/katex/-/katex-{VERSION}.tgz"


def package_bytes(package: Path) -> bytes:
    if not package.exists():
        package.parent.mkdir(parents=True, exist_ok=True)
        try:
            with urllib.request.urlopen(URL, timeout=30) as response:
                data = response.read()
        except Exception as exc:
            raise SystemExit(f"cannot download pinned KaTeX package: {exc}") from exc
        if hashlib.sha256(data).hexdigest() != PACKAGE_SHA256:
            raise SystemExit("KaTeX package checksum mismatch")
        temporary = package.with_suffix(".tmp")
        temporary.write_bytes(data)
        temporary.replace(package)
    data = package.read_bytes()
    if hashlib.sha256(data).hexdigest() != PACKAGE_SHA256:
        raise SystemExit(f"KaTeX package checksum mismatch: {package}")
    return data


def assets(data: bytes) -> list[tuple[str, bytes]]:
    required = ["katex.min.js", "katex.min.css", "contrib/auto-render.min.js"]
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as package:
        names = required + sorted(
            member.name.removeprefix("package/dist/")
            for member in package.getmembers()
            if member.isfile() and member.name.startswith("package/dist/fonts/")
        )
        names.append("LICENSE")
        result = []
        for name in names:
            package_name = "package/LICENSE" if name == "LICENSE" else f"package/dist/{name}"
            member = package.extractfile(package_name)
            if member is None:
                raise SystemExit(f"missing KaTeX asset: {package_name}")
            result.append((name, member.read()))
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, default=CACHE)
    parser.add_argument("--output", type=Path, default=ROOT / "src/katex_assets.inc")
    args = parser.parse_args()

    files = assets(package_bytes(args.package))
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode="w", format=tarfile.USTAR_FORMAT) as bundle:
        for name, contents in files:
            info = tarfile.TarInfo(name)
            info.size = len(contents)
            info.mode = 0o644
            info.mtime = 0
            bundle.addfile(info, io.BytesIO(contents))

    encoded = base64.b64encode(archive.getvalue()).decode("ascii")
    chunks = [encoded[i:i + 2048] for i in range(0, len(encoded), 2048)]
    text = f"/* Generated from KaTeX {VERSION}; regenerate with tools/embed_katex.py. */\n"
    text += "static const char *const myne_katex_chunks[] = {\n"
    text += "".join(f'"{chunk}",\n' for chunk in chunks)
    text += "};\n"
    text += f"static const size_t myne_katex_size = {len(archive.getvalue())};\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)
    print(f"embedded {len(files)} KaTeX files ({len(archive.getvalue())} bytes)")


if __name__ == "__main__":
    main()
