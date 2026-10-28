
# Myne : A Lean4 Document Platform 

Myne is a fast and light-weight Lean4 document platform for creating and managing mathematical documents in a Lean-first way. Myne is meant as an alternitive to tools like [Verso](https://verso.lean-lang.org/) and [leanblueprint](https://github.com/PatrickMassot/leanblueprint).

Myne consumes Lean4 files and LateX files, treating lean comments as part of the document content, and
integrates them seamlessly into the document structure.

Myne complies to either TeX which can be compiled with standard LaTeX tools, or Verso-esque website formats.

## Build and quick start

After cloning, run `git submodule update --init --recursive`, then `make`. The first build downloads the pinned KaTeX browser archive and verifies its SHA-256 checksum; generated `.inc` files are ignored by Git. The Makefile compiles the C modules under `src/` and links them into one executable. It uses TCC when available and otherwise uses `cc`; override with `make CC=cc` or `make CC=tcc`. On Windows, use the native MinGW-w64 toolchain (for example, MSYS2 UCRT64 GCC and Make) and run `make`; this produces `myne.exe` without Cygwin. The same source tree also builds on Unix-like systems. KaTeX, fonts, and Myne's built-in CSS and JavaScript are embedded in the executable, so the binary generates offline HTML without a separate assets directory.

From an example directory, run `../../bin/myne`. Myne reads `myne.toml` in the current directory. For example:

```sh
cd examples/basic
../../bin/myne
cd out/tex && latexmk -pdf -interaction=nonstopmode sample.tex
```

To render and compile all example projects, run `make pdfs` from the repository root. `examples/thesis` keeps the RPI TeX root, class, and command mappings while filling its chapters with excerpts from the separate Thesis repository. `examples/showcase` demonstrates the authoring features together.

Generated pages are under `out/html/`, and compile-ready TeX plus local assets are under `out/tex/`. `-f html`, `-f tex`, or `-f both` selects which directory is generated. `--watch` rebuilds on source and asset changes. `--serve` implies watch mode and serves HTML at `http://127.0.0.1:8000/`, with automatic browser refresh. Serving honors the manifest's format: with `format = "both"`, Myne runs `latexmk` after each build, puts the PDF beside the generated TeX root, and serves it through the PDF link. `latexmk` must be on `PATH`; compilation failures are reported and leave the last successful site and PDF in place. Use `-f html --serve` for HTML-only previews or `--port N` to change the port. A one-shot `-f both` build still generates TeX without compiling it; the build action compiles PDFs separately.

Run `myne [-i root|project-directory] [-f html|tex|both] [-o outputDir] [-s plain|verso|hutter|custom.css] [--config file] [--elaborated-deps] [--watch] [--serve] [--port N]`. The root and custom CSS paths are relative to the manifest directory. `bibliography` accepts a `.bib` path or an array of paths. Web options belong under `[web]`; `pageHeading` controls the deepest heading level that starts a web page and defaults to `section`. Set `tableOfContents = true` there to show links to headings one level below each higher-level page. The contents list appears after the page's other content. The persistent light/dark slider appears at the bottom right by default; set `darkModeToggle = false` under `[web]` to hide it. `commandMap` performs literal replacements for HTML rendering only. Normal builds process Lean source text without invoking the Lean elaborator. For HTML, unsupported LaTeX commands and their braced arguments are omitted.

## GitHub Actions

Use the [Myne build action](.github/actions/build-myne/action.yml) in a workflow to compile a project into static HTML. It checks out and builds Myne, then provides the generated site's absolute path as the `site-directory` output. Set `upload-pages-artifact: true` to upload that directory for GitHub Pages. Set `project-directory` to the folder containing your Myne project; optionally set `config-file` (relative to that folder) and `output-directory` (relative to the repository workspace, default `out`). The HTML is written to `<output-directory>/html`.

To include a PDF, set `build-pdf: true` and `tex-main-file` to the main TeX file Myne generates (for example, `Final.tex`). The action generates HTML and TeX together, compiles the TeX with a prebuilt TeX Live container, copies the PDF into the HTML site, fixes the PDF links, and exposes its path as `pdf-file`. The PDF is included when `upload-pages-artifact` is true. The TeX container is downloaded on the runner; TeX packages are not installed with `apt-get` on each build. Set `elaborated-deps: true` if the dependency graph should use Lean elaboration; this requires Lean or Lake to be available in the job before invoking the action. PDF builds require an Ubuntu runner with Docker and an output directory inside the workspace.

```yaml
- uses: James-Oswald/Myne/.github/actions/build-myne@main
  with:
    project-directory: src
    output-directory: myne-out
    build-pdf: true
    tex-main-file: Final.tex
    elaborated-deps: true
    upload-pages-artifact: true
```

This workflow builds the project in `docs/` and publishes it to GitHub Pages. Replace `docs` with your project folder. Enable GitHub Pages with **GitHub Actions** as the publishing source in repository settings.

```yaml
name: Publish Myne site

on:
  push:
    branches: [main]

permissions:
  contents: read

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v6
      - uses: James-Oswald/Myne/.github/actions/build-myne@main
        with:
          project-directory: docs
          upload-pages-artifact: true

  deploy:
    needs: build
    runs-on: ubuntu-latest
    permissions:
      pages: write
      id-token: write
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    steps:
      - id: deployment
        uses: actions/deploy-pages@v5
```

The generated directory can also be passed to another host's upload or deploy action through `steps.<build-step-id>.outputs.site-directory`. GitHub Pages requires `pages: write` and `id-token: write` on the deployment job; see [GitHub's Pages workflow guide](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).

