#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <jpeglib.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#define SERVER_PORT 8080
#define LISTEN_BACKLOG 32
#define MAX_SESSIONS 8
#define FRAME_WIDTH 320
#define FRAME_HEIGHT 200
#define DISPLAY_DEFAULT ":99"
#define WAD_PATH_DEFAULT "/opt/doom/freedoom1.wad"
#define DOOM_BIN "chocolate-doom"
#define JPEG_QUALITY 80
#define STREAM_BOUNDARY "frame"
#define FRAME_INTERVAL_USEC 33333  // ~30fps

struct Session {
    bool active;
    int id;
    pid_t doom_pid;
    Display *display;
    Window window;
    bool xtest_available;
    unsigned char *rgb_buf;
    time_t last_activity;
    uint64_t frame_id;
};

static struct Session sessions[MAX_SESSIONS];
static pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *display_name = DISPLAY_DEFAULT;
static char wad_path[PATH_MAX] = WAD_PATH_DEFAULT;
static int server_port = SERVER_PORT;

static KeySym resolve_keysym(const char *name);

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

static Window find_doom_window(Display *display);
static bool refresh_session_window(struct Session *session);
static bool ensure_window_display(struct Session *session);
static unsigned char extract_component(unsigned long pixel, unsigned long mask);
static int x11_error_handler(Display *display, XErrorEvent *error);

static void handle_sigchld(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        // Intentionally empty; sessions are cleaned up lazily on next request.
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

    if (session->display) {
        XCloseDisplay(session->display);
    }
    if (session->rgb_buf) {
        free(session->rgb_buf);
    }

    memset(session, 0, sizeof(*session));
}

static bool wad_is_readable(const char *path) {
    return path && path[0] != '\0' && access(path, R_OK) == 0;
}

static void configure_wad_path(const char *override_path) {
    const char *candidates[] = {
        override_path,
        WAD_PATH_DEFAULT,
        "/root/freedoom1.wad",
        "/usr/share/games/doom/freedoom1.wad",
        "./freedoom1.wad",
        NULL
    };

    for (size_t i = 0; candidates[i]; ++i) {
        const char *candidate = candidates[i];
        if (!candidate || candidate[0] == '\0') {
            continue;
        }
        if (wad_is_readable(candidate)) {
            strncpy(wad_path, candidate, sizeof(wad_path) - 1);
            wad_path[sizeof(wad_path) - 1] = '\0';
            doom_logf("using WAD at %s", wad_path);
            return;
        }
    }

    const char *fallback = override_path && override_path[0] ? override_path : WAD_PATH_DEFAULT;
    strncpy(wad_path, fallback, sizeof(wad_path) - 1);
    wad_path[sizeof(wad_path) - 1] = '\0';
    doom_logf("warning: unable to find readable WAD (last tried %s)", wad_path);
}

