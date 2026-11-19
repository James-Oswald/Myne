#include "myne_internal.h"

static char *tex_name(const char *rel) {
    char *p = xstrdup(rel);
    if (ends(p, ".lean")) {
        strcpy(p + strlen(p) - 5, ".tex");
    }
    return p;
}

static char *appendix_name(const char *rel) {
    char *name = tex_name(rel);
    if (ends(name, ".tex")) {
        name[strlen(name) - 4] = 0;
    }
    Buf path = {0};
    add(&path, name);
    add(&path, ".myne-appendix.tex");
    free(name);
    return path.s;
}

/* Keep reference keys separate from the prose math translator. */
int tex_reference_at(const char *s, size_t n, size_t i, size_t *end, char **arg) {
    const char *commands[] = {"\\label", "\\ref", "\\pageref", "\\eqref",
                              "\\autoref", "\\cref", "\\Cref"};
    for (size_t k = 0; k < sizeof(commands) / sizeof(*commands); k++) {
        size_t len = strlen(commands[k]);
        if (i + len > n || memcmp(s + i, commands[k], len)) continue;
        size_t p = i + len;
        if (p < n && s[p] == '*') p++;
        if (arg_at(s, n, &p, arg)) {
            *end = p;
            return 1;
        }
    }
    return 0;
}

/* PDFTeX's label names must survive writes to the auxiliary file. Encode
   Unicode keys and reserve the same prefix for escaping literal ASCII keys. */
static void write_ref_keys(Buf *b, const char *arg) {
    static const char prefix[] = "myne-utf8:", hex[] = "0123456789abcdef";
    const char *start = arg;
    while (*start) {
        const char *end = start;
        while (*end && *end != ',') end++;
        const char *key = start, *tail = end;
        while (key < tail && isspace((unsigned char)*key)) key++;
        while (tail > key && isspace((unsigned char)tail[-1])) tail--;
        int encode = (size_t)(tail - key) >= sizeof(prefix) - 1 &&
                     !memcmp(key, prefix, sizeof(prefix) - 1);
        for (const char *p = key; p < tail && !encode; p++) {
            encode = (unsigned char)*p >= 128;
        }
        if (encode) {
            addn(b, start, key - start);
            add(b, prefix);
            for (const char *p = key; p < tail; p++) {
                unsigned char c = (unsigned char)*p;
                ch(b, hex[c >> 4]);
                ch(b, hex[c & 15]);
            }
            addn(b, tail, end - tail);
        } else {
            addn(b, start, end - start);
        }
        if (!*end) break;
        ch(b, ',');
        start = end + 1;
    }
}

static int document_class_at(const char *s, size_t n, size_t i, size_t *end, char **arg) {
    static const char command[] = "\\documentclass";
    size_t p = i + sizeof(command) - 1;
    if (p > n || memcmp(s + i, command, sizeof(command) - 1)) return 0;
    while (p < n && isspace((unsigned char)s[p])) p++;
    if (p < n && s[p] == '[') {
        int braces = 0;
        p++;
        while (p < n && (braces || s[p] != ']')) {
            if (s[p] == '\\' && p + 1 < n) {
                p += 2;
                continue;
            }
            if (s[p] == '{') braces++;
            if (s[p] == '}') braces--;
            p++;
        }
        if (p == n) return 0;
        p++;
    }
    if (!arg_at(s, n, &p, arg)) return 0;
    *end = p;
    return 1;
}