`-i` also accepts a project directory. For example, `./bin/myne -i examples/basic --serve` loads `examples/basic/myne.toml` and serves that project's output from the repository root.

## Visual Studio Code extension

The [Myne KaTeX Preview extension](vscode-extension/README.md) opens a live side preview for Lean, Markdown, and LaTeX files. It renders inline and display math with KaTeX and works offline. To package it as a VSIX, run `npm run prepare-assets` from `vscode-extension/` to build the browser assets from the KaTeX submodule, then run `vsce package` there. Generated packaging assets are Git-ignored.

For a Lean `/-- ... -/` comment attached to a `def`, `lemma`, or `theorem`, HTML places the comment and highlighted declaration inside one box. TeX emits the comment in an AMS `definition`, `lemma`, or `theorem` environment and omits the Lean source. Module comments (`/-! ... -/`) remain document prose.

## Usage / Features

Myne can be run at the command line using the myne command. Myne can be configured using a `myne.toml` configuration file or passing configuration options directly as command line arguments.

### Rendering
Myne's main feature is rendering Lean4 and LaTeX content into either LaTeX or web formats. It focuses on speed and seamless integration of Lean4 code and documentation.

To use Myne from the command line, you can run `myne` command. The command will look for a `myne.toml` configuration file in the current directory by default, unless overridden by command line arguments. Configuration options for `myne.toml` are:
- `root` : Specifies the root input Lean4 or LaTeX source file. 
- `format` : Specifies the output format. Supports `tex` for LaTeX directory output and `html` for Verso-esque website output. `both` for generating both formats. Defaults to `html`.
- `outputDir` : Specifies the output directory. defaults to `./out`
- `bibliography` : Links a bibtex bibliography `.bib` file at the end of document. This is useful if root document is a `.lean` file and you dont want to add a `/-! \bibliography{...} -/` comment manually, which would otherwise be required to include the bibliography.
- `commandMap` : A dictionary mapping unsupported LaTeX commands to their replacements for web rendering. This is useful when targeting web output and certain LaTeX commands are not supported, IE, you have a special format `.cls` file that defines custom title or section commands that would otherwise disapear on web rendering if not mapped to supported HTML equivalents.
- `[declarationMap]` : Maps quoted Lean declaration prefixes to `"def"`, `"lemma"`, or `"theorem"` in both HTML and TeX. Myne matches after attributes and modifiers, requires whitespace or the declaration end after the prefix, and chooses the longest configured match before its built-in declaration names. For example, `"def∇" = "def"` and `"inductive∇" = "def"` classify custom Lean syntax as definitions.
- `[web]` : Starts the web rendering options table.
- `[web].style` : Specifies a CSS style file to be used for web output. This allows customizing the appearance of the generated HTML. Built in styles are specified without the extension, while custom styles should include a path with `.css`.
- `[web].pageHeading` : The deepest heading level that starts a new web page. Supports `part`, `chapter`, `section`, `subsection`, and `subsubsection`; defaults to `section`. The selected level and higher-level headings each start pages.
- `[web].tableOfContents` : Set to `true` to show links to headings exactly one level below pages above `pageHeading`. Defaults to `false`.
- `[web].darkModeToggle` : Set to `false` to hide the light/dark theme slider. It is shown at the bottom right by default, and the visitor's choice is shared across generated pages and saved in the browser.
- `[web].dependencyGraph` : Use `"basic"` (the default) for fast string matching, or `"elaborated"` to ask Lean for constants in elaborated declaration types and proof terms. `--elaborated-deps` enables the same mode for one build. This slower mode requires Lean (or Lake for a Lake project) and stops the build if Lean reports an error. It connects declarations included in the Myne document; imported constants without a rendered declaration are read during elaboration but are not shown as nodes.
- The web output includes a `dependencies.html` graph of documented Lean definitions, lemmas, and theorems when declarations are found. Basic mode infers edges by matching names as strings and displays a visible warning about missed or false dependencies; elaborated mode gets references from Lean. Definitions with no dependents are pruned repeatedly until stable; lemmas and theorems remain. Arrows point from dependencies toward declarations they build. The graph offers a layered layout and a Section panels layout. Layered orders declarations by dependency depth; Section panels places each document section in its own labeled, colored panel with a compact grid of declarations. Cycles are grouped into components. Hovering a node highlights its direct dependencies and shows its `@name`, `@nl` (natural language), and `@formula` annotations when present. Drag nodes to rearrange them, drag the background to pan, and use the zoom, fit, reset, layout, and direction controls. Node colors distinguish definitions, lemmas, and theorems. Click once to keep a node selected; right-click or press Escape to clear selection. Double-click opens its declaration. The graph is linked from the page header.

