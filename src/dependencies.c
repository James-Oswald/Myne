#include "myne_internal.h"

static DependencyNode *dependency_add_node(const char *name, int external) {
    if (ndependency_nodes == dependency_node_cap) {
        dependency_node_cap = dependency_node_cap ? dependency_node_cap * 2 : 128;
        dependency_nodes = realloc(dependency_nodes,
                                   (size_t)dependency_node_cap * sizeof(*dependency_nodes));
        if (!dependency_nodes) exit(2);
    }
    DependencyNode *node = &dependency_nodes[ndependency_nodes];
    memset(node, 0, sizeof(*node));
    node->name = xstrdup(name);
    node->graph_id = ndependency_nodes++;
    node->external = external;
    node->file = node->page = node->segment = -1;
    node->kind = LEAN_DEF;
    return node;
}

static void dependency_add_edge(int from, int to) {
    if (from == to) return;
    if (!elaborated_dependency_graph) {
        for (int i = 0; i < ndependency_edges; i++) {
            if (dependency_edges[i].from == from && dependency_edges[i].to == to) return;
        }
    }
    if (ndependency_edges == dependency_edge_cap) {
        dependency_edge_cap = dependency_edge_cap ? dependency_edge_cap * 2 : 128;
        dependency_edges = realloc(dependency_edges,
                                   (size_t)dependency_edge_cap * sizeof(*dependency_edges));
        if (!dependency_edges) exit(2);
    }
    dependency_edges[ndependency_edges++] = (DependencyEdge){from, to};
}

static int dependency_graph_index(const char *name) {
    for (int i = 0; i < ndependency_nodes; i++) {
        const char *known = dependency_nodes[i].resolved_name ?
                            dependency_nodes[i].resolved_name : dependency_nodes[i].name;
        if (!strcmp(known, name)) return i;
    }
    return -1;
}

static void dependency_trace_syntax(void);

static void dependency_add_lean_string(Buf *out, const char *value) {
    ch(out, '"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') ch(out, '\\');
        ch(out, (char)*p);
    }
    ch(out, '"');
}

