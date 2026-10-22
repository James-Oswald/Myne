# Myne KaTeX Preview

This Visual Studio Code extension opens a live preview beside the active editor and renders KaTeX math in Lean, Myne, Markdown, and LaTeX files. The preview updates as the file changes. The original text stays visible, so prose and document structure remain in context.

It recognizes `$...$`, `$$...$$`, `\\(...\\)`, and `\\[...\\]`. For VSIX packaging, the build script generates the browser assets from Myne's KaTeX submodule; the generated copy is ignored by Git. The packaged extension renders offline.

## Use it

1. Open a `.lean`, `.tex`, or `.md` file.
2. Run **Myne: Open KaTeX Preview** from the Command Palette, or click the preview icon in the editor title bar.
3. Edit the file; the preview refreshes automatically.

## Package from source

From this directory, run `npm run prepare-assets`, then run `vsce package` (install `@vscode/vsce` if needed). Install the resulting VSIX with **Extensions: Install from VSIX...** in Visual Studio Code. The first command builds KaTeX from `../vendor/katex` using Corepack and Yarn, then copies only the generated browser distribution into the ignored `media/katex/` packaging directory.

KaTeX is distributed under the license in `media/katex/LICENSE`.