Arguments:
- `-i <input-file-or-project-directory>`: Specifies a Lean4 or LaTeX root file, or loads `myne.toml` from a project directory. Defaults to the root file in the manifest.
- `-f <format>`: output format. Supports `tex` for LaTeX directory output, `html` for website output, or `both`. Defaults to `html`.
- `-o <output>`: Specifies the output directory. Defaults to `./out`
- `-s <style>`: Specifies a CSS style file to be used for web output. This allows customizing the appearance of the generated HTML. Built in styles are specified without the extension, while custom styles should include a path with `.css`
- `--config <file>`: Read a manifest other than `myne.toml`.
- `--watch`: Rebuild when project files change.
- `--serve`: Watch and serve HTML on localhost, with automatic refresh.
- `--port <number>`: Set the serve port; defaults to 8000.

#### Latex Support for Web output:

When targeting web output, Myne will convert LaTeX content into HTML that can be rendered in a web browser, however we only support a small subset of LaTeX commands for being web safe
for now:
##### Math 
- `$...$` for inline math (the overwhelming majority of math commands are supported)
- `$$...$$` for display math
##### Text Formatting
- `\textit{...}` for italic text
- `\textbf{...}` for bold text
##### Sections and Paragraphs
- `\part{...}` for parts
- `\chapter{...}` for chapters
- `\section{...}` for sections
- `\subsection{...}` for subsections
- `\subsubsection{...}` for subsubsections
- `\paragraph{...}` for paragraphs
##### Citations
- `\cite{...}` for citations within the text
- `\citet{...}` for textual citations
- `\bibliography{...}` for including a bibliography file and reference list.
##### Footnotes
- `\footnote{...}` for footnotes within the text

Any other commands are ignored when rendering for the web output.


### Real Time Updating

Myne should provide a watch and serve mode that automatically rebuilds and serves the document in real time as changes are made to the source files, for both web and local output.

### Dependency Visualization