static char *resolve_tex_asset(const char *source, const char *name) {
    char *path = path_from(source, name);
    char *rel = project_rel(path);
    free(path);
    if (!rel) {
        path = join(basedir, name);
        rel = project_rel(path);
        free(path);
    }
    return rel;
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
            } else if (tex_reference_at(s, n, i, &end, &arg)) {
                kind = 5;
            } else if (document_class_at(s, n, i, &end, &arg)) {
                kind = 6;
            } else if (i + strlen("\\bibliographystyle") <= n &&
                       !strncmp(s + i, "\\bibliographystyle", strlen("\\bibliographystyle"))) {
                size_t p = i + strlen("\\bibliographystyle");
                if (arg_at(s, n, &p, &arg)) {
                    kind = 4;
                    end = p;
                }
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
            char *rel = resolve_tex_asset(source, arg);
            if (!rel) {
                die("missing graphic %s from %s", arg, source);
            } else {
                add(&b, rel);
                free(rel);
            }
        } else if (kind == 3) {
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
                    missing_bibliography(source, s, i, v, path);
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
        } else if (kind == 5) {
            write_ref_keys(&b, arg);
        } else if (kind == 6) {
            char *label = xstrdup(arg);
            Buf filename = {0};
            add(&filename, trim(label));
            if (!ends(filename.s, ".cls")) add(&filename, ".cls");
            char *rel = resolve_tex_asset(source, filename.s);
            if (rel) {
                if (!ends(arg, ".cls")) rel[strlen(rel) - 4] = 0;
                add(&b, rel);
            } else {
                add(&b, arg); /* A class provided by the TeX installation. */
            }
            free(rel);
            free(filename.s);
            free(label);
        } else {
            char *name = xstrdup(arg);
            char *trimmed = trim(name);
            Buf bst = {0};
            add(&bst, trimmed);
            if (!ends(bst.s, ".bst")) {
                add(&bst, ".bst");
            }
            char *path = path_from(source, bst.s), *rel = project_rel(path);
            if (rel) {
                if (ends(rel, ".bst")) {
                    rel[strlen(rel) - 4] = 0;
                }
                add(&b, rel);
                free(rel);
            } else {
                add(&b, arg);
            }
            free(path);
            free(bst.s);
            free(name);
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
    add(b, "\\makeatletter\n"
           "\\theoremstyle{plain}\n"
           "\\@ifundefined{theorem}{\\newtheorem{theorem}{Theorem}}{}\n"
           "\\@ifundefined{lemma}{\\newtheorem{lemma}{Lemma}}{}\n"
           "\\@ifundefined{corollary}{\\newtheorem{corollary}{Corollary}}{}\n"
           "\\theoremstyle{definition}\n"
           "\\@ifundefined{definition}{\\newtheorem{definition}{Definition}}{}\n"
           "\\makeatother\n");
}

static const char *document_command(const char *source, const char *command) {
    size_t n = strlen(source), len = strlen(command);
    for (size_t i = 0; i < n;) {
        if (source[i] == '%' && (i == 0 || source[i - 1] != '\\')) {
            while (i < n && source[i] != '\n') i++;
            continue;
        }
        if (i + len < n && !memcmp(source + i, command, len)) {
            size_t next = i + len;
            char *name = NULL;
            if (arg_at(source, n, &next, &name)) {
                int match = !strcmp(name, "document");
                free(name);
                if (match) return source + i;
            }
        }
        i++;
    }
    return NULL;
}

static char *insert_appendices(const char *source, const char *appendices) {
    const char *end = document_command(source, "\\end");
    if (!end) {
        die("TeX root has no \\end{document} for appendices");
        return xstrdup(source);
    }
    Buf result = {0};
    addn(&result, source, end - source);
    add(&result, "\n");
    add(&result, appendices);
    add(&result, end);
    return result.s;
}

static int project_uses_cref(void) {
    for (int i = 0; i < nfiles; i++) {
        if (!files[i].for_tex) continue;
        const char *text = files[i].lean ? files[i].tex : files[i].data;
        const char *appendix = files[i].appendix_tex;
        if ((text && (strstr(text, "\\cref{") || strstr(text, "\\Cref{"))) ||
            (appendix && (strstr(appendix, "\\cref{") || strstr(appendix, "\\Cref{")))) {
            return 1;
        }
    }
    return 0;
}

