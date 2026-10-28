#include "myne_internal.h"

static void nav_html(Buf *b, int active) {
    int root_heading = 7;
    for (int i = 1; i < npages; i++) {
        if (!pages[i].appendix_group && pages[i].heading >= 2 &&
            pages[i].heading < root_heading) {
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
        int category = pages[i].references || pages[i].appendix_group;
        int depth = (pages[i].references ? page_heading_level : pages[i].heading) -
                    root_heading;
        if (depth < 0) {
            depth = 0;
        } else if (depth > 4) {
            depth = 4;
        }
        char nav_class[100];
        snprintf(nav_class, sizeof(nav_class), "nav-entry nav-depth-%d heading-%s%s%s ", depth,
                 heading_name(pages[i].heading),
                 pages[i].appendix_group ? " appendix-group" : "",
                 pages[i].references ? " references-group" : "");
        add(b, nav_class);
        if (pages[i].heading == 2 && !category) {
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
    if (!web_table_of_contents) {
        return;
    }
    int root = page == 0;
    int root_heading = 7;
    if (root) {
        for (int j = 1; j < npages; j++) {
            if (!pages[j].appendix && !pages[j].appendix_group &&
                pages[j].heading >= 2 && pages[j].heading < root_heading) {
                root_heading = pages[j].heading;
            }
        }
        if (root_heading == 7) {
            return;
        }
    } else if (pages[page].heading < 2 || pages[page].heading >= page_heading_level) {
        return;
    }
    int child_pages = 0;
    for (int j = page + 1; j < npages; j++) {
        if (!root && (pages[j].heading <= pages[page].heading || pages[j].appendix_group)) {
            break;
        }
        if (root ? (!pages[j].appendix &&
                    (pages[j].heading == root_heading || pages[j].appendix_group))
                 : pages[j].heading == pages[page].heading + 1) {
            child_pages++;
        }
    }
    if (!child_pages && !pages[page].toc_count) {
        return;
    }
    add(b, root ? "<nav class=page-toc aria-label=\"Document contents\"><h2>Contents</h2><ul>"
                : "<nav class=page-toc aria-label=\"On this page\"><h2>Contents</h2><ul>");
    if (pages[page].toc.s) {
        addn(b, pages[page].toc.s, pages[page].toc.n);
    }
    for (int j = page + 1; j < npages; j++) {
        if (!root && (pages[j].heading <= pages[page].heading || pages[j].appendix_group)) {
            break;
        }
        if (root ? (pages[j].appendix ||
                    (pages[j].heading != root_heading && !pages[j].appendix_group))
                 : pages[j].heading != pages[page].heading + 1) {
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
               "they build. Only declarations in this Myne document become nodes. Click once "
               "to select a node and keep its dependencies highlighted. Right-click or press "
               "Escape to clear selection; double-click to open its declaration. Drag nodes or "
               "the background to rearrange and pan.</p><div "
               "class=dependency-toolbar role=toolbar aria-label=\"Graph controls\"><button "
               "type=button data-graph-action=zoom-out aria-label=\"Zoom out\">−</button><button "
               "type=button data-graph-action=zoom-in aria-label=\"Zoom in\">+</button><button "
               "type=button data-graph-action=fit>Fit graph</button><button type=button "
               "data-graph-action=reset>Reset layout</button><label class=graph-layout-label>Layout "
               "<select data-graph-action=layout aria-label=\"Graph layout\"><option "
               "value=layered>Layered</option><option value=sections>Section panels</option></select>"
               "</label><button type=button "
               "data-graph-action=direction>Top to bottom</button><span class=graph-legend>"
               "<i class=legend-definition></i> Definitions <i class=legend-lemma></i> Lemmas "
               "<i class=legend-theorem></i> Theorems</span></div><div "
               "class=dependency-scroll><svg class=dependency-graph role=img aria-label=\"Lean "
               "declaration dependency graph\" viewBox=\"0 0 1200 800\"><defs><marker id=\"dep-arrow\" "
               "markerUnits=\"userSpaceOnUse\" viewBox=\"0 0 8 8\" markerWidth=\"10\" markerHeight=\"10\" "
               "refX=\"8\" refY=\"4\" orient=\"auto\"><path "
               "d=\"M0,0 L8,4 L0,8 z\" class=dependency-arrow></path></marker></defs><g "
               "class=\"dependency-edges\">");
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
        add(&html, "\" data-section-id=\"");
        escape(&html, node->slug ? node->slug : "index.html");
        add(&html, "\" data-section=\"");
        escape(&html, node->page >= 0 && node->page < npages ? pages[node->page].title : "Declarations");
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

void write_html(const char *dir) {
    mkdirs(dir);
    char *dst = join(dir, "katex");
    unpack_katex(dst);
    free(dst);
    char *a = join(dir, "style.css"), *b = join(dir, "runtime.js");
    if (write_all(a, myne_embedded_style(), strlen(myne_embedded_style())) ||
        write_all(b, myne_embedded_runtime(), strlen(myne_embedded_runtime()))) {
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
        if (i == 0 && pages[0].byline.n) {
            addn(&html, pages[0].byline.s, pages[0].byline.n);
        }
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
        if (pages[i].appendix_group) {
            add(&html, "<ul class=part-list>");
            for (int j = i + 1; j < npages; j++) {
                if (!pages[j].appendix) {
                    continue;
                }
                add(&html, "<li><a href=\"");
                escape(&html, pages[j].slug);
                add(&html, "\">");
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

