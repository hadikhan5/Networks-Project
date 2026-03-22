#include "common.h"
#include <dirent.h>

typedef struct {
    int sock;
    TrackerConfig cfg;
} ClientArgs;

/* Small helper to keep all tracker replies in one style. */
static void send_simple(int sock, const char *msg) {
    send_all(sock, msg, strlen(msg));
}

/* Handle LIST protocol reply. */
static void handle_list(int sock, const TrackerConfig *cfg) {
    DIR *dir = opendir(cfg->torrents_dir);
    struct dirent *ent;
    int count = 0, idx = 1;
    char lines[8192] = "";
    char line[1024], path[PATHBUF];
    if (!dir) {
        send_simple(sock, "REP LIST 0\nREP LIST END\n");
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len > 6 && strcmp(ent->d_name + len - 6, ".track") == 0) count++;
    }
    rewinddir(dir);
    snprintf(line, sizeof(line), "REP LIST %d\n", count);
    strncat(lines, line, sizeof(lines) - strlen(lines) - 1);
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        FILE *fp;
        char filename[256] = "", filesize[64] = "", md5[128] = "";
        if (!(len > 6 && strcmp(ent->d_name + len - 6, ".track") == 0)) continue;
        build_path(path, sizeof(path), cfg->torrents_dir, ent->d_name);
        fp = fopen(path, "r");
        if (!fp) continue;
        while (fgets(line, sizeof(line), fp)) {
            trim_newline(line);
            if (strncmp(line, "Filename: ", 10) == 0) strncpy(filename, line + 10, sizeof(filename) - 1);
            else if (strncmp(line, "Filesize: ", 10) == 0) strncpy(filesize, line + 10, sizeof(filesize) - 1);
            else if (strncmp(line, "MD5: ", 5) == 0) strncpy(md5, line + 5, sizeof(md5) - 1);
        }
        fclose(fp);
        snprintf(line, sizeof(line), "%d %s %s %s\n", idx++, filename, filesize[0] ? filesize : "0", md5[0] ? md5 : "dummy_md5");
        strncat(lines, line, sizeof(lines) - strlen(lines) - 1);
    }
    closedir(dir);
    strncat(lines, "REP LIST END\n", sizeof(lines) - strlen(lines) - 1);
    send_simple(sock, lines);
}

/* Handle createtracker protocol command. */
static void handle_createtracker(int sock, const TrackerConfig *cfg, char *cmd) {
    char filename[256], description[256], md5[128], ip[64];
    long filesize;
    int port;
    char path[PATHBUF];
    FILE *fp;
    if (sscanf(cmd, "createtracker %255s %ld %255s %127s %63s %d", filename, &filesize, description, md5, ip, &port) != 6) {
        send_simple(sock, "createtracker fail\n");
        return;
    }
    snprintf(path, sizeof(path), "%s/%s.track", cfg->torrents_dir, filename);
    if (access(path, F_OK) == 0) {
        send_simple(sock, "createtracker ferr\n");
        return;
    }
    fp = fopen(path, "w");
    if (!fp) {
        send_simple(sock, "createtracker fail\n");
        return;
    }
    fprintf(fp, "Filename: %s\n", filename);
    fprintf(fp, "Filesize: %ld\n", filesize);
    fprintf(fp, "Description: %s\n", description);
    fprintf(fp, "MD5: %s\n", md5);
    fprintf(fp, "#list of peers follows next\n");
    fprintf(fp, "%s:%d:%d:%ld:%ld\n", ip, port, 0, filesize > 0 ? filesize - 1 : 0, (long)time(NULL));
    fclose(fp);
    send_simple(sock, "createtracker succ\n");
}