[FUTURE FEATURE, DO NOT IMPLEMENT YET]
Eventually Myne should provide [lean-atlas](https://github.com/NyxFoundation/lean-atlas) like support for dependency visualization. 

### Type and Proof State Information

[FUTURE FEATURE, DO NOT IMPLEMENT YET]
Myne should provide the ability to display type and proof state information for Lean4 code within the document, similar to how Lean4's interactive mode shows this information, and how Verso allows. 


## Philosophy

Myne is meant to solve two problems with Verso and blueprint. Blueprint keeps track of Lean4 and Tex content separately and requires manual specification of what lemmas are used by what theorems, which can make it cumbersome to manage documents that interleave Lean4 code and LaTeX content. Verso solves this issue by introducing a `.lean` document format that uses the elaborator to directly write Lean4 content within the document, but it does not handle anything like dependency visualization, and worse, its slow. 

Core principles: 
- Myne keeps normal builds fast by processing source text directly. Elaborated dependency graphs are available as an explicit, slower mode for projects that need precise declaration references.
- Use Lean's built in documentation system for integrating tex content in lean files. Lean supports 
`/-- doc comments -/` and `/-! module doc comments -/` for documenting Lean files. We can extract these
comments and parse tex content inside of them. 
- Use tex style `$math$` and `$$math$$` for inline and display math within the extracted documentation comments, rather than verso `$[math]` and `$$[math]`.

## Functional Requirements

### Implementation
- Myne is written in a subset of C99 and targets the [Tiny C Compiler (TCC)](https://bellard.org/tcc/) for fast compilation and execution, as Myne is primarily a string processor, it uses [StringZilla](https://github.com/ashvardanian/stringzilla) and parallelism for efficient string manipulation.
- Myne uses makefiles for build automation, allowing for efficient incremental builds and easy integration with the TCC compiler.

### Web Rendering
- Myne should use [KaTeX](https://katex.org/) for rendering and directly include the necessary minified KaTeX CSS and JavaScript in the output directory.
- Myne should extract documentation comments and convert them into HTML, preserving the tex content within `$...$` and `$$...$$` for inline and display math respectively.
- On the web, the doc comment and code after `/-- ... -/` share a styled box. By default, the highlighted code ends at the first blank line. In TeX, `def`, `lemma`, and `theorem` comments become AMS theorem environments and the Lean code is omitted.

- Footnotes should be displayed in the margin for web output, unless the screen is too narrow to accommodate them, in which case they should be displayed inline at the bottom of the content.
- Citations should be displayed in the margin for web output, unless the screen is too narrow to accommodate them, in which case they should be displayed inline at the bottom of the content.

- When rendering tex to web, Myne should use the supported commands, and respect sectioning.
- Sectioning for web should work as follows:
- Anything before the first page-producing heading is rendered on the introduction page.
- By default, `\part{...}`, `\chapter{...}`, and `\section{...}` each start a web page because `pageHeading` defaults to `section`.
- Set `pageHeading = "chapter"` to restore chapter-level page splitting, or select a deeper supported heading for smaller pages.
- Headings below the configured page level render within the current page, beginning at `<h2>` beneath the page title.
- In the root Lean file, the first `#` sets the document title. In an included Lean file, the first `#` becomes its chapter; later `#` headings and `##` headings become sections, and deeper Markdown headings retain that hierarchy in the side navigation.
- Web headings use hierarchical numbers, and definitions, lemmas, and theorems share a counter scoped to the current section.

- Myne supports raw CSS styling for web output, providing some default styles and allowing users to create their own. Myne ships with a set of built-in styles that can be used by specifying their names in the `style` configuration option. Built in styles include:
- `plain` : where theorems, lemmas, and definitions are displayed in a simple round cornered box with a name.
- `verso` : where theorems, lemmas, and definitions are displayed in a simple round cornered box with a name, similar to the style used in the Verso website.
- `hutter` : In the style of Macrus Hutters 2025 book, with definitions in green boxes and theorems and lemmas in red boxes.


### Tex Rendering
- Myne should output valid LaTeX that can be compiled with standard LaTeX engines, preserving all commands and environments used in the source files.

## Myne Project Structure

Myne projects are structured as inclusion (dependency) trees of `.tex` and `.lean` files. Dependencies are made with `\input{...}`, `\bibliography{...}`, and `\includegraphics{}` in tex and lean files. Note that this dependency tree is only enforced for rendering purposes, and not for Lean's module system, which may import files that are outside of this tree.

If the root file is a latex file, it should contain a document class command with a title. If the root file is a lean file, its module doc should contain a top level heading `# ...` that represents the title of the document.


## Annotated Lean declarations

Attached Lean doc comments accept `@name`, `@nl`, `@formula`, and `@proof` fields. `@name` supplies the declaration title; `@nl` supplies its natural-language statement and is the default prose field; `@formula` supplies a display formula; and `@proof` supplies a natural-language proof for lemmas and theorems. Fields begin on their own line and continue until the next field. See `examples/annotations/main.lean` for a complete example.

Add `@capture block` on its own line in a doc comment to include multiple following declarations, including blank lines, in one web code box. End the group with a standalone `--myne:end` line. The marker is excluded from the rendered code. Myne reports an error if the marker is missing. The TeX output still renders the comment once and omits its Lean code. See `examples/showcase/Chapters/Arithmetic.lean` for a complete example. For example:
Use `--myne:appendix_start NAME` and `--myne:appendix_end` as standalone Lean line comments to move the enclosed material to a named appendix after the main text. In web output, appendices are grouped under an **Appendices** category, with each appendix on its own page beneath it and lettered A, B, and so on; declaration numbering restarts within each appendix. TeX output starts an appendix section. Markers must be paired and cannot nest.


```lean
/--
@name Doubling
@capture block
@nl
Doubling adds a number to itself.
-/
def double (n : Nat) := n + n

theorem double_eq_add (n : Nat) : double n = n + n := by rfl
--myne:end
```


For Lean-root projects, `texTemplate` can name a TeX root template and `texClass` can name a local document class. The template must contain `{{MYNE_CONTENT}}` where the rendered Lean document belongs and `{{MYNE_THEOREM_SETUP}}` in its preamble. Myne follows the template's `\input` and `\include` references and writes the rendered template using the template's filename.
