Dependencies are pinned Git submodules. Initialize them with `git submodule update --init --recursive` after cloning.

- `stringzilla` is pinned to v5.1.2. Myne includes only its `find.h` C API.
- `katex` is pinned to v0.16.22. `src/katex_assets.inc` embeds the browser distribution from the matching published npm package, including its license and fonts. The generated include is ignored by Git and rebuilt by `make`. The first build downloads and verifies the pinned npm archive; subsequent builds use the local `.cache` copy.

To refresh the embedded browser distribution after updating the KaTeX submodule, update the version, URL, and checksum in `tools/embed_katex.py`, then run `make clean && make`. The archive can also be supplied with `python3 tools/embed_katex.py --package PATH/TO/katex.tgz`.
