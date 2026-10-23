#include "myne_internal.h"

#ifdef _WIN32
void normalize_path(char *path) {
    for (; *path; path++) if (*path == '\\') *path = '/';
}
int is_reparse_point(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
char *myne_realpath(const char *path, char *resolved) {
    if (!_fullpath(resolved, path, PATH_MAX)) return NULL;
    normalize_path(resolved);
    return resolved;
}
#endif

void die(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    fputs("myne: ", stderr);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
    failed = 1;
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        perror("malloc");
        exit(2);
    }
    return p;
}

char *slice(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *xstrdup(const char *s) {
    return slice(s, strlen(s));
}

void addn(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 64;
        b->s = realloc(b->s, b->cap);
        if (!b->s) {
            exit(2);
        }
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

void add(Buf *b, const char *s) {
    addn(b, s, strlen(s));
}

void ch(Buf *b, char c) {
    addn(b, &c, 1);
}

char *join(const char *a, const char *b) {
    Buf x = {0};
    add(&x, a);
    if (x.n && x.s[x.n - 1] != '/') {
        ch(&x, '/');
    }
    add(&x, b);
    return x.s;
}

int ends(const char *s, const char *e) {
    size_t a = strlen(s), b = strlen(e);
    return a >= b && !strcmp(s + a - b, e);
}

const char *finds(const char *s, size_t n, const char *pat) {
    return (const char *)sz_find(s, n, pat, strlen(pat));
}

char *trim(char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) {
        s[--n] = 0;
    }
    return s;
}

static char *unquote(const char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s != '"' && *s != '\'') {
        return xstrdup(s);
    }
    char q = *s++;
    Buf b = {0};
    while (*s && *s != q) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n':
                ch(&b, '\n');
                break;
            case 't':
                ch(&b, '\t');
                break;
            default:
                ch(&b, *s);
            }
        } else {
            ch(&b, *s);
        }
        s++;
    }
    return b.s ? b.s : xstrdup("");
}

static int config_bool(const char *value, int *result) {
    char *v = unquote(value);
    int valid = 1;
    if (!strcasecmp(v, "true") || !strcmp(v, "1")) {
        *result = 1;
    } else if (!strcasecmp(v, "false") || !strcmp(v, "0")) {
        *result = 0;
    } else {
        valid = 0;
    }
    free(v);
    return valid;
}

int mkdirs(const char *path) {
    char *s = xstrdup(path);
    for (char *p = s + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(s, 0755) && errno != EEXIST) {
                free(s);
                return -1;
            }
            *p = '/';
        }
    }
    int r = mkdir(s, 0755);
    free(s);
    return r == 0 || errno == EEXIST ? 0 : -1;
}

static int parent_dirs(const char *path) {
    char *s = xstrdup(path), *p = strrchr(s, '/');
    if (p) {
        *p = 0;
    }
    int r = p ? mkdirs(s) : 0;
    free(s);
    return r;
}

char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    Buf b = {0};
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f))) {
        addn(&b, tmp, n);
    }
    if (ferror(f)) {
        free(b.s);
        b.s = NULL;
    }
    fclose(f);
    return b.s ? b.s : xstrdup("");
}

int write_all(const char *path, const char *s, size_t n) {
    if (parent_dirs(path)) {
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    int ok = fwrite(s, 1, n, f) == n;
    if (fclose(f)) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    if (parent_dirs(dst)) {
        fclose(in);
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[16384];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in))) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    if (ferror(in)) {
        ok = 0;
    }
    if (fclose(in)) {
        ok = 0;
    }
    if (fclose(out)) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

long long stamp(const char *path) {
    struct stat st;
    if (stat(path, &st)) {
        return -1;
    }
#ifdef _WIN32
    return (long long)st.st_mtime * 1000000000LL + st.st_size;
#elif defined(__APPLE__)
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec + st.st_size;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + st.st_size;
#endif
}

/* Resolve through realpath so an include cannot escape the project tree. */
char *project_rel(const char *path) {
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        return NULL;
    }
    size_t n = strlen(basedir);
    if (strncmp(resolved, basedir, n) || resolved[n] != '/') {
        return NULL;
    }
    return xstrdup(resolved + n + 1);
}

