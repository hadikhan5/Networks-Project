#include "common.h"

/* ── Internal helper ───────────────────────────────────────────── */

/* Read the next non-blank line from a config file into buf.
   Skips empty lines so callers don't have to handle them.
   Returns 1 if a line was read, 0 at EOF. */
static int read_next_config_line(FILE *fp, char *buf, size_t buf_size) {
    while (fgets(buf, (int)buf_size, fp)) {
        trim_newline(buf);
        if (strlen(buf) == 0) continue;   /* skip blank lines */
        return 1;
    }
    return 0;   /* nothing left to read */
}

/* ── Public utilities ──────────────────────────────────────────── */

void trim_newline(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    /* Walk backward and remove any CR or LF characters. */
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

/* Determine which local IPv4 address the OS picks when routing to a remote host.
   We create a UDP socket, "connect" it (no packets sent — just sets routing),
   then ask getsockname() which local address the kernel chose. */
int get_local_ip_for_remote(const char *remote_ip, int remote_port,
                             char *out_ip, size_t out_len) {
    int probe_sock;
    struct sockaddr_in remote_addr, local_addr;
    socklen_t local_len = (socklen_t)sizeof(local_addr);

    if (!remote_ip || !out_ip || out_len == 0 || remote_port <= 0) return -1;

    probe_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe_sock < 0) return -1;

    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port   = htons((uint16_t)remote_port);
    if (inet_pton(AF_INET, remote_ip, &remote_addr.sin_addr) <= 0) {
        close(probe_sock);
        return -1;
    }

    /* Calling connect() on a UDP socket just sets the default destination —
       no actual packet is sent.  The kernel must choose an outbound interface,
       which means it fills in the source address we can then read back. */
    if (connect(probe_sock, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0) {
        close(probe_sock);
        return -1;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(probe_sock, (struct sockaddr *)&local_addr, &local_len) != 0) {
        close(probe_sock);
        return -1;
    }

    if (!inet_ntop(AF_INET, &local_addr.sin_addr, out_ip, (socklen_t)out_len)) {
        close(probe_sock);
        return -1;
    }

    close(probe_sock);
    return 0;
}

/* ── Config loaders ────────────────────────────────────────────── */

/* Tracker config file format (one value per line):
     <port>
     <torrents_dir> */
int load_tracker_config(const char *path, TrackerConfig *cfg) {
    FILE *fp = fopen(path, "r");
    char line[PATHBUF];
    if (!fp || !cfg) return -1;

    if (!read_next_config_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    cfg->listen_port = atoi(line);

    if (!read_next_config_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    strncpy(cfg->torrents_dir, line, sizeof(cfg->torrents_dir) - 1);
    cfg->torrents_dir[sizeof(cfg->torrents_dir) - 1] = '\0';

    fclose(fp);
    return 0;
}

/* Peer client config file format:
     <tracker_ip>
     <tracker_port>
     <refresh_interval_seconds> */
int load_peer_client_config(const char *path, PeerClientConfig *cfg) {
    FILE *fp = fopen(path, "r");
    char line[PATHBUF];
    if (!fp || !cfg) return -1;

    if (!read_next_config_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    strncpy(cfg->tracker_ip, line, sizeof(cfg->tracker_ip) - 1);
    cfg->tracker_ip[sizeof(cfg->tracker_ip) - 1] = '\0';

    if (!read_next_config_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    cfg->tracker_port = atoi(line);

    if (!read_next_config_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    cfg->refresh_interval = atoi(line);

    fclose(fp);
    return 0;
}

/* Peer server config file format:
     <listen_port>
     <shared_dir> */
int load_peer_server_config(const char *path, PeerServerConfig *cfg) {
    FILE *fp = fopen(path, "r");
    char line[PATHBUF];
    if (!fp || !cfg) return -1;

    if (!read_next_config_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    cfg->listen_port = atoi(line);

    if (!read_next_config_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    strncpy(cfg->shared_dir, line, sizeof(cfg->shared_dir) - 1);
    cfg->shared_dir[sizeof(cfg->shared_dir) - 1] = '\0';

    fclose(fp);
    return 0;
}

/* ── Socket / file helpers ─────────────────────────────────────── */

/* Create a directory at `path` if it doesn't exist yet. */
int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;   /* already exists */
    }
    return mkdir(path, 0777);
}

/* Keep calling send() until every byte in buf has been delivered,
   because a single send() call may not write the whole buffer. */
int send_all(int sock, const char *buf, size_t len) {
    size_t bytes_sent = 0;
    while (bytes_sent < len) {
        ssize_t written = send(sock, buf + bytes_sent, len - bytes_sent, 0);
        if (written <= 0) return -1;
        bytes_sent += (size_t)written;
    }
    return 0;
}

/* Read one character at a time until we hit '\n' or fill the buffer.
   This lets callers process one text line per call without knowing
   how the data is framed in the TCP stream. */
int recv_line(int sock, char *buf, size_t maxlen) {
    size_t i = 0;
    char c;
    while (i < maxlen - 1) {
        ssize_t n = recv(sock, &c, 1, 0);
        if (n == 0) break;    /* connection closed cleanly */
        if (n < 0) return -1; /* socket error              */
        buf[i++] = c;
        if (c == '\n') break; /* got a full line           */
    }
    buf[i] = '\0';
    return (int)i;
}

/* Open the file and seek to the end to find its size. */
long get_file_size(const char *path) {
    FILE *fp = fopen(path, "rb");
    long size;
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    size = ftell(fp);
    fclose(fp);
    return size;
}

/* Safely build a full path string: out = "<dir>/<file>". */
void build_path(char *out, size_t n, const char *dir, const char *file) {
    snprintf(out, n, "%s/%s", dir, file);
}

/* Returns 1 if `s` begins with `prefix`, ignoring ASCII case. */
int starts_with_ci(const char *s, const char *prefix) {
    size_t i;
    if (!s || !prefix) return 0;
    for (i = 0; prefix[i]; i++) {
        char a = s[i], b = prefix[i];
        if (!a) return 0;
        /* Fold upper-case to lower-case for comparison. */
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}