static int maybe_spawn_doom(struct Session *session) {
    const char *disable = getenv("DOOM_DISABLE_SPAWN");
    if (disable && disable[0] == '1') {
        doom_logf("DOOM_DISABLE_SPAWN=1 → skipping chocolate-doom launch for session %d", session->id);
        return 0;
    }

    if (!wad_is_readable(wad_path)) {
        doom_logf("cannot launch session %d — WAD missing or unreadable at %s", session->id, wad_path);
        return -1;
    }

    pid_t child = fork();
    if (child < 0) {
        doom_logf("failed to fork doom process: %s", strerror(errno));
        return -1;
    }
    if (child == 0) {
        setenv("DISPLAY", display_name, 1);
        setenv("SDL_VIDEODRIVER", "x11", 1);
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
        session->active = true;
        session->last_activity = time(NULL);
        session->rgb_buf = malloc(FRAME_WIDTH * FRAME_HEIGHT * 3);
        if (!session->rgb_buf) {
            doom_logf("memory allocation failure for session %d", session_id);
            session_free(session);
            session = NULL;
            goto done;
        }

        session->display = XOpenDisplay(display_name);
        if (!session->display) {
            doom_logf("unable to open X11 display %s (falling back to synthetic frames)", display_name);
        } else {
            int event_base = 0;
            int error_base = 0;
            int major = 0;
            int minor = 0;
            session->xtest_available = XTestQueryExtension(
                session->display, &event_base, &error_base, &major, &minor);
            if (!session->xtest_available) {
                doom_logf("warning: XTest extension not available; input will not work");
            }

            Window found = find_doom_window(session->display);
            if (found != None) {
                session->window = found;
                ensure_window_display(session);
                doom_logf("bound session %d to window 0x%lx", session_id, (unsigned long)session->window);
            } else {
                session->window = DefaultRootWindow(session->display);
                doom_logf("doom window not found on %s; using root window capture", display_name);
            }
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

    if (!session->display || !session->xtest_available) {
        return -1;
    }
    if (!payload || len == 0) {
        return -1;
    }

    size_t start = 0;
    size_t end = len;
    while (start < end && isspace((unsigned char)payload[start])) {
        start++;
    }
    while (end > start && isspace((unsigned char)payload[end - 1])) {
        end--;
    }
    if (end <= start) {
        return -1;
    }

    if (end - start >= 4 && strncmp(payload + start, "key:", 4) == 0) {
        start += 4;
        while (start < end && isspace((unsigned char)payload[start])) {
            start++;
        }
    }
    if (end <= start) {
        return -1;
    }

    size_t key_len = end - start;
    if (key_len >= 63) {
        key_len = 62;
    }

    char keybuf[64];
    memcpy(keybuf, payload + start, key_len);
    keybuf[key_len] = '\0';

    bool explicit_action = false;
    bool is_press = true;

    char *action_sep = strrchr(keybuf, ':');
    if (action_sep && action_sep[1] != '\0') {
        char action[16];
        size_t action_len = 0;
        while (action_sep[1 + action_len] != '\0' && action_len < sizeof(action) - 1) {
            action_len++;
        }
        memcpy(action, action_sep + 1, action_len);
        action[action_len] = '\0';
        for (size_t i = 0; i < action_len; ++i) {
            action[i] = (char)tolower((unsigned char)action[i]);
        }
        if (strcmp(action, "down") == 0 || strcmp(action, "press") == 0) {
            explicit_action = true;
            is_press = true;
        } else if (strcmp(action, "up") == 0 || strcmp(action, "release") == 0) {
            explicit_action = true;
            is_press = false;
        }
        *action_sep = '\0';
    }

    if (keybuf[0] == '\0') {
        return -1;
    }

    KeySym keysym = resolve_keysym(keybuf);
    if (keysym == NoSymbol) {
        doom_logf("unknown key: %s", keybuf);
        return -1;
    }

    KeyCode keycode = XKeysymToKeycode(session->display, keysym);
    if (keycode == 0) {
        doom_logf("no keycode for keysym: %s", keybuf);
        return -1;
    }

    refresh_session_window(session);
    ensure_window_display(session);

    if (explicit_action) {
        XTestFakeKeyEvent(session->display, keycode, is_press ? True : False, CurrentTime);
    } else {
        XTestFakeKeyEvent(session->display, keycode, True, CurrentTime);
        XTestFakeKeyEvent(session->display, keycode, False, CurrentTime);
    }
    XFlush(session->display);

    return 0;
}

static unsigned int mask_shift(unsigned long mask) {
    unsigned int shift = 0;
    if (mask == 0) {
        return 0;
    }
    while ((mask & 1UL) == 0UL) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

static unsigned int mask_bits(unsigned long mask) {
    unsigned int bits = 0;
    while (mask) {
        bits += mask & 1UL;
        mask >>= 1;
    }
    return bits;
}

static unsigned char extract_component(unsigned long pixel, unsigned long mask) {
    if (mask == 0) {
        return 0;
    }
    unsigned int shift = mask_shift(mask);
    unsigned int bits = mask_bits(mask);
    unsigned long value = (pixel & mask) >> shift;
    if (bits >= 8) {
        value >>= (bits - 8);
        return (unsigned char)value;
    }
    unsigned int max_value = (1u << bits) - 1u;
    if (max_value == 0) {
        return 0;
    }
    return (unsigned char)((value * 255u) / max_value);
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

static bool window_is_viewable(Display *display, Window window) {
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(display, window, &attrs)) {
        return false;
    }
    if (attrs.map_state != IsViewable) {
        return false;
    }
    return attrs.width >= FRAME_WIDTH && attrs.height >= FRAME_HEIGHT;
}

static bool ensure_window_display(struct Session *session) {
    if (!session || !session->display || session->window == None) {
        return false;
    }

    XWindowAttributes attrs;
    if (XGetWindowAttributes(session->display, session->window, &attrs)) {
        if (attrs.map_state != IsViewable) {
            XMapRaised(session->display, session->window);
        }
        if (!attrs.override_redirect) {
            XRaiseWindow(session->display, session->window);
        }
    }

    XSetInputFocus(session->display, session->window, RevertToPointerRoot, CurrentTime);
    return true;
}

static void find_window_recursive(Display *display, Window window, Window *result) {
    if (!display || !result || *result != None) {
        return;
    }
    if (window_is_viewable(display, window)) {
        *result = window;
        return;
    }

    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int nchildren = 0;
    if (!XQueryTree(display, window, &root_return, &parent_return, &children, &nchildren)) {
        return;
    }
    for (unsigned int i = 0; i < nchildren && *result == None; ++i) {
        find_window_recursive(display, children[i], result);
    }
    if (children) {
        XFree(children);
    }
}

static Window find_doom_window(Display *display) {
    if (!display) {
        return None;
    }
    Window result = None;
    Window root = DefaultRootWindow(display);
    find_window_recursive(display, root, &result);
    return result;
}

static bool refresh_session_window(struct Session *session) {
    if (!session->display) {
        return false;
    }
    Window root = DefaultRootWindow(session->display);
    Window current = session->window;

    if (current != None && current != root) {
        if (window_is_viewable(session->display, current)) {
            return true;
        }
        doom_logf("X11 window 0x%lx for session %d is no longer viewable", (unsigned long)current, session->id);
        current = None;
    }

    Window found = find_doom_window(session->display);
    if (found != None) {
        if (found != session->window) {
            doom_logf("bound session %d to window 0x%lx", session->id, (unsigned long)found);
        }
        session->window = found;
        ensure_window_display(session);
        return true;
    }

    if (session->window != root) {
        doom_logf("doom window not available; capturing root window");
    }
    session->window = root;
    return false;
}

struct KeyAlias {
    const char *incoming;
    const char *keysym_name;
};

static const struct KeyAlias key_aliases[] = {
    {" ", "space"},
    {"space", "space"},
    {"spacebar", "space"},
    {"arrowup", "Up"},
    {"up", "Up"},
    {"arrowdown", "Down"},
    {"down", "Down"},
    {"arrowleft", "Left"},
    {"left", "Left"},
    {"arrowright", "Right"},
    {"right", "Right"},
    {"ctrl", "Control_L"},
    {"control", "Control_L"},
    {"control_l", "Control_L"},
    {"controlleft", "Control_L"},
    {"ctrl_l", "Control_L"},
    {"control_r", "Control_R"},
    {"controlright", "Control_R"},
    {"ctrl_r", "Control_R"},
    {"alt", "Alt_L"},
    {"alt_l", "Alt_L"},
    {"altleft", "Alt_L"},
    {"alt_r", "Alt_R"},
    {"altright", "Alt_R"},
    {"shift", "Shift_L"},
    {"shift_l", "Shift_L"},
    {"shiftleft", "Shift_L"},
    {"shift_r", "Shift_R"},
    {"shiftright", "Shift_R"},
    {"enter", "Return"},
    {"return", "Return"},
    {"escape", "Escape"},
    {"esc", "Escape"},
    {"tab", "Tab"},
    {"backspace", "BackSpace"},
    {"capslock", "Caps_Lock"},
    {"meta", "Super_L"},
    {"meta_l", "Super_L"},
    {"metal", "Super_L"},
    {"meta_r", "Super_R"},
    {"metar", "Super_R"},
};

static KeySym resolve_keysym(const char *name) {
    if (!name || name[0] == '\0') {
        return NoSymbol;
    }

    char trimmed[64];
    size_t len = strnlen(name, sizeof(trimmed) - 1);
    memcpy(trimmed, name, len);
    trimmed[len] = '\0';

    char lowered[64];
    for (size_t i = 0; i < len; ++i) {
        lowered[i] = (char)tolower((unsigned char)trimmed[i]);
    }
    lowered[len] = '\0';

    for (size_t i = 0; i < sizeof(key_aliases) / sizeof(key_aliases[0]); ++i) {
        if (strcmp(lowered, key_aliases[i].incoming) == 0) {
            KeySym alias = XStringToKeysym(key_aliases[i].keysym_name);
            if (alias != NoSymbol) {
                return alias;
            }
        }
    }

    if (strncmp(trimmed, "Key", 3) == 0 && len == 4) {
        unsigned char c = (unsigned char)trimmed[3];
        if (isalpha(c)) {
            char keystr[2] = {(char)tolower(c), '\0'};
            KeySym sym = XStringToKeysym(keystr);
            if (sym != NoSymbol) {
                return sym;
            }
        }
    }

    if (strncmp(trimmed, "Digit", 5) == 0 && len == 6) {
        unsigned char c = (unsigned char)trimmed[5];
        if (isdigit(c)) {
            char keystr[2] = {(char)c, '\0'};
            KeySym sym = XStringToKeysym(keystr);
            if (sym != NoSymbol) {
                return sym;
            }
        }
    }

    KeySym direct = XStringToKeysym(trimmed);
    if (direct != NoSymbol) {
        return direct;
    }

    KeySym lower_sym = XStringToKeysym(lowered);
    if (lower_sym != NoSymbol) {
        return lower_sym;
    }

    if (len == 1) {
        unsigned char c = (unsigned char)trimmed[0];
        if (isalpha(c)) {
            char keystr[2] = {(char)tolower(c), '\0'};
            KeySym sym = XStringToKeysym(keystr);
            if (sym != NoSymbol) {
                return sym;
            }
        }
        return (KeySym)c;
    }

    return NoSymbol;
}

static int x11_error_handler(Display *display, XErrorEvent *error) {
    char description[256] = {0};
    if (display) {
        XGetErrorText(display, error->error_code, description, sizeof(description));
    }
    doom_logf("X11 error %d (request %u.%u resource=0x%lx): %s",
              error->error_code,
              error->request_code,
              error->minor_code,
              error->resourceid,
              description[0] ? description : "unknown");
    return 0;
}

static bool capture_frame(struct Session *session) {
    if (!session->display) {
        generate_test_pattern(session);
        session->frame_id++;
        return true;
    }

    refresh_session_window(session);
    Window target = session->window;
    if (target == None) {
        target = DefaultRootWindow(session->display);
    }

    XImage *image = XGetImage(session->display, target, 0, 0,
                              FRAME_WIDTH, FRAME_HEIGHT, AllPlanes, ZPixmap);
    if (!image) {
        doom_logf("XGetImage failed for window 0x%lx — switching to synthetic frames",
                  (unsigned long)target);
        generate_test_pattern(session);
        session->frame_id++;
        return true;
    }

    if (image->bits_per_pixel < 16) {
        doom_logf("unsupported XImage depth %d — switching to synthetic frames",
                  image->bits_per_pixel);
        XDestroyImage(image);
        generate_test_pattern(session);
        session->frame_id++;
        return true;
    }

    unsigned char *dst = session->rgb_buf;
    unsigned long red_mask = image->red_mask;
    unsigned long green_mask = image->green_mask;
    unsigned long blue_mask = image->blue_mask;

    for (int y = 0; y < FRAME_HEIGHT; ++y) {
        for (int x = 0; x < FRAME_WIDTH; ++x) {
            unsigned long pixel = XGetPixel(image, x, y);
            size_t idx = (y * FRAME_WIDTH + x) * 3;
            dst[idx + 0] = extract_component(pixel, red_mask);
            dst[idx + 1] = extract_component(pixel, green_mask);
            dst[idx + 2] = extract_component(pixel, blue_mask);
        }
    }

    XDestroyImage(image);
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
                send_response(client_fd, 500, "text/plain", "input error");
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

    if (!XInitThreads()) {
        doom_logf("warning: XInitThreads failed; X11 calls may not be thread-safe");
    }
    XSetErrorHandler(x11_error_handler);

    const char *display_override = getenv("DOOM_DISPLAY");
    if (!display_override || display_override[0] == '\0') {
        display_override = getenv("DOOM_FRAMEBUFFER");  // backward compatibility
    }
    if (display_override && display_override[0] != '\0') {
        display_name = display_override;
    }
    const char *wad_override = getenv("DOOM_WAD_PATH");
    if (wad_override && wad_override[0] == '\0') {
        wad_override = NULL;
    }
    configure_wad_path(wad_override);
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

    doom_logf("doom_server listening on port %d (display=%s)", server_port, display_name);

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
