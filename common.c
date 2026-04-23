#include "common.h"

static int read_next_config_line(FILE *fp, char *buf, size_t n) {
    while (fgets(buf, (int)n, fp)) {
        trim_newline(buf);
        if (buf[0]) return 1;
    }
    return 0;
}

void trim_newline(char *s) {
    size_t len;
    if (!s) return;
    len = strlen(s);
    while (len && (s[len - 1] == '\n' || s[len - 1] == '\r')) s[--len] = '\0';
}

int get_local_ip_for_remote(const char *remote_ip, int remote_port, char *out_ip, size_t out_len) {
    int sock;
    struct sockaddr_in remote_addr, local_addr;
    socklen_t local_len = (socklen_t)sizeof(local_addr);

    if (!remote_ip || !out_ip || out_len == 0 || remote_port <= 0) return -1;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons((uint16_t)remote_port);
    if (inet_pton(AF_INET, remote_ip, &remote_addr.sin_addr) <= 0 ||
        connect(sock, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0) {
        close(sock);
        return -1;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(sock, (struct sockaddr *)&local_addr, &local_len) != 0 ||
        !inet_ntop(AF_INET, &local_addr.sin_addr, out_ip, (socklen_t)out_len)) {
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

    if (!read_next_config_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    cfg->listen_port = atoi(line);

    if (!read_next_config_line(fp, line, sizeof(line))) { fclose(fp); return -1; }
    strncpy(cfg->torrents_dir, line, sizeof(cfg->torrents_dir) - 1);
    cfg->torrents_dir[sizeof(cfg->torrents_dir) - 1] = '\0';

    cfg->peer_timeout_seconds = 900;
    if (read_next_config_line(fp, line, sizeof(line))) cfg->peer_timeout_seconds = atoi(line);

    fclose(fp);
    return 0;
}

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

int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    return mkdir(path, 0777);
}

int send_all(int sock, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
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

void compute_md5_file(const char *path, char out_hex[33]) {
    char cmd[700];
    FILE *fp;
    snprintf(cmd, sizeof(cmd),
             "{ md5sum \"%s\" 2>/dev/null || md5 -q \"%s\" 2>/dev/null; } | awk '{print $1}'",
             path, path);
    fp = popen(cmd, "r");
    if (!fp || fscanf(fp, "%32s", out_hex) != 1) strncpy(out_hex, "unknown", 33);
    if (fp) pclose(fp);
}

int parse_tracker_file(const char *path,
                      char filename_out[256], long *filesize_out,
                      char md5_out[33],
                      PeerEntry *peers, int max_peers) {
    FILE *fp = fopen(path, "r");
    char line[512];
    int n = 0;

    if (!fp) return -1;
    if (filename_out) filename_out[0] = '\0';
    if (md5_out) strncpy(md5_out, "unknown", 33);
    if (filesize_out) *filesize_out = 0;

    while (fgets(line, sizeof(line), fp)) {
        trim_newline(line);
        if (line[0] == '#' || !line[0]) continue;
        if (strncmp(line, "Filename: ", 10) == 0) {
            if (filename_out) strncpy(filename_out, line + 10, 255);
        } else if (strncmp(line, "Filesize: ", 10) == 0) {
            if (filesize_out) *filesize_out = atol(line + 10);
        } else if (strncmp(line, "MD5: ", 5) == 0) {
            if (md5_out) strncpy(md5_out, line + 5, 32);
        } else if (peers && n < max_peers) {
            if (sscanf(line, "%63[^:]:%d:%ld:%ld:%ld",
                       peers[n].ip, &peers[n].port,
                       &peers[n].start_byte, &peers[n].end_byte, &peers[n].timestamp) == 5) {
                n++;
            }
        }
    }

    fclose(fp);
    return n;
}
