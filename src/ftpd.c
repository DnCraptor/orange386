/*
 * Minimal FTP server (RFC 959 + EPSV/SIZE/MDTM/REST/UTF8) over the FAT
 * volumes inside the attached disk images.  See ftpd.h for the life cycle.
 *
 * Virtual tree: "/" lists one directory per mounted volume ("/FDD-0",
 * "/ATA0-0", ...); below that, paths are FatFs paths "N:/..." of the image
 * FatFs (src/imgfs, UTF-8 names).  Transfers are always binary.
 */
#define _GNU_SOURCE   /* accept4() */
#include "ftpd.h"
#include "imgfs/imgfs.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FTPD_POLL_MS     200
#define FTPD_DATA_WAIT_MS 15000
#define FTPD_LINE_MAX    1024
#define FTPD_PATH_MAX    600
#define FTPD_BUF_SIZE    (64u * 1024u)

typedef struct {
    char name[16];
    int fd;
    bool readonly;
    bool mounted;
    FATFS fs;
} volume_t;

static volume_t volumes[FTPD_MAX_VOLUMES];
static int volume_count;

static char cfg_user[64];
static char cfg_password[64];

static int listen_fd = -1;
static pthread_t server_thread;
static bool server_running;
static atomic_bool stop_requested;

static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static char status_line[96];

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&status_lock);
    va_start(ap, fmt);
    vsnprintf(status_line, sizeof(status_line), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&status_lock);
}

void ftpd_status(char *buf, size_t size)
{
    pthread_mutex_lock(&status_lock);
    snprintf(buf, size, "%s", status_line);
    pthread_mutex_unlock(&status_lock);
}

int ftpd_volume_count(void)
{
    return volume_count;
}

bool ftpd_volume(int index, const char **name, bool *mounted)
{
    if (index < 0 || index >= volume_count)
        return false;
    *name = volumes[index].name;
    *mounted = volumes[index].mounted;
    return true;
}

/* ------------------------------------------------------------------------
 * Session
 * ---------------------------------------------------------------------- */

typedef struct {
    int ctrl;
    char peer[INET6_ADDRSTRLEN];
    struct sockaddr_in local;     /* control connection, local side */
    char inbuf[FTPD_LINE_MAX];
    size_t inlen;
    bool user_ok;
    bool logged_in;
    char cwd[FTPD_PATH_MAX];      /* virtual, always absolute */
    int pasv_fd;                  /* listening data socket (PASV/EPSV) */
    bool have_port;               /* PORT address valid */
    struct sockaddr_in port_addr;
    FSIZE_t rest;                 /* REST offset for the next RETR/STOR */
    char rnfr[FTPD_PATH_MAX];     /* pending RNFR, virtual path */
    uint8_t *buf;
} session_t;

