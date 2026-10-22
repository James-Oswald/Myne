const vscode = require('vscode');
const path = require('path');

let previewPanel;
let previewUri;

function escapeHtml(value) {
  return value.replace(/[&<>"']/g, (character) => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;'
  })[character]);
}

function previewHtml(extensionPath, webview, document) {
  const mediaPath = vscode.Uri.file(path.join(extensionPath, 'media', 'katex'));
  const katexCss = webview.asWebviewUri(vscode.Uri.joinPath(mediaPath, 'katex.min.css'));
  const katexJs = webview.asWebviewUri(vscode.Uri.joinPath(mediaPath, 'katex.min.js'));
  const autoRenderJs = webview.asWebviewUri(vscode.Uri.joinPath(mediaPath, 'contrib', 'auto-render.min.js'));
  const nonce = [...Array(24)].map(() => Math.random().toString(36)[2]).join('');
  const source = escapeHtml(document.getText());

  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src ${webview.cspSource} data:; font-src ${webview.cspSource}; style-src ${webview.cspSource} 'unsafe-inline'; script-src ${webview.cspSource} 'nonce-${nonce}';">
  <link rel="stylesheet" href="${katexCss}">
  <style>
    :root { color-scheme: light dark; }
    body { margin: 0; padding: 1.5rem; color: var(--vscode-editor-foreground); background: var(--vscode-editor-background); font: var(--vscode-editor-font-size, 14px)/1.65 var(--vscode-editor-font-family, monospace); }
    .source { white-space: pre-wrap; overflow-wrap: anywhere; }
    .katex-display { overflow-x: auto; overflow-y: hidden; padding: 0.25rem 0; }
    .error { color: var(--vscode-errorForeground); }
  </style>
  <title>Myne KaTeX Preview</title>
</head>
<body>
  <main class="source">${source}</main>
  <script src="${katexJs}"></script>
  <script src="${autoRenderJs}"></script>
  <script nonce="${nonce}">
    renderMathInElement(document.querySelector('.source'), {
      delimiters: [
        { left: '$$', right: '$$', display: true },
        { left: '\\[', right: '\\]', display: true },
        { left: '\\(', right: '\\)', display: false },
        { left: '$', right: '$', display: false }
      ],
      throwOnError: false
    });
  </script>
</body>
</html>`;
}

function activate(context) {
  const extensionPath = context.extensionPath;

  context.subscriptions.push(vscode.workspace.onDidChangeTextDocument((event) => {
    if (previewPanel && previewUri && event.document.uri.toString() === previewUri.toString()) {
      previewPanel.webview.html = previewHtml(extensionPath, previewPanel.webview, event.document);
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand('myne.openKatexPreview', () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor) {
      vscode.window.showInformationMessage('Open a Myne, Lean, Markdown, or LaTeX file to preview its math.');
      return;
    }

    const document = editor.document;
    const mediaRoot = vscode.Uri.file(path.join(extensionPath, 'media'));
    if (previewPanel) {
      previewPanel.reveal(vscode.ViewColumn.Beside);
    } else {
      previewPanel = vscode.window.createWebviewPanel(
        'myneKatexPreview',
        'Myne KaTeX Preview',
        vscode.ViewColumn.Beside,
        { enableScripts: true, localResourceRoots: [mediaRoot] }
      );
      previewPanel.onDidDispose(() => {
        previewPanel = undefined;
        previewUri = undefined;
      }, null, context.subscriptions);
    }

    previewUri = document.uri;
    previewPanel.title = `Math Preview: ${path.basename(document.fileName)}`;
    previewPanel.webview.html = previewHtml(extensionPath, previewPanel.webview, document);
  }));
}

function deactivate() {
  if (previewPanel) previewPanel.dispose();
}

module.exports = { activate, deactivate };
