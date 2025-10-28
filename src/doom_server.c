#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <jpeglib.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 8080
#define LISTEN_BACKLOG 32
#define MAX_SESSIONS 8
#define FRAME_WIDTH 320
#define FRAME_HEIGHT 200
#define SESSION_DIR_DEFAULT "/root/doom_sessions"
#define FB_DEFAULT "/dev/fb0"
#define WAD_PATH_DEFAULT "/root/freedoom1.wad"
#define DOOM_BIN "chocolate-doom"
#define JPEG_QUALITY 80
#define STREAM_BOUNDARY "frame"
#define FRAME_INTERVAL_USEC 33333  // ~30fps

struct Session {
    bool active;
    int id;
    pid_t doom_pid;
    int fb_fd;
    unsigned char *rgb_buf;
    unsigned char *fb_buf;
    size_t fb_buf_size;
    char fifo_path[256];
    time_t last_activity;
    uint64_t frame_id;
};

static struct Session sessions[MAX_SESSIONS];
static pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *framebuffer_path = FB_DEFAULT;
static const char *session_dir = SESSION_DIR_DEFAULT;
static const char *wad_path = WAD_PATH_DEFAULT;
static int server_port = SERVER_PORT;

static void doom_logf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    fprintf(stderr, "[%s] ", ts);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static void handle_sigchld(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        // Intentionally empty; sessions are cleaned up lazily on next request.
    }
}

static void ensure_session_dir(void) {
    struct stat st = {0};
    if (stat(session_dir, &st) == -1) {
        if (mkdir(session_dir, 0777) == -1) {
            doom_logf("failed to create session dir %s: %s", session_dir, strerror(errno));
        }
    }
}

static void session_free(struct Session *session) {
    if (!session->active) {
        return;
    }

    doom_logf("tearing down session %d", session->id);

    if (session->doom_pid > 0) {
        kill(session->doom_pid, SIGTERM);
        waitpid(session->doom_pid, NULL, WNOHANG);
    }

    if (session->fb_fd >= 0) {
        close(session->fb_fd);
    }
    if (session->rgb_buf) {
        free(session->rgb_buf);
    }
    if (session->fb_buf) {
        free(session->fb_buf);
    }
    unlink(session->fifo_path);

    memset(session, 0, sizeof(*session));
    session->fb_fd = -1;
}

static int maybe_spawn_doom(struct Session *session) {
    const char *disable = getenv("DOOM_DISABLE_SPAWN");
    if (disable && disable[0] == '1') {
        doom_logf("DOOM_DISABLE_SPAWN=1 → skipping chocolate-doom launch for session %d", session->id);
        return 0;
    }

    pid_t child = fork();
    if (child < 0) {
        doom_logf("failed to fork doom process: %s", strerror(errno));
        return -1;
    }
    if (child == 0) {
        setenv("SDL_VIDEODRIVER", "fbcon", 1);
        setenv("SDL_FBDEV", framebuffer_path, 1);
        execlp(DOOM_BIN, DOOM_BIN,
               "-iwad", wad_path,
               "-width", "320",
               "-height", "200",
               "-nosound",
               "-nomusic",
               "-window",  // keep keyboard focus logic simple
               NULL);
        _exit(1);
    }

    session->doom_pid = child;
    doom_logf("spawned chocolate-doom (pid=%d) for session %d", child, session->id);
    return 0;
}

static struct Session *session_get_or_create(int session_id) {
    if (session_id < 0 || session_id >= MAX_SESSIONS) {
        return NULL;
    }

    pthread_mutex_lock(&sessions_lock);
    struct Session *session = &sessions[session_id];