static void reply(session_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply(session_t *s, const char *fmt, ...)
{
    char line[FTPD_PATH_MAX + 64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof(line) - 3)
        n = (int)(sizeof(line) - 3);
    line[n++] = '\r';
    line[n++] = '\n';
    const char *p = line;
    size_t left = (size_t)n;
    while (left) {
        ssize_t w = send(s->ctrl, p, left, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        p += w;
        left -= (size_t)w;
    }
}

/* Read one command line.  1: line in out, 0: connection closed or stop
 * requested, never blocks longer than FTPD_POLL_MS between stop checks. */
static int read_line(session_t *s, char *out, size_t out_size)
{
    for (;;) {
        char *nl = memchr(s->inbuf, '\n', s->inlen);
        if (nl) {
            size_t len = (size_t)(nl - s->inbuf);
            size_t copy = len < out_size - 1 ? len : out_size - 1;
            memcpy(out, s->inbuf, copy);
            out[copy] = 0;
            if (copy && out[copy - 1] == '\r')
                out[copy - 1] = 0;
            memmove(s->inbuf, nl + 1, s->inlen - len - 1);
            s->inlen -= len + 1;
            return 1;
        }
        if (s->inlen == sizeof(s->inbuf))
            s->inlen = 0;                 /* overlong line: drop it */
        if (atomic_load(&stop_requested))
            return 0;
        struct pollfd pfd = { .fd = s->ctrl, .events = POLLIN };
        int r = poll(&pfd, 1, FTPD_POLL_MS);
        if (r < 0 && errno != EINTR)
            return 0;
        if (r <= 0)
            continue;
        ssize_t n = recv(s->ctrl, s->inbuf + s->inlen,
                         sizeof(s->inbuf) - s->inlen, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0;
        s->inlen += (size_t)n;
    }
}

/* ------------------------------------------------------------------------
 * Virtual paths
 * ---------------------------------------------------------------------- */

/* Join arg onto cwd and fold "." / ".." into out (absolute, no trailing /). */
static bool make_vpath(const session_t *s, const char *arg, char *out, size_t size)
{
    char tmp[FTPD_PATH_MAX * 2];
    if (!arg || !arg[0])
        arg = ".";
    if (arg[0] == '/' || arg[0] == '\\')
        snprintf(tmp, sizeof(tmp), "%s", arg);
    else
        snprintf(tmp, sizeof(tmp), "%s/%s", s->cwd, arg);

    char *comps[128];
    int n = 0;
    char *save = NULL;
    for (char *p = tmp; *p; ++p)
        if (*p == '\\')
            *p = '/';
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, "."))
            continue;
        if (!strcmp(t, "..")) {
            if (n > 0)
                --n;
            continue;
        }
        if (n == (int)(sizeof(comps) / sizeof(comps[0])))
            return false;
        comps[n++] = t;
    }
    size_t len = 0;
    out[0] = 0;
    if (n == 0) {
        snprintf(out, size, "/");
        return true;
    }
    for (int i = 0; i < n; ++i) {
        int w = snprintf(out + len, size - len, "/%s", comps[i]);
        if (w < 0 || (size_t)w >= size - len)
            return false;
        len += (size_t)w;
    }
    return true;
}

typedef enum { VP_ROOT, VP_VOLUME, VP_ENTRY } vp_kind;

typedef struct {
    vp_kind kind;
    int vol;                       /* volume index (VP_VOLUME, VP_ENTRY) */
    char fat[FTPD_PATH_MAX + 8];   /* "N:/rest" FatFs path */
} vp_t;

/* Map a virtual path to a volume and FatFs path.  false: unknown volume. */
static bool split_vpath(const char *vpath, vp_t *vp)
{
    if (!strcmp(vpath, "/")) {
        vp->kind = VP_ROOT;
        vp->vol = -1;
        vp->fat[0] = 0;
        return true;
    }
    const char *name = vpath + 1;
    const char *rest = strchr(name, '/');
    size_t nlen = rest ? (size_t)(rest - name) : strlen(name);
    for (int i = 0; i < volume_count; ++i) {
        if (!volumes[i].mounted || strlen(volumes[i].name) != nlen ||
            strncasecmp(volumes[i].name, name, nlen) != 0)
            continue;
        vp->vol = i;
        vp->kind = rest ? VP_ENTRY : VP_VOLUME;
        snprintf(vp->fat, sizeof(vp->fat), "%d:%s", i, rest ? rest : "/");
        return true;
    }
    return false;
}

static void reply_fresult(session_t *s, FRESULT fr)
{
    switch (fr) {
    case FR_NO_FILE:
    case FR_NO_PATH:
        reply(s, "550 No such file or directory.");
        break;
    case FR_EXIST:
        reply(s, "550 File exists.");
        break;
    case FR_DENIED:
        reply(s, "550 Permission denied (read-only, full, or directory not empty).");
        break;
    case FR_WRITE_PROTECTED:
        reply(s, "550 Volume is write-protected.");
        break;
    case FR_INVALID_NAME:
        reply(s, "553 File name not allowed.");
        break;
    default:
        reply(s, "451 Disk error (FatFs %d).", (int)fr);
        break;
    }
}

/* ------------------------------------------------------------------------
 * Data connection
 * ---------------------------------------------------------------------- */

static void close_pasv(session_t *s)
{
    if (s->pasv_fd >= 0)
        close(s->pasv_fd);
    s->pasv_fd = -1;
}

static int open_pasv(session_t *s, unsigned *port)
{
    close_pasv(s);
    s->have_port = false;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a = s->local;
    a.sin_port = 0;
    socklen_t al = sizeof(a);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 1) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &al) != 0) {
        close(fd);
        return -1;
    }
    s->pasv_fd = fd;
    *port = ntohs(a.sin_port);
    return 0;
}

