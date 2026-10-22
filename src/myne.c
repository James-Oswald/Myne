#ifndef _WIN32
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stringzilla/find.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <strings.h>
#define mkdir(path, mode) _mkdir(path)
#define lstat(path, st) stat(path, st)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define getcwd _getcwd
#define access _access
typedef SOCKET myne_socket;
#define socket_close(s) closesocket(s)
#define SOCKET_INVALID INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
typedef int myne_socket;
#define socket_close(s) close(s)
#define SOCKET_INVALID (-1)
#endif
#include "app_assets.inc"
#include "katex_assets.inc"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifdef _WIN32
static void normalize_path(char *path) {
    for (; *path; path++) if (*path == '\\') *path = '/';
}
static int is_reparse_point(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
static char *myne_realpath(const char *path, char *resolved) {
    if (!_fullpath(resolved, path, PATH_MAX)) return NULL;
    normalize_path(resolved);
    return resolved;
}
#define realpath myne_realpath
#endif
#define MAX_FILES 256
#define MAX_PAGES 128
#define MAX_SEGS 4096
#define MAX_BIB 512

enum { SEG_TEXT, SEG_CODE, SEG_BOX_OPEN, SEG_BOX_CLOSE, SEG_APPENDIX_START };

enum { LEAN_CODE, LEAN_DEF, LEAN_LEMMA, LEAN_THEOREM };

/* Buffers own their storage. The document arrays are reset before each build. */
typedef struct {
    char *s;
    size_t n, cap;
} Buf;

typedef struct {
    char *rel, *data, *tex;
    int lean, state, has_boxes;
    long long stamp;
} File;

typedef struct {
    int kind;
    char *s;
    char *doc_name, *doc_nl, *doc_formula;
    int file;
} Segment;

typedef struct {
    char *title, *slug, *number;
    Buf body, notes, toc;
    int toc_count;
    int part, heading;
} Page;

typedef struct {
    char *name, *source, *slug, *doc_name, *doc_nl, *doc_formula, *resolved_name;
    int kind, segment, page, file, graph_id;
    size_t offset;
} DependencyNode;

typedef struct {
    char *name, **deps;
    int file, ndeps, cap;
} LeanDependencyDecl;

typedef struct {
    char *key, *author, *title, *year;
    int number;
} Bib;

typedef struct {
    char *from, *to;
} Mapping;

typedef struct {
    char *from;
    int kind;
} DeclarationMapping;

static File files[MAX_FILES];
static int nfiles;
static Segment segs[MAX_SEGS], appendix_segs[MAX_SEGS];
static int nsegs, nappendix_segs, appendix_capture;
static Page pages[MAX_PAGES];
static int npages, current, part, bib_page;
static DependencyNode dependency_nodes[MAX_SEGS];
static int ndependency_nodes;
static LeanDependencyDecl *lean_dependency_decls;
static int nlean_dependency_decls, lean_dependency_cap;
static unsigned char *elaborated_edges;
static int elaborated_edge_width, elaborated_dependency_graph;
static Bib bibs[MAX_BIB];
static int nbibs, ncites;
static Mapping maps[64];
static int nmaps;
static DeclarationMapping declaration_maps[64];
static int ndeclaration_maps;
static char basedir[PATH_MAX], output[PATH_MAX], config_path[PATH_MAX];
static char *root, *format, *style, *tex_template, *tex_class, *page_heading, *bibcfg[64];
static int nbibcfg;
static int live, port = 8000, failed, root_index, tex_template_index = -1, tex_class_index = -1;
static int dark_mode_toggle = 1, page_heading_level, web_table_of_contents;
static int chapter_number, section_number, subsection_number, subsubsection_number;
static int chapter_active, statement_number;
static char statement_scope[64];
static volatile unsigned long epoch;
static pthread_mutex_t translation_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t epoch_lock = PTHREAD_MUTEX_INITIALIZER;
static int nfoot;

/* Identify the declaration immediately following a Lean doc comment. */
static int lean_kind(const char *s, size_t n) {
    size_t i = 0;
    const char *modifiers[] = {"private", "protected", "noncomputable", "partial", "unsafe"};
    for (;;) {
        while (i < n && isspace((unsigned char)s[i])) {
            i++;
        }
        if (i + 1 < n && s[i] == '-' && s[i + 1] == '-') {
            while (i < n && s[i] != '\n') {
                i++;
            }
            continue;
        }
        if (i + 1 < n && s[i] == '@' && s[i + 1] == '[') {
            i += 2;
            int depth = 1;
            while (i < n && depth) {
                if (s[i] == '[') {
                    depth++;
                } else if (s[i] == ']') {
                    depth--;
                }
                i++;
            }
            continue;
        }
        int found = 0;
        for (size_t j = 0; j < sizeof(modifiers) / sizeof(*modifiers); j++) {
            size_t len = strlen(modifiers[j]);
            if (i + len < n && !memcmp(s + i, modifiers[j], len) &&
                isspace((unsigned char)s[i + len])) {
                i += len;
                while (i < n && isspace((unsigned char)s[i])) {
                    i++;
                }
                found = 1;
                break;
            }
        }
        if (!found) {
            break;
        }
    }
    int mapped = LEAN_CODE;
    size_t longest = 0;
    for (int j = 0; j < ndeclaration_maps; j++) {
        size_t len = strlen(declaration_maps[j].from);
        if (len > longest && i + len <= n &&
            !memcmp(s + i, declaration_maps[j].from, len) &&
            (i + len == n || isspace((unsigned char)s[i + len]))) {
            longest = len;
            mapped = declaration_maps[j].kind;
        }
    }
    if (longest) {
        return mapped;
    }
    const char *definitions[] = {"def", "abbrev", "opaque", "structure", "class", "inductive"};
    for (size_t j = 0; j < sizeof(definitions) / sizeof(*definitions); j++) {
        size_t len = strlen(definitions[j]);
        if (i + len <= n && !memcmp(s + i, definitions[j], len) &&
            (i + len == n || isspace((unsigned char)s[i + len]))) {
            return LEAN_DEF;
        }
    }
    const char *propositions[] = {"lemma", "theorem"};
    for (int kind = LEAN_LEMMA; kind <= LEAN_THEOREM; kind++) {
        size_t len = strlen(propositions[kind - LEAN_LEMMA]);
        if (i + len <= n && !memcmp(s + i, propositions[kind - LEAN_LEMMA], len) &&
            (i + len == n || isspace((unsigned char)s[i + len]))) {
            return kind;
        }
    }
    return LEAN_CODE;
}

static const char *lean_kind_name(int kind) {
    const char *names[] = {"code", "def", "lemma", "theorem"};
    return names[kind];
}

static const char *lean_environment(int kind) {
    const char *names[] = {NULL, "definition", "lemma", "theorem"};
    return names[kind];
}

static void addn(Buf *b, const char *s, size_t n);
static void add(Buf *b, const char *s);
static void ch(Buf *b, char c);
static void die(const char *fmt, ...);

typedef struct {
    Buf name, nl, formula, proof, plain;
    int annotated, capture;
} LeanDoc;

static void lean_doc_free(LeanDoc *doc) {
    free(doc->name.s);
    free(doc->nl.s);
    free(doc->formula.s);
    free(doc->proof.s);
    free(doc->plain.s);
    memset(doc, 0, sizeof(*doc));
}

static int lean_doc_tag(const char *s, size_t n, const char **tag, size_t *tag_len,
                        size_t *value_start) {
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    if (i < n && s[i] == '*') {
        i++;
        if (i < n && s[i] == ' ') {
            i++;
        }
        while (i < n && (s[i] == ' ' || s[i] == '\t')) {
            i++;
        }
    }
    if (i >= n || s[i] != '@') {
        return 0;
    }
    size_t begin = ++i;
    while (i < n && isalpha((unsigned char)s[i])) {
        i++;
    }
    size_t len = i - begin;
    if (!((len == 4 && !memcmp(s + begin, "name", 4)) ||
          (len == 2 && !memcmp(s + begin, "nl", 2)) ||
          (len == 7 && !memcmp(s + begin, "formula", 7)) ||
          (len == 5 && !memcmp(s + begin, "proof", 5)) ||
          (len == 7 && !memcmp(s + begin, "capture", 7)))) {
        return 0;
    }
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == ':')) {
        i++;
    }
    *tag = s + begin;
    *tag_len = len;
    *value_start = i;
    return 1;
}

static void lean_doc_append(Buf *b, const char *s, size_t n) {
    while (n && isspace((unsigned char)*s)) {
        s++;
        n--;
    }
    while (n && isspace((unsigned char)s[n - 1])) {
        n--;
    }
    if (n) {
        if (b->n && b->s[b->n - 1] != '\n') {
            ch(b, '\n');
        }
        addn(b, s, n);
        ch(b, '\n');
    }
}

static size_t lean_doc_trimmed_len(const Buf *b) {
    size_t n = b->n;
    while (n && isspace((unsigned char)b->s[n - 1])) {
        n--;
    }
    return n;
}

static LeanDoc lean_doc_parse(const char *s, size_t n) {
    LeanDoc doc = {0};
    Buf *field = &doc.plain;
    size_t i = 0;
    while (i < n) {
        size_t end = i;
        while (end < n && s[end] != '\n') {
            end++;
        }
        const char *tag = NULL;
        size_t tag_len = 0, value_start = 0;
        if (lean_doc_tag(s + i, end - i, &tag, &tag_len, &value_start)) {
            doc.annotated = 1;
            if (tag_len == 7 && !memcmp(tag, "capture", 7)) {
                const char *value = s + i + value_start;
                size_t value_len = end - i - value_start;
                while (value_len && isspace((unsigned char)value[value_len - 1])) {
                    value_len--;
                }
                if (value_len != 5 || memcmp(value, "block", 5) || doc.capture) {
                    die("expected a single @capture block directive");
                }
                doc.capture = 1;
                field = NULL;
            } else if (tag_len == 4) {
                field = &doc.name;
                lean_doc_append(field, s + i + value_start, end - i - value_start);
                field = NULL;
            } else {
                field = tag_len == 2 ? &doc.nl : tag_len == 7 ? &doc.formula : &doc.proof;
                lean_doc_append(field, s + i + value_start, end - i - value_start);
            }
        } else {
            if (!field) {
                field = &doc.plain;
            }
            lean_doc_append(field, s + i, end - i);
        }
        i = end < n ? end + 1 : end;
    }
    return doc;
}

static void lean_doc_body(Buf *b, LeanDoc *doc, int kind, int for_tex) {
    Buf *natural = doc->nl.n ? &doc->nl : &doc->plain;
    if (natural->n) {
        addn(b, natural->s, natural->n);
    }
    if (doc->formula.n) {
        size_t formula_len = lean_doc_trimmed_len(&doc->formula);
        if (b->n && b->s[b->n - 1] != '\n') {
            ch(b, '\n');
        }
        ch(b, '\n');
        if (formula_len < 2 || strncmp(doc->formula.s, "$$", 2)) {
            add(b, "$$");
        }
        addn(b, doc->formula.s, formula_len);
        if (formula_len < 2 || strncmp(doc->formula.s + formula_len - 2, "$$", 2)) {
            add(b, "$$");
        }
        add(b, "\n\n");
    }
    if (doc->proof.n && (kind == LEAN_LEMMA || kind == LEAN_THEOREM)) {
        if (for_tex) {
            add(b, "\\begin{proof}\n");
        } else {
            add(b, "\\textit{Proof.} ");
        }
        addn(b, doc->proof.s, doc->proof.n);
        if (for_tex) {
            add(b, "\n\\end{proof}\n");
        } else {
            add(b, "\n");
        }
    }
}

static void die(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    fputs("myne: ", stderr);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
    failed = 1;
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        perror("malloc");
        exit(2);
    }
    return p;
}

