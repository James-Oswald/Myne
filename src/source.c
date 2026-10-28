#include "myne_internal.h"

static pthread_mutex_t translation_lock = PTHREAD_MUTEX_INITIALIZER;


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
void flatten(int id, int depth);

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
void flatten(int id, int depth) {
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
    int top_heading_seen = 0, appendix_open = 0;
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
    f->appendix_tex = appendix_tex.s;
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

void translate_parallel(void) {
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