/* Connected data socket for the transfer announced with 150, or -1. */
static int open_data(session_t *s)
{
    int fd = -1;
    if (s->pasv_fd >= 0) {
        int waited = 0;
        while (waited < FTPD_DATA_WAIT_MS && !atomic_load(&stop_requested)) {
            struct pollfd pfd = { .fd = s->pasv_fd, .events = POLLIN };
            int r = poll(&pfd, 1, FTPD_POLL_MS);
            if (r > 0) {
                fd = accept4(s->pasv_fd, NULL, NULL, SOCK_CLOEXEC);
                break;
            }
            if (r < 0 && errno != EINTR)
                break;
            waited += FTPD_POLL_MS;
        }
        close_pasv(s);
    } else if (s->have_port) {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&s->port_addr,
                               sizeof(s->port_addr)) != 0) {
            close(fd);
            fd = -1;
        }
        s->have_port = false;
    }
    return fd;
}

static bool send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len) {
        if (atomic_load(&stop_requested))
            return false;
        ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        p += w;
        len -= (size_t)w;
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Listings
 * ---------------------------------------------------------------------- */

static const char *const month_names[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void format_list_line(char *out, size_t size, const char *name,
                             bool is_dir, bool readonly, FSIZE_t fsize,
                             WORD fdate, WORD ftime)
{
    int year = 1980 + (fdate >> 9);
    int mon = (fdate >> 5) & 15;
    int day = fdate & 31;
    if (mon < 1 || mon > 12)
        mon = 1;
    if (day < 1)
        day = 1;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char when[16];
    if (year == tm.tm_year + 1900)
        snprintf(when, sizeof(when), "%s %2d %02u:%02u", month_names[mon - 1], day,
                 (unsigned)(ftime >> 11) & 31u, (unsigned)(ftime >> 5) & 63u);
    else
        snprintf(when, sizeof(when), "%s %2d  %4d", month_names[mon - 1], day, year);
    const char *mode = is_dir ? (readonly ? "dr-xr-xr-x" : "drwxr-xr-x")
                              : (readonly ? "-r--r--r--" : "-rw-r--r--");
    snprintf(out, size, "%s 1 dos dos %12llu %s %s\r\n",
             mode, (unsigned long long)fsize, when, name);
}

/* LIST (long) or NLST (names) of a virtual path. */
static void cmd_list(session_t *s, const char *arg, bool names_only)
{
    /* Skip "ls" options some clients send (LIST -al). */
    while (arg && arg[0] == '-') {
        while (*arg && *arg != ' ')
            ++arg;
        while (*arg == ' ')
            ++arg;
    }
    char vpath[FTPD_PATH_MAX];
    vp_t vp;
    if (!make_vpath(s, arg, vpath, sizeof(vpath)) || !split_vpath(vpath, &vp)) {
        reply(s, "550 No such file or directory.");
        return;
    }

    DIR dir;
    FILINFO fno;
    bool single = false;
    if (vp.kind == VP_ENTRY) {
        FRESULT fr = f_stat(vp.fat, &fno);
        if (fr != FR_OK) {
            reply_fresult(s, fr);
            return;
        }
        single = !(fno.fattrib & AM_DIR);
    }
    if (vp.kind != VP_ROOT && !single) {
        FRESULT fr = f_opendir(&dir, vp.fat);
        if (fr != FR_OK) {
            reply_fresult(s, fr);
            return;
        }
    }

    reply(s, "150 Opening data connection for directory list.");
    int dfd = open_data(s);
    if (dfd < 0) {
        if (vp.kind != VP_ROOT && !single)
            f_closedir(&dir);
        reply(s, "425 Cannot open data connection.");
        return;
    }

    char line[FF_LFN_BUF * 3 + 96];
    bool ok = true;
    if (vp.kind == VP_ROOT) {
        for (int i = 0; i < volume_count && ok; ++i) {
            if (!volumes[i].mounted)
                continue;
            if (names_only)
                snprintf(line, sizeof(line), "%.*s\r\n",
                         (int)sizeof(volumes[i].name) - 1, volumes[i].name);
            else
                format_list_line(line, sizeof(line), volumes[i].name, true,
                                 volumes[i].readonly, 0, (WORD)(46u << 9 | 1u << 5 | 1u), 0);
            ok = send_all(dfd, line, strlen(line));
        }
    } else if (single) {
        if (names_only)
            snprintf(line, sizeof(line), "%s\r\n", fno.fname);
        else
            format_list_line(line, sizeof(line), fno.fname, false,
                             volumes[vp.vol].readonly || (fno.fattrib & AM_RDO),
                             fno.fsize, fno.fdate, fno.ftime);
        ok = send_all(dfd, line, strlen(line));
    } else {
        for (;;) {
            FRESULT fr = f_readdir(&dir, &fno);
            if (fr != FR_OK || !fno.fname[0])
                break;
            if (names_only)
                snprintf(line, sizeof(line), "%s\r\n", fno.fname);
            else
                format_list_line(line, sizeof(line), fno.fname,
                                 (fno.fattrib & AM_DIR) != 0,
                                 volumes[vp.vol].readonly || (fno.fattrib & AM_RDO),
                                 fno.fsize, fno.fdate, fno.ftime);
            if (!(ok = send_all(dfd, line, strlen(line))))
                break;
        }
        f_closedir(&dir);
    }
    close(dfd);
    if (ok)
        reply(s, "226 Transfer complete.");
    else
        reply(s, "426 Connection closed; transfer aborted.");
}

/* ------------------------------------------------------------------------
 * File transfer
 * ---------------------------------------------------------------------- */

static bool entry_path(session_t *s, const char *arg, char *vpath, vp_t *vp)
{
    if (!arg || !arg[0]) {
        reply(s, "501 Missing file name.");
        return false;
    }
    if (!make_vpath(s, arg, vpath, FTPD_PATH_MAX) || !split_vpath(vpath, vp)) {
        reply(s, "550 No such file or directory.");
        return false;
    }
    if (vp->kind != VP_ENTRY) {
        reply(s, "550 Not a file.");
        return false;
    }
    return true;
}

static void cmd_retr(session_t *s, const char *arg)
{
    char vpath[FTPD_PATH_MAX];
    vp_t vp;
    FSIZE_t rest = s->rest;
    s->rest = 0;
    if (!entry_path(s, arg, vpath, &vp))
        return;
    FIL f;
    FRESULT fr = f_open(&f, vp.fat, FA_READ);
    if (fr != FR_OK) {
        reply_fresult(s, fr);
        return;
    }
    if (rest && f_lseek(&f, rest) != FR_OK) {
        f_close(&f);
        reply(s, "554 Invalid REST offset.");
        return;
    }
    reply(s, "150 Opening BINARY mode data connection for %s (%llu bytes).",
          arg, (unsigned long long)f_size(&f));
    int dfd = open_data(s);
    if (dfd < 0) {
        f_close(&f);
        reply(s, "425 Cannot open data connection.");
        return;
    }
    unsigned long long total = rest;
    bool ok = true;
    for (;;) {
        UINT br = 0;
        fr = f_read(&f, s->buf, FTPD_BUF_SIZE, &br);
        if (fr != FR_OK) {
            ok = false;
            break;
        }
        if (!br)
            break;
        if (!send_all(dfd, s->buf, br)) {
            ok = false;
            break;
        }
        total += br;
        set_status("%s: RETR %s %llu KB", s->peer, vpath, total >> 10);
    }
    close(dfd);
    f_close(&f);
    set_status("%s: connected", s->peer);
    if (ok)
        reply(s, "226 Transfer complete.");
    else if (fr != FR_OK)
        reply(s, "451 Disk read error (FatFs %d).", (int)fr);
    else
        reply(s, "426 Connection closed; transfer aborted.");
}

static void cmd_stor(session_t *s, const char *arg, bool append)
{
    char vpath[FTPD_PATH_MAX];
    vp_t vp;
    FSIZE_t rest = s->rest;
    s->rest = 0;
    if (!entry_path(s, arg, vpath, &vp))
        return;
    FIL f;
    BYTE mode = FA_WRITE;
    if (append)
        mode |= FA_OPEN_APPEND;
    else if (rest)
        mode |= FA_OPEN_ALWAYS;
    else
        mode |= FA_CREATE_ALWAYS;
    FRESULT fr = f_open(&f, vp.fat, mode);
    if (fr != FR_OK) {
        reply_fresult(s, fr);
        return;
    }
    if (rest && !append &&
        ((fr = f_lseek(&f, rest)) != FR_OK || (fr = f_truncate(&f)) != FR_OK)) {
        f_close(&f);
        reply_fresult(s, fr);
        return;
    }
    reply(s, "150 Ok to send data.");
    int dfd = open_data(s);
    if (dfd < 0) {
        f_close(&f);
        reply(s, "425 Cannot open data connection.");
        return;
    }
    unsigned long long total = 0;
    bool ok = true;
    bool disk_error = false;
    for (;;) {
        if (atomic_load(&stop_requested)) {
            ok = false;
            break;
        }
        struct pollfd pfd = { .fd = dfd, .events = POLLIN };
        int r = poll(&pfd, 1, FTPD_POLL_MS);
        if (r < 0 && errno != EINTR) {
            ok = false;
            break;
        }
        if (r <= 0)
            continue;
        ssize_t n = recv(dfd, s->buf, FTPD_BUF_SIZE, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0)
            break;
        UINT bw = 0;
        fr = f_write(&f, s->buf, (UINT)n, &bw);
        if (fr != FR_OK || bw != (UINT)n) {
            ok = false;
            disk_error = true;
            break;
        }
        total += (unsigned long long)n;
        set_status("%s: STOR %s %llu KB", s->peer, vpath, total >> 10);
    }
    close(dfd);
    FRESULT cr = f_close(&f);
    set_status("%s: connected", s->peer);
    if (ok && cr == FR_OK)
        reply(s, "226 Transfer complete.");
    else if (disk_error && fr == FR_OK)
        reply(s, "452 Volume full.");
    else if (disk_error || cr != FR_OK)
        reply(s, "451 Disk write error (FatFs %d).", (int)(fr != FR_OK ? fr : cr));
    else
        reply(s, "426 Connection closed; transfer aborted.");
}

/* ------------------------------------------------------------------------
 * Command dispatch
 * ---------------------------------------------------------------------- */

static void cmd_cwd(session_t *s, const char *arg)
{
    char vpath[FTPD_PATH_MAX];
    vp_t vp;
    if (!make_vpath(s, arg, vpath, sizeof(vpath)) || !split_vpath(vpath, &vp)) {
        reply(s, "550 No such directory.");
        return;
    }
    if (vp.kind == VP_ENTRY) {
        FILINFO fno;
        FRESULT fr = f_stat(vp.fat, &fno);
        if (fr != FR_OK || !(fno.fattrib & AM_DIR)) {
            reply(s, "550 No such directory.");
            return;
        }
    }
    snprintf(s->cwd, sizeof(s->cwd), "%s", vpath);
    reply(s, "250 Directory changed to %s.", s->cwd);
}

static void cmd_size_mdtm(session_t *s, const char *arg, bool mdtm)
{
    char vpath[FTPD_PATH_MAX];
    vp_t vp;
    if (!entry_path(s, arg, vpath, &vp))
        return;
    FILINFO fno;
    FRESULT fr = f_stat(vp.fat, &fno);
    if (fr != FR_OK) {
        reply_fresult(s, fr);
        return;
    }
    if (fno.fattrib & AM_DIR) {
        reply(s, "550 Not a regular file.");
        return;
    }
    if (!mdtm) {
        reply(s, "213 %llu", (unsigned long long)fno.fsize);
        return;
    }
    reply(s, "213 %04u%02u%02u%02u%02u%02u",
          1980u + (fno.fdate >> 9), (fno.fdate >> 5) & 15u, fno.fdate & 31u,
          (fno.ftime >> 11) & 31u, (fno.ftime >> 5) & 63u, (fno.ftime & 31u) * 2u);
}

static void cmd_port(session_t *s, const char *arg)
{
    unsigned h[4], p[2];
    if (!arg || sscanf(arg, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3],
                       &p[0], &p[1]) != 6 ||
        h[0] > 255 || h[1] > 255 || h[2] > 255 || h[3] > 255 ||
        p[0] > 255 || p[1] > 255) {
        reply(s, "501 Bad PORT argument.");
        return;
    }
    close_pasv(s);
    memset(&s->port_addr, 0, sizeof(s->port_addr));
    s->port_addr.sin_family = AF_INET;
    s->port_addr.sin_addr.s_addr = htonl(h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3]);
    s->port_addr.sin_port = htons((uint16_t)(p[0] << 8 | p[1]));
    s->have_port = true;
    reply(s, "200 PORT command successful.");
}