static char *slice(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *xstrdup(const char *s) {
    return slice(s, strlen(s));
}

static void addn(Buf *b, const char *s, size_t n) {
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

static void add(Buf *b, const char *s) {
    addn(b, s, strlen(s));
}

static void ch(Buf *b, char c) {
    addn(b, &c, 1);
}

static char *join(const char *a, const char *b) {
    Buf x = {0};
    add(&x, a);
    if (x.n && x.s[x.n - 1] != '/') {
        ch(&x, '/');
    }
    add(&x, b);
    return x.s;
}

static int ends(const char *s, const char *e) {
    size_t a = strlen(s), b = strlen(e);
    return a >= b && !strcmp(s + a - b, e);
}

static const char *finds(const char *s, size_t n, const char *pat) {
    return (const char *)sz_find(s, n, pat, strlen(pat));
}

static char *trim(char *s) {
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

static int mkdirs(const char *path) {
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

static char *read_all(const char *path) {
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

static int write_all(const char *path, const char *s, size_t n) {
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

static int copy_file(const char *src, const char *dst) {
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

static long long stamp(const char *path) {
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
static char *project_rel(const char *path) {
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

static char *path_from(const char *from, const char *ref) {
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
static char *resolve(const char *from, const char *ref) {
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
static int arg_at(const char *s, size_t n, size_t *pos, char **arg) {
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

static int include_at(const char *s, size_t n, size_t i, size_t *end, char **arg) {
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

static int graphic_at(const char *s, size_t n, size_t i, size_t *end, char **arg) {
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

static int file_index(const char *rel) {
    for (int i = 0; i < nfiles; i++) {
        if (!strcmp(files[i].rel, rel)) {
            return i;
        }
    }
    return -1;
}

/* A visiting file signals a cycle; completed files are reused. */
static int gather(const char *rel) {
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

static void clear_project(void) {
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
static void load_config(const char *path) {
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

/* Parse a standalone appendix marker and return its line's end. */
static int appendix_directive(const char *s, size_t n, size_t i, size_t *next, char **name) {
    if (i && s[i - 1] != '\n') {
        return 0;
    }
    size_t line_end = i;
    while (line_end < n && s[line_end] != '\n') {
        line_end++;
    }
    size_t p = i;
    while (p < line_end && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) {
        p++;
    }
    static const char start[] = "--myne:appendix_start";
    static const char end[] = "--myne:appendix_end";
    if (line_end - p >= sizeof(start) - 1 &&
        !memcmp(s + p, start, sizeof(start) - 1) &&
        (p + sizeof(start) - 1 == line_end || isspace((unsigned char)s[p + sizeof(start) - 1]))) {
        p += sizeof(start) - 1;
        while (p < line_end && isspace((unsigned char)s[p])) {
            p++;
        }
        size_t tail = line_end;
        while (tail > p && isspace((unsigned char)s[tail - 1])) {
            tail--;
        }
        if (tail == p) {
            die("--myne:appendix_start requires a name");
            *next = line_end < n ? line_end + 1 : line_end;
            return 1;
        }
        *name = slice(s + p, tail - p);
        *next = line_end < n ? line_end + 1 : line_end;
        return 1;
    }
    if (line_end - p >= sizeof(end) - 1 &&
        !memcmp(s + p, end, sizeof(end) - 1)) {
        p += sizeof(end) - 1;
        while (p < line_end && isspace((unsigned char)s[p])) {
            p++;
        }
        if (p == line_end) {
            *next = line_end < n ? line_end + 1 : line_end;
            return 2;
        }
    }
    return 0;
}

/* Segments keep document order while distinguishing prose from Lean code. */
static void segment(int kind, const char *s, size_t n) {
    if (!n && kind != SEG_BOX_CLOSE) {
        return;
    }
    Segment *target = appendix_capture ? appendix_segs : segs;
    int *count = appendix_capture ? &nappendix_segs : &nsegs;
    if (*count >= MAX_SEGS) {
        die("document has too many segments");
        return;
    }
    target[*count].kind = kind;
    target[*count].doc_name = NULL;
    target[*count].doc_nl = NULL;
    target[*count].doc_formula = NULL;
    target[*count].file = -1;
    target[(*count)++].s = slice(s, n);
}

static void markdown_tex(Buf *b, const char *s, size_t n, int isroot, int *top_heading_seen);
static void flatten(int id, int depth);

/* Expand includes at their source location to retain document order. */
static void expand_text(const char *source, const char *s, size_t n, int depth) {
    size_t start = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] == '%' && (i == 0 || s[i - 1] != '\\')) {
            while (i < n && s[i] != '\n') {
                i++;
            }
            continue;
        }
        size_t end;
        char *ref = NULL;
        if (s[i] == '\\' && graphic_at(s, n, i, &end, &ref)) {
            segment(0, s + start, i - start);
            char *path = path_from(source, ref), *relative = project_rel(path);
            if (!relative) {
                die("missing graphic %s from %s", ref, source);
            } else {
                Buf graphic = {0};
                add(&graphic, "\\includegraphics{");
                add(&graphic, relative);
                ch(&graphic, '}');
                segment(0, graphic.s, graphic.n);
                free(graphic.s);
            }
            free(path);
            free(relative);
            free(ref);
            i = end;
            start = i;
        } else if (s[i] == '\\' && include_at(s, n, i, &end, &ref)) {
            segment(0, s + start, i - start);
            char *child = resolve(source, ref);
            free(ref);
            if (!child) {
                die("missing include in %s", source);
                return;
            }
            int j = file_index(child);
            free(child);
            if (j < 0) {
                die("untracked include in %s", source);
                return;
            }
            flatten(j, depth + 1);
            i = end;
            start = i;
        } else {
            i++;
        }
    }
    segment(SEG_TEXT, s + start, n - start);
}

typedef struct {
    size_t end, next;
} LeanCodeEnd;

/* A capture marker must be a whole line comment outside strings and block comments. */
static LeanCodeEnd lean_code_end(const char *s, size_t n, size_t begin, int capture,
                                 const char *rel) {
    if (!capture) {
        for (size_t i = begin; i < n;) {
            size_t line_end = i;
            while (line_end < n && s[line_end] != '\n') {
                line_end++;
            }
            size_t next = 0;
            char *name = NULL;
            int directive = appendix_directive(s, n, i, &next, &name);
            free(name);
            if (directive) {
                size_t end = i;
                while (end > begin && isspace((unsigned char)s[end - 1])) {
                    end--;
                }
                return (LeanCodeEnd){end, i};
            }
            size_t blank_line = line_end + 1;
            if (line_end < n && blank_line < n && s[blank_line] == '\r') {
                blank_line++;
            }
            if (line_end < n && blank_line < n && s[blank_line] == '\n') {
                size_t end = line_end;
                while (end > begin && isspace((unsigned char)s[end - 1])) {
                    end--;
                }
                return (LeanCodeEnd){end, line_end + 1};
            }
            i = line_end < n ? line_end + 1 : line_end;
        }
        return (LeanCodeEnd){n, n};
    }
    int block_depth = 0, quoted = 0, escaped = 0;
    for (size_t i = begin; i < n;) {
        size_t line_end = i;
        while (line_end < n && s[line_end] != '\n') {
            line_end++;
        }
        if (!block_depth && !quoted) {
            size_t p = i;
            while (p < line_end && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) {
                p++;
            }
            size_t tail = line_end;
            while (tail > p && (s[tail - 1] == ' ' || s[tail - 1] == '\t' || s[tail - 1] == '\r')) {
                tail--;
            }
            if (tail - p == 10 && !memcmp(s + p, "--myne:end", 10)) {
                size_t end = i;
                while (end > begin && isspace((unsigned char)s[end - 1])) {
                    end--;
                }
                if (end == begin) {
                    die("empty @capture block in %s", rel);
                }
                return (LeanCodeEnd){end, line_end < n ? line_end + 1 : line_end};
            }
        }
        for (size_t p = i; p < line_end;) {
            if (quoted) {
                if (escaped) {
                    escaped = 0;
                } else if (s[p] == '\\') {
                    escaped = 1;
                } else if (s[p] == '"') {
                    quoted = 0;
                }
                p++;
            } else if (p + 1 < line_end && s[p] == '/' && s[p + 1] == '-') {
                block_depth++;
                p += 2;
            } else if (block_depth && p + 1 < line_end && s[p] == '-' && s[p + 1] == '/') {
                block_depth--;
                p += 2;
            } else if (!block_depth && p + 1 < line_end && s[p] == '-' && s[p + 1] == '-') {
                break;
            } else if (!block_depth && s[p] == '"') {
                quoted = 1;
                p++;
            } else {
                p++;
            }
        }
        i = line_end < n ? line_end + 1 : line_end;
    }
    die("missing --myne:end for @capture block in %s", rel);
    return (LeanCodeEnd){n, n};
}

/* Lean prose lives in /-! and /-- comments; the latter owns the next code block. */
static void flatten(int id, int depth) {
    if (depth > MAX_FILES) {
        die("inclusion depth exceeded");
        return;
    }
    File *f = &files[id];
    const char *s = f->data;
    size_t n = strlen(s);
    if (!f->lean) {
        expand_text(f->rel, s, n, depth);
        return;
    }
    int top_heading_seen = 0;
    for (size_t i = 0; i < n;) {
        size_t next = 0;
        char *appendix_name = NULL;
        int directive = appendix_directive(s, n, i, &next, &appendix_name);
        if (directive) {
            if (failed) {
                free(appendix_name);
                return;
            }
            if (directive == 1) {
                if (appendix_capture) {
                    die("nested --myne:appendix_start in %s", f->rel);
                    free(appendix_name);
                    return;
                }
                if (nappendix_segs >= MAX_SEGS) {
                    die("document has too many appendix segments");
                    free(appendix_name);
                    return;
                }
                appendix_segs[nappendix_segs++] =
                    (Segment){SEG_APPENDIX_START, appendix_name, NULL, NULL, NULL, -1};
                appendix_capture = 1;
            } else {
                if (!appendix_capture) {
                    die("--myne:appendix_end without appendix_start in %s", f->rel);
                    return;
                }
                appendix_capture = 0;
            }
            i = next;
            continue;
        }
        int doc =
            i + 3 <= n && s[i] == '/' && s[i + 1] == '-' && (s[i + 2] == '!' || s[i + 2] == '-');
        if (!doc) {
            i++;
            continue;
        }
        int kind = s[i + 2];
        size_t start = i + 3, j = start;
        int nesting = 1;
        while (j + 1 < n && nesting) {
            if (s[j] == '/' && s[j + 1] == '-') {
                nesting++;
                j += 2;
            } else if (s[j] == '-' && s[j + 1] == '/') {
                nesting--;
                if (!nesting) {
                    break;
                }
                j += 2;
            } else {
                j++;
            }
        }
        if (nesting) {
            die("unclosed Lean comment in %s", f->rel);
            return;
        }
        size_t begin = j + 2;
        size_t code_end = begin;
        size_t resume = begin;
        if (kind == '-') {
            while (begin < n && isspace((unsigned char)s[begin])) {
                begin++;
            }
            LeanDoc doc = lean_doc_parse(s + start, j - start);
            char *hover_name = doc.name.n ? slice(doc.name.s, lean_doc_trimmed_len(&doc.name)) : NULL;
            char *hover_nl = doc.nl.n ? slice(doc.nl.s, lean_doc_trimmed_len(&doc.nl)) : NULL;
            char *hover_formula = doc.formula.n ? slice(doc.formula.s, lean_doc_trimmed_len(&doc.formula)) : NULL;
            LeanCodeEnd boundary = lean_code_end(s, n, begin, doc.capture, f->rel);
            code_end = boundary.end;
            resume = boundary.next;
            if (code_end > begin) {
                int declaration = lean_kind(s + begin, code_end - begin);
                const char *kind_name = lean_kind_name(declaration);
                Buf label = {0};
                add(&label, kind_name);
                ch(&label, '|');
                if (doc.name.n) {
                    addn(&label, doc.name.s, lean_doc_trimmed_len(&doc.name));
                } else {
                    add(&label, kind_name);
                }
                segment(SEG_BOX_OPEN, label.s, label.n);
                free(label.s);
                Buf body = {0};
                if (doc.annotated) {
                    lean_doc_body(&body, &doc, declaration, 0);
                } else {
                    addn(&body, s + start, j - start);
                }
                expand_text(f->rel, body.s ? body.s : "", body.n, depth);
                free(body.s);
                segment(SEG_CODE, s + begin, code_end - begin);
                Segment *code = appendix_capture ? &appendix_segs[nappendix_segs - 1]
                                                 : &segs[nsegs - 1];
                code->file = id;
                code->doc_name = hover_name;
                code->doc_nl = hover_nl;
                code->doc_formula = hover_formula;
                hover_name = hover_nl = hover_formula = NULL;
            }
            lean_doc_free(&doc);
            free(hover_name);
            free(hover_nl);
            free(hover_formula);
        }
        if (kind == '-' && code_end > begin) {
            segment(SEG_BOX_CLOSE, "", 0);
            i = resume;
            continue;
        } else {
            Buf body = {0};
            markdown_tex(&body, s + start, j - start, id == root_index, &top_heading_seen);
            expand_text(f->rel, body.s ? body.s : "", body.n, depth);
            free(body.s);
        }
        i = j + 2;
    }
}

/* Convert Markdown headings into the common TeX and HTML heading model. */
static void markdown_tex(Buf *b, const char *s, size_t n, int isroot, int *top_heading_seen) {
    for (size_t i = 0; i < n;) {
        size_t e = i;
        while (e < n && s[e] != '\n') {
            e++;
        }
        size_t h = i;
        while (h < e && (s[h] == ' ' || s[h] == '\t')) {
            h++;
        }
        size_t k = h;
        while (k < e && s[k] == '#') {
            k++;
        }
        if (k > h && k < e && s[k] == ' ') {
            const char *cmd = k - h == 1 ? (!*top_heading_seen && isroot ? "title"
                                             : !*top_heading_seen        ? "chapter"
                                                                        : "section")
                                         : (k - h == 2   ? "section"
                                            : k - h == 3 ? "subsection"
                                                         : "subsubsection");
            if (k - h == 1) {
                *top_heading_seen = 1;
            }
            size_t tail = e;
            while (tail > k + 1 && isspace((unsigned char)s[tail - 1])) {
                tail--;
            }
            ch(b, '\\');
            add(b, cmd);
            ch(b, '{');
            addn(b, s + k + 1, tail - k - 1);
            add(b, "}\n");
            if (k - h == 1 && isroot) {
                add(b, "\\maketitle\n");
            }
        } else {
            addn(b, s + i, e - i);
            ch(b, '\n');
        }
        i = e < n ? e + 1 : e;
    }
}

/* Each Lean file has an independent TeX translation for the worker pool. */
static void translate_lean(File *f, int isroot) {
    const char *s = f->data;
    size_t n = strlen(s);
    Buf b = {0}, appendix_tex = {0};
    int top_heading_seen = 0, appendix_open = 0, appendix_seen = 0;
    for (size_t i = 0; i < n;) {
        size_t next = 0;
        char *appendix_name = NULL;
        int directive = appendix_directive(s, n, i, &next, &appendix_name);
        if (directive) {
            if (failed) {
                free(appendix_name);
                break;
            }
            if (directive == 1) {
                if (appendix_open) {
                    die("nested --myne:appendix_start in %s", f->rel);
                    free(appendix_name);
                    break;
                }
                if (!appendix_seen++) {
                    add(&appendix_tex, "\\appendix\n");
                }
                add(&appendix_tex, "\\section{");
                add(&appendix_tex, appendix_name);
                add(&appendix_tex, "}\n");
                appendix_open = 1;
            } else {
                if (!appendix_open) {
                    die("--myne:appendix_end without appendix_start in %s", f->rel);
                    break;
                }
                appendix_open = 0;
            }
            free(appendix_name);
            i = next;
            continue;
        }
        Buf *out = appendix_open ? &appendix_tex : &b;
        int doc =
            i + 3 <= n && s[i] == '/' && s[i + 1] == '-' && (s[i + 2] == '!' || s[i + 2] == '-');
        if (!doc) {
            i++;
            continue;
        }
        int kind = s[i + 2];
        size_t start = i + 3, j = start;
        int nesting = 1;
        while (j + 1 < n && nesting) {
            if (s[j] == '/' && s[j + 1] == '-') {
                nesting++;
                j += 2;
            } else if (s[j] == '-' && s[j + 1] == '/') {
                nesting--;
                if (!nesting) {
                    break;
                }
                j += 2;
            } else {
                j++;
            }
        }
        if (nesting) {
            break;
        }
        i = j + 2;
        if (kind == '-') {
            while (i < n && isspace((unsigned char)s[i])) {
                i++;
            }
            size_t begin = i;
            LeanDoc doc = lean_doc_parse(s + start, j - start);
            LeanCodeEnd boundary = lean_code_end(s, n, i, doc.capture, f->rel);
            i = boundary.next;
            if (boundary.end > begin) {
                int declaration = lean_kind(s + begin, boundary.end - begin);
                const char *environment = lean_environment(declaration);
                if (environment) {
                    add(out, "\n\\begin{");
                    add(out, environment);
                    if (doc.name.n) {
                        size_t name_len = lean_doc_trimmed_len(&doc.name);
                        add(out, "}[");
                        addn(out, doc.name.s, name_len);
                        add(out, "]\n");
                    } else {
                        add(out, "}\n");
                    }
                    f->has_boxes = 1;
                }
                if (doc.annotated) {
                    Buf body = {0};
                    lean_doc_body(&body, &doc, declaration, 1);
                    markdown_tex(out, body.s ? body.s : "", body.n, isroot, &top_heading_seen);
                    free(body.s);
                } else {
                    markdown_tex(out, s + start, j - start, isroot, &top_heading_seen);
                }
                if (environment) {
                    add(out, "\n\\end{");
                    add(out, environment);
                    add(out, "}\n");
                }
                lean_doc_free(&doc);
                continue;
            }
            lean_doc_free(&doc);
        }
        markdown_tex(out, s + start, j - start, isroot, &top_heading_seen);
    }
    if (appendix_open) {
        die("missing --myne:appendix_end in %s", f->rel);
    }
    add(&b, appendix_tex.s ? appendix_tex.s : "");
    free(appendix_tex.s);
    f->tex = b.s ? b.s : xstrdup("");
}

static void *translate_worker(void *arg) {
    int *next = arg;
    for (;;) {
        pthread_mutex_lock(&translation_lock);
        int id = (*next)++;
        pthread_mutex_unlock(&translation_lock);
        if (id >= nfiles) {
            break;
        }
        if (files[id].lean) {
            translate_lean(&files[id], id == root_index);
        }
    }
    return NULL;
}

static void translate_parallel(void) {
    int next = 0;
    pthread_t threads[4];
    int count = nfiles < 4 ? nfiles : 4;
    for (int i = 0; i < count; i++) {
        pthread_create(&threads[i], NULL, translate_worker, &next);
    }
    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
    }
}

/* Apply literal replacements in manifest order before HTML parsing. */
static char *replace_map(const char *s) {
    char *current = xstrdup(s);
    for (int j = 0; j < nmaps; j++) {
        Buf out = {0};
        const char *p = current, *hit;
        size_t needle = strlen(maps[j].from);
        if (!needle) {
            continue;
        }
        while ((hit = finds(p, strlen(p), maps[j].from))) {
            addn(&out, p, hit - p);
            add(&out, maps[j].to);
            p = hit + needle;
        }
        add(&out, p);
        free(current);
        current = out.s;
    }
    return current;
}

static Bib *bib_lookup(const char *key) {
    for (int i = 0; i < nbibs; i++) {
        if (!strcmp(bibs[i].key, key)) {
            return &bibs[i];
        }
    }
    return NULL;
}

static char *field(const char *entry, const char *name) {
    size_t n = strlen(entry), len = strlen(name);
    for (size_t i = 0; i + len < n; i++) {
        if (i && (isalnum((unsigned char)entry[i - 1]) || entry[i - 1] == '_')) {
            continue;
        }
        if (strncasecmp(entry + i, name, len)) {
            continue;
        }
        size_t p = i + len;
        while (p < n && isspace((unsigned char)entry[p])) {
            p++;
        }
        if (p >= n || entry[p] != '=') {
            continue;
        }
        p++;
        while (p < n && isspace((unsigned char)entry[p])) {
            p++;
        }
        if (p >= n) {
            break;
        }
        char close = entry[p] == '{' ? '}' : entry[p] == '"' ? '"' : ',';
        if (close != ',') {
            p++;
        }
        size_t start = p;
        int depth = close == '}' ? 1 : 0;
        while (p < n) {
            if (close == '}') {
                if (entry[p] == '{') {
                    depth++;
                }
                if (entry[p] == '}' && !--depth) {
                    break;
                }
            } else if (entry[p] == close) {
                break;
            }
            p++;
        }
        char *v = slice(entry + start, p - start), *t = trim(v);
        if (t != v) {
            memmove(v, t, strlen(t) + 1);
        }
        return v;
    }
    return xstrdup("");
}

/* Parse the BibTeX fields needed for numbered HTML references. */
static void parse_bib(const char *rel) {
    char *path = join(basedir, rel), *s = read_all(path);
    free(path);
    if (!s) {
        die("cannot read bibliography %s", rel);
        return;
    }
    size_t n = strlen(s);
    for (size_t i = 0; i < n && nbibs < MAX_BIB;) {
        if (s[i] != '@') {
            i++;
            continue;
        }
        while (i < n && s[i] != '{') {
            i++;
        }
        if (i >= n) {
            break;
        }
        size_t k = ++i;
        while (i < n && s[i] != ',') {
            i++;
        }
        if (i >= n) {
            break;
        }
        char *key = slice(s + k, i - k), *clean = trim(key);
        if (clean != key) {
            memmove(key, clean, strlen(clean) + 1);
        }
        size_t start = ++i;
        int depth = 1;
        while (i < n && depth) {
            if (s[i] == '{') {
                depth++;
            }
            if (s[i] == '}') {
                depth--;
            }
            i++;
        }
        if (depth) {
            free(key);
            break;
        }
        if (!bib_lookup(key)) {
            char *entry = slice(s + start, i - start - 1);
            Bib *b = &bibs[nbibs++];
            b->key = key;
            b->author = field(entry, "author");
            b->title = field(entry, "title");
            b->year = field(entry, "year");
            b->number = 0;
            free(entry);
        } else {
            free(key);
        }
    }
    free(s);
}

/* Bibliographies can come from the manifest or a TeX bibliography command. */
static void load_bibliographies(void) {
    for (int i = 0; i < nbibcfg; i++) {
        parse_bib(bibcfg[i]);
    }
    for (int i = 0; i < nfiles; i++) {
        const char *s = files[i].data;
        size_t n = strlen(s);
        for (size_t j = 0; j + 13 < n; j++) {
            if (!strncmp(s + j, "\\bibliography", 13)) {
                size_t p = j + 13;
                char *a = NULL;
                if (!arg_at(s, n, &p, &a)) {
                    continue;
                }
                char *one = strtok(a, ",");
                while (one) {
                    char *ref = trim(one);
                    Buf b = {0};
                    add(&b, ref);
                    if (!ends(ref, ".bib")) {
                        add(&b, ".bib");
                    }
                    char *path = path_from(files[i].rel, b.s), *rel = project_rel(path);
                    if (rel && access(path, R_OK) == 0) {
                        parse_bib(rel);
                    } else {
                        die("missing bibliography %s from %s", ref, files[i].rel);
                    }
                    free(path);
                    free(rel);
                    free(b.s);
                    one = strtok(NULL, ",");
                }
                free(a);
                j = p - 1;
            }
        }
    }
}

static void escape_n(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&':
            add(b, "&amp;");
            break;
        case '<':
            add(b, "&lt;");
            break;
        case '>':
            add(b, "&gt;");
            break;
        case '"':
            add(b, "&quot;");
            break;
        default:
            ch(b, s[i]);
        }
    }
}

static void escape(Buf *b, const char *s) {
    escape_n(b, s, strlen(s));
}

static char *slug(int id, const char *kind) {
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s-%03d.html", kind, id);
    return xstrdup(tmp);
}

static const char *heading_name(int type) {
    const char *names[] = {"introduction", "title", "part", "chapter", "section",
                           "subsection", "subsubsection", "paragraph"};
    return type >= 0 && type < (int)(sizeof(names) / sizeof(*names)) ? names[type] : "page";
}

static void set_statement_scope(const char *number) {
    snprintf(statement_scope, sizeof(statement_scope), "%s", number ? number : "");
    statement_number = 0;
}

static void number_heading(Buf *number, int type) {
    char value[64] = {0};
    if (type == 2) {
        chapter_active = 0;
        section_number = subsection_number = subsubsection_number = 0;
        set_statement_scope("");
    } else if (type == 3) {
        chapter_number++;
        chapter_active = 1;
        section_number = subsection_number = subsubsection_number = 0;
        snprintf(value, sizeof(value), "%d", chapter_number);
        set_statement_scope(value);
    } else if (type == 4) {
        section_number++;
        subsection_number = subsubsection_number = 0;
        if (chapter_active) {
            snprintf(value, sizeof(value), "%d.%d", chapter_number, section_number);
        } else {
            snprintf(value, sizeof(value), "%d", section_number);
        }
        set_statement_scope(value);
    } else if (type == 5) {
        subsection_number++;
        subsubsection_number = 0;
        if (chapter_active && section_number) {
            snprintf(value, sizeof(value), "%d.%d.%d", chapter_number, section_number,
                     subsection_number);
        } else if (chapter_active) {
            snprintf(value, sizeof(value), "%d.%d", chapter_number, subsection_number);
        } else if (section_number) {
            snprintf(value, sizeof(value), "%d.%d", section_number, subsection_number);
        } else {
            snprintf(value, sizeof(value), "%d", subsection_number);
        }
    } else if (type == 6) {
        subsubsection_number++;
        if (chapter_active && section_number) {
            snprintf(value, sizeof(value), "%d.%d.%d.%d", chapter_number, section_number,
                     subsection_number, subsubsection_number);
        } else if (chapter_active) {
            snprintf(value, sizeof(value), "%d.%d.%d", chapter_number, subsection_number,
                     subsubsection_number);
        } else if (section_number) {
            snprintf(value, sizeof(value), "%d.%d.%d", section_number, subsection_number,
                     subsubsection_number);
        } else {
            snprintf(value, sizeof(value), "%d.%d", subsection_number, subsubsection_number);
        }
    }
    if (*value) {
        add(number, value);
    }
}

static void next_statement_number(Buf *number) {
    char value[96];
    statement_number++;
    if (*statement_scope) {
        snprintf(value, sizeof(value), "%s.%d", statement_scope, statement_number);
    } else {
        snprintf(value, sizeof(value), "%d", statement_number);
    }
    add(number, value);
}

static void new_page(const char *title, int heading, const char *number) {
    if (npages >= MAX_PAGES) {
        die("too many pages");
        return;
    }
    if (heading == 2) {
        part++;
    }
    Page *p = &pages[npages];
    p->title = xstrdup(title);
    p->slug = npages ? slug(npages, heading_name(heading)) : xstrdup("index.html");
    p->number = number && *number ? xstrdup(number) : NULL;
    p->part = part;
    p->heading = heading;
    current = npages++;
}

static void inline_html(Buf *b, const char *s, size_t n, int depth);

static void cite_html(Buf *b, const char *key, int textual) {
    Bib *entry = bib_lookup(key);
    if (!entry) {
        die("unknown citation %s", key);
        escape(b, key);
        return;
    }
    if (!entry->number) {
        entry->number = ++ncites;
    }
    if (textual) {
        escape(b, entry->author);
        add(b, " (");
        escape(b, entry->year);
        add(b, ")");
        char number[32];
        snprintf(number, sizeof(number), "%d", entry->number);
        add(b, "<sup class=notemark>");
        add(b, number);
        add(b, "</sup>");
    } else {
        add(b, "<sup class=notemark>[<a href=\"#ref-");
        escape(b, key);
        add(b, "\">");
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%d", entry->number);
        add(b, tmp);
        add(b, "</a>]</sup>");
    }
    Buf note = {0};
    escape(&note, entry->author);
    add(&note, ", <i>");
    escape(&note, entry->title);
    add(&note, "</i> (");
    escape(&note, entry->year);
    add(&note, ")");
    add(b, "<span class=sidenote>");
    add(b, note.s);
    add(b, "</span>");
    add(&pages[current].notes, "<li>");
    add(&pages[current].notes, note.s);
    add(&pages[current].notes, "</li>");
    free(note.s);
}

/* Interpret the supported inline TeX commands and math delimiters. */
static void inline_html(Buf *b, const char *s, size_t n, int depth) {
    if (depth > 16) {
        return;
    }
    for (size_t i = 0; i < n;) {
        if (s[i] == '%' && (i == 0 || s[i - 1] != '\\')) {
            while (i < n && s[i] != '\n') {
                i++;
            }
            continue;
        }
        if (s[i] == '$') {
            size_t k = i + 1;
            int display = k < n && s[k] == '$';
            if (display) {
                k++;
            }
            const char *end = finds(s + k, n - k, display ? "$$" : "$");
            if (end) {
                add(b, display ? "<div class=math>$$" : "<span class=math>$");
                escape_n(b, s + k, end - (s + k));
                add(b, display ? "$$</div>" : "$</span>");
                i = (size_t)(end - s) + (display ? 2 : 1);
                continue;
            }
        }
        if (s[i] == '\\') {
            size_t name = i + 1;
            while (name < n && isalpha((unsigned char)s[name])) {
                name++;
            }
            if (name == i + 1) {
                if (i + 1 < n && strchr("%&_#{}$", s[i + 1])) {
                    escape_n(b, s + i + 1, 1);
                    i += 2;
                    continue;
                }
                i++;
                continue;
            }
            char *cmd = slice(s + i + 1, name - i - 1);
            size_t p = name;
            char *arg = NULL;
            if (!strcmp(cmd, "includegraphics") && p < n && s[p] == '[') {
                while (p < n && s[p] != ']') {
                    p++;
                }
                if (p < n) {
                    p++;
                }
            }
            int has = arg_at(s, n, &p, &arg);
            if (has && (!strcmp(cmd, "textit") || !strcmp(cmd, "textbf"))) {
                add(b, !strcmp(cmd, "textit") ? "<em>" : "<strong>");
                inline_html(b, arg, strlen(arg), depth + 1);
                add(b, !strcmp(cmd, "textit") ? "</em>" : "</strong>");
            } else if (has && (!strcmp(cmd, "cite") || !strcmp(cmd, "citet"))) {
                char *key = strtok(arg, ",");
                int first = 1;
                while (key) {
                    if (!first++) {
                        add(b, "; ");
                    }
                    cite_html(b, trim(key), !strcmp(cmd, "citet"));
                    key = strtok(NULL, ",");
                }
            } else if (has && !strcmp(cmd, "footnote")) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "%d", ++nfoot);
                add(b, "<sup class=notemark>");
                add(b, tmp);
                add(b, "</sup><span class=sidenote>");
                inline_html(b, arg, strlen(arg), depth + 1);
                add(b, "</span>");
                add(&pages[current].notes, "<li>");
                inline_html(&pages[current].notes, arg, strlen(arg), depth + 1);
                add(&pages[current].notes, "</li>");
            } else if (has && !strcmp(cmd, "includegraphics")) {
                add(b, "<img alt=\"\" src=\"");
                escape(b, arg);
                add(b, "\">");
            } else if (has && !strcmp(cmd, "bibliography")) {
                bib_page = current;
            } else if (!strcmp(cmd, "begin") && has && !strcmp(arg, "equation")) {
                const char *end = finds(s + p, n - p, "\\end{equation}");
                if (end) {
                    add(b, "<div class=math>$$");
                    escape_n(b, s + p, end - (s + p));
                    add(b, "$$</div>");
                    p = (size_t)(end - s) + 14;
                }
            }
            /* Unknown commands and their first braced argument are omitted. */
            i = has ? p : name;
            free(cmd);
            free(arg);
            continue;
        }
        if (s[i] == '\n') {
            size_t j = i;
            while (j < n && (s[j] == '\n' || s[j] == '\r' || s[j] == ' ')) {
                j++;
            }
            if (j - i > 1 && j < n) {
                add(b, "<br><br>");
            } else {
                ch(b, ' ');
            }
            i = j;
            continue;
        }
        escape_n(b, s + i, 1);
        i++;
    }
}

static int structural(const char *name) {
    const char *names[] = {"title",      "part",          "chapter",  "section",
                           "subsection", "subsubsection", "paragraph"};
    for (int i = 0; i < 7; i++) {
        if (!strcmp(name, names[i])) {
            return i + 1;
        }
    }
    return 0;
}

static char *doc_title;

/* Structural commands split the document into navigable pages. */
static void text_html(const char *s) {
    size_t n = strlen(s), start = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] == '%' && (i == 0 || s[i - 1] != '\\')) {
            while (i < n && s[i] != '\n') {
                i++;
            }
            continue;
        }
        if (s[i] == '\\') {
            size_t k = i + 1;
            while (k < n && isalpha((unsigned char)s[k])) {
                k++;
            }
            char *cmd = slice(s + i + 1, k - i - 1);
            int type = structural(cmd);
            if (!strcmp(cmd, "appendix")) {
                inline_html(&pages[current].body, s + start, i - start, 0);
                Buf number = {0};
                number_heading(&number, 2);
                new_page("Appendix", 2, number.s);
                free(number.s);
                i = k;
                start = i;
                free(cmd);
                continue;
            }
            if (type) {
                size_t p = k;
                char *arg = NULL;
                if (arg_at(s, n, &p, &arg)) {
                    inline_html(&pages[current].body, s + start, i - start, 0);
                    if (type == 1) {
                        free(doc_title);
                        doc_title = xstrdup(arg);
                    } else {
                        Buf number = {0};
                        number_heading(&number, type);
                        if (type >= 2 && type <= page_heading_level) {
                            new_page(arg, type, number.s);
                        } else {
                            int toc_heading = web_table_of_contents &&
                                              type == pages[current].heading + 1;
                            int toc_id = ++pages[current].toc_count;
                            char tag[32];
                            int level = type - page_heading_level + 1;
                            if (level < 2) {
                                level = 2;
                            } else if (level > 6) {
                                level = 6;
                            }
                            if (toc_heading) {
                                snprintf(tag, sizeof(tag), "<h%d id=\"page-heading-%d\">", level,
                                         toc_id);
                                add(&pages[current].toc, "<li><a href=\"#page-heading-");
                                char id[16];
                                snprintf(id, sizeof(id), "%d", toc_id);
                                add(&pages[current].toc, id);
                                add(&pages[current].toc, "\">");
                                if (number.n) {
                                    add(&pages[current].toc, "<span class=heading-number>");
                                    escape(&pages[current].toc, number.s);
                                    add(&pages[current].toc, "</span> ");
                                }
                                escape(&pages[current].toc, arg);
                                add(&pages[current].toc, "</a></li>");
                            } else {
                                pages[current].toc_count--;
                                snprintf(tag, sizeof(tag), "<h%d>", level);
                            }
                            add(&pages[current].body, tag);
                            if (number.n) {
                                add(&pages[current].body, "<span class=heading-number>");
                                escape(&pages[current].body, number.s);
                                add(&pages[current].body, "</span> ");
                            }
                            inline_html(&pages[current].body, arg, strlen(arg), 0);
                            snprintf(tag, sizeof(tag), "</h%d>", level);
                            add(&pages[current].body, tag);
                        }
                        free(number.s);
                    }
                    free(arg);
                    i = p;
                    start = i;
                    free(cmd);
                    continue;
                }
            }
            free(cmd);
        }
        i++;
    }
    inline_html(&pages[current].body, s + start, n - start, 0);
}

static int keyword(const char *s, size_t n) {
    const char *kw[] = {"def",  "theorem", "lemma",     "by",        "import",  "namespace",
                        "end",  "where",   "inductive", "structure", "example", "variable",
                        "open", "simp",    "have",      "let",       "match",   "with",
                        "fun",  "if",      "then",      "else"};
    for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++) {
        if (strlen(kw[i]) == n && !strncmp(s, kw[i], n)) {
            return 1;
        }
    }
    return 0;
}

static int dependency_name_char(unsigned char c) {
    return isalnum(c) || c == '_' || c == '\'' || c == '.' || c >= 0x80;
}

static int dependency_declaration(const char *s, size_t begin, size_t end, size_t *keyword,
                                  size_t *name, size_t *name_len, int *kind) {
    size_t p = begin;
    while (p < end && isspace((unsigned char)s[p])) {
        p++;
    }
    if (p != begin) {
        return 0;
    }
    for (;;) {
        if (p + 1 < end && s[p] == '@' && s[p + 1] == '[') {
            int depth = 1;
            p += 2;
            while (p < end && depth) {
                if (s[p] == '[') depth++;
                else if (s[p] == ']') depth--;
                p++;
            }
            while (p < end && isspace((unsigned char)s[p])) p++;
            continue;
        }
        size_t word = p;
        while (p < end && (isalpha((unsigned char)s[p]) || s[p] == '_')) p++;
        size_t word_len = p - word;
        const char *modifiers[] = {"private", "protected", "noncomputable", "partial", "unsafe"};
        int modifier = 0;
        for (size_t i = 0; i < sizeof(modifiers) / sizeof(*modifiers); i++) {
            if (word_len == strlen(modifiers[i]) && !memcmp(s + word, modifiers[i], word_len)) {
                modifier = 1;
                break;
            }
        }
        if (!modifier) {
            p = word;
            break;
        }
        while (p < end && isspace((unsigned char)s[p])) p++;
    }
    size_t cmd = p;
    while (p < end && (isalpha((unsigned char)s[p]) || s[p] == '_')) p++;
    size_t cmd_len = p - cmd;
    const char *definitions[] = {"def", "abbrev", "opaque", "structure", "class", "inductive"};
    int declaration_kind = LEAN_CODE;
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++) {
        if (cmd_len == strlen(definitions[i]) && !memcmp(s + cmd, definitions[i], cmd_len)) {
            declaration_kind = LEAN_DEF;
            break;
        }
    }
    if ((cmd_len == 5 && !memcmp(s + cmd, "lemma", 5))) declaration_kind = LEAN_LEMMA;
    if ((cmd_len == 7 && !memcmp(s + cmd, "theorem", 7))) declaration_kind = LEAN_THEOREM;
    if (declaration_kind == LEAN_CODE || (p < end && !isspace((unsigned char)s[p]))) {
        return 0;
    }
    while (p < end && isspace((unsigned char)s[p])) p++;
    size_t id_start = p;
    if (p + 2 < end && (unsigned char)s[p] == 0xc2 && (unsigned char)s[p + 1] == 0xab) {
        p += 2;
        while (p + 1 < end && !((unsigned char)s[p] == 0xc2 && (unsigned char)s[p + 1] == 0xbb)) p++;
        if (p + 1 < end) p += 2;
    } else {
        while (p < end && dependency_name_char((unsigned char)s[p])) p++;
    }
    if (p == id_start) return 0;
    *keyword = cmd;
    *name = id_start;
    *name_len = p - id_start;
    *kind = declaration_kind;
    return 1;
}

static void dependency_collect_code(const char *source, int segment) {
    while (isspace((unsigned char)*source)) source++;
    size_t n = strlen(source), current_start = 0;
    int current_node = -1;
    for (size_t line = 0; line < n;) {
        size_t end = line;
        while (end < n && source[end] != '\n') end++;
        size_t keyword = 0, name = 0, name_len = 0;
        int kind = LEAN_CODE;
        if (dependency_declaration(source, line, end, &keyword, &name, &name_len, &kind)) {
            if (current_node >= 0) {
                dependency_nodes[current_node].source = slice(source + current_start, line - current_start);
            }
            if (ndependency_nodes >= MAX_SEGS) {
                die("too many declarations for the dependency graph");
                return;
            }
            current_node = ndependency_nodes++;
            DependencyNode *node = &dependency_nodes[current_node];
            memset(node, 0, sizeof(*node));
            node->name = slice(source + name, name_len);
            node->file = segs[segment].file;
            node->graph_id = current_node;
            node->doc_name = segs[segment].doc_name ? xstrdup(segs[segment].doc_name) : NULL;
            node->doc_nl = segs[segment].doc_nl ? xstrdup(segs[segment].doc_nl) : NULL;
            node->doc_formula = segs[segment].doc_formula ? xstrdup(segs[segment].doc_formula) : NULL;
            node->kind = kind;
            node->segment = segment;
            node->page = -1;
            node->offset = keyword;
            current_start = line;
        }
        line = end < n ? end + 1 : end;
    }
    if (current_node >= 0) {
        dependency_nodes[current_node].source = slice(source + current_start, n - current_start);
    }
}

static void dependency_collect(void) {
    for (int i = 0; i < nsegs; i++) {
        if (segs[i].kind == SEG_CODE) {
            dependency_collect_code(segs[i].s, i);
        }
    }
}

static int dependency_uses(const char *source, const char *name);

static LeanDependencyDecl *dependency_decl(const char *name, int file, int create) {
    for (int i = 0; i < nlean_dependency_decls; i++) {
        if (lean_dependency_decls[i].file == file &&
            !strcmp(lean_dependency_decls[i].name, name)) return &lean_dependency_decls[i];
    }
    if (!create) return NULL;
    if (nlean_dependency_decls == lean_dependency_cap) {
        lean_dependency_cap = lean_dependency_cap ? lean_dependency_cap * 2 : 128;
        lean_dependency_decls = realloc(lean_dependency_decls,
                                         (size_t)lean_dependency_cap * sizeof(*lean_dependency_decls));
        if (!lean_dependency_decls) exit(2);
    }
    LeanDependencyDecl *decl = &lean_dependency_decls[nlean_dependency_decls++];
    memset(decl, 0, sizeof(*decl));
    decl->name = xstrdup(name);
    decl->file = file;
    return decl;
}

static void dependency_parse_lean_output(char *output_text, int file) {
    char *save = NULL;
    for (char *line = strtok_r(output_text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "MYNE_DECL\t", 10)) {
            dependency_decl(line + 10, file, 1);
        } else if (!strncmp(line, "MYNE_EDGE\t", 10)) {
            char *from = line + 10, *sep = strchr(from, '\t');
            if (!sep || !sep[1]) continue;
            *sep++ = 0;
            LeanDependencyDecl *decl = dependency_decl(from, file, 1);
            if (decl->ndeps == decl->cap) {
                decl->cap = decl->cap ? decl->cap * 2 : 8;
                decl->deps = realloc(decl->deps, (size_t)decl->cap * sizeof(*decl->deps));
                if (!decl->deps) exit(2);
            }
            decl->deps[decl->ndeps++] = xstrdup(sep);
        }
    }
}

static size_t dependency_skip_header_space(const char *s, size_t p) {
    for (;;) {
        while (isspace((unsigned char)s[p])) p++;
        if (s[p] == '-' && s[p + 1] == '-') {
            while (s[p] && s[p] != '\n') p++;
        } else if (s[p] == '/' && s[p + 1] == '-') {
            int depth = 1;
            p += 2;
            while (s[p] && depth) {
                if (s[p] == '/' && s[p + 1] == '-') { depth++; p += 2; }
                else if (s[p] == '-' && s[p + 1] == '/') { depth--; p += 2; }
                else p++;
            }
        } else {
            return p;
        }
    }
}

static size_t dependency_import_position(const char *s, int *is_module) {
    size_t p = dependency_skip_header_space(s, 0);
    int module = !strncmp(s + p, "module", 6) &&
                 (s[p + 6] == '\n' || s[p + 6] == '\r' || !s[p + 6]);
    *is_module = module;
    if (module) {
        while (s[p] && s[p] != '\n') p++;
        if (s[p]) p++;
        p = dependency_skip_header_space(s, p);
    }
    if (!strncmp(s + p, "prelude", 7) &&
        (s[p + 7] == '\n' || s[p + 7] == '\r' || !s[p + 7])) {
        while (s[p] && s[p] != '\n') p++;
        if (s[p]) p++;
        return p;
    }
    return module ? p : 0;
}

/* A Myne manifest may live below the Lake project root (for example in src/). */
static char *dependency_lake_root(void) {
    char *dir = xstrdup(basedir);
    for (;;) {
        char *leanfile = join(dir, "lakefile.lean");
        char *tomlfile = join(dir, "lakefile.toml");
        int found = access(leanfile, F_OK) == 0 || access(tomlfile, F_OK) == 0;
        free(leanfile);
        free(tomlfile);
        if (found) return dir;
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = 0;
    }
    free(dir);
    return NULL;
}

/* Lean elaborates the original source, then reports constants in each resulting type/proof. */
static void dependency_elaborate_file(int file) {
#ifdef _WIN32
    char temporary[PATH_MAX];
    char temp_file[PATH_MAX];
    DWORD temp_len = GetTempPathA((DWORD)sizeof(temporary), temporary);
    if (!temp_len || temp_len >= sizeof(temporary) ||
        !GetTempFileNameA(temporary, "myn", 0, temp_file) || !DeleteFileA(temp_file)) {
        die("cannot create temporary Lean directory");
        return;
    }
    snprintf(temporary, sizeof(temporary), "%s", temp_file);
    if (!CreateDirectoryA(temporary, NULL)) {
        die("cannot create temporary Lean directory");
        return;
    }
    normalize_path(temporary);
#else
    char temporary[] = "/tmp/myne-lean-XXXXXX";
    if (!mkdtemp(temporary)) {
        die("cannot create temporary Lean directory: %s", strerror(errno));
        return;
    }
#endif
    char *source_path = join(temporary, "source.lean");
    char *result_path = join(temporary, "result.txt");
    char *lake_root = dependency_lake_root();
    Buf source = {0};
    int is_module = 0;
    size_t import_at = dependency_import_position(files[file].data, &is_module);
    addn(&source, files[file].data, import_at);
    add(&source, is_module ? "public import Lean\n" : "import Lean\n");
    add(&source, files[file].data + import_at);
    add(&source, "\n\nopen Lean Elab Command\n"
                 "elab \"#myne_dependencies\" : command => do\n"
                 "  let env ← getEnv\n"
                 "  for (name, ci) in env.constants do\n"
                 "    if !env.isImportedConst name then\n"
                 "      IO.println s!\"MYNE_DECL\\t{name}\"\n"
                 "      for dep in ci.getUsedConstantsAsSet do\n"
                 "        IO.println s!\"MYNE_EDGE\\t{name}\\t{dep}\"\n"
                 "#myne_dependencies\n");
    if (write_all(source_path, source.s, source.n)) {
        die("cannot write temporary Lean source for %s", files[file].rel);
        goto cleanup;
    }
#ifdef _WIN32
    SECURITY_ATTRIBUTES output_security = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE output_handle = CreateFileA(result_path, GENERIC_WRITE, FILE_SHARE_READ,
                                       &output_security,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output_handle == INVALID_HANDLE_VALUE) {
        die("cannot create Lean output for %s", files[file].rel);
        goto cleanup;
    }
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output_handle;
    startup.hStdError = output_handle;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    const char *program = lake_root ? "lake env lean" : "lean";
    size_t command_cap = strlen(program) + strlen(source_path) * 2 + 32;
    char *command = xmalloc(command_cap);
    snprintf(command, command_cap, "%s \"%s\"", program, source_path);
    BOOL created = CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                  NULL, lake_root ? lake_root : basedir, &startup, &process);
    CloseHandle(output_handle);
    free(command);
    if (!created) {
        die("cannot start Lean for %s (Windows error %lu)", files[file].rel,
            (unsigned long)GetLastError());
        goto cleanup;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    char *result = read_all(result_path);
    if (exit_code != 0 || !result) {
        die("Lean elaboration failed for %s:\n%.6000s", files[file].rel,
            result ? result : "Lean produced no output");
    } else {
        dependency_parse_lean_output(result, file);
    }
    free(result);
#else
    int out_fd = open(result_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd < 0) {
        die("cannot create Lean output for %s", files[file].rel);
        goto cleanup;
    }
    pid_t child = fork();
    if (child == 0) {
        if (chdir(lake_root ? lake_root : basedir)) {
            perror("myne: cannot enter Lean project");
            _exit(127);
        }
        dup2(out_fd, STDOUT_FILENO);
        dup2(out_fd, STDERR_FILENO);
        close(out_fd);
        if (lake_root) {
            execlp("lake", "lake", "env", "lean", source_path, (char *)NULL);
        } else {
            execlp("lean", "lean", source_path, (char *)NULL);
        }
        perror("myne: cannot run Lean or Lake");
        _exit(127);
    }
    close(out_fd);
    if (child < 0) {
        die("cannot start Lean for %s: %s", files[file].rel, strerror(errno));
        goto cleanup;
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    char *result = read_all(result_path);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || !result) {
        die("Lean elaboration failed for %s:\n%.6000s", files[file].rel,
            result ? result : "Lean produced no output");
    } else {
        dependency_parse_lean_output(result, file);
    }
    free(result);
#endif
cleanup:
    free(source.s);
    free(lake_root);
    unlink(source_path);
    unlink(result_path);
#ifdef _WIN32
    RemoveDirectoryA(temporary);
#else
    rmdir(temporary);
#endif
    free(source_path);
    free(result_path);
}

static int dependency_name_matches(const char *full, const char *name) {
    if (!strcmp(full, name)) return 1;
    size_t a = strlen(full), b = strlen(name);
    return a > b && full[a - b - 1] == '.' && !strcmp(full + a - b, name);
}

static int dependency_decl_index(const char *name, int preferred_file) {
    for (int i = 0; i < nlean_dependency_decls; i++) {
        if (lean_dependency_decls[i].file == preferred_file &&
            !strcmp(lean_dependency_decls[i].name, name)) return i;
    }
    for (int i = 0; i < nlean_dependency_decls; i++) {
        if (!strcmp(lean_dependency_decls[i].name, name)) return i;
    }
    return -1;
}

static void dependency_trace_elaborated(int root, int declaration, unsigned char *seen) {
    if (seen[declaration]) return;
    seen[declaration] = 1;
    LeanDependencyDecl *decl = &lean_dependency_decls[declaration];
    for (int i = 0; i < decl->ndeps; i++) {
        const char *used = decl->deps[i];
        int documented = -1;
        for (int j = 0; j < ndependency_nodes; j++) {
            if (dependency_nodes[j].resolved_name &&
                !strcmp(used, dependency_nodes[j].resolved_name)) {
                documented = j;
                break;
            }
        }
        if (documented >= 0) {
            if (documented != root)
                elaborated_edges[(size_t)root * elaborated_edge_width + documented] = 1;
            continue;
        }
        int helper = dependency_decl_index(used, decl->file);
        if (helper >= 0) dependency_trace_elaborated(root, helper, seen);
    }
}

static void dependency_elaborate(void) {
    for (int file = 0; file < nfiles && !failed; file++) {
        if (!files[file].lean) continue;
        dependency_elaborate_file(file);
    }
    if (failed) return;
    for (int i = 0; i < ndependency_nodes; i++) {
        DependencyNode *node = &dependency_nodes[i];
        int match = -1, exact = -1;
        for (int j = 0; j < nlean_dependency_decls; j++) {
            LeanDependencyDecl *decl = &lean_dependency_decls[j];
            if (decl->file != node->file || !dependency_name_matches(decl->name, node->name))
                continue;
            if (!strcmp(decl->name, node->name)) exact = j;
            if (match >= 0 && exact < 0) match = -2;
            else if (match != -2) match = j;
        }
        if (exact >= 0) match = exact;
        if (match < 0) {
            die("cannot resolve elaborated declaration %s in %s%s", node->name,
                node->file >= 0 ? files[node->file].rel : "unknown file",
                match == -2 ? " (ambiguous name)" : "");
            return;
        }
        node->resolved_name = xstrdup(lean_dependency_decls[match].name);
    }
    elaborated_edge_width = ndependency_nodes;
    elaborated_edges = calloc((size_t)ndependency_nodes * ndependency_nodes, 1);
    if (!elaborated_edges) exit(2);
    for (int i = 0; i < ndependency_nodes; i++) {
        int declaration = dependency_decl_index(dependency_nodes[i].resolved_name,
                                                dependency_nodes[i].file);
        unsigned char *seen = calloc((size_t)nlean_dependency_decls, 1);
        if (!seen) exit(2);
        dependency_trace_elaborated(i, declaration, seen);
        free(seen);
    }
}

static int dependency_edge(int from, int to) {
    if (from == to) return 0;
    if (elaborated_dependency_graph)
        return elaborated_edges[(size_t)dependency_nodes[from].graph_id * elaborated_edge_width +
                                dependency_nodes[to].graph_id];
    return dependency_uses(dependency_nodes[from].source, dependency_nodes[to].name);
}

static void dependency_prune(void) {
    for (;;) {
        int dependents[MAX_SEGS] = {0};
        for (int i = 0; i < ndependency_nodes; i++) {
            for (int j = 0; j < ndependency_nodes; j++) {
                if (dependency_edge(i, j)) {
                    dependents[j]++;
                }
            }
        }
        int removed = 0, kept = 0;
        for (int i = 0; i < ndependency_nodes; i++) {
            DependencyNode *node = &dependency_nodes[i];
            if (node->kind == LEAN_DEF && dependents[i] == 0) {
                free(node->name);
                free(node->source);
                free(node->slug);
                free(node->doc_name);
                free(node->doc_nl);
                free(node->doc_formula);
                free(node->resolved_name);
                memset(node, 0, sizeof(*node));
                removed++;
                continue;
            }
            if (kept != i) {
                dependency_nodes[kept] = *node;
                memset(node, 0, sizeof(*node));
            }
            kept++;
        }
        ndependency_nodes = kept;
        if (!removed) break;
    }
}

/* Highlight Lean lexically; rendering does not invoke the Lean compiler. */
static void code_html(const char *s, int segment) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    Buf *b = &pages[current].body;
    add(b, "<pre class=\"lean-code\"><code>");
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        for (int j = 0; j < ndependency_nodes; j++) {
            if (dependency_nodes[j].segment == segment && dependency_nodes[j].offset == i) {
                add(b, "<span class=declaration-anchor id=\"decl-");
                char id[24];
                snprintf(id, sizeof(id), "%d", j + 1);
                add(b, id);
                add(b, "\"></span>");
            }
        }
        if (i + 1 < n && s[i] == '-' && s[i + 1] == '-') {
            size_t j = i;
            while (j < n && s[j] != '\n') {
                j++;
            }
            add(b, "<span class=comment>");
            escape_n(b, s + i, j - i);
            add(b, "</span>");
            i = j;
        } else if (s[i] == '"') {
            size_t j = i + 1;
            while (j < n && (s[j] != '"' || s[j - 1] == '\\')) {
                j++;
            }
            if (j < n) {
                j++;
            }
            add(b, "<span class=str>");
            escape_n(b, s + i, j - i);
            add(b, "</span>");
            i = j;
        } else if (isalpha((unsigned char)s[i]) || s[i] == '_') {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) {
                j++;
            }
            if (keyword(s + i, j - i)) {
                add(b, "<span class=kw>");
            }
            escape_n(b, s + i, j - i);
            if (keyword(s + i, j - i)) {
                add(b, "</span>");
            }
            i = j;
        } else if (isdigit((unsigned char)s[i])) {
            size_t j = i + 1;
            while (j < n && isdigit((unsigned char)s[j])) {
                j++;
            }
            add(b, "<span class=num>");
            escape_n(b, s + i, j - i);
            add(b, "</span>");
            i = j;
        } else {
            escape_n(b, s + i, 1);
            i++;
        }
    }
    add(b, "</code></pre>");
}