    if (!session->active) {
        memset(session, 0, sizeof(*session));
        session->id = session_id;
        session->fb_fd = -1;
        session->active = true;
        session->last_activity = time(NULL);
        session->fb_buf_size = FRAME_WIDTH * FRAME_HEIGHT * 4;
        session->fb_buf = malloc(session->fb_buf_size);
        session->rgb_buf = malloc(FRAME_WIDTH * FRAME_HEIGHT * 3);
        if (!session->fb_buf || !session->rgb_buf) {
            doom_logf("memory allocation failure for session %d", session_id);
            session_free(session);
            session = NULL;
            goto done;
        }

        ensure_session_dir();
        snprintf(session->fifo_path, sizeof(session->fifo_path),
                 "%s/input_%d", session_dir, session_id);
        if (mkfifo(session->fifo_path, 0666) == -1 && errno != EEXIST) {
            doom_logf("mkfifo failed for %s: %s", session->fifo_path, strerror(errno));
            session_free(session);
            session = NULL;
            goto done;
        }

        session->fb_fd = open(framebuffer_path, O_RDONLY);
        if (session->fb_fd < 0) {
            doom_logf("unable to open framebuffer %s: %s (falling back to synthetic frames)",
                 framebuffer_path, strerror(errno));
        }

        if (maybe_spawn_doom(session) != 0) {
            session_free(session);
            session = NULL;
            goto done;
        }

        doom_logf("session %d initialized", session_id);
    } else {
        session->last_activity = time(NULL);
    }

done:
    pthread_mutex_unlock(&sessions_lock);
    return session;
}

static int session_write_input(struct Session *session, const char *payload, size_t len) {
    session->last_activity = time(NULL);
    int fd = open(session->fifo_path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        doom_logf("open FIFO %s failed: %s", session->fifo_path, strerror(errno));
        return -1;
    }
    ssize_t written = write(fd, payload, len);
    close(fd);
    if (written < 0) {
        doom_logf("write FIFO failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void generate_test_pattern(struct Session *session) {
    unsigned char *rgb = session->rgb_buf;
    for (int y = 0; y < FRAME_HEIGHT; y++) {
        for (int x = 0; x < FRAME_WIDTH; x++) {
            size_t idx = (y * FRAME_WIDTH + x) * 3;
            rgb[idx + 0] = (x + session->frame_id) % 256;
            rgb[idx + 1] = (y * 2) % 256;
            rgb[idx + 2] = (session->frame_id * 5) % 256;
        }
    }
}

static bool capture_frame(struct Session *session) {
    if (session->fb_fd < 0) {
        generate_test_pattern(session);
        session->frame_id++;
        return true;
    }

    size_t expected = session->fb_buf_size;
    ssize_t read_bytes = pread(session->fb_fd, session->fb_buf, expected, 0);
    if (read_bytes != (ssize_t)expected) {
        doom_logf("framebuffer read returned %zd (expected %zu) — switching to synthetic frames",
             read_bytes, expected);
        close(session->fb_fd);
        session->fb_fd = -1;
        generate_test_pattern(session);
        session->frame_id++;
        return true;
    }

    unsigned char *src = session->fb_buf;
    unsigned char *dst = session->rgb_buf;
    for (size_t i = 0, j = 0; i < expected; i += 4, j += 3) {
        // Framebuffer is assumed to be BGRA.
        unsigned char b = src[i + 0];
        unsigned char g = src[i + 1];
        unsigned char r = src[i + 2];
        dst[j + 0] = r;
        dst[j + 1] = g;
        dst[j + 2] = b;
    }

    session->frame_id++;
    return true;
}

static unsigned char *encode_rgb_to_jpeg(struct Session *session, unsigned long *jpeg_size) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char *jpeg_buf = NULL;
    unsigned long out_size = 0;
    jpeg_mem_dest(&cinfo, &jpeg_buf, &out_size);

    cinfo.image_width = FRAME_WIDTH;
    cinfo.image_height = FRAME_HEIGHT;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, JPEG_QUALITY, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    JSAMPROW row_pointer[1];
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = session->rgb_buf + (cinfo.next_scanline * FRAME_WIDTH * 3);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    *jpeg_size = out_size;

    jpeg_destroy_compress(&cinfo);
    return jpeg_buf;
}

struct http_request {
    char method[8];
    char path[256];
    char query[256];
    char version[16];
    char *body;
    size_t body_len;
};

static void free_request(struct http_request *req) {
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}

static int parse_request(int client_fd, struct http_request *req) {
    memset(req, 0, sizeof(*req));
    size_t buf_cap = 8192;
    char *buf = calloc(1, buf_cap + 1);
    if (!buf) {
        return -1;
    }

    ssize_t received = recv(client_fd, buf, buf_cap, 0);
    if (received <= 0) {
        free(buf);
        return -1;
    }

    buf[received] = '\0';

    sscanf(buf, "%7s %255s %15s", req->method, req->path, req->version);

    char *question = strchr(req->path, '?');
    if (question) {
        strncpy(req->query, question + 1, sizeof(req->query) - 1);
        *question = '\0';
    }

    char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t header_len = body_start - buf;
        size_t body_bytes = received - header_len;
        req->body = malloc(body_bytes + 1);
        if (req->body) {
            memcpy(req->body, body_start, body_bytes);
            req->body[body_bytes] = '\0';
            req->body_len = body_bytes;
        }
    }

    free(buf);
    return 0;
}

