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
            b->editor = field(entry, "editor");
            b->venue = field(entry, "venue");
            if (!*b->venue) {
                free(b->venue);
                b->venue = field(entry, "journal");
            }
            if (!*b->venue) {
                free(b->venue);
                b->venue = field(entry, "booktitle");
            }
            if (!*b->venue) {
                free(b->venue);
                b->venue = field(entry, "publisher");
            }
            b->year = field(entry, "year");
            b->url = field(entry, "url");
            b->doi = field(entry, "doi");
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

/* Escape HTML while translating LaTeX's text quotation marks. */
void escape_latex_text(Buf *b, const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        if (i + 1 < n && s[i] == '`' && s[i + 1] == '`') {
            add(b, "&ldquo;");
            i += 2;
        } else if (i + 1 < n && s[i] == '\'' && s[i + 1] == '\'') {
            add(b, "&rdquo;");
            i += 2;
        } else {
            escape_n(b, s + i, 1);
            i++;
        }
    }
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

/* BibTeX grouping braces protect capitalization; they are not printed. Keep math
   delimiters intact for the bundled KaTeX auto-renderer. */
static void bib_text_html(Buf *b, const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        if (s[i] == '$') {
            size_t start = i, delim = i + 1 < n && s[i + 1] == '$' ? 2 : 1;
            const char *end = finds(s + i + delim, n - i - delim, delim == 2 ? "$$" : "$");
            if (end) {
                i = (size_t)(end - s) + delim;
                escape_n(b, s + start, i - start);
                continue;
            }
        }
        if (s[i] == '\\' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '[')) {
            const char *end = finds(s + i + 2, n - i - 2, s[i + 1] == '(' ? "\\)" : "\\]");
            if (end) {
                size_t start = i;
                i = (size_t)(end - s) + 2;
                escape_n(b, s + start, i - start);
                continue;
            }
        }
        if (s[i] == '{' || s[i] == '}') {
            i++;
            continue;
        }
        if (i + 1 < n && s[i] == '`' && s[i + 1] == '`') {
            add(b, "&ldquo;");
            i += 2;
            continue;
        }
        if (i + 1 < n && s[i] == '\'' && s[i + 1] == '\'') {
            add(b, "&rdquo;");
            i += 2;
            continue;
        }
        if (s[i] == '\\' && i + 1 < n) {
            char command = s[i + 1];
            const char *accent = command == '~' ? "\xcc\x83" : command == '\'' ? "\xcc\x81"
                                 : command == '`' ? "\xcc\x80" : command == '^' ? "\xcc\x82"
                                 : command == '"' ? "\xcc\x88" : NULL;
            if (accent) {
                i += 2;
                while (i < n && s[i] == '{') {
                    i++;
                }
                if (i < n && s[i] != '}') {
                    escape_n(b, s + i, 1);
                    i++;
                    add(b, accent);
                }
                continue;
            }
            if (strchr("&%_#{}$", command)) {
                escape_n(b, s + i + 1, 1);
                i += 2;
                continue;
            }
            if (!strncmp(s + i, "\\LaTeX", 6)) {
                add(b, "LaTeX");
                i += 6;
                continue;
            }
            if (!strncmp(s + i, "\\TeX", 4)) {
                add(b, "TeX");
                i += 4;
                continue;
            }
        }
        escape_n(b, s + i, 1);
        i++;
    }
}

static void bib_title_html(Buf *b, const Bib *entry) {
    const char *url = NULL;
    Buf doi_url = {0};
    if (!strncmp(entry->url, "https://", 8) || !strncmp(entry->url, "http://", 7)) {
        url = entry->url;
    } else if (*entry->doi) {
        const char *doi = entry->doi;
        if (!strncasecmp(doi, "https://doi.org/", 16)) {
            doi += 16;
        } else if (!strncasecmp(doi, "http://doi.org/", 15)) {
            doi += 15;
        } else if (!strncasecmp(doi, "doi:", 4)) {
            doi += 4;
        }
        if (!strpbrk(doi, " \t\r\n\"<>")) {
            add(&doi_url, "https://doi.org/");
            add(&doi_url, doi);
            url = doi_url.s;
        }
    }
    add(b, "<i>");
    if (url) {
        add(b, "<a href=\"");
        escape(b, url);
        add(b, "\">");
    }
    bib_text_html(b, entry->title);
    if (url) {
        add(b, "</a>");
    }
    add(b, "</i>");
    free(doi_url.s);
}

static void bib_reference_html(Buf *b, const Bib *entry, const char *separator) {
    bib_text_html(b, entry->author);
    add(b, separator);
    bib_title_html(b, entry);
    if (web_reference_editors && *entry->editor) {
        add(b, ". Edited by ");
        bib_text_html(b, entry->editor);
    }
    if (web_reference_publication_venue && *entry->venue) {
        add(b, ". ");
        bib_text_html(b, entry->venue);
    }
    add(b, " (");
    bib_text_html(b, entry->year);
    add(b, ")");
}