char *path_from(const char *from, const char *ref) {
    char *dir = xstrdup(from), *p = strrchr(dir, '/');
    if (p) {
        *p = 0;
    } else {
        dir[0] = 0;
    }
    char *a = join(basedir, dir), *b = join(a, ref);
    free(dir);
    free(a);
    return b;
}

/* Includes may omit the extension; prefer TeX before Lean. */
char *resolve(const char *from, const char *ref) {
    char *s = path_from(from, ref), *r = project_rel(s);
    free(s);
    if (r) {
        return r;
    }
    if (ends(ref, ".tex") || ends(ref, ".lean")) {
        return NULL;
    }
    char *q = path_from(from, ref);
    Buf b = {0};
    add(&b, q);
    add(&b, ".tex");
    r = project_rel(b.s);
    if (!r) {
        b.n = strlen(q);
        b.s[b.n] = 0;
        add(&b, ".lean");
        r = project_rel(b.s);
    }
    free(q);
    free(b.s);
    return r;
}

/* Read a balanced brace argument, preserving nested groups. */
int arg_at(const char *s, size_t n, size_t *pos, char **arg) {
    size_t i = *pos;
    while (i < n && isspace((unsigned char)s[i])) {
        i++;
    }
    if (i >= n || s[i] != '{') {
        return 0;
    }
    size_t start = ++i;
    int depth = 1;
    while (i < n && depth) {
        if (s[i] == '\\' && i + 1 < n) {
            i += 2;
            continue;
        }
        if (s[i] == '{') {
            depth++;
        }
        if (s[i] == '}') {
            depth--;
        }
        i++;
    }
    if (depth) {
        return 0;
    }
    *arg = slice(s + start, i - start - 1);
    *pos = i;
    return 1;
}

int include_at(const char *s, size_t n, size_t i, size_t *end, char **arg) {
    size_t k = 0;
    if (i + 6 <= n && !memcmp(s + i, "\\input", 6)) {
        k = 6;
    }
    if (i + 8 <= n && !memcmp(s + i, "\\include", 8)) {
        k = 8;
    }
    if (!k || (i + k < n && isalpha((unsigned char)s[i + k]))) {
        return 0;
    }
    size_t p = i + k;
    if (!arg_at(s, n, &p, arg)) {
        return 0;
    }
    *end = p;
    return 1;
}

int graphic_at(const char *s, size_t n, size_t i, size_t *end, char **arg) {
    if (i + 16 > n || memcmp(s + i, "\\includegraphics", 16)) {
        return 0;
    }
    size_t p = i + 16;
    if (p < n && isalpha((unsigned char)s[p])) {
        return 0;
    }
    while (p < n && isspace((unsigned char)s[p])) {
        p++;
    }
    if (p < n && s[p] == '[') {
        while (p < n && s[p] != ']') {
            p++;
        }
        if (p < n) {
            p++;
        }
    }
    if (!arg_at(s, n, &p, arg)) {
        return 0;
    }
    *end = p;
    return 1;
}

int file_index(const char *rel) {
    for (int i = 0; i < nfiles; i++) {
        if (!strcmp(files[i].rel, rel)) {
            return i;
        }
    }
    return -1;
}

