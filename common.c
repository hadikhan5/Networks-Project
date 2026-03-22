#include "common.h"

/* Read next non-empty config line. */
static int read_nonempty_line(FILE *fp, char *buf, size_t n) {
    while (fgets(buf, (int)n, fp)) {
        trim_newline(buf);
        if (strlen(buf) == 0) continue;
        return 1;
    }
    return 0;
}

void trim_newline(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

/* Determine which local IPv4 address is used for reaching a remote endpoint. */
int get_local_ip_for_remote(const char *remote_ip, int remote_port, char *out_ip, size_t out_len) {
    int sock;
    struct sockaddr_in remote, local;
    socklen_t local_len = (socklen_t)sizeof(local);

    if (!remote_ip || !out_ip || out_len == 0 || remote_port <= 0) return -1;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons((uint16_t)remote_port);
    if (inet_pton(AF_INET, remote_ip, &remote.sin_addr) <= 0) {
        close(sock);
        return -1;
    }

    /* UDP connect is enough to let the OS pick the outbound interface/address. */
    if (connect(sock, (struct sockaddr *)&remote, sizeof(remote)) != 0) {
        close(sock);
        return -1;
    }

    memset(&local, 0, sizeof(local));
    if (getsockname(sock, (struct sockaddr *)&local, &local_len) != 0) {
        close(sock);
        return -1;
    }

    if (!inet_ntop(AF_INET, &local.sin_addr, out_ip, (socklen_t)out_len)) {
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}

int load_tracker_config(const char *path, TrackerConfig *cfg) {
    FILE *fp = fopen(path, "r");
    char line[PATHBUF];
    if (!fp || !cfg) return -1;
    if (!read_nonempty_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    cfg->port = atoi(line);
    if (!read_nonempty_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    strncpy(cfg->torrents_dir, line, sizeof(cfg->torrents_dir) - 1);
    cfg->torrents_dir[sizeof(cfg->torrents_dir) - 1] = '\0';
    fclose(fp);
    return 0;
}

int load_peer_client_config(const char *path, PeerClientConfig *cfg) {
    FILE *fp = fopen(path, "r");
    char line[PATHBUF];
    if (!fp || !cfg) return -1;
    if (!read_nonempty_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    strncpy(cfg->tracker_ip, line, sizeof(cfg->tracker_ip) - 1);
    cfg->tracker_ip[sizeof(cfg->tracker_ip) - 1] = '\0';
    if (!read_nonempty_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    cfg->tracker_port = atoi(line);
    if (!read_nonempty_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    cfg->refresh_interval = atoi(line);
    fclose(fp);
    return 0;
}

int load_peer_server_config(const char *path, PeerServerConfig *cfg) {
    FILE *fp = fopen(path, "r");
    char line[PATHBUF];
    if (!fp || !cfg) return -1;
    if (!read_nonempty_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    cfg->listen_port = atoi(line);
    if (!read_nonempty_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    strncpy(cfg->shared_dir, line, sizeof(cfg->shared_dir) - 1);
    cfg->shared_dir[sizeof(cfg->shared_dir) - 1] = '\0';
    fclose(fp);
    return 0;
}

int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return mkdir(path, 0777);
}

int send_all(int sock, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sock, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

int recv_line(int sock, char *buf, size_t maxlen) {
    size_t i = 0;
    char c;
    while (i < maxlen - 1) {
        ssize_t n = recv(sock, &c, 1, 0);
        if (n == 0) break;
        if (n < 0) return -1;
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return (int)i;
}

long get_file_size(const char *path) {
    FILE *fp = fopen(path, "rb");
    long size;
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    size = ftell(fp);
    fclose(fp);
    return size;
}

void build_path(char *out, size_t n, const char *dir, const char *file) {
    snprintf(out, n, "%s/%s", dir, file);
}

int starts_with_ci(const char *s, const char *prefix) {
    size_t i;
    if (!s || !prefix) return 0;
    for (i = 0; prefix[i]; i++) {
        char a = s[i], b = prefix[i];
        if (!a) return 0;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}