static void citation_number(Buf *b, const Bib *entry) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%d", entry->number);
    add(b, tmp);
}

static void cite_group_html(Buf *b, char *keys, int textual) {
    Bib *entries[MAX_BIB];
    int count = 0;
    for (char *key = strtok(keys, ","); key; key = strtok(NULL, ",")) {
        key = trim(key);
        Bib *entry = bib_lookup(key);
        if (!entry) {
            die("unknown citation %s", key);
            continue;
        }
        if (!entry->number) {
            entry->number = ++ncites;
        }
        if (count >= MAX_BIB) {
            die("too many citations in one group");
            break;
        }
        entries[count++] = entry;
    }
    if (!count) {
        return;
    }
    if (textual) {
        for (int i = 0; i < count; i++) {
            if (i) add(b, "; ");
            bib_text_html(b, entries[i]->author);
            add(b, " (");
            bib_text_html(b, entries[i]->year);
            add(b, ")");
        }
        ch(b, ' ');
    }
    char anchor[32];
    snprintf(anchor, sizeof(anchor), "cite-%d", ++pages[current].citation_count);
    add(b, "<span class=citation id=\"");
    add(b, anchor);
    add(b, "\">[");
    for (int i = 0; i < count; i++) {
        if (i) add(b, ",");
        add(b, "<a href=\"#ref-");
        escape(b, entries[i]->key);
        add(b, "\">");
        citation_number(b, entries[i]);
        add(b, "</a>");
    }
    add(b, "]</span>");
    for (int i = 0; i < count; i++) {
        char note_id[64];
        snprintf(note_id, sizeof(note_id), "%s-note-%d", anchor, i + 1);
        Buf note = {0};
        add(&note, "<span class=reference-number>[");
        citation_number(&note, entries[i]);
        add(&note, "]</span> ");
        bib_reference_html(&note, entries[i], ", ");
        add(b, "<span class=\"sidenote reference-sidenote\" data-ref-number=\"");
        citation_number(b, entries[i]);
        add(b, "\" data-cite-anchor=\"");
        add(b, anchor);
        add(b, "\" data-note-id=\"");
        add(b, note_id);
        add(b, "\">");
        add(b, note.s);
        add(b, "</span>");
        add(&pages[current].notes, "<li class=reference-note data-note-id=\"");
        add(&pages[current].notes, note_id);
        add(&pages[current].notes, "\">");
        add(&pages[current].notes, note.s);
        add(&pages[current].notes, "</li>");
        free(note.s);
    }
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
                if (i + 1 < n && s[i + 1] == '\\') {
                    add(b, "<br>");
                    i += 2;
                    continue;
                }
                if (i + 1 < n && strchr("%&_#{}$", s[i + 1])) {
                    escape_n(b, s + i + 1, 1);
                    i += 2;
                    continue;
                }
                i++;
                continue;
            }
            char *cmd = slice(s + i + 1, name - i - 1);
            if (!strcmp(cmd, "and")) {
                add(b, " and ");
                i = name;
                while (i < n && isspace((unsigned char)s[i])) {
                    i++;
                }
                free(cmd);
                continue;
            }
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
                cite_group_html(b, arg, !strcmp(cmd, "citet"));
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
            } else if (has && !strcmp(cmd, "begin") && !strcmp(arg, "abstract")) {
                const char *end = finds(s + p, n - p, "\\end{abstract}");
                if (!end) {
                    die("unclosed abstract environment");
                } else {
                    char *contents = slice(s + p, end - (s + p));
                    char *trimmed = trim(contents);
                    add(&pages[0].body, "<section class=abstract><h2>Abstract</h2><p>");
                    int previous_page = current;
                    current = 0;
                    inline_html(&pages[0].body, trimmed, strlen(trimmed), depth + 1);
                    current = previous_page;
                    add(&pages[0].body, "</p></section>");
                    free(contents);
                    p = (size_t)(end - s) + strlen("\\end{abstract}");
                    while (p < n && isspace((unsigned char)s[p])) {
                        p++;
                    }
                }
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
            if (!b->n) {
                i = j;
                continue;
            }
            if (j - i > 1 && j < n) {
                add(b, "<br><br>");
            } else {
                ch(b, ' ');
            }
            i = j;
            continue;
        }
        if (i + 1 < n && s[i] == '`' && s[i + 1] == '`') {
            add(b, "&ldquo;");
            i += 2;
            continue;
        }
        if (i + 1 < n && s[i] == '\'' && s[i + 1] == '\'') {
            add(b, "&rdquo;");
            i += 2;
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
    if (current == 0 && ends(files[root_index].rel, ".tex")) {
        const char *begin = strstr(s, "\\begin{document}");
        if (begin) {
            const char *scan = s;
            while (scan < begin) {
                if (*scan == '%' && (scan == s || scan[-1] != '\\')) {
                    while (scan < begin && *scan != '\n') {
                        scan++;
                    }
                    continue;
                }
                if (*scan == '\\') {
                    const char *name = scan + 1;
                    while (name < begin && isalpha((unsigned char)*name)) {
                        name++;
                    }
                    size_t len = name - (scan + 1);
                    if ((len == 5 && !strncmp(scan + 1, "title", 5)) ||
                        (len == 6 && !strncmp(scan + 1, "author", 6))) {
                        size_t p = name - s;
                        char *arg = NULL;
                        if (arg_at(s, begin - s, &p, &arg)) {
                            char **target = len == 5 ? &doc_title : &doc_author;
                            free(*target);
                            *target = arg;
                            scan = s + p;
                            continue;
                        }
                    }
                    scan = name;
                    continue;
                }
                scan++;
            }
            s = begin + strlen("\\begin{document}");
            while (isspace((unsigned char)*s)) {
                s++;
            }
        }
    }
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
            if (!strcmp(cmd, "author")) {
                size_t p = k;
                char *arg = NULL;
                if (arg_at(s, n, &p, &arg)) {
                    inline_html(&pages[current].body, s + start, i - start, 0);
                    free(doc_author);
                    doc_author = arg;
                    i = p;
                    start = i;
                    free(cmd);
                    continue;
                }
            }
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
                                escape_latex_text(&pages[current].toc, arg);
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

static int references_page(void) {
    int existing = bib_page >= 0 ? bib_page : npages - 1;
    if (npages == 1) {
        return existing;
    }
    int top_heading = page_heading_level;
    for (int i = 1; i < npages; i++) {
        if (!pages[i].appendix && !pages[i].appendix_group &&
            pages[i].heading < top_heading) {
            top_heading = pages[i].heading;
        }
    }
    if (existing > 0 && pages[existing].heading == top_heading &&
        (!strcasecmp(pages[existing].title, "References") ||
         !strcasecmp(pages[existing].title, "Bibliography"))) {
        pages[existing].references = 1;
        return existing;
    }
    if (npages >= MAX_PAGES) {
        die("too many pages");
        return -1;
    }
    int index = npages;
    for (int i = 1; i < npages; i++) {
        if (pages[i].appendix_group) {
            index = i;
            break;
        }
    }
    if (index < npages) {
        memmove(pages + index + 1, pages + index, (npages - index) * sizeof(Page));
    }
    memset(&pages[index], 0, sizeof(Page));
    npages++;
    Page *reference = &pages[index];
    reference->title = xstrdup("References");
    reference->slug = xstrdup("references.html");
    reference->heading = top_heading;
    reference->part = pages[index - 1].part + 1;
    reference->references = 1;
    for (int i = 0; i < ndependency_nodes; i++) {
        if (dependency_nodes[i].page >= index) {
            dependency_nodes[i].page++;
        }
    }
    if (current >= index) {
        current++;
    }
    if (bib_page >= index) {
        bib_page++;
    }
    return index;
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
    int page = references_page();
    if (page < 0) {
        return;
    }
    for (int i = 0; i < npages; i++) {
        if (i == page || !pages[i].body.s) {
            continue;
        }
        Buf linked = {0};
        const char *cursor = pages[i].body.s, *hit;
        while ((hit = strstr(cursor, "href=\"#ref-"))) {
            addn(&linked, cursor, hit - cursor);
            add(&linked, "href=\"");
            escape(&linked, pages[page].slug);
            add(&linked, "#ref-");
            cursor = hit + strlen("href=\"#ref-");
        }
        add(&linked, cursor);
        free(pages[i].body.s);
        pages[i].body = linked;
    }
    Buf *b = &pages[page].body;
    add(b, npages == 1 ? "<section class=references><h2>References</h2><ol>"
                       : "<section class=\"references standalone\"><ol>");
    for (int num = 1; num <= ncites; num++) {
        for (int i = 0; i < nbibs; i++) {
            if (bibs[i].number == num) {
                add(b, "<li id=\"ref-");
                escape(b, bibs[i].key);
                add(b, "\">");
                bib_reference_html(b, &bibs[i], ". ");
                add(b, "</li>");
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
                    escape_latex_text(&pages[current].body, title);
                    add(&pages[current].body, "</span>");
                }
                free(number.s);
            } else {
                escape_latex_text(&pages[current].body, title);
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
    free(pages[0].title);
    pages[0].title = xstrdup(doc_title);
    if (doc_author && *doc_author) {
        int previous_page = current;
        current = 0;
        add(&pages[0].byline, "<p class=authors>");
        char *trimmed = trim(doc_author);
        inline_html(&pages[0].byline, trimmed, strlen(trimmed), 0);
        add(&pages[0].byline, "</p>");
        current = previous_page;
    }
    append_references();
    return failed ? -1 : 0;
}