static void append_references(void) {
    if (ncites == 0 && bib_page >= 0) {
        for (int i = 0; i < nbibs; i++) {
            bibs[i].number = ++ncites;
        }
    }
    if (ncites == 0) {
        return;
    }
    int page = bib_page >= 0 ? bib_page : npages - 1;
    Buf *b = &pages[page].body;
    add(b, "<section class=references><h2>References</h2><ol>");
    for (int num = 1; num <= ncites; num++) {
        for (int i = 0; i < nbibs; i++) {
            if (bibs[i].number == num) {
                add(b, "<li id=\"ref-");
                escape(b, bibs[i].key);
                add(b, "\">");
                escape(b, bibs[i].author);
                add(b, ". <i>");
                escape(b, bibs[i].title);
                add(b, "</i> (");
                escape(b, bibs[i].year);
                add(b, ")</li>");
            }
        }
    }
    add(b, "</ol></section>");
}

static void remove_tree(const char *path) {
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
static void unpack_katex(const char *dir) {
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

static int asset(const char *s) {
    const char *ext[] = {".bib",  ".cls", ".sty", ".png", ".jpg",
                         ".jpeg", ".gif", ".svg", ".pdf", ".eps"};
    for (size_t i = 0; i < sizeof(ext) / sizeof(*ext); i++) {
        if (ends(s, ext[i])) {
            return 1;
        }
    }
    return 0;
}

/* Copy local assets without following symlinked directories. */
static void project_assets(const char *rel, const char *html, const char *tex, int depth) {
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

static char *tex_name(const char *rel) {
    char *p = xstrdup(rel);
    if (ends(p, ".lean")) {
        strcpy(p + strlen(p) - 5, ".tex");
    }
    return p;
}

/* Preserve TeX commands while correcting paths for copied assets and Lean inputs. */
static char *rewrite_refs(const char *s, const char *source) {
    Buf b = {0};
    size_t n = strlen(s), start = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] == '%' && (i == 0 || s[i - 1] != '\\')) {
            while (i < n && s[i] != '\n') {
                i++;
            }
            continue;
        }
        size_t end = 0;
        char *arg = NULL;
        int kind = 0;
        if (s[i] == '\\') {
            if (include_at(s, n, i, &end, &arg)) {
                kind = 1;
            } else if (graphic_at(s, n, i, &end, &arg)) {
                kind = 2;
            } else if (i + 13 < n && !strncmp(s + i, "\\bibliography", 13)) {
                size_t p = i + 13;
                if (arg_at(s, n, &p, &arg)) {
                    kind = 3;
                    end = p;
                }
            }
        }
        if (!kind) {
            i++;
            continue;
        }
        addn(&b, s + start, i - start);
        addn(&b, s + i, end - i - strlen(arg) - 1);
        if (kind == 1) {
            char *rel = resolve(source, arg);
            if (!rel) {
                die("missing include %s from %s", arg, source);
            } else {
                char *name = tex_name(rel);
                if (!ends(arg, ".tex") && !ends(arg, ".lean") && ends(name, ".tex")) {
                    name[strlen(name) - 4] = 0;
                }
                add(&b, name);
                free(name);
                free(rel);
            }
        } else if (kind == 2) {
            char *path = path_from(source, arg), *rel = project_rel(path);
            if (!rel) {
                die("missing graphic %s from %s", arg, source);
            } else {
                add(&b, rel);
                free(rel);
            }
            free(path);
        } else {
            char *list = xstrdup(arg), *one = strtok(list, ",");
            int first = 1;
            while (one) {
                char *v = trim(one);
                Buf name = {0};
                add(&name, v);
                if (!ends(v, ".bib")) {
                    add(&name, ".bib");
                }
                char *path = path_from(source, name.s), *rel = project_rel(path);
                if (!rel) {
                    die("missing bibliography %s from %s", v, source);
                } else {
                    if (!first++) {
                        ch(&b, ',');
                    }
                    if (ends(rel, ".bib")) {
                        rel[strlen(rel) - 4] = 0;
                    }
                    add(&b, rel);
                    free(rel);
                }
                free(path);
                free(name.s);
                one = strtok(NULL, ",");
            }
            free(list);
        }
        ch(&b, '}');
        free(arg);
        i = end;
        start = i;
    }
    add(&b, s + start);
    return b.s;
}