static bool write_allowed(session_t *s, const vp_t *vp)
{
    if (vp->kind != VP_ENTRY) {
        reply(s, "550 Permission denied.");
        return false;
    }
    if (volumes[vp->vol].readonly) {
        reply(s, "550 Volume is write-protected.");
        return false;
    }
    return true;
}

static void handle_command(session_t *s, char *line)
{
    char *arg = strchr(line, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ')
            ++arg;
    } else {
        arg = line + strlen(line);
    }
    for (char *c = line; *c; ++c)
        if (*c >= 'a' && *c <= 'z')
            *c = (char)(*c - 'a' + 'A');
    const char *cmd = line;

    if (!strcmp(cmd, "USER")) {
        s->logged_in = false;
        s->user_ok = !strcmp(arg, cfg_user);
        reply(s, "331 Password required.");
        return;
    }
    if (!strcmp(cmd, "PASS")) {
        if (s->user_ok && !strcmp(arg, cfg_password)) {
            s->logged_in = true;
            reply(s, "230 Logged in.");
        } else {
            reply(s, "530 Login incorrect.");
        }
        return;
    }
    if (!strcmp(cmd, "QUIT")) {
        reply(s, "221 Goodbye.");
        return;
    }
    if (!strcmp(cmd, "SYST")) {
        reply(s, "215 UNIX Type: L8");
        return;
    }
    if (!strcmp(cmd, "FEAT")) {
        reply(s, "211-Features:");
        reply(s, " SIZE");
        reply(s, " MDTM");
        reply(s, " REST STREAM");
        reply(s, " EPSV");
        reply(s, " PASV");
        reply(s, " UTF8");
        reply(s, "211 End");
        return;
    }
    if (!strcmp(cmd, "OPTS")) {
        if (!strncasecmp(arg, "UTF8", 4))
            reply(s, "200 Always in UTF8 mode.");
        else
            reply(s, "501 Option not understood.");
        return;
    }
    if (!strcmp(cmd, "NOOP") || !strcmp(cmd, "CLNT")) {
        reply(s, "200 OK.");
        return;
    }
    if (!s->logged_in) {
        reply(s, "530 Please login with USER and PASS.");
        return;
    }

    char vpath[FTPD_PATH_MAX];
    vp_t vp;
    FRESULT fr;

    if (!strcmp(cmd, "PWD") || !strcmp(cmd, "XPWD")) {
        reply(s, "257 \"%s\" is the current directory.", s->cwd);
    } else if (!strcmp(cmd, "CWD") || !strcmp(cmd, "XCWD")) {
        cmd_cwd(s, arg);
    } else if (!strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP")) {
        cmd_cwd(s, "..");
    } else if (!strcmp(cmd, "TYPE")) {
        reply(s, "200 Type set to %c (transfers are binary).", arg[0] ? arg[0] : 'I');
    } else if (!strcmp(cmd, "MODE")) {
        reply(s, (arg[0] == 'S' || arg[0] == 's') ? "200 Mode set to S." : "504 Only stream mode.");
    } else if (!strcmp(cmd, "STRU")) {
        reply(s, (arg[0] == 'F' || arg[0] == 'f') ? "200 Structure set to F." : "504 Only file structure.");
    } else if (!strcmp(cmd, "ALLO")) {
        reply(s, "202 No storage allocation necessary.");
    } else if (!strcmp(cmd, "PASV")) {
        unsigned port;
        if (open_pasv(s, &port) != 0) {
            reply(s, "425 Cannot open passive connection.");
            return;
        }
        uint32_t a = ntohl(s->local.sin_addr.s_addr);
        reply(s, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
              a >> 24, (a >> 16) & 255u, (a >> 8) & 255u, a & 255u, port >> 8, port & 255u);
    } else if (!strcmp(cmd, "EPSV")) {
        unsigned port;
        if (open_pasv(s, &port) != 0) {
            reply(s, "425 Cannot open passive connection.");
            return;
        }
        reply(s, "229 Entering Extended Passive Mode (|||%u|).", port);
    } else if (!strcmp(cmd, "PORT")) {
        cmd_port(s, arg);
    } else if (!strcmp(cmd, "REST")) {
        char *end = NULL;
        unsigned long long v = strtoull(arg, &end, 10);
        if (!arg[0] || (end && *end)) {
            reply(s, "501 Bad REST offset.");
            return;
        }
        s->rest = (FSIZE_t)v;
        reply(s, "350 Restarting at %llu.", v);
    } else if (!strcmp(cmd, "LIST")) {
        cmd_list(s, arg, false);
    } else if (!strcmp(cmd, "NLST")) {
        cmd_list(s, arg, true);
    } else if (!strcmp(cmd, "RETR")) {
        cmd_retr(s, arg);
    } else if (!strcmp(cmd, "STOR") || !strcmp(cmd, "APPE")) {
        if (!arg[0]) {
            reply(s, "501 Missing file name.");
            return;
        }
        if (!make_vpath(s, arg, vpath, sizeof(vpath)) || !split_vpath(vpath, &vp)) {
            reply(s, "550 No such directory.");
            return;
        }
        if (!write_allowed(s, &vp))
            return;
        cmd_stor(s, arg, !strcmp(cmd, "APPE"));
    } else if (!strcmp(cmd, "SIZE")) {
        cmd_size_mdtm(s, arg, false);
    } else if (!strcmp(cmd, "MDTM")) {
        cmd_size_mdtm(s, arg, true);
    } else if (!strcmp(cmd, "DELE") || !strcmp(cmd, "RMD") || !strcmp(cmd, "XRMD") ||
               !strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD")) {
        if (!arg[0]) {
            reply(s, "501 Missing name.");
            return;
        }
        if (!make_vpath(s, arg, vpath, sizeof(vpath)) || !split_vpath(vpath, &vp)) {
            reply(s, "550 No such file or directory.");
            return;
        }
        if (!write_allowed(s, &vp))
            return;
        const bool mkd = cmd[0] == 'M' || cmd[1] == 'M';
        fr = mkd ? f_mkdir(vp.fat) : f_unlink(vp.fat);
        if (fr != FR_OK)
            reply_fresult(s, fr);
        else if (mkd)
            reply(s, "257 \"%s\" created.", vpath);
        else
            reply(s, "250 Deleted.");
    } else if (!strcmp(cmd, "RNFR")) {
        if (!arg[0] || !make_vpath(s, arg, vpath, sizeof(vpath)) ||
            !split_vpath(vpath, &vp)) {
            reply(s, "550 No such file or directory.");
            return;
        }
        if (!write_allowed(s, &vp))
            return;
        FILINFO fno;
        if ((fr = f_stat(vp.fat, &fno)) != FR_OK) {
            reply_fresult(s, fr);
            return;
        }
        snprintf(s->rnfr, sizeof(s->rnfr), "%s", vpath);
        reply(s, "350 Ready for RNTO.");
    } else if (!strcmp(cmd, "RNTO")) {
        vp_t from;
        if (!s->rnfr[0]) {
            reply(s, "503 RNFR required first.");
            return;
        }
        bool have_from = split_vpath(s->rnfr, &from);
        s->rnfr[0] = 0;
        if (!have_from || !arg[0] || !make_vpath(s, arg, vpath, sizeof(vpath)) ||
            !split_vpath(vpath, &vp)) {
            reply(s, "550 No such file or directory.");
            return;
        }
        if (!write_allowed(s, &vp))
            return;
        if (vp.vol != from.vol) {
            reply(s, "553 Cannot rename across volumes.");
            return;
        }
        /* f_rename() takes the new name without its drive prefix. */
        const char *to = strchr(vp.fat, ':');
        fr = f_rename(from.fat, to ? to + 1 : vp.fat);
        if (fr != FR_OK)
            reply_fresult(s, fr);
        else
            reply(s, "250 Renamed.");
    } else if (!strcmp(cmd, "ABOR")) {
        reply(s, "226 No transfer in progress.");
    } else if (!strcmp(cmd, "STAT")) {
        reply(s, "211 orange386 FTP, %d volume(s).", volume_count);
    } else if (!strcmp(cmd, "HELP")) {
        reply(s, "214 USER PASS PWD CWD CDUP LIST NLST RETR STOR APPE DELE MKD RMD "
                 "RNFR RNTO SIZE MDTM REST PASV EPSV PORT TYPE QUIT");
    } else {
        reply(s, "502 Command not implemented.");
    }
}

