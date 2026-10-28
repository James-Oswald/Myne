#include "myne_internal.h"

#include "app_assets.inc"
#include "katex_assets.inc"

void remove_tree(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryA(path);
        else DeleteFileA(path);
        return;
    }
#endif
    struct stat st;
    if (lstat(path, &st)) {
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    DIR *d = opendir(path);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            char *p = join(path, e->d_name);
            remove_tree(p);
            free(p);
        }
    }
    closedir(d);
    rmdir(path);
}

static int b64(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

/* Decode the embedded tar archive into the HTML output's KaTeX directory. */
void unpack_katex(const char *dir) {
    unsigned char *bytes = xmalloc(myne_katex_size);
    size_t used = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < sizeof(myne_katex_chunks) / sizeof(*myne_katex_chunks); i++) {
        for (const char *p = myne_katex_chunks[i]; *p; p++) {
            int v = b64(*p);
            if (v < 0) {
                continue;
            }
            acc = (acc << 6) | (unsigned)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (used < myne_katex_size) {
                    bytes[used++] = (unsigned char)(acc >> bits);
                }
                acc &= (1u << bits) - 1;
            }
        }
    }
    if (used != myne_katex_size) {
        die("embedded KaTeX archive is damaged");
        free(bytes);
        return;
    }
    for (size_t at = 0; at + 512 <= used;) {
        const char *header = (const char *)bytes + at;
        size_t len = strnlen(header, 100);
        if (!len) {
            break;
        }
        char *name = slice(header, len);
        size_t size = strtoul(header + 124, NULL, 8);
        at += 512;
        if (name[0] == '/' || strstr(name, "..") || size > used - at) {
            die("embedded KaTeX archive has invalid entry");
            free(name);
            break;
        }
        char *path = join(dir, name);
        if (write_all(path, (const char *)bytes + at, size)) {
            die("cannot write KaTeX asset %s", name);
        }
        free(path);
        free(name);
        at += (size + 511) & ~(size_t)511;
    }
    free(bytes);
}

int asset(const char *s) {
    const char *ext[] = {".bib",  ".bst", ".cls", ".sty", ".png", ".jpg",
                         ".jpeg", ".gif", ".svg", ".pdf", ".eps"};
    for (size_t i = 0; i < sizeof(ext) / sizeof(*ext); i++) {
        if (ends(s, ext[i])) {
            return 1;
        }
    }
    return 0;
}

/* Copy local assets without following symlinked directories. */
void project_assets(const char *rel, const char *html, const char *tex, int depth) {
    if (depth > 32) {
        return;
    }
    char *src = join(basedir, rel);
    DIR *d = opendir(src);
    if (!d) {
        free(src);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcmp(e->d_name, ".git")) {
            char *child = rel[0] ? join(rel, e->d_name) : xstrdup(e->d_name),
                 *path = join(basedir, child);
            struct stat st;
            if (!lstat(path, &st)
#ifdef _WIN32
                && !is_reparse_point(path)
#endif
                && S_ISDIR(st.st_mode)) {
                if (strncmp(path, output, strlen(output)) && strcmp(e->d_name, "vendor")) {
                    project_assets(child, html, tex, depth + 1);
                }
            } else if (!lstat(path, &st)
#ifdef _WIN32
                       && !is_reparse_point(path)
#endif
                       && S_ISREG(st.st_mode) && asset(child)) {
                char *a = join(html, child), *b = join(tex, child);
                if (copy_file(path, a) || copy_file(path, b)) {
                    die("cannot copy asset %s", child);
                }
                free(a);
                free(b);
            }
            free(child);
            free(path);
        }
    }
    closedir(d);
    free(src);
}


const char *myne_embedded_style(void) { return myne_style; }
const char *myne_embedded_runtime(void) { return myne_runtime; }
