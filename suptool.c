#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Some MiNT headers may lack popen prototypes */
extern FILE *popen(const char *command, const char *type);
extern int pclose(FILE *stream);

#define LISTEN_PORT 80
#define LISTEN_BACKLOG 4
#define RECV_BUF_SIZE 16384
#define SERVER_NAME "mint-http-fm"

struct entry {
    char *name;
    int is_dir;
    long size;
};

static char *str_dup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = (char *)malloc(len);
    if (p) {
        memcpy(p, s, len);
    }
    return p;
}

static int entry_cmp(const void *a, const void *b) {
    const struct entry *ea = (const struct entry *)a;
    const struct entry *eb = (const struct entry *)b;
    int r = strcasecmp(ea->name, eb->name);
    if (r != 0) {
        return r;
    }
    /* If names equal, directories first */
    return eb->is_dir - ea->is_dir;
}

static int normalize_path(const char *url_path, char *out, size_t out_sz) {
    if (out == NULL || out_sz < 2) {
        return -1;
    }
    size_t pos = 0;
    out[pos++] = '.';
    out[pos] = '\0';

    if (url_path == NULL || url_path[0] == '\0' || (url_path[0] == '/' && url_path[1] == '\0')) {
        return 0;
    }

    const char *p = url_path;
    while (*p == '/') {
        p++;
    }

    while (*p) {
        const char *seg_start = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - seg_start);
        while (*p == '/') {
            p++;
        }
        if (seg_len == 0) {
            continue;
        }
        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
            return -1; /* reject traversal */
        }
        if (pos + 1 + seg_len >= out_sz) {
            return -1;
        }
        out[pos++] = '/';
        memcpy(out + pos, seg_start, seg_len);
        pos += seg_len;
        out[pos] = '\0';
    }
    return 0;
}