static void serve_client(int ctrl, const struct sockaddr_in *peer)
{
    session_t *s = calloc(1, sizeof(*s));
    if (!s) {
        close(ctrl);
        return;
    }
    s->buf = malloc(FTPD_BUF_SIZE);
    if (!s->buf) {
        free(s);
        close(ctrl);
        return;
    }
    s->ctrl = ctrl;
    s->pasv_fd = -1;
    snprintf(s->cwd, sizeof(s->cwd), "/");
    inet_ntop(AF_INET, &peer->sin_addr, s->peer, sizeof(s->peer));
    socklen_t ll = sizeof(s->local);
    getsockname(ctrl, (struct sockaddr *)&s->local, &ll);

    set_status("%s: connected", s->peer);
    printf("ftpd: client %s connected\n", s->peer);
    reply(s, "220 orange386 FTP server ready.");

    char line[FTPD_LINE_MAX];
    while (read_line(s, line, sizeof(line))) {
        if (!line[0])
            continue;
        handle_command(s, line);
        if (!strncasecmp(line, "QUIT", 4) && line[4] == 0)
            break;
    }

    close_pasv(s);
    close(ctrl);
    printf("ftpd: client %s disconnected\n", s->peer);
    free(s->buf);
    free(s);
    set_status("Waiting for a client");
}