/* Install AMS theorem environments once in the TeX root's preamble. */
static void theorem_setup(Buf *b) {
    add(b, "\\usepackage{amsthm}\n"
           "\\makeatletter\n"
           "\\theoremstyle{plain}\n"
           "\\@ifundefined{theorem}{\\newtheorem{theorem}{Theorem}}{}\n"
           "\\@ifundefined{lemma}{\\newtheorem{lemma}{Lemma}}{}\n"
           "\\theoremstyle{definition}\n"
           "\\@ifundefined{definition}{\\newtheorem{definition}{Definition}}{}\n"
           "\\makeatother\n");
}

static char *replace_once(const char *source, const char *needle, const char *value, int *found) {
    const char *at = strstr(source, needle);
    Buf b = {0};
    if (!at) {
        if (found) {
            *found = 0;
        }
        return xstrdup(source);
    }
    if (found) {
        *found = 1;
    }
    addn(&b, source, at - source);
    add(&b, value);
    add(&b, at + strlen(needle));
    return b.s;
}

static char *remove_root_title(const char *source) {
    const char *title = strstr(source, "\\title");
    if (!title) {
        return xstrdup(source);
    }
    size_t pos = (size_t)(title - source) + strlen("\\title");
    char *arg = NULL;
    if (!arg_at(source, strlen(source), &pos, &arg)) {
        return xstrdup(source);
    }
    free(arg);
    while (source[pos] && isspace((unsigned char)source[pos])) {
        pos++;
    }
    if (strncmp(source + pos, "\\maketitle", strlen("\\maketitle"))) {
        return xstrdup(source);
    }
    pos += strlen("\\maketitle");
    while (source[pos] == '\n' || source[pos] == '\r') {
        pos++;
    }
    Buf b = {0};
    addn(&b, source, title - source);
    add(&b, source + pos);
    return b.s;
}