static int send_all(int fd, const void *data, size_t len) {
    const char *p = (const char *)data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_simple_response(int client_fd,
                                 int status,
                                 const char *reason,
                                 const char *content_type,
                                 const char *body) {
    char header[256];
    size_t body_len = body ? strlen(body) : 0;
    int header_len = snprintf(header,
                              sizeof(header),
                              "HTTP/1.0 %d %s\r\n"
                              "Server: %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %lu\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              status,
                              reason,
                              SERVER_NAME,
                              content_type ? content_type : "text/plain",
                              (unsigned long)body_len);
    if (header_len > 0) {
        send_all(client_fd, header, (size_t)header_len);
    }
    if (body && body_len > 0) {
        send_all(client_fd, body, body_len);
    }
}

static void send_not_found(int client_fd) {
    send_simple_response(client_fd, 404, "Not Found", "text/plain", "Not Found\n");
}

static void send_bad_request(int client_fd, const char *msg) {
    if (msg == NULL) {
        msg = "Bad Request\n";
    }
    send_simple_response(client_fd, 400, "Bad Request", "text/plain", msg);
}

static void send_method_not_allowed(int client_fd) {
    send_simple_response(client_fd, 405, "Method Not Allowed", "text/plain", "Method Not Allowed\n");
}

static void send_internal_error(int client_fd) {
    send_simple_response(client_fd, 500, "Internal Server Error", "text/plain", "Internal Server Error\n");
}

static void send_header_only(int client_fd,
                             int status,
                             const char *reason,
                             const char *content_type,
                             unsigned long content_length,
                             const char *extra_header) {
    char header[512];
    int header_len = snprintf(header,
                              sizeof(header),
                              "HTTP/1.0 %d %s\r\n"
                              "Server: %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %lu\r\n"
                              "Connection: close\r\n"
                              "%s"
                              "\r\n",
                              status,
                              reason,
                              SERVER_NAME,
                              content_type ? content_type : "application/octet-stream",
                              content_length,
                              extra_header ? extra_header : "");
    if (header_len > 0) {
        send_all(client_fd, header, (size_t)header_len);
    }
}

static void build_child_url(const char *base_url, const char *name, int is_dir, char *out, size_t out_sz) {
    const char *base = (base_url && base_url[0]) ? base_url : "/";
    size_t base_len = strlen(base);
    int ends_with_slash = base_len > 0 && base[base_len - 1] == '/';
    snprintf(out, out_sz, "%s%s%s%s", base, ends_with_slash ? "" : "/", name, is_dir ? "/" : "");
}

static void parent_url(const char *base_url, char *out, size_t out_sz) {
    if (!base_url || strcmp(base_url, "/") == 0) {
        snprintf(out, out_sz, "%s", "/");
        return;
    }
    size_t len = strlen(base_url);
    while (len > 0 && base_url[len - 1] == '/') {
        len--;
    }
    if (len == 0) {
        snprintf(out, out_sz, "%s", "/");
        return;
    }
    size_t i = len;
    while (i > 0 && base_url[i - 1] != '/') {
        i--;
    }
    if (i == 0 || (i == 1 && base_url[0] == '/')) {
        snprintf(out, out_sz, "%s", "/");
        return;
    }
    if (i >= out_sz) {
        i = out_sz - 1;
    }
    memcpy(out, base_url, i);
    out[i] = '\0';
}

static void serve_index(int client_fd, const char *url_path) {
    char line[2048];
    char fs_path[512];
    if (normalize_path(url_path, fs_path, sizeof(fs_path)) != 0) {
        send_bad_request(client_fd, "Invalid path\n");
        return;
    }

    DIR *dir = opendir(fs_path);
    if (dir == NULL) {
        send_internal_error(client_fd);
        return;
    }

    const char *current_url = (url_path && url_path[0]) ? url_path : "/";
    const char *header =
        "HTTP/1.0 200 OK\r\n"
        "Server: " SERVER_NAME "\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n";
    const char *head =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>Falcon File Manager</title>"
        "<style>"
        "body{font-family:monospace;background:#f5f5f5;color:#111;padding:16px;margin:0;}"
        ".layout{display:flex;gap:16px;align-items:flex-start;}"
        ".pane{flex:1;background:#fff;border:1px solid #ddd;border-radius:6px;padding:12px;box-shadow:0 2px 4px rgba(0,0,0,0.06);}"
        "table{border-collapse:collapse;width:100%;}"
        "th,td{border-bottom:1px solid #eee;padding:6px;text-align:left;}"
        "a{color:#004fa3;text-decoration:none;}a:hover{text-decoration:underline;}"
        "#console-log{background:#0b0b0b;color:#00e676;height:320px;overflow:auto;padding:8px;border-radius:4px;white-space:pre-wrap;}"
        "#console-input{width:100%;box-sizing:border-box;padding:6px;margin-top:6px;font-family:monospace;}"
        "</style>"
        "</head><body data-path=\"%s\">"
        "<div class=\"layout\">"
        "<div class=\"pane\">"
        "<h1>Falcon File Manager — %s</h1>"
        "<form id=\"upload-form\"><input type=\"file\" id=\"upload-file\"/>"
        "<button type=\"submit\">Upload</button></form>"
        "<p>Listing directory: <code>%s</code></p>"
        "<table id=\"file-table\"><tr><th>Name</th><th>Size (bytes)</th><th>Actions</th></tr>";
    send_all(client_fd, header, strlen(header));
    char head_buf[2048];
    snprintf(head_buf, sizeof(head_buf), head, current_url, current_url, current_url);
    send_all(client_fd, head_buf, strlen(head_buf));

    char parent[512];
    parent_url(current_url, parent, sizeof(parent));
    char parent_row[512];
    snprintf(parent_row,
             sizeof(parent_row),
             "<tr><td><a href=\"%.400s\">..</a></td><td>-</td><td></td></tr>",
             parent);
    send_all(client_fd, parent_row, strlen(parent_row));

    struct entry *entries = NULL;
    size_t entry_count = 0;
    size_t entry_cap = 0;

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", fs_path, name);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (entry_count == entry_cap) {
            size_t new_cap = entry_cap ? entry_cap * 2 : 32;
            struct entry *tmp = (struct entry *)realloc(entries, new_cap * sizeof(struct entry));
            if (!tmp) {
                continue;
            }
            entries = tmp;
            entry_cap = new_cap;
        }

        entries[entry_count].name = str_dup(name);
        if (!entries[entry_count].name) {
            continue;
        }
        entries[entry_count].is_dir = S_ISDIR(st.st_mode);
        entries[entry_count].size = (long)st.st_size;
        entry_count++;
    }
    closedir(dir);

    if (entry_count > 1) {
        qsort(entries, entry_count, sizeof(struct entry), entry_cmp);
    }

    for (size_t i = 0; i < entry_count; ++i) {
        const char *name = entries[i].name;
        if (entries[i].is_dir) {
            char child_url[1024];
            build_child_url(current_url, name, 1, child_url, sizeof(child_url));
            snprintf(line,
                     sizeof(line),
                     "<tr><td><a href=\"%.700s\">%.200s/</a></td><td>-</td><td></td></tr>",
                     child_url,
                     name);
        } else {
            char child_url[1024];
            build_child_url(current_url, name, 0, child_url, sizeof(child_url));
            snprintf(line,
                     sizeof(line),
                     "<tr><td>%.200s</td><td>%ld</td><td><a href=\"/file%.700s\">download</a> | <a href=\"/delete%.700s\">delete</a></td></tr>",
                     name,
                     entries[i].size,
                     child_url,
                     child_url);
        }
        send_all(client_fd, line, strlen(line));
    }

    for (size_t i = 0; i < entry_count; ++i) {
        free(entries[i].name);
    }
    free(entries);

    const char *footer =
        "</table>"
        "<p>Upload via form above nebo: <code>curl -T file.bin http://&lt;host&gt;/upload/path/file.bin</code></p>"
        "</div>" /* pane */
        "<div class=\"pane\">"
        "<h2>Remote Terminal</h2>"
        "<div id=\"console-log\"></div>"
        "<form id=\"console-form\">"
        "<input id=\"console-input\" type=\"text\" placeholder=\"Command\" autocomplete=\"off\" />"
        "<button type=\"submit\">Run</button>"
        "</form>"
        "</div>" /* pane */
        "</div>" /* layout */ 
        "<script>"
        "const form=document.getElementById('upload-form');"
        "const fileInput=document.getElementById('upload-file');"
        "const currentPath=document.body.dataset.path||'/';"
        "form.addEventListener('submit',async(e)=>{e.preventDefault();const f=fileInput.files[0];if(!f){alert('Vyberte soubor');return;}let base=currentPath.endsWith('/')?currentPath.slice(0,-1):currentPath;if(base===''){base='/';}const target=(base==='/'?'' : base)+'/'+encodeURIComponent(f.name);const res=await fetch('/upload'+target,{method:'PUT',body:f,headers:{'Content-Length':f.size}});if(res.ok){location.reload();}else{alert('Upload selhal: '+res.status);}});"
        "const clog=document.getElementById('console-log');"
        "const cform=document.getElementById('console-form');"
        "const cinput=document.getElementById('console-input');"
        "function appendLog(text){clog.textContent+=text+'\\n';clog.scrollTop=clog.scrollHeight;}"
        "cform.addEventListener('submit',async(e)=>{e.preventDefault();const cmd=cinput.value.trim();if(!cmd){return;}appendLog('> '+cmd);cinput.value='';const body=new TextEncoder().encode(cmd);const res=await fetch('/exec',{method:'POST',body:body,headers:{'Content-Length':body.length}});const txt=await res.text();appendLog(txt);});"
        "document.getElementById('file-table').addEventListener('click',(e)=>{const a=e.target.closest('a');if(!a){return;}const href=a.getAttribute('href');if(!href){return;}e.preventDefault();window.location.href=href;});"
        "</script>"
        "</body></html>";
    send_all(client_fd, footer, strlen(footer));
}