/* Handle updatetracker protocol command. */
static void handle_updatetracker(int sock, const TrackerConfig *cfg, char *cmd) {
    char filename[256], ip[64], path[PATHBUF];
    long startb, endb;
    int port;
    FILE *fp;
    if (sscanf(cmd, "updatetracker %255s %ld %ld %63s %d", filename, &startb, &endb, ip, &port) != 5) {
        send_simple(sock, "updatetracker fail\n");
        return;
    }
    snprintf(path, sizeof(path), "%s/%s.track", cfg->torrents_dir, filename);
    if (access(path, F_OK) != 0) {
        snprintf(path, sizeof(path), "updatetracker %s ferr\n", filename);
        send_simple(sock, path);
        return;
    }
    fp = fopen(path, "a");
    if (!fp) {
        snprintf(path, sizeof(path), "updatetracker %s fail\n", filename);
        send_simple(sock, path);
        return;
    }
    fprintf(fp, "%s:%d:%ld:%ld:%ld\n", ip, port, startb, endb, (long)time(NULL));
    fclose(fp);
    snprintf(path, sizeof(path), "updatetracker %s succ\n", filename);
    send_simple(sock, path);
}

static void handle_get_track(int sock, const TrackerConfig *cfg, const char *trackname) {
    char path[PATHBUF], line[MAXLINE];
    char md5[128] = "dummy_md5";
    FILE *fp;
    snprintf(path, sizeof(path), "%s/%s", cfg->torrents_dir, trackname);
    fp = fopen(path, "r");
    if (!fp) {
        send_simple(sock, "REP GET BEGIN\nERROR file_not_found\nREP GET END dummy_md5\n");
        return;
    }
    send_simple(sock, "REP GET BEGIN\n");
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MD5: ", 5) == 0) {
            char *v = line + 5;
            trim_newline(v);
            strncpy(md5, v, sizeof(md5) - 1);
            md5[sizeof(md5) - 1] = '\0';
            strncat(md5, "", 1);
        }
        send_simple(sock, line);
    }
    fclose(fp);
    snprintf(line, sizeof(line), "REP GET END %s\n", md5[0] ? md5 : "dummy_md5");
    send_simple(sock, line);
}

/* Worker thread: one request per connection, then close. */
static void *client_thread(void *arg) {
    ClientArgs *c = (ClientArgs *)arg;
    char buf[MAXLINE];
    int n = recv_line(c->sock, buf, sizeof(buf));
    if (n > 0) {
        trim_newline(buf);
        printf("[tracker] received: %s\n", buf);
        if (strcmp(buf, "REQ LIST") == 0 || strcmp(buf, "<REQ LIST>") == 0) {
            handle_list(c->sock, &c->cfg);
        } else if (starts_with_ci(buf, "createtracker ")) {
            handle_createtracker(c->sock, &c->cfg, buf);
        } else if (starts_with_ci(buf, "updatetracker ")) {
            handle_updatetracker(c->sock, &c->cfg, buf);
        } else if (starts_with_ci(buf, "GET ")) {
            char fname[256];
            if (sscanf(buf, "GET %255s", fname) == 1) handle_get_track(c->sock, &c->cfg, fname);
            else send_simple(c->sock, "GET invalid\n");
        } else {
            send_simple(c->sock, "invalid command\n");
        }
    }
    close(c->sock);
    free(c);
    return NULL;
}

int main(int argc, char **argv) {
    TrackerConfig cfg;
    int sockfd, clientfd, opt = 1;
    struct sockaddr_in addr, client;
    socklen_t clen = sizeof(client);

    const char *cfgpath = (argc > 1) ? argv[1] : "config/sconfig";
    if (load_tracker_config(cfgpath, &cfg) != 0) {
        fprintf(stderr, "failed to load tracker config: %s\n", cfgpath);
        return 1;
    }
    ensure_dir(cfg.torrents_dir);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(sockfd, 10) < 0) {
        perror("listen");
        return 1;
    }

    printf("[tracker] listening on port %d, dir=%s\n", cfg.port, cfg.torrents_dir);

    while (1) {
        pthread_t tid;
        ClientArgs *args;
        clientfd = accept(sockfd, (struct sockaddr *)&client, &clen);
        if (clientfd < 0) continue;
        args = (ClientArgs *)malloc(sizeof(ClientArgs));
        args->sock = clientfd;
        args->cfg = cfg;
        pthread_create(&tid, NULL, client_thread, args);
        pthread_detach(tid);
    }
    return 0;
}