static void write_tex(const char *dir) {
    mkdirs(dir);
    int has_boxes = 0;
    for (int i = 0; i < nfiles; i++) {
        has_boxes |= files[i].has_boxes;
    }
    for (int i = 0; i < nfiles; i++) {
        File *f = &files[i];
        char *rel = tex_name(f->rel), *path = join(dir, rel);
        Buf b = {0};
        if (i == root_index && f->lean && tex_template_index >= 0) {
            add(&b, f->tex);
        } else if (i == tex_template_index && tex_template_index >= 0) {
            int content_found = 0, setup_found = 0;
            char *content = remove_root_title(files[root_index].tex);
            char *with_content = replace_once(f->data, "{{MYNE_CONTENT}}", content, &content_found);
            Buf setup = {0};
            if (has_boxes) {
                theorem_setup(&setup);
            }
            char *complete = replace_once(with_content, "{{MYNE_THEOREM_SETUP}}",
                                          setup.s ? setup.s : "", &setup_found);
            if (!content_found) {
                die("TeX template is missing {{MYNE_CONTENT}}: %s", f->rel);
            }
            if (!setup_found) {
                die("TeX template is missing {{MYNE_THEOREM_SETUP}}: %s", f->rel);
            }
            add(&b, complete);
            free(content);
            free(with_content);
            free(complete);
            free(setup.s);
        } else if (i == tex_class_index && tex_class_index >= 0) {
            add(&b, f->data);
        } else if (i == root_index && f->lean) {
            add(&b, "\\documentclass{article}\n\\usepackage{amsmath,amssymb,natbib}\n");
            if (has_boxes) {
                theorem_setup(&b);
            }
            add(&b, "\\begin{document}\n");
            add(&b, f->tex);
            if (!strstr(f->tex, "\\bibliography{") && nbibcfg) {
                add(&b, "\n\\bibliographystyle{plainnat}\n\\bibliography{");
                for (int k = 0; k < nbibcfg; k++) {
                    if (k) {
                        ch(&b, ',');
                    }
                    char *v = xstrdup(bibcfg[k]);
                    if (ends(v, ".bib")) {
                        v[strlen(v) - 4] = 0;
                    }
                    add(&b, v);
                    free(v);
                }
                add(&b, "}\n");
            }
            add(&b, "\\end{document}\n");
        } else if (i == root_index && has_boxes) {
            const char *begin = strstr(f->data, "\\begin{document}");
            if (!begin) {
                die("TeX root has no \\begin{document}: %s", f->rel);
                add(&b, f->data);
            } else {
                addn(&b, f->data, begin - f->data);
                theorem_setup(&b);
                add(&b, begin);
            }
        } else {
            add(&b, f->lean ? f->tex : f->data);
        }
        char *body = rewrite_refs(b.s, f->rel);
        if (write_all(path, body, strlen(body))) {
            die("cannot write %s", path);
        }
        free(body);
        free(b.s);
        free(path);
        free(rel);
    }
}