static void serve_file(int client_fd, const char *path) {
    char fs_path[512];
    if (normalize_path(path, fs_path, sizeof(fs_path)) != 0) {
        send_bad_request(client_fd, "Invalid filename\n");
        return;
    }

    int fd = open(fs_path, O_RDONLY);
    if (fd < 0) {
        send_not_found(client_fd);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        send_not_found(client_fd);
        return;
    }

    const char *disp_name = path;
    const char *slash = strrchr(path, '/');
    if (slash && slash[1] != '\0') {
        disp_name = slash + 1;
    }

    char dispo[256];
    int dispo_len = snprintf(dispo,
                             sizeof(dispo),
                             "Content-Disposition: attachment; filename=\"%s\"\r\n",
                             disp_name);
    send_header_only(client_fd, 200, "OK", "application/octet-stream", (unsigned long)st.st_size, dispo_len > 0 ? dispo : "");

    char buffer[4096];
    ssize_t n = 0;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        if (send_all(client_fd, buffer, (size_t)n) != 0) {
            break;
        }
    }
    close(fd);
}

static void handle_delete(int client_fd, const char *name) {
    char fs_path[512];
    if (normalize_path(name, fs_path, sizeof(fs_path)) != 0 || strcmp(fs_path, ".") == 0) {
        send_bad_request(client_fd, "Invalid filename\n");
        return;
    }

    struct stat st;
    if (stat(fs_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_simple_response(client_fd, 404, "Not Found", "text/plain", "File not found or cannot delete\n");
        return;
    }

    if (unlink(fs_path) != 0) {
        send_simple_response(client_fd, 404, "Not Found", "text/plain", "File not found or cannot delete\n");
        return;
    }
    send_simple_response(client_fd, 200, "OK", "text/plain", "Deleted\n");
}

static long parse_content_length(const char *headers, size_t header_len) {
    const char *p = headers;
    const char *end = headers + header_len;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = line_end ? (size_t)(line_end - p + 1) : (size_t)(end - p);
        if (line_len == 0) {
            break;
        }
        if (strncasecmp(p, "Content-Length:", 15) == 0) {
            const char *val = p + 15;
            while (*val == ' ' || *val == '\t') {
                val++;
            }
            return strtol(val, NULL, 10);
        }
        if (!line_end) {
            break;
        }
        p += line_len;
    }
    return -1;
}

