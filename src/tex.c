#include "myne_internal.h"

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
            char *path = path_from(source, arg), *rel = project_rel(path);
            if (!rel) {
                die("missing graphic %s from %s", arg, source);
            } else {
                add(&b, rel);
                free(rel);
            }
            free(path);
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

void write_tex(const char *dir) {
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


char *document_pdf_name(void) {
    int source = tex_template_index >= 0 ? tex_template_index : root_index;
    char *pdf = tex_name(files[source].rel);
    if (ends(pdf, ".tex")) {
        strcpy(pdf + strlen(pdf) - 4, ".pdf");
    }
    return pdf;
}