static void nav_html(Buf *b, int active) {
    int root_heading = 7;
    for (int i = 1; i < npages; i++) {
        if (pages[i].heading >= 2 && pages[i].heading < root_heading) {
            root_heading = pages[i].heading;
        }
    }
    if (root_heading == 7) {
        root_heading = page_heading_level;
    }
    add(b, "<nav class=toc-nav aria-label=\"Table of contents\"><a class=nav-title href=\"index.html\">");
    escape(b, doc_title ? doc_title : "Myne");
    add(b, "</a>");
    for (int i = 0; i < npages; i++) {
        if (!i && npages > 1) {
            continue;
        }
        add(b, "<a class=\"");
        if (i == active) {
            add(b, "active ");
        }
        int depth = pages[i].heading - root_heading;
        if (depth < 0) {
            depth = 0;
        } else if (depth > 4) {
            depth = 4;
        }
        char nav_class[80];
        snprintf(nav_class, sizeof(nav_class), "nav-entry nav-depth-%d heading-%s ", depth,
                 heading_name(pages[i].heading));
        add(b, nav_class);
        if (pages[i].heading == 2) {
            add(b, "part");
        }
        add(b, "\" href=\"");
        escape(b, pages[i].slug);
        add(b, "\">");
        if (pages[i].number) {
            add(b, "<span class=heading-number>");
            escape(b, pages[i].number);
            add(b, "</span> ");
        }
        escape(b, pages[i].title);
        add(b, "</a>");
    }
    add(b, "</nav>");
}

static void theme_toggle_html(Buf *b) {
    if (dark_mode_toggle) {
        add(b, "<button class=theme-toggle type=button role=switch aria-label=\"Color theme\" "
               "aria-checked=\"false\"><span class=theme-label>Light</span>"
               "<span class=theme-switch aria-hidden=\"true\"><span class=theme-thumb></span></span>"
               "<span class=theme-label>Dark</span></button>");
    }
}

static void page_toc_html(Buf *b, int page) {
    if (!web_table_of_contents || pages[page].heading < 2 ||
        pages[page].heading >= page_heading_level) {
        return;
    }
    int child_pages = 0;
    for (int j = page + 1; j < npages; j++) {
        if (pages[j].heading <= pages[page].heading) {
            break;
        }
        if (pages[j].heading == pages[page].heading + 1) {
            child_pages++;
        }
    }
    if (!child_pages && !pages[page].toc_count) {
        return;
    }
    add(b, "<nav class=page-toc aria-label=\"On this page\"><h2>Contents</h2><ul>");
    if (pages[page].toc.s) {
        addn(b, pages[page].toc.s, pages[page].toc.n);
    }
    for (int j = page + 1; j < npages; j++) {
        if (pages[j].heading <= pages[page].heading) {
            break;
        }
        if (pages[j].heading != pages[page].heading + 1) {
            continue;
        }
        add(b, "<li><a href=\"");
        escape(b, pages[j].slug);
        add(b, "\">");
        if (pages[j].number) {
            add(b, "<span class=heading-number>");
            escape(b, pages[j].number);
            add(b, "</span> ");
        }
        escape(b, pages[j].title);
        add(b, "</a></li>");
    }
    add(b, "</ul></nav>");
}

static char *document_pdf_name(void) {
    int source = tex_template_index >= 0 ? tex_template_index : root_index;
    char *pdf = tex_name(files[source].rel);
    if (ends(pdf, ".tex")) {
        strcpy(pdf + strlen(pdf) - 4, ".pdf");
    }
    return pdf;
}

static void header_html(Buf *b) {
    add(b, "<header class=site-header><a class=site-title href=\"index.html\">");
    escape(b, doc_title ? doc_title : "Myne");
    add(b, "</a><nav class=header-actions aria-label=\"Document actions\">");
    if (!strcmp(format, "both")) {
        char *pdf = document_pdf_name();
        add(b, "<a class=header-action href=\"../tex/");
        escape(b, pdf);
        add(b, "\">PDF</a>");
        free(pdf);
    }
    if (ndependency_nodes) {
        add(b, "<a class=header-action href=\"dependencies.html\">Dependency graph</a>");
    }
    add(b, "</nav></header>");
}

static int dependency_uses(const char *source, const char *name) {
    size_t len = strlen(name);
    if (!len) return 0;
    for (const char *p = source; (p = strstr(p, name)); p += len) {
        int left = p == source ||
                   !(isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '\'' ||
                     (unsigned char)p[-1] >= 0x80);
        int right = !p[len] ||
                    !(isalnum((unsigned char)p[len]) || p[len] == '_' || p[len] == '\'' ||
                      (unsigned char)p[len] >= 0x80);
        if (left && right) return 1;
    }
    return 0;
}