int keyword(const char *s, size_t n) {
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
            char *declaration_name = slice(source + name, name_len);
            DependencyNode *node = dependency_add_node(declaration_name, 0);
            free(declaration_name);
            current_node = node->graph_id;
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

void dependency_collect(void) {
    for (int i = 0; i < nsegs; i++) {
        if (segs[i].kind == SEG_CODE) {
            dependency_collect_code(segs[i].s, i);
        }
    }
    if (trace_library_dependencies && !elaborated_dependency_graph)
        dependency_trace_syntax();
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
                 "  let env ← getEnv\n");
    if (trace_library_dependencies) {
        add(&source,
            "  let roots : Array String := #[");
        int first = 1;
        for (int i = 0; i < ndependency_nodes; i++) {
            if (dependency_nodes[i].file != file || dependency_nodes[i].external) continue;
            if (!first) add(&source, ", ");
            dependency_add_lean_string(&source, dependency_nodes[i].name);
            first = 0;
        }
        add(&source,
            "]\n"
            "  let mut todo : Array Name := #[]\n"
            "  for (name, _) in env.constants do\n"
            "    if !env.isImportedConst name then\n"
            "      let full := name.toString\n"
            "      if roots.any (fun root => full == root || full.endsWith (\".\" ++ root)) then\n"
            "        todo := todo.push name\n"
            "  let mut seen : Std.HashSet Name := {}\n"
            "  while !todo.isEmpty do\n"
            "    let name := todo.back!\n"
            "    todo := todo.pop\n"
            "    if seen.contains name then continue\n"
            "    seen := seen.insert name\n"
            "    let some ci := env.find? name | continue\n"
            "    IO.println s!\"MYNE_DECL\\t{name}\"\n"
            "    for dep in ci.getUsedConstantsAsSet do\n"
            "      IO.println s!\"MYNE_EDGE\\t{name}\\t{dep}\"\n"
            "      if !seen.contains dep then todo := todo.push dep\n");
    } else {
        add(&source,
            "  for (name, ci) in env.constants do\n"
            "    if !env.isImportedConst name then\n"
            "      IO.println s!\"MYNE_DECL\\t{name}\"\n"
            "      for dep in ci.getUsedConstantsAsSet do\n"
            "        IO.println s!\"MYNE_EDGE\\t{name}\\t{dep}\"\n");
    }
    add(&source, "#myne_dependencies\n");
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

static void dependency_trace_libraries(void) {
    /* Newly discovered nodes are processed by the same loop until every reachable
       declaration has been visited. Missing environment entries remain named leaves. */
    for (int i = 0; i < ndependency_nodes; i++) {
        DependencyNode *node = &dependency_nodes[i];
        const char *name = node->resolved_name ? node->resolved_name : node->name;
        int declaration = dependency_decl_index(name, node->file);
        if (declaration < 0) continue;
        LeanDependencyDecl *decl = &lean_dependency_decls[declaration];
        for (int j = 0; j < decl->ndeps; j++) {
            const char *used = decl->deps[j];
            int prerequisite = dependency_graph_index(used);
            if (prerequisite < 0) {
                prerequisite = dependency_add_node(used, 1)->graph_id;
            }
            dependency_add_edge(prerequisite, i);
        }
    }
}

void dependency_elaborate(void) {
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
    if (trace_library_dependencies) {
        dependency_trace_libraries();
        return;
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

int dependency_edge(int from, int to) {
    if (from == to) return 0;
    if (trace_library_dependencies) {
        for (int i = 0; i < ndependency_edges; i++) {
            if (dependency_edges[i].from == to && dependency_edges[i].to == from) return 1;
        }
        return 0;
    }
    if (elaborated_dependency_graph)
        return elaborated_edges[(size_t)dependency_nodes[from].graph_id * elaborated_edge_width +
                                dependency_nodes[to].graph_id];
    return dependency_uses(dependency_nodes[from].source, dependency_nodes[to].name);
}

void dependency_prune(void) {
    if (trace_library_dependencies) return;
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

static int dependency_syntax_keyword(const char *name) {
    const char *extra[] = {"abbrev", "opaque", "class", "instance", "axiom", "Prop", "Type",
                           "Sort", "exact", "apply", "simp_all", "rw", "rfl", "intro",
                           "intros", "constructor", "trivial", "decide", "assumption",
                           "cases", "case", "induction", "calc", "show", "from", "at",
                           "unfold", "change", "refine", "repeat", "first", "all_goals",
                           "any_goals", "done", "sorry", "admit", "return", "do", "in",
                           "variable", "variables", "section", "end", "set_option"};
    if (keyword(name, strlen(name))) return 1;
    for (size_t i = 0; i < sizeof(extra) / sizeof(*extra); i++) {
        if (!strcmp(name, extra[i])) return 1;
    }
    return 0;
}

static void dependency_trace_syntax(void) {
    int documented = ndependency_nodes;
    for (int i = 0; i < documented; i++) {
        const char *source = dependency_nodes[i].source;
        if (!source) continue;
        for (int j = 0; j < documented; j++) {
            if (i != j && dependency_uses(source, dependency_nodes[j].name))
                dependency_add_edge(j, i);
        }
        const char *body = strstr(source, ":=");
        size_t n = strlen(source), comment_depth = 0;
        int string = 0;
        for (size_t p = 0; p < n;) {
            if (comment_depth) {
                if (p + 1 < n && source[p] == '/' && source[p + 1] == '-') {
                    comment_depth++; p += 2;
                } else if (p + 1 < n && source[p] == '-' && source[p + 1] == '/') {
                    comment_depth--; p += 2;
                } else p++;
                continue;
            }
            if (string) {
                if (source[p] == '\\' && p + 1 < n) p += 2;
                else if (source[p++] == '"') string = 0;
                continue;
            }
            if (p + 1 < n && source[p] == '-' && source[p + 1] == '-') {
                while (p < n && source[p] != '\n') p++;
                continue;
            }
            if (p + 1 < n && source[p] == '/' && source[p + 1] == '-') {
                comment_depth = 1; p += 2; continue;
            }
            if (source[p] == '"') { string = 1; p++; continue; }
            if (!(isalpha((unsigned char)source[p]) || (unsigned char)source[p] >= 0x80)) {
                p++; continue;
            }
            size_t start = p++;
            while (p < n && dependency_name_char((unsigned char)source[p])) p++;
            char *name = slice(source + start, p - start);
            int candidate = strlen(name) > 1 && !dependency_syntax_keyword(name) &&
                            (strchr(name, '.') || isupper((unsigned char)name[0]) ||
                             (body && source + start > body + 1));
            if (candidate && dependency_graph_index(name) < 0) {
                int external = dependency_add_node(name, 1)->graph_id;
                dependency_add_edge(external, i);
            } else if (candidate) {
                int known = dependency_graph_index(name);
                if (known >= documented) dependency_add_edge(known, i);
            }
            free(name);
        }
    }
}

