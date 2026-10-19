"""End-to-end checks for Myne's two fixtures and mixed-source behavior."""
from __future__ import annotations

import http.client
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
MYNE = REPO / "myne"


def run(*args: str, cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(MYNE), *args], cwd=cwd, capture_output=True, text=True)


class MyneTests(unittest.TestCase):
    def test_basic_example(self) -> None:
        project = REPO / "examples/basic"
        result = run(cwd=project)
        self.assertEqual(result.returncode, 0, result.stderr)
        html = (project / "out/html/index.html").read_text()
        tex = (project / "out/tex/sample.tex").read_text()
        for fragment in ("Basic Arithmetic Examples", "<h2>Definitions</h2>",
                         "lean-block theorem", "Euclid", "katex/katex.min.js"):
            self.assertIn(fragment, html)
        definition = html.index('<div class="lean-block def">')
        doc = html.index('<div class="lean-doc">', definition)
        prose = html.index("Given a natural number", doc)
        code = html.index('<pre class="lean-code">', prose)
        self.assertLess(definition, doc)
        self.assertLess(doc, prose)
        self.assertLess(prose, code)
        doc_content = html.split('<div class="lean-doc">', 1)[1].split(
            '</div><pre class="lean-code">', 1)[0]
        self.assertIn("Given a natural number", doc_content)
        self.assertIn("\\begin{definition}", tex)
        self.assertIn("\\begin{theorem}", tex)
        definition_tex = tex.split("\\begin{definition}", 1)[1].split(
            "\\end{definition}", 1)[0]
        self.assertIn("Given a natural number", definition_tex)
        self.assertIn("\\newtheorem{definition}{Definition}", tex)
        self.assertNotIn("\\begin{verbatim}", tex)
        self.assertNotIn("def add (", tex)
        self.assertNotIn("theorem add_symm (", tex)
        self.assertIn("\\bibliography{refs}", tex)
        self.assertTrue((project / "out/html/katex/fonts/KaTeX_Main-Regular.woff2").is_file())
        self.assertTrue((project / "out/tex/refs.bib").is_file())

    def test_thesis_example(self) -> None:
        project = REPO / "examples/thesis"
        result = run(cwd=project)
        self.assertEqual(result.returncode, 0, result.stderr)
        html = (project / "out/html/index.html").read_text()
        chapter = (project / "out/html/chapter-001.html").read_text()
        self.assertIn("A Theory of Machine Intelligence Measures", html)
        self.assertIn("<h2>Abstract</h2>", html)
        self.assertIn("lean-block def", chapter)
        self.assertIn('<div class="lean-doc"> -- i </div><pre class="lean-code">', chapter)
        self.assertIn("\\input{Chapters/rpichap1}",
                      (project / "out/tex/rpithes.tex").read_text())
        self.assertIn("\\usepackage{amsthm}",
                      (project / "out/tex/rpithes.tex").read_text())
        lean_tex = (project / "out/tex/Chapters/rpichap1.tex").read_text()
        self.assertIn("\\begin{definition}", lean_tex)
        self.assertNotIn("\\begin{verbatim}", lean_tex)
        self.assertNotIn("def one_plus_one", lean_tex)
        self.assertTrue((project / "out/tex/Chapters/rpichap1.tex").is_file())
        self.assertTrue((project / "out/tex/thesis.cls").is_file())
        self.assertTrue((project / "out/tex/rpi_seal.png").is_file())

    def test_explicit_lean_include_graphic_and_cycle(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "sub").mkdir()
            (base / "myne.toml").write_text('root = "root.tex"\nformat = "both"\n')
            (base / "root.tex").write_text(
                "\\documentclass{article}\\title{Synthetic}\\begin{document}"
                "\\input{sub/child.lean}\\end{document}")
            (base / "sub/child.lean").write_text(
                "/-! \\chapter{Mixed Chapter} -/\n"
                "/-- Image \\includegraphics{icon.png} -/\n"
                "def example : Nat := 1\n\n"
                "/-- A small lemma. -/\n@[simp] lemma small : True := by trivial\n")
            (base / "sub/icon.png").write_bytes(b"image bytes")
            result = run(cwd=base)
            self.assertEqual(result.returncode, 0, result.stderr)
            html = (base / "out/html/chapter-001.html").read_text()
            self.assertIn('src="sub/icon.png"', html)
            self.assertIn("lean-block def", html)
            self.assertIn("lean-block lemma", html)
            self.assertIn("\\input{sub/child.tex}", (base / "out/tex/root.tex").read_text())
            child_tex = (base / "out/tex/sub/child.tex").read_text()
            self.assertIn("\\includegraphics{sub/icon.png}", child_tex)
            self.assertIn("\\begin{lemma}", child_tex)
            self.assertNotIn("lemma small : True", child_tex)
            (base / "root.tex").write_text("\\input{sub/child.lean}\\input{missing}")
            failed = run(cwd=base)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("missing include", failed.stderr)
            (base / "root.tex").write_text("\\input{sub/other}")
            (base / "sub/other.tex").write_text("\\input{../root}")
            failed = run(cwd=base)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("cycle", failed.stderr)

    def test_binary_has_no_runtime_asset_dependency(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            binary = base / "myne"
            shutil.copy2(MYNE, binary)
            (base / "myne.toml").write_text('root = "doc.lean"\n')
            (base / "doc.lean").write_text("/-! # Standalone -/\n")
            result = subprocess.run([str(binary)], cwd=base, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((base / "out/html/katex/katex.min.js").is_file())
            self.assertTrue((base / "out/html/katex/fonts/KaTeX_Main-Regular.woff2").is_file())
            self.assertTrue((base / "out/html/style.css").is_file())
            self.assertTrue((base / "out/html/runtime.js").is_file())

    def test_serve_directory_from_repository_root(self) -> None:
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        proc = subprocess.Popen([str(MYNE), "-i", "examples/basic", "--serve",
                                 "--port", str(port)], cwd=REPO,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
                    conn.request("GET", "/")
                    response = conn.getresponse()
                    html = response.read().decode()
                    conn.close()
                    if response.status == 200:
                        break
                except OSError:
                    pass
                self.assertIsNone(proc.poll(), "server exited before listening")
                self.assertLess(time.monotonic(), deadline, "server did not start")
                time.sleep(0.1)
            self.assertIn("Basic Arithmetic Examples", html)
        finally:
            proc.terminate()
            proc.wait(timeout=5)

    def test_watch_and_serve_refresh(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "myne.toml").write_text('root = "main.lean"\n')
            source = base / "main.lean"
            source.write_text("/-! # Before -/\n")
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            proc = subprocess.Popen([str(MYNE), "--serve", "--port", str(port)],
                                    cwd=base, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
            try:
                def get(path: str) -> tuple[int, str]:
                    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
                    conn.request("GET", path)
                    response = conn.getresponse()
                    data = response.read().decode()
                    status = response.status
                    conn.close()
                    return status, data

                deadline = time.monotonic() + 5
                while True:
                    try:
                        status, before = get("/__myne_epoch")
                        if status == 200:
                            break
                    except OSError:
                        pass
                    self.assertLess(time.monotonic(), deadline, "server did not start")
                    time.sleep(0.1)
                self.assertIn("Before", get("/")[1])
                source.write_text("/-! # After -/\n")
                deadline = time.monotonic() + 5
                while get("/__myne_epoch")[1] == before:
                    self.assertLess(time.monotonic(), deadline, "watch did not rebuild")
                    time.sleep(0.1)
                self.assertIn("After", get("/")[1])
                source.write_text("/-! # Broken \\input{missing} -/\n")
                time.sleep(1)
                self.assertIn("After", get("/")[1])
            finally:
                proc.terminate()
                proc.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
