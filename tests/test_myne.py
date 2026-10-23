"""End-to-end checks for Myne's examples and mixed-source behavior."""
from __future__ import annotations

import http.client
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
MYNE = REPO / "bin" / ("myne.exe" if os.name == "nt" else "myne")


def run(*args: str, cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(MYNE), *args], cwd=cwd, capture_output=True, text=True)


class MyneTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("lean"), "Lean 4 is required")
    def test_elaborated_graph_resolves_notation_and_tactics_only_when_enabled(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "myne.toml").write_text(
                'root = "main.lean"\nformat = "html"\n[web]\n'
                'dependencyGraph = "basic"\n'
            )
            (base / "main.lean").write_text(
                '/-! # Elaborated graph -/\n'
                '/-- @name Base -/\n'
                'def base (n : Nat) : Nat := n + 1\n\n'
                'notation "inc" => base\n\n'
                '/-- @name Seed -/\n'
                '@[simp] theorem seed : base 0 = 1 := by rfl\n\n'
                '/-- @name Notation -/\n'
                'theorem notation_theorem (n : Nat) : inc n = n + 1 := by rfl\n\n'
                '/-- @name Tactic -/\n'
                'theorem tactic_theorem : base 0 = 1 := by simp\n'
            )
            basic = run(cwd=base)
            self.assertEqual(basic.returncode, 0, basic.stderr)
            basic_graph = (base / "out/html/dependencies.html").read_text()
            self.assertIn("Warning: syntactic dependency graph.", basic_graph)
            self.assertIn("data-graph-page", basic_graph)
            self.assertNotIn('data-from="0" data-to="2"', basic_graph)
            self.assertNotIn('data-from="1" data-to="3"', basic_graph)

            full = run("--elaborated-deps", cwd=base)
            self.assertEqual(full.returncode, 0, full.stderr)
            full_graph = (base / "out/html/dependencies.html").read_text()
            self.assertIn('data-from="0" data-to="2"', full_graph)
            self.assertIn('data-from="1" data-to="3"', full_graph)
            self.assertIn("This graph uses Lean's elaborated types", full_graph)
            self.assertNotIn("Warning: syntactic dependency graph.", full_graph)

    @unittest.skipUnless(shutil.which("lean"), "Lean 4 is required")
    def test_failed_elaboration_does_not_retry_without_source_change(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "myne.toml").write_text('root = "main.lean"\nformat = "html"\n')
            (base / "main.lean").write_text(
                'import MyneMissingModule\n'
                '/-! # Failure -/\n'
                '/-- @name Example -/\n'
                'theorem example : True := by trivial\n'
            )
            log = base / "watch.log"
            with log.open("w") as stderr:
                proc = subprocess.Popen(
                    [str(MYNE), "--watch", "--elaborated-deps"],
                    cwd=base, stdout=subprocess.DEVNULL, stderr=stderr,
                )
                try:
                    deadline = time.monotonic() + 15
                    while "Lean elaboration failed" not in log.read_text():
                        self.assertIsNone(proc.poll(), "watch exited before reporting failure")
                        self.assertLess(time.monotonic(), deadline, "Lean failure was not reported")
                        time.sleep(0.1)
                    time.sleep(1.2)
                    self.assertEqual(log.read_text().count("Lean elaboration failed"), 1)
                finally:
                    proc.terminate()
                    proc.wait(timeout=5)

    def test_declaration_map_in_html_and_tex(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "myne.toml").write_text(
                'root = "main.lean"\nformat = "both"\n'
                '[declarationMap]\n'
                '"def" = "theorem"\n'
                '"def∇" = "def"\n'
                '"inductive∇" = "def"\n'
                '"custom lemma" = "lemma"\n'
                '"custom=lemma" = "lemma"\n'
                '"theorem" = "def"\n'
            )
            (base / "main.lean").write_text(
                '/-! # Mapped Declarations -/\n'
                '/-- @name Recursive definition -/\n'
                '@[simp] noncomputable def∇ recursive : Nat := 1\n\n'
                '/-- @name Recursive type -/\n'
                'inductive∇ Recursive where\n| base\n\n'
                '/-- @name Custom lemma -/\n'
                'private custom lemma result : True := by trivial\n\n'
                '/-- @name Quoted equals key -/\n'
                'custom=lemma result2 : True := by trivial\n\n'
                '/-- @name Builtin override -/\n'
                'theorem override : True := by trivial\n\n'
                '/-- @name Longest match -/\n'
                'def ordinary : Nat := 1\n\n'
                '/-- @name Boundary -/\n'
                'def∇Extra unrelated : Nat := 1\n'
            )
            result = run(cwd=base)
            self.assertEqual(result.returncode, 0, result.stderr)
            html = (base / "out/html/index.html").read_text()
            tex = (base / "out/tex/main.tex").read_text()
            self.assertEqual(html.count('class="lean-block def"'), 3)
            self.assertEqual(html.count('class="lean-block lemma"'), 2)
            self.assertEqual(html.count('class="lean-block theorem"'), 1)
            self.assertEqual(html.count('class="lean-block code"'), 1)
            self.assertEqual(tex.count('\\begin{definition}'), 3)
            self.assertEqual(tex.count('\\begin{lemma}'), 2)
            self.assertEqual(tex.count('\\begin{theorem}'), 1)

    def test_named_appendix_markers_group_pages_after_main_text(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "myne.toml").write_text(
                'root = "main.lean"\nformat = "both"\n'
                '[web]\npageHeading = "section"\n'
            )
            (base / "main.lean").write_text(
                "/-! # Appendix test\n\section{Main}\nMain content. -/\n"
                "--myne:appendix_start Supplementary Results\n"
                "/-! \subsection{Supplementary details}\nMoved appendix prose. -/\n"
                "/-- @name Appended theorem\n"
                "@nl\nThis theorem belongs in the appendix.\n"
                "@formula\n$$x = x$$\n-/\n"
                "theorem appended : True := by trivial\n"
                "--myne:appendix_end\n"
                "/-! \section{After appendix}\nThis remains in the main text. -/\n"
            )
            result = run(cwd=base)
            self.assertEqual(result.returncode, 0, result.stderr)
            html_dir = base / "out/html"
            main = (html_dir / "section-001.html").read_text()
            after = (html_dir / "section-002.html").read_text()
            appendix_group = (html_dir / "section-003.html").read_text()
            appendix = (html_dir / "section-004.html").read_text()
            all_html = "\n".join(p.read_text() for p in sorted(html_dir.glob("*.html")))
            self.assertIn("Main content", main)
            self.assertIn("This remains in the main text", after)
            self.assertNotIn("Appended theorem", after)
            self.assertIn(
                "<h1><span class=heading-number>A</span> Appendix: Supplementary Results</h1>",
                appendix,
            )
            self.assertIn("<h1>Appendices</h1>", appendix_group)
            self.assertIn('href="section-004.html"', appendix_group)
            self.assertIn("Theorem A.1", appendix)
            self.assertIn("Supplementary details", appendix)
            self.assertIn("Moved appendix prose", appendix)
            self.assertIn("lean-block theorem", appendix)
            self.assertNotIn("--myne:appendix_", all_html)
            tex = (base / "out/tex/main.tex").read_text()
            self.assertLess(tex.index("\\section{After appendix}"), tex.index("\\appendix"))
            self.assertLess(tex.index("\\appendix"), tex.index("\\section{Supplementary Results}"))
            self.assertIn("\\begin{theorem}[Appended theorem]", tex)
            self.assertNotIn("--myne:appendix_", tex)

    def test_appendix_uses_configured_web_top_level(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "myne.toml").write_text(
                'root = "main.lean"\n'
                '[web]\npageHeading = "subsection"\n'
            )
            (base / "main.lean").write_text(
                "/-! # Appendix headings -/\n"
                "--myne:appendix_start Supplemental proofs\n"
                "/-! Proof details. -/\n"
                "--myne:appendix_end\n"
            )
            result = run(cwd=base)
            self.assertEqual(result.returncode, 0, result.stderr)
            group = (base / "out/html/subsection-001.html").read_text()
            appendix = (base / "out/html/subsection-002.html").read_text()
            self.assertIn("<h1>Appendices</h1>", group)
            self.assertIn("Appendix: Supplemental proofs", appendix)
            self.assertIn("<span class=heading-number>A</span>", appendix)
            self.assertIn("nav-depth-0 heading-subsection appendix-group", appendix)

    def test_appendix_markers_reject_unmatched_commands(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "myne.toml").write_text('root = "main.lean"\n')
            source = base / "main.lean"
            source.write_text("--myne:appendix_end\n/-! # Invalid -/\n")
            result = run(cwd=base)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("appendix_end without appendix_start", result.stderr)
            source.write_text("--myne:appendix_start Missing end\n/-! # Invalid -/\n")
            result = run(cwd=base)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing --myne:appendix_end", result.stderr)

    def test_declaration_map_rejects_invalid_values(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "main.lean").write_text('/-! # Invalid -/\n')
            (base / "myne.toml").write_text(
                'root = "main.lean"\n[declarationMap]\n"def∇" = "axiom"\n'
            )
            result = run(cwd=base)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("declarationMap", result.stderr)

    def test_basic_example(self) -> None:
        project = REPO / "examples/basic"
        result = run(cwd=project)
        self.assertEqual(result.returncode, 0, result.stderr)
        html = (project / "out/html/index.html").read_text()
        definitions = (project / "out/html/section-001.html").read_text()
        all_html = "\n".join(
            path.read_text() for path in sorted((project / "out/html").glob("*.html"))
        )
        tex = (project / "out/tex/sample.tex").read_text()
        for fragment in ("Basic Arithmetic Examples",
                         "<h1><span class=heading-number>1</span> Definitions</h1>",
                         "lean-block theorem", "Euclid", "katex/katex.min.js"):
            self.assertIn(fragment, all_html)
        self.assertIn(
            'href="section-001.html"><span class=heading-number>1</span> Definitions</a>',
            html,
        )
        self.assertIn("<span class=statement-label>Definition 1.1</span>", definitions)
        self.assertIn("<span class=statement-label>Theorem 2.1</span>", all_html)
        definition = definitions.index('<div class="lean-block def">')
        doc = definitions.index('<div class="lean-doc">', definition)
        prose = definitions.index("Given a natural number", doc)
        code = definitions.index('<pre class="lean-code">', prose)
        self.assertLess(definition, doc)
        self.assertLess(doc, prose)
        self.assertLess(prose, code)
        doc_content = definitions.split('<div class="lean-doc">', 1)[1].split(
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
        all_html = "\n".join(path.read_text() for path in
                             sorted((project / "out/html").glob("*.html")))
        self.assertIn("A Theory of Machine Intelligence Measures", html)
        self.assertIn('href="../tex/rpithes.pdf"', html)
        self.assertIn('href="section-001.html"><span class=heading-number>1</span> Acknowledgment</a>', html)
        self.assertIn('href="section-002.html"><span class=heading-number>2</span> Abstract</a>', html)
        self.assertIn('href="part-007.html">References</a>', html)
        self.assertIn("Abstract", all_html)
        self.assertIn("Acknowledgment", all_html)
        self.assertIn("References", all_html)
        self.assertIn("Programming System", all_html)
        self.assertIn("Machine Intelligence Measures", all_html)
        self.assertIn("Diagonal Halting Set", all_html)
        self.assertIn("ProgrammingSystem.K", all_html)
        self.assertIn("ProgrammingSystem.H", all_html)
        self.assertNotIn("--myne:end", all_html)
        root_tex = (project / "out/tex/rpithes.tex").read_text()
        self.assertIn("\\input{Chapters/rpichap1}", root_tex)
        self.assertIn("\\input{Chapters/rpichap2}", root_tex)
        self.assertIn("\\input{Chapters/rpichap3}", root_tex)
        self.assertIn("\\usepackage{amsthm}", root_tex)
        first_tex = (project / "out/tex/Chapters/rpichap1.tex").read_text()
        self.assertIn("\\begin{definition}[Programming System]", first_tex)
        self.assertNotIn("structure ProgrammingSystem", first_tex)
        second_tex = (project / "out/tex/Chapters/rpichap2.tex").read_text()
        self.assertIn("formal-measures literature", second_tex)
        third_tex = (project / "out/tex/Chapters/rpichap3.tex").read_text()
        self.assertIn("\\begin{definition}[Diagonal Halting Set]", third_tex)
        self.assertNotIn("ProgrammingSystem.H", third_tex)
        self.assertTrue((project / "out/tex/thesis.cls").is_file())
        self.assertTrue((project / "out/tex/rpi_seal.png").is_file())

    def test_showcase_capture_block(self) -> None:
        project = REPO / "examples/showcase"
        result = run(cwd=project)
        self.assertEqual(result.returncode, 0, result.stderr)
        html = (project / "out/html/section-002.html").read_text()
        self.assertEqual(html.count('class="lean-block'), 2)
        first, second = html.split('class="lean-block', 2)[1:]
        self.assertIn("def</span> double", first)
        self.assertIn("double_eq_add", first)
        self.assertIn("double_zero", second)
        self.assertNotIn("--myne:end", html)
        self.assertNotIn("@capture", html)
        chapter_tex = (project / "out/tex/Chapters/Arithmetic.tex").read_text()
        self.assertEqual(chapter_tex.count("\\begin{definition}"), 1)
        self.assertEqual(chapter_tex.count("\\begin{theorem}"), 1)
        self.assertNotIn("double_eq_add", chapter_tex)
        template = (project / "out/tex/template.tex").read_text()
        self.assertIn("\\bibliography{references}", template)
        self.assertIn("\\input{Chapters/Arithmetic.tex}", template)
        index = (project / "out/html/index.html").read_text()
        self.assertIn('href="../tex/template.pdf"', index)
        self.assertIn("custom.css", index)
        self.assertIn("footnote", index)
        self.assertIn("knuth1984", index)
        self.assertTrue((project / "out/html/custom.css").is_file())

    def test_capture_marker_ignores_block_comments_and_requires_end(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "myne.toml").write_text('root = "main.lean"\n')
            source = base / "main.lean"
            source.write_text(
                "/-! # Capture -/\n"
                "/--\n@capture block\nA group.\n-/\n"
                "def first : Nat := 1\n\n"
                "/-\n--myne:end\n-/\n"
                "def second : Nat := 2\n"
                "--myne:end \t\n"
                "/-- Later. -/\ndef third : Nat := 3\n"
            )
            result = run(cwd=base)
            self.assertEqual(result.returncode, 0, result.stderr)
            html = (base / "out/html/index.html").read_text()
            self.assertEqual(html.count('class="lean-block'), 2)
            self.assertIn("def</span> second", html)
            self.assertIn("def</span> third", html)
            source.write_text(source.read_text().replace(
                "def second : Nat := 2\n--myne:end \t", "def second : Nat := 2"))
            failed = run(cwd=base)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("missing --myne:end", failed.stderr)

    def test_explicit_lean_include_graphic_and_cycle(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "sub").mkdir()
            (base / "myne.toml").write_text('root = "root.tex"\nformat = "both"\n')
            (base / "root.tex").write_text(
                "\\documentclass{article}\\title{Synthetic}\\begin{document}"
                "\\input{sub/child.lean}\\end{document}")
            (base / "sub/child.lean").write_text(
                "/-! # Mixed Chapter\n# Mixed Section -/\n"
                "/-- Image \\includegraphics{icon.png} -/\n"
                "structure Example where\n  value : Nat\n\n"
                "/-- A small lemma. -/\n@[simp] lemma small : True := by trivial\n")
            (base / "sub/icon.png").write_bytes(b"image bytes")
            result = run(cwd=base)
            self.assertEqual(result.returncode, 0, result.stderr)
            chapter = (base / "out/html/chapter-001.html").read_text()
            html = (base / "out/html/section-002.html").read_text()
            self.assertIn("nav-depth-0 heading-chapter", chapter)
            self.assertIn("nav-depth-1 heading-section", chapter)
            self.assertIn('src="sub/icon.png"', html)
            self.assertIn("lean-block def", html)
            self.assertIn("lean-block lemma", html)
            self.assertIn("<span class=statement-label>Definition 1.1.1</span>", html)
            self.assertIn("<span class=statement-label>Lemma 1.1.2</span>", html)
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

    def test_annotated_formula_delimiters_do_not_capture_proof(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "myne.toml").write_text('root = "main.lean"\n')
            (base / "main.lean").write_text(
                "/-! # Display Math -/\n"
                "/--\n"
                "@name Delimiter Regression\n"
                "@nl\n"
                "A theorem with a displayed formula.\n"
                "@formula\n"
                "$$x = y$$\n"
                "@proof\n"
                "Construct $R_T$, then use $x = y$.\n"
                "-/\n"
                "theorem delimiter_regression : True := by trivial\n"
            )
            result = run(cwd=base)
            self.assertEqual(result.returncode, 0, result.stderr)
            html = (base / "out/html/index.html").read_text()
            self.assertIn('<div class=math>$$x = y$$</div>', html)
            self.assertIn(
                "<span class=statement-label>Theorem 1</span>"
                "<span class=statement-title> — Delimiter Regression</span>",
                html,
            )
            self.assertIn(
                '<em>Proof.</em> Construct <span class=math>$R_T$</span>, '
                'then use <span class=math>$x = y$</span>.',
                html,
            )
            self.assertNotIn('</div> $$', html)
            self.assertNotIn('</div> $<span class=math>', html)

    def test_web_page_heading_and_dark_mode_options(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "myne.toml").write_text(
                'root = "main.tex"\n'
                'darkModeToggle = true\n'
            )
            (base / "main.tex").write_text(
                "\\title{Configurable Web Output}\n"
                "\\chapter{Chapter One}\n"
                "Chapter introduction.\n"
                "\\section{Section One}\n"
                "Section content.\n"
                "\\subsection{Details}\n"
                "Detailed content.\n"
            )
            result = run(cwd=base)
            self.assertEqual(result.returncode, 0, result.stderr)
            html_dir = base / "out/html"
            chapter = (html_dir / "chapter-001.html").read_text()
            section = (html_dir / "section-002.html").read_text()
            self.assertIn(
                'href="section-002.html"><span class=heading-number>1.1</span> Section One</a>',
                chapter,
            )
            self.assertIn("nav-depth-0 heading-chapter", chapter)
            self.assertIn("nav-depth-1 heading-section", chapter)
            self.assertIn(
                "<h2><span class=heading-number>1.1.1</span> Details</h2>", section
            )
            self.assertIn('class=theme-toggle', section)
            self.assertIn("localStorage.getItem('myne-theme')", section)
            self.assertIn(":root[data-theme=dark]", (html_dir / "style.css").read_text())
            self.assertIn("localStorage.setItem('myne-theme',theme)",
                          (html_dir / "runtime.js").read_text())

            (base / "myne.toml").write_text(
                'root = "main.tex"\n'
                'outputDir = "chapter-out"\n'
                'pageHeading = "chapter"\n'
                'darkModeToggle = false\n'
            )
            result = run(cwd=base)
            self.assertEqual(result.returncode, 0, result.stderr)
            chapter_dir = base / "chapter-out/html"
            chapter = (chapter_dir / "chapter-001.html").read_text()
            self.assertFalse(list(chapter_dir.glob("section-*.html")))
            self.assertIn(
                "<h2><span class=heading-number>1.1</span> Section One</h2>", chapter
            )
            self.assertIn(
                "<h3><span class=heading-number>1.1.1</span> Details</h3>", chapter
            )
            self.assertNotIn('class=theme-toggle', chapter)

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