static void *server_main(void *arg)
{
    (void)arg;
    set_status("Waiting for a client");
    while (!atomic_load(&stop_requested)) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        int r = poll(&pfd, 1, FTPD_POLL_MS);
        if (r <= 0)
            continue;
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int c = accept4(listen_fd, (struct sockaddr *)&peer, &pl, SOCK_CLOEXEC);
        if (c < 0)
            continue;
        serve_client(c, &peer);
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * Start / stop
 * ---------------------------------------------------------------------- */

static void unmount_all(void)
{
    for (int i = 0; i < volume_count; ++i) {
        if (volumes[i].mounted) {
            char drv[4];
            snprintf(drv, sizeof(drv), "%d:", i);
            f_mount(NULL, drv, 0);
            volumes[i].mounted = false;
        }
        imgfs_set_drive((BYTE)i, -1, 0);
        if (volumes[i].fd >= 0) {
            fsync(volumes[i].fd);
            close(volumes[i].fd);
            volumes[i].fd = -1;
        }
    }
}

bool ftpd_start(const ftpd_drive *drives, int count, unsigned port,
                const char *user, const char *password,
                char *err, size_t err_size)
{
    ftpd_stop();
    if (count > FTPD_MAX_VOLUMES)
        count = FTPD_MAX_VOLUMES;

    int mounted = 0;
    volume_count = count;
    for (int i = 0; i < count; ++i) {
        volume_t *v = &volumes[i];
        memset(v, 0, sizeof(*v));
        snprintf(v->name, sizeof(v->name), "%s", drives[i].name);
        v->readonly = drives[i].readonly;
        v->fd = drives[i].fd >= 0 ? fcntl(drives[i].fd, F_DUPFD_CLOEXEC, 0) : -1;
        if (v->fd < 0)
            continue;
        imgfs_set_drive((BYTE)i, v->fd, v->readonly);
        char drv[4];
        snprintf(drv, sizeof(drv), "%d:", i);
        FRESULT fr = f_mount(&v->fs, drv, 1);
        v->mounted = fr == FR_OK;
        if (v->mounted)
            ++mounted;
        else
            printf("ftpd: %s: no FAT volume (FatFs %d)\n", v->name, (int)fr);
    }
    if (!mounted) {
        snprintf(err, err_size, "No FAT volume in the attached images");
        unmount_all();
        return false;
    }

    snprintf(cfg_user, sizeof(cfg_user), "%s", user ? user : "");
    snprintf(cfg_password, sizeof(cfg_password), "%s", password ? password : "");

    listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };
    if (listen_fd < 0 ||
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(listen_fd, 4) != 0) {
        snprintf(err, err_size, "Port %u: %s", port, strerror(errno));
        if (listen_fd >= 0)
            close(listen_fd);
        listen_fd = -1;
        unmount_all();
        return false;
    }

    atomic_store(&stop_requested, false);
    if (pthread_create(&server_thread, NULL, server_main, NULL) != 0) {
        snprintf(err, err_size, "Cannot start the server thread");
        close(listen_fd);
        listen_fd = -1;
        unmount_all();
        return false;
    }
    server_running = true;
    printf("ftpd: serving %d volume(s) on port %u\n", mounted, port);
    return true;
}

void ftpd_stop(void)
{
    if (server_running) {
        atomic_store(&stop_requested, true);
        pthread_join(server_thread, NULL);
        server_running = false;
    }
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    unmount_all();
}
