#include "common.h"
#include <dirent.h>

typedef struct {
    int connection;
    TrackerConfig cfg;
} ClientJob;

static void send_reply(int sock, const char *msg) {
    send_all(sock, msg, strlen(msg));
}

static void handle_list_req(int sock, const TrackerConfig *cfg) {
    DIR *dir = opendir(cfg->torrents_dir);
    struct dirent *entry;
    int total = 0, idx = 1;
    char line[1024], path[PATHBUF];

    if (!dir) {
        send_reply(sock, "REP LIST 0\nREP LIST END\n");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        size_t n = strlen(entry->d_name);
        if (n > 6 && strcmp(entry->d_name + n - 6, ".track") == 0) total++;
    }
    rewinddir(dir);

    snprintf(line, sizeof(line), "REP LIST %d\n", total);
    send_reply(sock, line);

    while ((entry = readdir(dir)) != NULL) {
        char filename[256], md5[33];
        long filesize;
        size_t n = strlen(entry->d_name);
        if (!(n > 6 && strcmp(entry->d_name + n - 6, ".track") == 0)) continue;
        build_path(path, sizeof(path), cfg->torrents_dir, entry->d_name);
        if (parse_tracker_file(path, filename, &filesize, md5, NULL, 0) < 0) continue;
        snprintf(line, sizeof(line), "%d %s %ld %s\n", idx++, filename, filesize, md5);
        send_reply(sock, line);
    }
    closedir(dir);
    send_reply(sock, "REP LIST END\n");
}

static void handle_createtracker_req(int sock, const TrackerConfig *cfg, char *raw_cmd) {
    char filename[256], description[256], md5[128], peer_ip[64], track_path[PATHBUF];
    long filesize;
    int peer_port;
    FILE *fp;

    if (sscanf(raw_cmd, "createtracker %255s %ld %255s %127s %63s %d",
               filename, &filesize, description, md5, peer_ip, &peer_port) != 6) {
        send_reply(sock, "createtracker fail\n");
        return;
    }

    snprintf(track_path, sizeof(track_path), "%s/%s.track", cfg->torrents_dir, filename);
    if (access(track_path, F_OK) == 0) {
        send_reply(sock, "createtracker ferr\n");
        return;
    }

    fp = fopen(track_path, "w");
    if (!fp) {
        send_reply(sock, "createtracker fail\n");
        return;
    }

    fprintf(fp, "Filename: %s\n", filename);
    fprintf(fp, "Filesize: %ld\n", filesize);
    fprintf(fp, "Description: %s\n", description);
    fprintf(fp, "MD5: %s\n", md5);
    fprintf(fp, "#list of peers follows next\n");
    fprintf(fp, "%s:%d:%d:%ld:%ld\n",
            peer_ip, peer_port, 0, filesize > 0 ? filesize - 1 : 0, (long)time(NULL));
    fclose(fp);

    send_reply(sock, "createtracker succ\n");
}

