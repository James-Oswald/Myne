#include "myne_internal.h"

static char config_path[PATH_MAX];

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
        if (render_html_document()) return -1;
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

static void usage(void) {
    puts("Usage: myne [-i root|project-dir] [-f html|tex|both] [-o outputDir] [-s style] [--config "
         "file] "
         "[--elaborated-deps] [--trace-library-deps] [--watch] [--serve] [--port N]");
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
        } else if (!strcmp(a, "--trace-library-deps")) {
            trace_library_dependencies = 1;
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