static void dependency_graph_html(const char *dir) {
    if (!ndependency_nodes) return;
    Buf html = {0};
    add(&html, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta "
               "name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Dependency "
               "Graph — ");
    escape(&html, doc_title ? doc_title : "Myne");
    add(&html, "</title>");
    if (dark_mode_toggle) {
        add(&html,
            "<script>try{let t=localStorage.getItem('myne-theme');if(t!=='light'&&t!=='dark')"
            "t=matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light';"
            "document.documentElement.dataset.theme=t}catch(_){}</script>");
    }
    add(&html, "<link rel=\"stylesheet\" href=\"style.css\"><link rel=\"stylesheet\" "
               "href=\"katex/katex.min.css\"><script defer src=\"katex/katex.min.js\"></script>"
               "<script defer src=\"katex/contrib/auto-render.min.js\"></script><script defer "
               "src=\"runtime.js\"></script>");
    if (ends(style, ".css")) {
        add(&html, "<link rel=\"stylesheet\" href=\"custom.css\">");
    }
    add(&html, "</head><body data-graph-page data-style=\"");
    escape(&html, ends(style, ".css") ? "custom" : style);
    add(&html, "\">");
    header_html(&html);
    theme_toggle_html(&html);
    add(&html, "<main class=dependency-page><h1>Dependency graph</h1>");
    if (elaborated_dependency_graph) {
        add(&html, "<p class=graph-help>This graph uses Lean's elaborated types and proof terms, "
                   "including names resolved through notation and tactics.</p>");
    } else {
        add(&html, "<aside class=graph-warning role=note><strong>Warning: syntactic dependency "
                   "graph.</strong> Edges come from text matching and may miss dependencies "
                   "introduced by notation or tactics, or include false matches. Build with "
                   "<code>--elaborated-deps</code> for Lean's elaborated dependencies.</aside>");
    }
    add(&html, "<p class=graph-help>Arrows point from prerequisites toward the declarations "
               "they build. Only declarations in this Myne document become nodes. Click a node "
               "to open it; drag nodes or the background to rearrange and pan.</p><div "
               "class=dependency-toolbar role=toolbar aria-label=\"Graph controls\"><button "
               "type=button data-graph-action=zoom-out aria-label=\"Zoom out\">−</button><button "
               "type=button data-graph-action=zoom-in aria-label=\"Zoom in\">+</button><button "
               "type=button data-graph-action=fit>Fit graph</button><button type=button "
               "data-graph-action=reset>Reset layout</button><button type=button "
               "data-graph-action=direction>Top to bottom</button><span class=graph-legend>"
               "<i class=legend-definition></i> Definitions <i class=legend-lemma></i> Lemmas "
               "<i class=legend-theorem></i> Theorems</span></div><div "
               "class=dependency-scroll><svg class=dependency-graph role=img aria-label=\"Lean "
               "declaration dependency graph\" viewBox=\"0 0 1200 800\"><defs><marker id=\"dep-arrow\" "
               "markerWidth=\"8\" markerHeight=\"8\" refX=\"7\" refY=\"4\" orient=\"auto\"><path "
               "d=\"M0,0 L8,4 L0,8 z\" class=dependency-arrow></path></marker></defs><g "
               "class=dependency-edges\">");
    for (int i = 0; i < ndependency_nodes; i++) {
        for (int j = 0; j < ndependency_nodes; j++) {
            if (dependency_edge(i, j)) {
                add(&html, "<path class=dependency-edge data-from=\"");
                char index[24];
                snprintf(index, sizeof(index), "%d", j);
                add(&html, index);
                add(&html, "\" data-to=\"");
                snprintf(index, sizeof(index), "%d", i);
                add(&html, index);
                add(&html, "\" marker-end=\"url(#dep-arrow)\"></path>");
            }
        }
    }
    add(&html, "</g><g class=dependency-nodes>");
    for (int i = 0; i < ndependency_nodes; i++) {
        DependencyNode *node = &dependency_nodes[i];
        int width = (int)strlen(node->name) * 9 + 36;
        if (width < 150) width = 150;
        if (width > 320) width = 320;
        char id[24], dims[128];
        snprintf(id, sizeof(id), "decl-%d", i + 1);
        add(&html, "<a class=dependency-link href=\"");
        escape(&html, node->slug ? node->slug : "index.html");
        ch(&html, '#');
        add(&html, id);
        add(&html, "\" aria-label=\"Open ");
        escape(&html, node->doc_name ? node->doc_name : node->name);
        add(&html, "\" data-doc-name=\"");
        escape(&html, node->doc_name ? node->doc_name : node->name);
        add(&html, "\" data-doc-nl=\"");
        escape(&html, node->doc_nl ? node->doc_nl : "");
        add(&html, "\" data-doc-formula=\"");
        escape(&html, node->doc_formula ? node->doc_formula : "");
        add(&html, "\"><g class=\"dependency-node ");
        add(&html, node->kind == LEAN_DEF ? "dependency-definition" :
                    node->kind == LEAN_LEMMA ? "dependency-lemma" : "dependency-theorem");
        add(&html, "\" data-width=\"");
        snprintf(dims, sizeof(dims), "%d\" data-index=\"%d", width, i);
        add(&html, dims);
        add(&html, "\"><rect x=\"");
        snprintf(dims, sizeof(dims), "%d\" y=\"-28\" width=\"%d\" height=\"56\" rx=\"8\"/>", -width / 2, width);
        add(&html, dims);
        add(&html, "<text class=dependency-name x=\"0\" y=\"-2\">");
        escape(&html, node->name);
        add(&html, "</text><text class=dependency-kind x=\"0\" y=\"17\">");
        add(&html, node->kind == LEAN_DEF ? "definition"
                    : node->kind == LEAN_LEMMA ? "lemma" : "theorem");
        add(&html, "</text></g></a>");
    }
    add(&html, "</g></svg></div><div class=dependency-tooltip role=tooltip hidden></div></main></body></html>\n");
    char *path = join(dir, "dependencies.html");
    if (write_all(path, html.s, html.n)) die("cannot write %s", path);
    free(path);
    free(html.s);
}

static void write_html(const char *dir) {
    mkdirs(dir);
    char *dst = join(dir, "katex");
    unpack_katex(dst);
    free(dst);
    char *a = join(dir, "style.css"), *b = join(dir, "runtime.js");
    if (write_all(a, myne_style, strlen(myne_style)) ||
        write_all(b, myne_runtime, strlen(myne_runtime))) {
        die("cannot write web assets");
    }
    free(a);
    free(b);
    int custom = ends(style, ".css");
    if (custom) {
        char *source = join(basedir, style), *target = join(dir, "custom.css");
        if (copy_file(source, target)) {
            die("cannot copy custom CSS %s", source);
        }
        free(source);
        free(target);
    }
    for (int i = 0; i < npages; i++) {
        Buf html = {0};
        add(&html, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta "
                   "name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>");
        if (pages[i].number) {
            escape(&html, pages[i].number);
            ch(&html, ' ');
        }
        escape(&html, pages[i].title);
        add(&html, " — ");
        escape(&html, doc_title ? doc_title : "Myne");
        add(&html, "</title>");
        if (dark_mode_toggle) {
            add(&html,
                "<script>try{let t=localStorage.getItem('myne-theme');if(t!=='light'&&t!=='dark')"
                "t=matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light';"
                "document.documentElement.dataset.theme=t}catch(_){}</script>");
        }
        add(&html, "<link rel=\"stylesheet\" href=\"style.css\"><link rel=\"stylesheet\" "
                   "href=\"katex/katex.min.css\">");
        if (custom) {
            add(&html, "<link rel=\"stylesheet\" href=\"custom.css\">");
        }
        add(&html, "<script defer src=\"katex/katex.min.js\"></script><script defer "
                   "src=\"katex/contrib/auto-render.min.js\"></script><script defer "
                   "src=\"runtime.js\"></script></head><body data-style=\"");
        escape(&html, custom ? "custom" : style);
        add(&html, live ? "\" data-live=\"1\">" : "\" data-live=\"0\">");
        header_html(&html);
        nav_html(&html, i);
        theme_toggle_html(&html);
        add(&html, "<main><h1>");
        if (pages[i].number) {
            add(&html, "<span class=heading-number>");
            escape(&html, pages[i].number);
            add(&html, "</span> ");
        }
        escape(&html, pages[i].title);
        add(&html, "</h1>");
        addn(&html, pages[i].body.s ? pages[i].body.s : "", pages[i].body.n);
        if (pages[i].heading == 2) {
            add(&html, "<ul class=part-list>");
            for (int j = i + 1; j < npages && pages[j].part == pages[i].part; j++) {
                add(&html, "<li><a href=\"");
                escape(&html, pages[j].slug);
                add(&html, "\">");
                if (pages[j].number) {
                    add(&html, "<span class=heading-number>");
                    escape(&html, pages[j].number);
                    add(&html, "</span> ");
                }
                escape(&html, pages[j].title);
                add(&html, "</a></li>");
            }
            add(&html, "</ul>");
        }
        if (pages[i].notes.n) {
            add(&html, "<aside class=mobile-notes><h2>Notes</h2><ol>");
            addn(&html, pages[i].notes.s, pages[i].notes.n);
            add(&html, "</ol></aside>");
        }
        page_toc_html(&html, i);
        add(&html, "</main></body></html>\n");
        char *path = join(dir, pages[i].slug);
        if (write_all(path, html.s, html.n)) {
            die("cannot write %s", path);
        }
        free(path);
        free(html.s);
    }
    dependency_graph_html(dir);
}

/* Stage each build so parsing and writing errors leave the last successful output alone. */
static int build(void) {
    clear_project();
    free(doc_title);
    doc_title = NULL;
    char *rel = NULL;
    if (root[0] == '/') {
        rel = project_rel(root);
    } else {
        char *path = join(basedir, root);
        rel = project_rel(path);
        free(path);
    }
    if (!rel) {
        die("root file is missing or outside the project: %s", root);
        return -1;
    }
    root_index = gather(rel);
    free(rel);
    if (failed || root_index < 0) {
        return -1;
    }
    tex_template_index = tex_class_index = -1;
    if (strcmp(format, "html") && tex_template && *tex_template) {
        char *path = join(basedir, tex_template);
        char *template_rel = project_rel(path);
        free(path);
        if (!template_rel) {
            die("TeX template is missing or outside the project: %s", tex_template);
        } else {
            tex_template_index = gather(template_rel);
            free(template_rel);
        }
    }
    if (strcmp(format, "html") && tex_class && *tex_class) {
        char *path = join(basedir, tex_class);
        char *class_rel = project_rel(path);
        free(path);
        if (!class_rel) {
            die("TeX class is missing or outside the project: %s", tex_class);
        } else {
            tex_class_index = gather(class_rel);
            free(class_rel);
        }
    }
    if (failed) {
        return -1;
    }
    translate_parallel();
    flatten(root_index, 0);
    if (appendix_capture) {
        die("missing --myne:appendix_end");
    }
    for (int i = 0; i < nappendix_segs && !failed; i++) {
        if (nsegs >= MAX_SEGS) {
            die("document has too many segments");
            break;
        }
        segs[nsegs++] = appendix_segs[i];
        appendix_segs[i].s = NULL;
        appendix_segs[i].doc_name = NULL;
        appendix_segs[i].doc_nl = NULL;
        appendix_segs[i].doc_formula = NULL;
    }
    if (failed) {
        return -1;
    }
    if (strcmp(format, "tex")) {
        load_bibliographies();
        if (failed) {
            return -1;
        }
        dependency_collect();
        if (elaborated_dependency_graph && ndependency_nodes) dependency_elaborate();
        if (failed) return -1;
        dependency_prune();
        new_page("Introduction", 0, NULL);
        for (int i = 0; i < nsegs; i++) {
            if (segs[i].kind == SEG_BOX_OPEN) {
                const char *kind = segs[i].s;
                const char *sep = strchr(kind, '|');
                size_t kind_len = sep ? (size_t)(sep - kind) : strlen(kind);
                int is_def = kind_len == 3 && !memcmp(kind, "def", 3);
                int is_lemma = kind_len == 5 && !memcmp(kind, "lemma", 5);
                int is_theorem = kind_len == 7 && !memcmp(kind, "theorem", 7);
                const char *heading = is_def       ? "Definition"
                                      : is_lemma   ? "Lemma"
                                      : is_theorem ? "Theorem"
                                                   : "Code";
                const char *title = sep ? sep + 1 : heading;
                add(&pages[current].body, "<div class=\"lean-block ");
                addn(&pages[current].body, kind, kind_len);
                add(&pages[current].body, "\"><header>");
                if (is_def || is_lemma || is_theorem) {
                    Buf number = {0};
                    next_statement_number(&number);
                    add(&pages[current].body, "<span class=statement-label>");
                    add(&pages[current].body, heading);
                    ch(&pages[current].body, ' ');
                    escape(&pages[current].body, number.s);
                    add(&pages[current].body, "</span>");
                    int default_title = strlen(title) == kind_len &&
                                        !strncasecmp(title, kind, kind_len);
                    if (!default_title) {
                        add(&pages[current].body, "<span class=statement-title> — ");
                        escape(&pages[current].body, title);
                        add(&pages[current].body, "</span>");
                    }
                    free(number.s);
                } else {
                    escape(&pages[current].body, title);
                }
                add(&pages[current].body, "</header><div class=\"lean-doc\">");
            } else if (segs[i].kind == SEG_CODE) {
                add(&pages[current].body, "</div>");
                for (int j = 0; j < ndependency_nodes; j++) {
                    if (dependency_nodes[j].segment == i) {
                        dependency_nodes[j].page = current;
                        free(dependency_nodes[j].slug);
                        dependency_nodes[j].slug = xstrdup(pages[current].slug);
                    }
                }
                code_html(segs[i].s, i);
            } else if (segs[i].kind == SEG_BOX_CLOSE) {
                add(&pages[current].body, "</div>");
            } else if (segs[i].kind == SEG_APPENDIX_START) {
                Buf title = {0}, number = {0};
                add(&title, "Appendix: ");
                add(&title, segs[i].s);
                number_heading(&number, page_heading_level);
                new_page(title.s, page_heading_level, number.s);
                free(title.s);
                free(number.s);
            } else {
                char *mapped = replace_map(segs[i].s);
                text_html(mapped);
                free(mapped);
            }
        }
        if (failed) {
            return -1;
        }
        if (!doc_title) {
            doc_title = xstrdup(files[root_index].rel);
        }
        if (npages == 1) {
            free(pages[0].title);
            pages[0].title = xstrdup(doc_title);
        }
        append_references();
    }
    if (mkdirs(output)) {
        die("cannot create output directory %s", output);
        return -1;
    }
    char *stage = join(output, ".myne-stage");
    remove_tree(stage);
    mkdirs(stage);
    char *html = join(stage, "html"), *tex = join(stage, "tex");
    if (strcmp(format, "tex")) {
        write_html(html);
    }
    if (strcmp(format, "html")) {
        write_tex(tex);
    }
    if (!failed && !strcmp(format, "both")) {
        project_assets("", html, tex, 0);
    } else if (!failed && !strcmp(format, "tex")) {
        char *dummy = join(stage, "unused");
        project_assets("", dummy, tex, 0);
        remove_tree(dummy);
        free(dummy);
    } else if (!failed) {
        char *dummy = join(stage, "unused");
        project_assets("", html, dummy, 0);
        remove_tree(dummy);
        free(dummy);
    }
    if (!failed && !strcmp(format, "both")) {
        char *pdf = document_pdf_name();
        char *old_tex = join(output, "tex"), *old_pdf = join(old_tex, pdf);
        char *new_pdf = join(tex, pdf);
        struct stat st;
        if (!stat(old_pdf, &st) && S_ISREG(st.st_mode) && copy_file(old_pdf, new_pdf)) {
            die("cannot preserve generated PDF %s", old_pdf);
        }
        free(pdf);
        free(old_tex);
        free(old_pdf);
        free(new_pdf);
    }
    if (!failed) {
        if (strcmp(format, "tex")) {
            char *final = join(output, "html");
            remove_tree(final);
            if (rename(html, final)) {
                die("cannot install HTML output");
            }
            free(final);
        }
        if (strcmp(format, "html")) {
            char *final = join(output, "tex");
            remove_tree(final);
            if (rename(tex, final)) {
                die("cannot install TeX output");
            }
            free(final);
        }
    }
    remove_tree(stage);
    free(stage);
    free(html);
    free(tex);
    if (!failed) {
        pthread_mutex_lock(&epoch_lock);
        epoch++;
        pthread_mutex_unlock(&epoch_lock);
        fprintf(stderr, "myne: built %d files, %d pages in %s\n", nfiles, npages, output);
    }
    return failed ? -1 : 0;
}

