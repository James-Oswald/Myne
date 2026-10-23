#ifndef MYNE_INTERNAL_H
#define MYNE_INTERNAL_H

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

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifdef _WIN32
void normalize_path(char *path);
int is_reparse_point(const char *path);
char *myne_realpath(const char *path, char *resolved);
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
    int appendix_group, appendix;
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


extern File files[MAX_FILES];
extern int nfiles;
extern Segment segs[MAX_SEGS], appendix_segs[MAX_SEGS];
extern int nsegs, nappendix_segs, appendix_capture;
extern Page pages[MAX_PAGES];
extern int npages, current, part, bib_page;
extern DependencyNode dependency_nodes[MAX_SEGS];
extern int ndependency_nodes;
extern LeanDependencyDecl *lean_dependency_decls;
extern int nlean_dependency_decls, lean_dependency_cap;
extern unsigned char *elaborated_edges;
extern int elaborated_edge_width, elaborated_dependency_graph;
extern Bib bibs[MAX_BIB];
extern int nbibs, ncites;
extern Mapping maps[64];
extern int nmaps;
extern DeclarationMapping declaration_maps[64];
extern int ndeclaration_maps;
extern char basedir[PATH_MAX], output[PATH_MAX];
extern char *root, *format, *style, *tex_template, *tex_class, *page_heading, *bibcfg[64];
extern int nbibcfg;
extern int live, port, failed, root_index, tex_template_index, tex_class_index;
extern int dark_mode_toggle, page_heading_level, web_table_of_contents;
extern int chapter_number, section_number, subsection_number, subsubsection_number;
extern int chapter_active, statement_number;
extern char statement_scope[64];
extern volatile unsigned long epoch;
extern pthread_mutex_t epoch_lock;
extern int nfoot;
extern char *doc_title;

void add(Buf *b, const char *s);
void addn(Buf *b, const char *s, size_t n);
int arg_at(const char *s, size_t n, size_t *pos, char **arg);
int asset(const char *s);
void ch(Buf *b, char c);
void clear_project(void);
int copy_file(const char *src, const char *dst);
void dependency_collect(void);
int dependency_edge(int from, int to);
void dependency_elaborate(void);
void dependency_prune(void);
void die(const char *fmt, ...);
char *document_pdf_name(void);
int ends(const char *s, const char *e);
void escape(Buf *b, const char *s);
int file_index(const char *rel);
const char *finds(const char *s, size_t n, const char *pat);
unsigned long long fingerprint_dir(const char *rel, int depth);
void flatten(int id, int depth);
int gather(const char *rel);
int graphic_at(const char *s, size_t n, size_t i, size_t *end, char **arg);
const char *heading_name(int type);
int include_at(const char *s, size_t n, size_t i, size_t *end, char **arg);
char *join(const char *a, const char *b);
int keyword(const char *s, size_t n);
void load_bibliographies(void);
void load_config(const char *path);
int mkdirs(const char *path);
char *path_from(const char *from, const char *ref);
void project_assets(const char *rel, const char *html, const char *tex, int depth);
char *project_rel(const char *path);
char *read_all(const char *path);
void remove_tree(const char *path);
char *resolve(const char *from, const char *ref);
char *slice(const char *s, size_t n);
long long stamp(const char *path);
int start_server(void);
int structural(const char *name);
void translate_parallel(void);
char *trim(char *s);
void unpack_katex(const char *dir);
int write_all(const char *path, const char *s, size_t n);
int render_html_document(void);
void write_html(const char *dir);
void write_tex(const char *dir);
void *xmalloc(size_t n);
char *xstrdup(const char *s);
const char *myne_embedded_style(void);
const char *myne_embedded_runtime(void);

#endif