static void handle_updatetracker_req(int sock, const TrackerConfig *cfg, char *raw_cmd) {
    char filename[256], peer_ip[64], track_path[PATHBUF], tmp_path[PATHBUF], line[512], out[256];
    long start_byte, end_byte, now = (long)time(NULL);
    int peer_port;
    int timeout = cfg->peer_timeout_seconds > 0 ? cfg->peer_timeout_seconds : 900;
    int found = 0, in_peers = 0;
    FILE *in, *outf;

    if (sscanf(raw_cmd, "updatetracker %255s %ld %ld %63s %d",
               filename, &start_byte, &end_byte, peer_ip, &peer_port) != 5) {
        send_reply(sock, "updatetracker fail\n");
        return;
    }

    snprintf(track_path, sizeof(track_path), "%s/%s.track", cfg->torrents_dir, filename);
    if (access(track_path, F_OK) != 0) {
        snprintf(out, sizeof(out), "updatetracker %s ferr\n", filename);
        send_reply(sock, out);
        return;
    }

    in = fopen(track_path, "r");
    if (!in) {
        snprintf(out, sizeof(out), "updatetracker %s fail\n", filename);
        send_reply(sock, out);
        return;
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", track_path);
    outf = fopen(tmp_path, "w");
    if (!outf) {
        fclose(in);
        snprintf(out, sizeof(out), "updatetracker %s fail\n", filename);
        send_reply(sock, out);
        return;
    }

    while (fgets(line, sizeof(line), in)) {
        char eip[64];
        int eport;
        long es, ee, ets;

        trim_newline(line);
        if (!in_peers) {
            fputs(line, outf);
            fputc('\n', outf);
            if (line[0] == '#') in_peers = 1;
            continue;
        }

        if (sscanf(line, "%63[^:]:%d:%ld:%ld:%ld", eip, &eport, &es, &ee, &ets) != 5) continue;
        if (now - ets > timeout) continue;

        if (strcmp(eip, peer_ip) == 0 && eport == peer_port) {
            fprintf(outf, "%s:%d:%ld:%ld:%ld\n", peer_ip, peer_port, start_byte, end_byte, now);
            found = 1;
        } else {
            fprintf(outf, "%s:%d:%ld:%ld:%ld\n", eip, eport, es, ee, ets);
        }
    }

    if (!in_peers) fputs("#list of peers follows next\n", outf);
    if (!found) fprintf(outf, "%s:%d:%ld:%ld:%ld\n", peer_ip, peer_port, start_byte, end_byte, now);

    fclose(in);
    fclose(outf);
    rename(tmp_path, track_path);

    snprintf(out, sizeof(out), "updatetracker %s succ\n", filename);
    send_reply(sock, out);
}

static void handle_get_req(int sock, const TrackerConfig *cfg, const char *track_filename) {
    char full_path[PATHBUF], line[MAXLINE], md5[128] = "unknown";
    FILE *fp;

    snprintf(full_path, sizeof(full_path), "%s/%s", cfg->torrents_dir, track_filename);
    fp = fopen(full_path, "r");
    if (!fp) {
        send_reply(sock, "REP GET BEGIN\nERROR file_not_found\nREP GET END unknown\n");
        return;
    }

    send_reply(sock, "REP GET BEGIN\n");
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MD5: ", 5) == 0) {
            strncpy(md5, line + 5, sizeof(md5) - 1);
            trim_newline(md5);
        }
        send_reply(sock, line);
    }
    fclose(fp);

    snprintf(line, sizeof(line), "REP GET END %s\n", md5);
    send_reply(sock, line);
}

static void peer_handler(int sock, const TrackerConfig *cfg) {
    char buf[MAXLINE], cmd[64];
    if (recv_line(sock, buf, sizeof(buf)) <= 0) return;
    trim_newline(buf);
    printf("[tracker] received: %s\n", buf);

    if (strcmp(buf, "REQ LIST") == 0) {
        handle_list_req(sock, cfg);
        return;
    }

    if (sscanf(buf, "%63s", cmd) != 1) {
        send_reply(sock, "invalid command\n");
        return;
    }

    if (strcmp(cmd, "createtracker") == 0) {
        handle_createtracker_req(sock, cfg, buf);
    } else if (strcmp(cmd, "updatetracker") == 0) {
        handle_updatetracker_req(sock, cfg, buf);
    } else if (strcmp(cmd, "GET") == 0) {
        char track_name[256];
        if (sscanf(buf, "%*s %255s", track_name) == 1) handle_get_req(sock, cfg, track_name);
        else send_reply(sock, "GET invalid\n");
    } else {
        send_reply(sock, "invalid command\n");
    }
}

static void *handle_client_connection(void *arg) {
    ClientJob *job = (ClientJob *)arg;
    peer_handler(job->connection, &job->cfg);
    close(job->connection);
    free(job);
    return NULL;
}

int main(int argc, char **argv) {
    TrackerConfig cfg;
    int listen_sock, conn_sock, reuse = 1;
    struct sockaddr_in bind_addr, peer_addr;
    socklen_t peer_len;
    const char *config_path = (argc > 1) ? argv[1] : "config/sconfig";

    if (load_tracker_config(config_path, &cfg) != 0) {
        fprintf(stderr, "failed to load tracker config: %s\n", config_path);
        return 1;
    }
    ensure_dir(cfg.torrents_dir);

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) return 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons((uint16_t)cfg.listen_port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) return 1;
    if (listen(listen_sock, 10) < 0) return 1;

    printf("[tracker] listening on port %d, dir=%s\n", cfg.listen_port, cfg.torrents_dir);

    while (1) {
        pthread_t worker;
        ClientJob *job;
        peer_len = sizeof(peer_addr);
        conn_sock = accept(listen_sock, (struct sockaddr *)&peer_addr, &peer_len);
        if (conn_sock < 0) continue;

        printf("server: got connection from %s\n", inet_ntoa(peer_addr.sin_addr));
        job = (ClientJob *)malloc(sizeof(ClientJob));
        if (!job) {
            close(conn_sock);
            continue;
        }
        job->connection = conn_sock;
        job->cfg = cfg;
        pthread_create(&worker, NULL, handle_client_connection, job);
        pthread_detach(worker);
    }
}
