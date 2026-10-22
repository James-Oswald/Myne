const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const extensionDir = path.resolve(__dirname, '..');
const katexDir = path.resolve(extensionDir, '..', 'vendor', 'katex');
const distDir = path.join(katexDir, 'dist');
const mediaDir = path.join(extensionDir, 'media', 'katex');

function run(command, args, cwd) {
  const result = spawnSync(command, args, { cwd, stdio: 'inherit' });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status || 1);
}

if (!fs.existsSync(path.join(katexDir, 'package.json'))) {
  throw new Error('KaTeX submodule is missing. Run: git submodule update --init vendor/katex');
}

run('corepack', ['yarn', 'install', '--immutable'], katexDir);
run('corepack', ['yarn', 'build'], katexDir);

fs.rmSync(mediaDir, { recursive: true, force: true });
fs.mkdirSync(path.join(mediaDir, 'contrib'), { recursive: true });
fs.copyFileSync(path.join(distDir, 'katex.min.js'), path.join(mediaDir, 'katex.min.js'));
fs.copyFileSync(path.join(distDir, 'katex.min.css'), path.join(mediaDir, 'katex.min.css'));
fs.copyFileSync(path.join(distDir, 'contrib', 'auto-render.min.js'), path.join(mediaDir, 'contrib', 'auto-render.min.js'));
fs.cpSync(path.join(distDir, 'fonts'), path.join(mediaDir, 'fonts'), { recursive: true });
fs.copyFileSync(path.join(katexDir, 'LICENSE'), path.join(mediaDir, 'LICENSE'));

console.log('Prepared VS Code extension assets from vendor/katex.');