/* A visiting file signals a cycle; completed files are reused. */
int gather(const char *rel) {
    int id = file_index(rel);
    if (id >= 0) {
        if (files[id].state == 1) {
            die("inclusion cycle at %s", rel);
            return -1;
        }
        return id;
    }
    if (nfiles >= MAX_FILES) {
        die("too many included files");
        return -1;
    }
    id = nfiles++;
    File *f = &files[id];
    memset(f, 0, sizeof(*f));
    f->rel = xstrdup(rel);
    f->lean = ends(rel, ".lean");
    f->state = 1;
    char *path = join(basedir, rel);
    f->data = read_all(path);
    f->stamp = stamp(path);
    free(path);
    if (!f->data) {
        die("cannot read %s", rel);
        return -1;
    }
    const char *s = f->data;
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        if (!f->lean && s[i] == '%' && (i == 0 || s[i - 1] != '\\')) {
            while (i < n && s[i] != '\n') {
                i++;
            }
            continue;
        }
        size_t end;
        char *ref = NULL;
        if (s[i] == '\\' && include_at(s, n, i, &end, &ref)) {
            char *child = resolve(rel, ref);
            if (!child) {
                die("missing include %s from %s", ref, rel);
            } else {
                gather(child);
                free(child);
            }
            free(ref);
            i = end;
            if (failed) {
                return -1;
            }
        } else {
            i++;
        }
    }
    f->state = 2;
    return id;
}

void clear_project(void) {
    for (int i = 0; i < nfiles; i++) {
        free(files[i].rel);
        free(files[i].data);
        free(files[i].tex);
        memset(&files[i], 0, sizeof(File));
    }
    for (int i = 0; i < nsegs; i++) {
        free(segs[i].s);
        free(segs[i].doc_name);
        free(segs[i].doc_nl);
        free(segs[i].doc_formula);
    }
    for (int i = 0; i < nappendix_segs; i++) {
        free(appendix_segs[i].s);
        free(appendix_segs[i].doc_name);
        free(appendix_segs[i].doc_nl);
        free(appendix_segs[i].doc_formula);
    }
    for (int i = 0; i < npages; i++) {
        free(pages[i].title);
        free(pages[i].slug);
        free(pages[i].number);
        free(pages[i].body.s);
        free(pages[i].notes.s);
        free(pages[i].toc.s);
        memset(&pages[i], 0, sizeof(Page));
    }
    for (int i = 0; i < ndependency_nodes; i++) {
        free(dependency_nodes[i].name);
        free(dependency_nodes[i].source);
        free(dependency_nodes[i].slug);
        free(dependency_nodes[i].doc_name);
        free(dependency_nodes[i].doc_nl);
        free(dependency_nodes[i].doc_formula);
        free(dependency_nodes[i].resolved_name);
        memset(&dependency_nodes[i], 0, sizeof(DependencyNode));
    }
    for (int i = 0; i < nlean_dependency_decls; i++) {
        free(lean_dependency_decls[i].name);
        for (int j = 0; j < lean_dependency_decls[i].ndeps; j++) {
            free(lean_dependency_decls[i].deps[j]);
        }
        free(lean_dependency_decls[i].deps);
    }
    free(lean_dependency_decls);
    lean_dependency_decls = NULL;
    nlean_dependency_decls = lean_dependency_cap = 0;
    free(elaborated_edges);
    elaborated_edges = NULL;
    elaborated_edge_width = 0;
    for (int i = 0; i < nbibs; i++) {
        free(bibs[i].key);
        free(bibs[i].author);
        free(bibs[i].title);
        free(bibs[i].year);
    }
    nfiles = nsegs = nappendix_segs = npages = nbibs = ncites = nfoot =
        ndependency_nodes = 0;
    appendix_capture = 0;
    current = part = 0;
    chapter_number = section_number = subsection_number = subsubsection_number = 0;
    chapter_active = statement_number = 0;
    statement_scope[0] = 0;
    bib_page = -1;
    failed = 0;
}

/* Find the assignment delimiter without confusing it with a quoted map key. */
static char *config_equals(char *line) {
    char quote = 0;
    int escaped = 0;
    for (char *p = line; *p; p++) {
        if (quote) {
            if (escaped) {
                escaped = 0;
            } else if (*p == '\\') {
                escaped = 1;
            } else if (*p == quote) {
                quote = 0;
            }
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '=') {
            return p;
        }
    }
    return NULL;
}