/* Poll source and asset timestamps, excluding the output directory itself. */
static unsigned long long fingerprint_dir(const char *rel, int depth) {
    if (depth > 32) {
        return 0;
    }
    char *dir = join(basedir, rel);
    DIR *d = opendir(dir);
    if (!d) {
        free(dir);
        return 0;
    }
    unsigned long long h = 1469598103934665603ULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcmp(e->d_name, ".git") &&
            strcmp(e->d_name, "vendor")) {
            char *child = rel[0] ? join(rel, e->d_name) : xstrdup(e->d_name),
                 *path = join(basedir, child);
            struct stat st;
            if (!lstat(path, &st)
#ifdef _WIN32
                && !is_reparse_point(path)
#endif
            ) {
                if (S_ISDIR(st.st_mode)) {
                    if (strncmp(path, output, strlen(output))) {
                        h ^= fingerprint_dir(child, depth + 1);
                    }
                } else if (S_ISREG(st.st_mode) &&
                           (asset(child) || ends(child, ".tex") || ends(child, ".lean") ||
                            ends(child, ".toml") || ends(child, ".css"))) {
                    unsigned long long x = (unsigned long long)stamp(path);
                    for (char *p = child; *p; p++) {
                        x = x * 1099511628211ULL + (unsigned char)*p;
                    }
                    h ^= x;
                }
            }
            free(child);
            free(path);
        }
    }
    closedir(d);
    free(dir);
    return h;
}

static const char *mime(const char *s) {
    if (ends(s, ".html")) {
        return "text/html; charset=utf-8";
    }
    if (ends(s, ".css")) {
        return "text/css";
    }
    if (ends(s, ".js")) {
        return "application/javascript";
    }
    if (ends(s, ".woff2")) {
        return "font/woff2";
    }
    if (ends(s, ".woff")) {
        return "font/woff";
    }
    if (ends(s, ".ttf")) {
        return "font/ttf";
    }
    if (ends(s, ".png")) {
        return "image/png";
    }
    if (ends(s, ".jpg") || ends(s, ".jpeg")) {
        return "image/jpeg";
    }
    if (ends(s, ".svg")) {
        return "image/svg+xml";
    }
    if (ends(s, ".pdf")) {
        return "application/pdf";
    }
    return "application/octet-stream";
}

static void send_all(myne_socket fd, const char *s, size_t n) {
    while (n) {
#ifdef _WIN32
        int chunk = n > INT_MAX ? INT_MAX : (int)n;
        int sent = send(fd, s, chunk, 0);
#else
        ssize_t sent = send(fd, s, n, 0);
#endif
        if (sent <= 0) {
            return;
        }
        s += sent;
        n -= sent;
    }
}

/* The browser polls the epoch endpoint and reloads after a successful rebuild. */
static void *server_thread(void *unused) {
    myne_socket fd = *(myne_socket *)unused;
    free(unused);
    fprintf(stderr, "myne: serving http://127.0.0.1:%d/\n", port);
    for (;;) {
        myne_socket client = accept(fd, NULL, NULL);
        if (client == SOCKET_INVALID) {
            continue;
        }
        char req[2048] = {0};
#ifdef _WIN32
        int n = recv(client, req, (int)sizeof(req) - 1, 0);
#else
        ssize_t n = recv(client, req, sizeof(req) - 1, 0);
#endif
        if (n <= 0 || strncmp(req, "GET /", 5)) {
            socket_close(client);
            continue;
        }
        char *path = req + 5, *end = strchr(path, ' ');
        if (!end) {
            socket_close(client);
            continue;
        }
        *end = 0;
        if (!strcmp(path, "__myne_epoch")) {
            char body[40], head[200];
            pthread_mutex_lock(&epoch_lock);
            unsigned long value = epoch;
            pthread_mutex_unlock(&epoch_lock);
            int len = snprintf(body, sizeof(body), "%lu", value);
            int h = snprintf(head, sizeof(head),
                             "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: "
                             "no-store\r\nContent-Length: %d\r\n\r\n",
                             len);
            send_all(client, head, h);
            send_all(client, body, len);
            socket_close(client);
            continue;
        }
        if (!*path) {
            path = "index.html";
        }
        int safe = path[0] != '/' && !strstr(path, "..");
        for (char *p = path; *p; p++) {
            if (!(isalnum((unsigned char)*p) || strchr("/._-", *p))) {
                safe = 0;
            }
        }
        int pdf_file = !strncmp(path, "tex/", 4) && ends(path, ".pdf");
        char *base = join(output, pdf_file ? "tex" : "html");
        char *file = safe ? join(base, pdf_file ? path + 4 : path) : NULL;
        free(base);
        struct stat st;
        int ok = file && !stat(file, &st) && S_ISREG(st.st_mode);
        if (!ok) {
            const char *msg = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            send_all(client, msg, strlen(msg));
        } else {
            FILE *f = fopen(file, "rb");
            if (f) {
                char head[300], buf[16384];
                int h = snprintf(head, sizeof(head),
                                 "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: "
                                 "%lld\r\nCache-Control: no-store\r\n\r\n",
                                 mime(file), (long long)st.st_size);
                send_all(client, head, h);
                size_t len;
                while ((len = fread(buf, 1, sizeof(buf), f))) {
                    send_all(client, buf, len);
                }
                fclose(f);
            }
        }
        free(file);
        socket_close(client);
    }
    return NULL;
}

static int start_server(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        fprintf(stderr, "myne: WSAStartup failed\n");
        return -1;
    }
#endif
    myne_socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCKET_INVALID) {
        perror("socket");
        return -1;
    }
    int yes = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 16)) {
        perror("myne serve");
        socket_close(fd);
        return -1;
    }
    myne_socket *arg = xmalloc(sizeof(myne_socket));
    *arg = fd;
    pthread_t thread;
    if (pthread_create(&thread, NULL, server_thread, arg)) {
        perror("pthread_create");
        socket_close(fd);
        free(arg);
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

static void usage(void) {
    puts("Usage: myne [-i root|project-dir] [-f html|tex|both] [-o outputDir] [-s style] [--config "
         "file] "
         "[--elaborated-deps] [--watch] [--serve] [--port N]");
}

int main(int argc, char **argv) {
    int watch = 0, serve = 0, explicit_config = 0;
    char *input_arg = NULL;
    int input_is_dir = 0;
    format = xstrdup("html");
    style = xstrdup("plain");
    page_heading = xstrdup("section");
    snprintf(output, sizeof(output), "out");
    snprintf(config_path, sizeof(config_path), "myne.toml");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            snprintf(config_path, sizeof(config_path), "%s", argv[++i]);
#ifdef _WIN32
            normalize_path(config_path);
#endif
            explicit_config = 1;
        } else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            input_arg = argv[++i];
#ifdef _WIN32
            normalize_path(input_arg);
#endif
        }
    }
    if (input_arg) {
        struct stat st;
        input_is_dir = !stat(input_arg, &st) && S_ISDIR(st.st_mode);
        if (input_is_dir && !explicit_config) {
            char *manifest = join(input_arg, "myne.toml");
            snprintf(config_path, sizeof(config_path), "%s", manifest);
            free(manifest);
            explicit_config = 1;
        }
    }
    char *config_abs = NULL;
    if (access(config_path, R_OK) == 0) {
        char resolved[PATH_MAX];
        if (realpath(config_path, resolved)) {
            config_abs = xstrdup(resolved);
        }
    }
    if (explicit_config && !config_abs) {
        die("cannot read config %s", config_path);
        return 1;
    }
    if (config_abs) {
        char *slash = strrchr(config_abs, '/');
        *slash = 0;
        snprintf(basedir, sizeof(basedir), "%s", config_abs);
        *slash = '/';
        load_config(config_abs);
        if (failed) {
            return 1;
        }
    } else if (!getcwd(basedir, sizeof(basedir))) {
        perror("getcwd");
        return 1;
    }
#ifdef _WIN32
    normalize_path(basedir);
#endif
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--config")) {
            i++;
            continue;
        }
        if ((!strcmp(a, "-i") || !strcmp(a, "-f") || !strcmp(a, "-o") || !strcmp(a, "-s") ||
             !strcmp(a, "--port")) &&
            i + 1 < argc) {
            char *v = argv[++i];
            if (!strcmp(a, "-i")) {
                if (!input_is_dir) {
                    root = v;
                }
            } else if (!strcmp(a, "-f")) {
                format = v;
            } else if (!strcmp(a, "-o")) {
                snprintf(output, sizeof(output), "%s", v);
#ifdef _WIN32
                normalize_path(output);
#endif
            } else if (!strcmp(a, "-s")) {
                style = v;
            } else {
                port = atoi(v);
            }
        } else if (!strcmp(a, "--watch")) {
            watch = 1;
        } else if (!strcmp(a, "--elaborated-deps")) {
            elaborated_dependency_graph = 1;
        } else if (!strcmp(a, "--serve")) {
            serve = watch = live = 1;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else {
            usage();
            die("unknown or incomplete option %s", a);
            return 1;
        }
    }
    if (!root || (strcmp(format, "html") && strcmp(format, "tex") && strcmp(format, "both"))) {
        usage();
        die("root or format is invalid");
        return 1;
    }
    if (serve && !strcmp(format, "tex")) {
        die("--serve requires html or both format");
        return 1;
    }
    if (port < 1 || port > 65535) {
        die("invalid port");
        return 1;
    }
    if (strcmp(style, "plain") && strcmp(style, "verso") && strcmp(style, "hutter") &&
        !ends(style, ".css")) {
        die("unknown style %s", style);
        return 1;
    }
    page_heading_level = structural(page_heading);
    if (page_heading_level < 2 || page_heading_level > 6) {
        die("pageHeading must be part, chapter, section, subsection, or subsubsection");
        return 1;
    }
    if (output[0] != '/') {
        char *absolute = join(basedir, output);
        snprintf(output, sizeof(output), "%s", absolute);
        free(absolute);
    }
    if (build() && !watch) {
        return 1;
    }
    if (serve && start_server()) {
        return 1;
    }
    if (!watch) {
        return 0;
    }
    unsigned long long previous = fingerprint_dir("", 0);
    struct timespec delay = {0, 500000000};
    for (;;) {
        nanosleep(&delay, NULL);
        unsigned long long next = fingerprint_dir("", 0);
        if (next != previous) {
            previous = next;
            build();
        }
    }
}