static ssize_t read_request(int client_fd, char *buffer, size_t buf_size, size_t *header_len, char **body_start) {
    size_t total = 0;
    int found = 0;
    while (total < buf_size) {
        ssize_t n = recv(client_fd, buffer + total, buf_size - total, 0);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
        for (size_t i = 0; i + 1 < total; ++i) {
            if (i + 3 < total && buffer[i] == '\r' && buffer[i + 1] == '\n' && buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
                *header_len = i;
                *body_start = buffer + i + 4;
                found = 1;
                break;
            }
            if (buffer[i] == '\n' && buffer[i + 1] == '\n') {
                *header_len = i;
                *body_start = buffer + i + 2;
                found = 1;
                break;
            }
        }
        if (found) {
            break;
        }
    }
    if (!found) {
        return -1;
    }
    return (ssize_t)total;
}

static void handle_upload(int client_fd, const char *name, const char *initial_body, size_t initial_len, long content_length) {
    char fs_path[512];
    if (normalize_path(name, fs_path, sizeof(fs_path)) != 0 || strcmp(fs_path, ".") == 0) {
        send_bad_request(client_fd, "Invalid filename\n");
        return;
    }
    if (content_length < 0) {
        send_bad_request(client_fd, "Missing Content-Length\n");
        return;
    }

    int fd = open(fs_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        send_internal_error(client_fd);
        return;
    }

    long written = 0;
    if (initial_len > 0) {
        if (write(fd, initial_body, initial_len) != (ssize_t)initial_len) {
            close(fd);
            send_internal_error(client_fd);
            return;
        }
        written += (long)initial_len;
    }

    char buf[4096];
    while (written < content_length) {
        long remaining = content_length - written;
        size_t to_read = (size_t)((remaining > (long)sizeof(buf)) ? (long)sizeof(buf) : remaining);
        ssize_t n = recv(client_fd, buf, to_read, 0);
        if (n <= 0) {
            close(fd);
            send_internal_error(client_fd);
            return;
        }
        if (write(fd, buf, (size_t)n) != n) {
            close(fd);
            send_internal_error(client_fd);
            return;
        }
        written += (long)n;
    }

    close(fd);
    send_simple_response(client_fd, 201, "Created", "text/plain", "Uploaded\n");
}

