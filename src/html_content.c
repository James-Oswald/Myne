#include "myne_internal.h"

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
void load_bibliographies(void) {
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

void escape(Buf *b, const char *s) {
    escape_n(b, s, strlen(s));
}

static char *slug(int id, const char *kind) {
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s-%03d.html", kind, id);
    return xstrdup(tmp);
}

const char *heading_name(int type) {
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

int structural(const char *name) {
    const char *names[] = {"title",      "part",          "chapter",  "section",
                           "subsection", "subsubsection", "paragraph"};
    for (int i = 0; i < 7; i++) {
        if (!strcmp(name, names[i])) {
            return i + 1;
        }
    }
    return 0;
}


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

int render_html_document(void) {
    new_page("Introduction", 0, NULL);
    int appendix_group_created = 0;
    int appendix_number = 0;
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
            if (!appendix_group_created) {
                new_page("Appendices", page_heading_level, NULL);
                pages[current].appendix_group = 1;
                appendix_group_created = 1;
            }
            Buf title = {0}, number = {0};
            char label[32];
            appendix_number++;
            int value = appendix_number;
            char reversed[32];
            int digits = 0;
            while (value > 0 && digits < (int)sizeof(reversed)) {
                value--;
                reversed[digits++] = (char)('A' + value % 26);
                value /= 26;
            }
            int pos = 0;
            while (digits > 0 && pos < (int)sizeof(label) - 1) {
                label[pos++] = reversed[--digits];
            }
            label[pos] = 0;
            add(&title, "Appendix: ");
            add(&title, segs[i].s);
            add(&number, label);
            set_statement_scope(label);
            new_page(title.s, page_heading_level, number.s);
            pages[current].appendix = 1;
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
    return 0;
}