static int parse_session_id(const char *query) {
    if (!query || query[0] == '\0') {
        return 0;
    }

    const char *needle = "session=";
    const char *found = strstr(query, needle);
    if (!found) {
        return 0;
    }
    found += strlen(needle);
    return atoi(found);
}

static void send_response(int client_fd, int status, const char *content_type, const char *body) {
    char header[512];
    int len = snprintf(header, sizeof(header),
                       "HTTP/1.1 %d\r\n"
                       "Content-Type: %s\r\n"
                       "Cache-Control: no-cache\r\n"
                       "Connection: close\r\n"
                       "Content-Length: %zu\r\n"
                       "\r\n",
                       status, content_type ? content_type : "text/plain",
                       body ? strlen(body) : 0UL);
    send(client_fd, header, len, 0);
    if (body && strlen(body) > 0) {
        send(client_fd, body, strlen(body), 0);
    }
}

static void serve_static(int client_fd, const char *rel_path) {
    char fs_path[512];
    snprintf(fs_path, sizeof(fs_path), "public/%s", rel_path[0] == '/' ? rel_path + 1 : rel_path);

    int fd = open(fs_path, O_RDONLY);
    if (fd < 0) {
        send_response(client_fd, 404, "text/plain", "not found");
        return;
    }

    struct stat st;
    fstat(fd, &st);
    size_t size = st.st_size;
    char *buf = malloc(size + 1);
    if (!buf) {
        close(fd);
        send_response(client_fd, 500, "text/plain", "oom");
        return;
    }

    ssize_t read_bytes = read(fd, buf, size);
    close(fd);
    if (read_bytes != (ssize_t)size) {
        free(buf);
        send_response(client_fd, 500, "text/plain", "read error");
        return;
    }
    buf[size] = '\0';

    const char *content_type = "text/plain";
    if (strstr(fs_path, ".html")) {
        content_type = "text/html; charset=utf-8";
    } else if (strstr(fs_path, ".js")) {
        content_type = "application/javascript";
    } else if (strstr(fs_path, ".css")) {
        content_type = "text/css";
    }

    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: %s\r\n"
                              "Cache-Control: no-cache\r\n"
                              "Connection: close\r\n"
                              "Content-Length: %zu\r\n"
                              "\r\n",
                              content_type, size);
    send(client_fd, header, header_len, 0);
    send(client_fd, buf, size, 0);
    free(buf);
}