static void handle_exec(int client_fd, const char *initial_body, size_t initial_len, long content_length) {
    const long MAX_CMD = 4096;
    if (content_length < 0 || content_length > MAX_CMD) {
        send_bad_request(client_fd, "Content-Length missing or too large\n");
        return;
    }

    char cmd_buf[4097];
    size_t have = initial_len > (size_t)content_length ? (size_t)content_length : initial_len;
    memcpy(cmd_buf, initial_body, have);

    while ((long)have < content_length) {
        ssize_t n = recv(client_fd, cmd_buf + have, (size_t)(content_length - (long)have), 0);
        if (n <= 0) {
            send_internal_error(client_fd);
            return;
        }
        have += (size_t)n;
    }
    cmd_buf[have < (size_t)MAX_CMD ? have : (size_t)MAX_CMD] = '\0';

    FILE *fp = popen(cmd_buf, "r");
    if (!fp) {
        send_internal_error(client_fd);
        return;
    }

    const size_t MAX_OUT = 65536;
    char *out = (char *)malloc(MAX_OUT + 1);
    if (!out) {
        pclose(fp);
        send_internal_error(client_fd);
        return;
    }

    size_t total = 0;
    while (total < MAX_OUT) {
        size_t space = MAX_OUT - total;
        size_t nread = fread(out + total, 1, space, fp);
        if (nread == 0) {
            break;
        }
        total += nread;
    }
    int hit_eof = feof(fp);
    pclose(fp);

    int truncated = (total == MAX_OUT && !hit_eof);
    if (truncated && total + 12 < MAX_OUT + 1) {
        const char *suffix = "\n[truncated]";
        size_t s_len = strlen(suffix);
        memcpy(out + total, suffix, s_len);
        total += s_len;
    }
    send_header_only(client_fd, 200, "OK", "text/plain", (unsigned long)total, "");
    if (total > 0) {
        send_all(client_fd, out, total);
    }
    free(out);
}

static void handle_client(int client_fd) {
    char buffer[RECV_BUF_SIZE];
    size_t header_len = 0;
    char *body_start = NULL;

    ssize_t total = read_request(client_fd, buffer, sizeof(buffer), &header_len, &body_start);
    if (total <= 0) {
        return;
    }

    buffer[header_len] = '\0'; /* terminate headers for parsing */

    char method[8] = {0};
    char path[512] = {0};
    if (sscanf(buffer, "%7s %511s", method, path) != 2) {
        send_bad_request(client_fd, NULL);
        return;
    }

    const char *body = body_start;
    size_t body_len = (size_t)(total - (body_start - buffer));

    if (strcasecmp(method, "GET") == 0) {
        if (strncmp(path, "/file/", 6) == 0) {
            serve_file(client_fd, path + 6);
        } else if (strncmp(path, "/delete/", 8) == 0) {
            handle_delete(client_fd, path + 8);
        } else {
            serve_index(client_fd, path);
        }
    } else if (strcasecmp(method, "PUT") == 0) {
        if (strncmp(path, "/upload/", 8) == 0) {
            long content_length = parse_content_length(buffer, header_len);
            handle_upload(client_fd, path + 8, body, body_len, content_length);
        } else {
            send_not_found(client_fd);
        }
    } else if (strcasecmp(method, "POST") == 0) {
        if (strcmp(path, "/exec") == 0) {
            long content_length = parse_content_length(buffer, header_len);
            handle_exec(client_fd, body, body_len, content_length);
        } else {
            send_not_found(client_fd);
        }
    } else {
        send_method_not_allowed(client_fd);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    if (chdir("/") != 0) {
        perror("chdir");
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(LISTEN_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Serving on port %d\n", LISTEN_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return EXIT_SUCCESS;
}
