
# Myne : A Lean4 Document Platform 

Myne is a fast and light-weight Lean4 document platform for creating and managing mathematical documents in a Lean-first way. Myne is meant as an alternitive to tools like [Verso](https://verso.lean-lang.org/) and [leanblueprint](https://github.com/PatrickMassot/leanblueprint).

Myne consumes Lean4 files and LateX files, treating lean comments as part of the document content, and
integrates them seamlessly into the document structure.

Myne complies to either TeX which can be compiled with standard LaTeX tools, or Verso-esque website formats.

## Usage / Features

Myne can be run at the command line using the myne command. Myne can be configured using a `myne.toml` configuration file or passing configuration options directly as command line arguments.

### Rendering
Myne's main feature is rendering Lean4 and LaTeX content into either LaTeX or web formats. It focuses on speed and seamless integration of Lean4 code and documentation.

To use Myne from the command line, you can run `myne` command. The command will look for a `myne.toml` configuration file in the current directory by default, unless overridden by command line arguments. Configuration options for `myne.toml` are:
- `root` : Specifies the root input Lean4 or LaTeX source file. 
- `outputDir` : Specifies the output directory. defaults to `./out`
- `bibliography` : Links a bibtex bibliography `.bib` file at the end of document. This is useful if root document is a `.lean` file and you dont want to add a `/-! \bibliography{...} -/` comment manually, which would otherwise be required to include the bibliography.
- `commandMap` : A dictionary mapping unsupported LaTeX commands to their replacements for web rendering. This is useful when targeting web output and certain LaTeX commands are not supported, IE, you have a special format `.cls` file that defines custom title or section commands that would otherwise disapear on web rendering if not mapped to supported HTML equivalents.

Arguments:
- `-i <input-file>`: Specifies the root input Lean4 or LaTeX source file. Defaults to the root file in the manifest.
- `-f <output-file>`: output format. Supports `tex` for Latex directory output `html` for Verso-esque website output. Defaults to `html`.
- `-o <output>`: Specifies the output directory. Defaults to `./out`

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
- Myne is FAST, Myne builds documents quickly by being a parallel string processor first rather than relying on the Lean elaborator for all processing. [FUTURE FEATURE, DO NOT IMPLEMENT YET] Slow builds with aditional features that require full elaboration, such as type and proof state information, dependency visualization, can be enabled in stages.
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
- The code after doc comments `/-- ... -/` should be rendered as lean code with Lean4 syntax highlighting, this should end on the first double newline after the code block. Display style should change depending on the type of content, IE if `theorem` is encountered on the line after the closing `-/`, it should identify it as a theorem and apply theorem styling, similarly for `lemma` and `def.`

- When rendering tex to web, Myne should use the supported commands, and respect sectioning.
- Sectioning for web should work as follows:
  - Anything before the first `\part{...}` or `\chapter{...}` is considered the introduction and is rendered on the introduction page.
  - `\part{...}` Collections of chapters in the side menu. If there is anthing between a `\part{...}` and the first `\chapter{...}`, An introduction page is created for the part, with a list of chapters.
  - `\chapter{...}`s are their own web pages, the chapter name is used as the page title
  - `\section{...}`s are rendered as `<h2>...</h2>`
  - `\subsection{...}`s are rendered as `<h3>...</h3>`
  - `\subsubsection{...}`s are rendered as `<h4>...</h4>`

### Tex Rendering
- Myne should output valid LaTeX that can be compiled with standard LaTeX engines, preserving all commands and environments used in the source files.

## Myne Project Structure

Myne projects are structured as inclusion (dependency) trees of `.tex` and `.lean` files. Dependencies are made with `\input{...}`, `\bibliography{...}`, and `\includegraphics{}` in tex and lean files. Note that this dependency tree is only enforced for rendering purposes, and not for Lean's module system, which may import files that are outside of this tree.

If the root file is a latex file, it should contain a document class command with a title. If the root file is a lean file, its module doc should contain a top level heading `# ...` that represents the title of the document. 