static void stream_mjpeg(int client_fd, struct Session *session) {
    const char header[] =
        "HTTP/1.1 200 OK\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" STREAM_BOUNDARY "\r\n"
        "\r\n";
    send(client_fd, header, strlen(header), 0);

    for (;;) {
        if (!capture_frame(session)) {
            break;
        }

        unsigned long jpeg_size = 0;
        unsigned char *jpeg_buf = encode_rgb_to_jpeg(session, &jpeg_size);
        if (!jpeg_buf || jpeg_size == 0) {
            doom_logf("jpeg encoding failed for session %d", session->id);
            break;
        }

        char frame_header[256];
        int header_len = snprintf(frame_header, sizeof(frame_header),
                                  "--" STREAM_BOUNDARY "\r\n"
                                  "Content-Type: image/jpeg\r\n"
                                  "Content-Length: %lu\r\n"
                                  "\r\n",
                                  jpeg_size);

        if (send(client_fd, frame_header, header_len, 0) < 0) {
            free(jpeg_buf);
            break;
        }
        if (send(client_fd, jpeg_buf, jpeg_size, 0) < 0) {
            free(jpeg_buf);
            break;
        }
        if (send(client_fd, "\r\n", 2, 0) < 0) {
            free(jpeg_buf);
            break;
        }

        free(jpeg_buf);
        usleep(FRAME_INTERVAL_USEC);
    }
}

static void handle_request(int client_fd) {
    struct http_request req;
    if (parse_request(client_fd, &req) != 0) {
        close(client_fd);
        return;
    }

    if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/healthz") == 0) {
        send_response(client_fd, 200, "text/plain", "ok");
    } else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/") == 0) {
        serve_static(client_fd, "index.html");
    } else if (strcmp(req.method, "GET") == 0 && strncmp(req.path, "/public/", 8) == 0) {
        serve_static(client_fd, req.path + 1);
    } else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/doom.mjpeg") == 0) {
        int session_id = parse_session_id(req.query);
        struct Session *session = session_get_or_create(session_id);
        if (!session) {
            send_response(client_fd, 503, "text/plain", "no session");
        } else {
            stream_mjpeg(client_fd, session);
        }
    } else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/input") == 0) {
        int session_id = parse_session_id(req.query);
        struct Session *session = session_get_or_create(session_id);
        if (!session) {
            send_response(client_fd, 503, "text/plain", "no session");
        } else if (req.body_len == 0) {
            send_response(client_fd, 400, "text/plain", "empty payload");
        } else {
            if (session_write_input(session, req.body, req.body_len) == 0) {
                send_response(client_fd, 200, "text/plain", "ok");
            } else {
                send_response(client_fd, 500, "text/plain", "fifo error");
            }
        }
    } else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/session/close") == 0) {
        send_response(client_fd, 501, "text/plain", "close not implemented yet");
    } else {
        send_response(client_fd, 404, "text/plain", "not found");
    }

    free_request(&req);
    close(client_fd);
}

static void *client_thread(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);
    handle_request(client_fd);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, handle_sigchld);

    const char *fb_override = getenv("DOOM_FRAMEBUFFER");
    if (fb_override && fb_override[0] != '\0') {
        framebuffer_path = fb_override;
    }
    const char *session_override = getenv("DOOM_SESSION_DIR");
    if (session_override && session_override[0] != '\0') {
        session_dir = session_override;
    }
    const char *wad_override = getenv("DOOM_WAD_PATH");
    if (wad_override && wad_override[0] != '\0') {
        wad_path = wad_override;
    }
    const char *port_override = getenv("DOOM_SERVER_PORT");
    if (port_override && port_override[0] != '\0') {
        int port = atoi(port_override);
        if (port > 0 && port < 65536) {
            server_port = port;
        }
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(server_port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    doom_logf("doom_server listening on port %d (framebuffer=%s)", server_port, framebuffer_path);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        int *fd_ptr = malloc(sizeof(int));
        if (!fd_ptr) {
            close(client_fd);
            continue;
        }
        *fd_ptr = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, fd_ptr) != 0) {
            close(client_fd);
            free(fd_ptr);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
