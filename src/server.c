#include "myne_internal.h"

unsigned long long fingerprint_dir(const char *rel, int depth) {
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

int start_server(void) {
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