/* Parse the documented manifest keys and mapping tables. */
void load_config(const char *path) {
    char *data = read_all(path);
    if (!data) {
        return;
    }
    enum { CONFIG_MAIN, CONFIG_COMMAND_MAP, CONFIG_DECLARATION_MAP } table = CONFIG_MAIN;
    for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n")) {
        char *p = trim(line);
        if (!*p || *p == '#') {
            continue;
        }
        if (*p == '[') {
            table = !strcmp(p, "[commandMap]") ? CONFIG_COMMAND_MAP
                    : !strcmp(p, "[declarationMap]") ? CONFIG_DECLARATION_MAP
                    : CONFIG_MAIN;
            continue;
        }
        char *eq = config_equals(p);
        if (!eq) {
            continue;
        }
        *eq++ = 0;
        char *key = trim(p), *value = trim(eq);
        if (table == CONFIG_COMMAND_MAP) {
            if (nmaps < 64) {
                maps[nmaps].from = unquote(key);
                maps[nmaps++].to = unquote(value);
            }
            continue;
        }
        if (table == CONFIG_DECLARATION_MAP) {
            size_t key_len = strlen(key);
            if (key_len < 2 || (key[0] != '"' && key[0] != '\'') ||
                key[key_len - 1] != key[0]) {
                die("declarationMap keys must be quoted strings");
                continue;
            }
            char *from = unquote(key), *to = unquote(value);
            int kind = !strcmp(to, "def") ? LEAN_DEF
                       : !strcmp(to, "lemma") ? LEAN_LEMMA
                       : !strcmp(to, "theorem") ? LEAN_THEOREM : LEAN_CODE;
            if (!*from || kind == LEAN_CODE) {
                die("declarationMap entries need a nonempty key and def, lemma, or theorem value");
            } else if (ndeclaration_maps == 64) {
                die("too many declarationMap entries");
            } else {
                int duplicate = 0;
                for (int j = 0; j < ndeclaration_maps; j++) {
                    duplicate |= !strcmp(declaration_maps[j].from, from);
                }
                if (duplicate) {
                    die("duplicate declarationMap key: %s", from);
                } else {
                    declaration_maps[ndeclaration_maps++] = (DeclarationMapping){from, kind};
                    from = NULL;
                }
            }
            free(from);
            free(to);
            continue;
        }
        if (!strcmp(key, "root")) {
            root = unquote(value);
        } else if (!strcmp(key, "format")) {
            format = unquote(value);
        } else if (!strcmp(key, "outputDir")) {
            char *v = unquote(value);
            snprintf(output, sizeof(output), "%s", v);
            free(v);
        } else if (!strcmp(key, "style")) {
            style = unquote(value);
        } else if (!strcmp(key, "darkModeToggle")) {
            if (!config_bool(value, &dark_mode_toggle)) {
                die("darkModeToggle must be true or false");
            }
        } else if (!strcmp(key, "tableOfContents")) {
            if (!config_bool(value, &web_table_of_contents)) {
                die("tableOfContents must be true or false");
            }
        } else if (!strcmp(key, "pageHeading")) {
            page_heading = unquote(value);
        } else if (!strcmp(key, "dependencyGraph")) {
            char *mode = unquote(value);
            if (!strcmp(mode, "basic")) elaborated_dependency_graph = 0;
            else if (!strcmp(mode, "elaborated")) elaborated_dependency_graph = 1;
            else die("dependencyGraph must be basic or elaborated");
            free(mode);
        } else if (!strcmp(key, "texTemplate")) {
            tex_template = unquote(value);
        } else if (!strcmp(key, "texClass")) {
            tex_class = unquote(value);
        } else if (!strcmp(key, "bibliography")) {
            if (*value == '[') {
                char *q = value + 1;
                while (*q && *q != ']' && nbibcfg < 64) {
                    while (*q && (*q == ',' || isspace((unsigned char)*q))) {
                        q++;
                    }
                    if (*q == '"' || *q == '\'') {
                        char quote = *q++, *e = strchr(q, quote);
                        if (!e) {
                            break;
                        }
                        bibcfg[nbibcfg++] = slice(q, e - q);
                        q = e + 1;
                    } else {
                        break;
                    }
                }
            } else if (nbibcfg < 64) {
                bibcfg[nbibcfg++] = unquote(value);
            }
        }
    }
    free(data);
}