static void warn_tex_captured_refs(void) {
    for (int i = 0; i < nsegs; i++) {
        if (segs[i].kind != SEG_BOX_OPEN || !segs[i].capture) continue;
        int code = i + 1;
        while (code < nsegs && segs[code].kind != SEG_CODE &&
               segs[code].kind != SEG_BOX_CLOSE) code++;
        if (code == nsegs || segs[code].kind != SEG_CODE) continue;
        const char *source = segs[code].s;
        size_t n = strlen(source);
        for (size_t line = 0; line < n;) {
            size_t end = line;
            while (end < n && source[end] != '\n') end++;
            char *name = lean_syntactic_name(source + line, end - line);
            if (name) {
                Buf ref = {0}, cref = {0};
                add(&ref, "\\ref{"); add(&ref, name); add(&ref, "}");
                add(&cref, "\\cref{"); add(&cref, name); add(&cref, "}");
                for (int f = 0; f < nfiles; f++) {
                    const char *text = files[f].lean ? files[f].tex : files[f].data;
                    if (text && (strstr(text, ref.s) || strstr(text, cref.s))) {
                        fprintf(stderr, "myne: warning: reference %s uses a declaration inside a captured block; use @label for a stable name\n",
                                name);
                        break;
                    }
                }
                free(ref.s);
                free(cref.s);
            }
            free(name);
            line = end < n ? end + 1 : end;
        }
    }
}

/* The copied TeX root keeps its own document class, front matter, and imports. */
static char *add_tex_setup(const char *source, int has_boxes, int has_cref) {
    int add_cref = has_cref && !strstr(source, "{cleveref}");
    if (!has_boxes && !add_cref) return xstrdup(source);
    const char *begin = document_command(source, "\\begin");
    if (!begin) {
        die("TeX root has no \\begin{document} for Myne setup");
        return xstrdup(source);
    }
    Buf result = {0};
    addn(&result, source, begin - source);
    add(&result, "\n");
    if (has_boxes) add(&result, "\\usepackage{amsthm}\n");
    if (add_cref) add(&result, "\\usepackage{cleveref}\n");
    if (has_boxes) theorem_setup(&result);
    add(&result, begin);
    return result.s;
}

void write_tex(const char *dir) {
    mkdirs(dir);
    if (!strcmp(format, "tex")) warn_tex_captured_refs();
    int has_boxes = 0;
    int has_cref = project_uses_cref();
    Buf appendices = {0};
    for (int i = 0; i < nfiles; i++) {
        if (!files[i].for_tex) continue;
        has_boxes |= files[i].has_boxes;
        if (files[i].appendix_tex && *files[i].appendix_tex) {
            if (!appendices.n) {
                add(&appendices, "\\appendix\n");
            }
            char *name = appendix_name(files[i].rel);
            add(&appendices, "\\input{");
            add(&appendices, name);
            add(&appendices, "}\n");
            free(name);
        }
    }
    for (int i = 0; i < nfiles; i++) {
        File *f = &files[i];
        if (!f->for_tex) continue;
        int final_root = i == tex_root_index;
        char *rel = tex_name(f->rel), *path = join(dir, rel);
        Buf b = {0};
        if (final_root && f->lean) {
            add(&b, "\\documentclass{article}\n\\usepackage{amsmath,amssymb,natbib}\n");
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
        } else {
            add(&b, f->lean ? f->tex : f->data);
        }
        char *body = rewrite_refs(b.s, f->rel);
        if (final_root) {
            char *with_setup = add_tex_setup(body, has_boxes, has_cref);
            free(body);
            body = with_setup;
        }
        if (final_root && appendices.n) {
            char *complete = insert_appendices(body, appendices.s);
            free(body);
            body = complete;
        }
        if (write_all(path, body, strlen(body))) {
            die("cannot write %s", path);
        }
        if (f->appendix_tex && *f->appendix_tex) {
            char *name = appendix_name(f->rel), *appendix_path = join(dir, name);
            char *appendix_body = rewrite_refs(f->appendix_tex, f->rel);
            if (write_all(appendix_path, appendix_body, strlen(appendix_body))) {
                die("cannot write %s", appendix_path);
            }
            free(appendix_body);
            free(appendix_path);
            free(name);
        }
        free(body);
        free(b.s);
        free(path);
        free(rel);
    }
    free(appendices.s);
}


char *document_pdf_name(void) {
    char *pdf = tex_name(files[tex_root_index].rel);
    if (ends(pdf, ".tex")) {
        strcpy(pdf + strlen(pdf) - 4, ".pdf");
    }
    return pdf;
}

