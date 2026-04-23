#include "common.h"
#include <dirent.h>
#include <fcntl.h>

typedef struct {
    int connection;
    PeerServerConfig server_cfg;
} FileServeJob;

typedef struct {
    char peer_ip[64];
    int peer_port;
    char filename[256];
    long offset;
    long length;
    char save_path[PATHBUF];
    int success;
} ChunkJob;

typedef struct {
    char peer_name[64];
    PeerClientConfig tracker_info;
    PeerServerConfig server_cfg;
} PeriodicCtx;

static PeriodicCtx g_periodic;
static PeerServerConfig g_serve_cfg;
static char g_serve_name[64];

static void usage(void) {
    printf("Usage: ./peer <name> <client_cfg> <server_cfg> (serve|autodownload|list|createtracker|updatetracker|gettrack) ...\n");
}

static int open_tcp_connection(const char *ip, int port) {
    int sock;
    struct sockaddr_in addr;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
        connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void send_tracker_request(const PeerClientConfig *tracker_info, const char *msg) {
    int sock = open_tcp_connection(tracker_info->tracker_ip, tracker_info->tracker_port);
    char buf[MAXLINE];
    int n;
    if (sock < 0) {
        printf("could not connect to tracker\n");
        return;
    }
    if (send_all(sock, msg, strlen(msg)) != 0) {
        close(sock);
        return;
    }
    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    close(sock);
}

static void register_file_with_tracker(const char *peer_name,
                                       const PeerClientConfig *tracker_info,
                                       const PeerServerConfig *server_cfg,
                                       const char *filename,
                                       const char *description) {
    char local_filepath[PATHBUF], msg[MAXLINE], my_ip[64], md5[33];
    long filesize;
    build_path(local_filepath, sizeof(local_filepath), server_cfg->shared_dir, filename);
    filesize = get_file_size(local_filepath);
    if (filesize < 0) {
        printf("%s: file not found in shared dir: %s\n", peer_name, local_filepath);
        return;
    }
    strncpy(my_ip, "127.0.0.1", 63);
    my_ip[63] = '\0';
    if (get_local_ip_for_remote(tracker_info->tracker_ip, tracker_info->tracker_port, my_ip, sizeof(my_ip)) != 0) {
        strncpy(my_ip, "127.0.0.1", 63);
        my_ip[63] = '\0';
    }
    compute_md5_file(local_filepath, md5);
    snprintf(msg, sizeof(msg), "createtracker %s %ld %s %s %s %d\n",
             filename, filesize, description, md5, my_ip, server_cfg->listen_port);
    printf("%s: %s", peer_name, msg);
    send_tracker_request(tracker_info, msg);
}

static void download_file_from_track(const char *peer_name,
                                     const PeerClientConfig *tracker_info,
                                     const PeerServerConfig *server_cfg,
                                     const char *track_path);

static void fetch_tracker_file(const char *peer_name,
                               const PeerClientConfig *tracker_info,
                               const PeerServerConfig *server_cfg,
                               const char *track_filename) {
    int sock;
    int got_begin = 0;
    char req[MAXLINE], line[MAXLINE], end_md5[64] = "", got_md5[33];
    char save_path[PATHBUF], tmp_path[PATHBUF];
    FILE *fp;

    sock = open_tcp_connection(tracker_info->tracker_ip, tracker_info->tracker_port);
    if (sock < 0) {
        printf("%s: could not connect to tracker\n", peer_name);
        return;
    }

    snprintf(req, sizeof(req), "GET %s\n", track_filename);
    printf("%s: GET %s\n", peer_name, track_filename);
    send_all(sock, req, strlen(req));

    build_path(save_path, sizeof(save_path), server_cfg->shared_dir, track_filename);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", save_path);
    fp = fopen(tmp_path, "w");
    if (!fp) {
        close(sock);
        return;
    }

    while (recv_line(sock, line, sizeof(line)) > 0) {
        if (strncmp(line, "REP GET BEGIN", 13) == 0) {
            got_begin = 1;
            continue;
        }
        if (strncmp(line, "REP GET END", 11) == 0) {
            sscanf(line, "REP GET END %63s", end_md5);
            break;
        }
        if (got_begin) fputs(line, fp);
    }

    fclose(fp);
    close(sock);

    if (!got_begin) {
        remove(tmp_path);
        printf("%s: bad GET reply for %s\n", peer_name, track_filename);
        return;
    }

    compute_md5_file(tmp_path, got_md5);
    if (end_md5[0] && strcmp(end_md5, "unknown") != 0 && strcmp(got_md5, end_md5) != 0) {
        remove(tmp_path);
        printf("%s: tracker MD5 mismatch for %s\n", peer_name, track_filename);
        return;
    }

    rename(tmp_path, save_path);
    printf("%s: saved tracker file to %s\n", peer_name, save_path);
    download_file_from_track(peer_name, tracker_info, server_cfg, save_path);
}

static void *download_chunk_thread(void *arg) {
    ChunkJob *job = (ChunkJob *)arg;
    char req[MAXLINE], buf[MAX_CHUNK];
    int sock, n, total = 0, fd;

    job->success = 0;
    sock = open_tcp_connection(job->peer_ip, job->peer_port);
    if (sock < 0) return NULL;

    snprintf(req, sizeof(req), "GETFILE %s %ld %ld\n", job->filename, job->offset, job->length);
    if (send_all(sock, req, strlen(req)) != 0) {
        close(sock);
        return NULL;
    }

    while (total < (int)job->length) {
        n = recv(sock, buf + total, (int)job->length - total, 0);
        if (n <= 0) break;
        total += n;
    }
    close(sock);

    if (total <= 0) return NULL;
    fd = open(job->save_path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return NULL;
    if (pwrite(fd, buf, (size_t)total, job->offset) >= 0) job->success = 1;
    close(fd);
    return NULL;
}

static int pick_best_peer(const PeerEntry *peers, int np, long offset) {
    int best = -1;
    long best_ts = -1;
    for (int i = 0; i < np; i++) {
        if (peers[i].end_byte >= offset && peers[i].timestamp > best_ts) {
            best = i;
            best_ts = peers[i].timestamp;
        }
    }
    return best;
}

static void download_file_from_track(const char *peer_name,
                                     const PeerClientConfig *tracker_info,
                                     const PeerServerConfig *server_cfg,
                                     const char *track_path) {
    char filename[256], md5[33], save_path[PATHBUF], my_ip[64];
    PeerEntry peers[MAX_PEERS];
    long filesize = 0, already, offset;
    int np, n;

    np = parse_tracker_file(track_path, filename, &filesize, md5, peers, MAX_PEERS);
    if (np <= 0 || filesize <= 0) {
        printf("%s: bad tracker file %s\n", peer_name, track_path);
        return;
    }

    build_path(save_path, sizeof(save_path), server_cfg->shared_dir, filename);
    already = get_file_size(save_path);
    if (already < 0) already = 0;
    if (already >= filesize) {
        printf("%s: %s already complete\n", peer_name, filename);
        remove(track_path);
        return;
    }

    printf("%s: starting download of %s (%ld bytes)\n", peer_name, filename, filesize);
    {
        int fd = open(save_path, O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) {
            ftruncate(fd, filesize);
            close(fd);
        }
    }

    strncpy(my_ip, "127.0.0.1", 63);
    my_ip[63] = '\0';
    if (get_local_ip_for_remote(tracker_info->tracker_ip, tracker_info->tracker_port, my_ip, sizeof(my_ip)) != 0) {
        strncpy(my_ip, "127.0.0.1", 63);
        my_ip[63] = '\0';
    }

    offset = already;
    while (offset < filesize) {
        pthread_t tids[8];
        ChunkJob jobs[8];
        n = 0;

        while (n < 8 && offset < filesize) {
            long chunk = filesize - offset;
            int best;
            if (chunk > MAX_CHUNK) chunk = MAX_CHUNK;

            best = pick_best_peer(peers, np, offset);
            if (best < 0) {
                offset += chunk;
                continue;
            }

            strncpy(jobs[n].peer_ip, peers[best].ip, 63);
            jobs[n].peer_ip[63] = '\0';
            jobs[n].peer_port = peers[best].port;
            strncpy(jobs[n].filename, filename, 255);
            jobs[n].filename[255] = '\0';
            jobs[n].offset = offset;
            jobs[n].length = chunk;
            strncpy(jobs[n].save_path, save_path, PATHBUF - 1);
            jobs[n].save_path[PATHBUF - 1] = '\0';
            jobs[n].success = 0;

            printf("%s: downloading %ld to %ld bytes of %s from %s %d\n",
                   peer_name, offset, offset + chunk - 1, filename, jobs[n].peer_ip, jobs[n].peer_port);

            pthread_create(&tids[n], NULL, download_chunk_thread, &jobs[n]);
            n++;
            offset += chunk;
        }

        for (int i = 0; i < n; i++) {
            char msg[MAXLINE];
            pthread_join(tids[i], NULL);
            if (!jobs[i].success) continue;
            snprintf(msg, sizeof(msg), "updatetracker %s %ld %ld %s %d\n",
                     filename, jobs[i].offset, jobs[i].offset + jobs[i].length - 1,
                     my_ip, server_cfg->listen_port);
            send_tracker_request(tracker_info, msg);
        }
    }

    already = get_file_size(save_path);
    if (already == filesize) {
        char got_md5[33];
        compute_md5_file(save_path, got_md5);
        if (strcmp(got_md5, md5) == 0 || strcmp(md5, "unknown") == 0) {
            printf("%s: File %s download complete\n", peer_name, filename);
            remove(track_path);
        } else {
            printf("%s: MD5 mismatch for %s (expected %s got %s)\n", peer_name, filename, md5, got_md5);
        }
    } else {
        printf("%s: incomplete download %s (%ld/%ld bytes)\n", peer_name, filename, already, filesize);
    }
}

static void *serve_file_request(void *arg) {
    FileServeJob *job = (FileServeJob *)arg;
    char line[MAXLINE], cmd[64], filename[256], filepath[PATHBUF];
    long offset, length;
    FILE *fp;

    if (recv_line(job->connection, line, sizeof(line)) <= 0) {
        close(job->connection);
        free(job);
        return NULL;
    }
    trim_newline(line);

    if (sscanf(line, "%63s %255s %ld %ld", cmd, filename, &offset, &length) != 4 ||
        (strcmp(cmd, "GETFILE") != 0 && strcmp(cmd, "GET") != 0)) {
        send_all(job->connection, "ERROR bad_request\n", 18);
        close(job->connection);
        free(job);
        return NULL;
    }

    if (length > MAX_CHUNK) {
        send_all(job->connection, "GET invalid\n", 12);
        close(job->connection);
        free(job);
        return NULL;
    }

    build_path(filepath, sizeof(filepath), job->server_cfg.shared_dir, filename);
    fp = fopen(filepath, "rb");
    if (!fp) {
        send_all(job->connection, "ERROR no_such_file\n", 19);
        close(job->connection);
        free(job);
        return NULL;
    }
    {
        char data[MAX_CHUNK];
        size_t r;
        fseek(fp, offset, SEEK_SET);
        r = fread(data, 1, (size_t)length, fp);
        send(job->connection, data, r, 0);
    }
    fclose(fp);

    close(job->connection);
    free(job);
    return NULL;
}

static void start_file_server(const char *peer_name, const PeerServerConfig *server_cfg) {
    int listen_sock, conn_sock, reuse = 1;
    struct sockaddr_in bind_addr, downloader_addr;
    socklen_t addr_len;

    ensure_dir(server_cfg->shared_dir);

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) return;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons((uint16_t)server_cfg->listen_port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) return;
    if (listen(listen_sock, 10) < 0) return;

    printf("%s: peer server listening on port %d, dir=%s\n", peer_name, server_cfg->listen_port, server_cfg->shared_dir);

    while (1) {
        FileServeJob *job;
        pthread_t worker;
        addr_len = sizeof(downloader_addr);
        conn_sock = accept(listen_sock, (struct sockaddr *)&downloader_addr, &addr_len);
        if (conn_sock < 0) continue;

        printf("server: got connection from %s\n", inet_ntoa(downloader_addr.sin_addr));
        job = (FileServeJob *)malloc(sizeof(FileServeJob));
        if (!job) {
            close(conn_sock);
            continue;
        }
        job->connection = conn_sock;
        job->server_cfg = *server_cfg;
        pthread_create(&worker, NULL, serve_file_request, job);
        pthread_detach(worker);
    }
}

static void *serve_thread_trampoline(void *arg) {
    (void)arg;
    start_file_server(g_serve_name, &g_serve_cfg);
    return NULL;
}

static void *periodic_update_thread(void *arg) {
    PeriodicCtx *ctx = (PeriodicCtx *)arg;
    int interval = ctx->tracker_info.refresh_interval > 0 ? ctx->tracker_info.refresh_interval : 900;
    char my_ip[64], msg[MAXLINE], filepath[PATHBUF];
    strncpy(my_ip, "127.0.0.1", 63);
    my_ip[63] = '\0';
    if (get_local_ip_for_remote(ctx->tracker_info.tracker_ip, ctx->tracker_info.tracker_port, my_ip, sizeof(my_ip)) != 0) {
        strncpy(my_ip, "127.0.0.1", 63);
        my_ip[63] = '\0';
    }

    while (1) {
        DIR *dir;
        struct dirent *entry;
        sleep((unsigned int)interval);
        dir = opendir(ctx->server_cfg.shared_dir);
        if (!dir) continue;

        while ((entry = readdir(dir)) != NULL) {
            long sz;
            size_t nl = strlen(entry->d_name);
            if (entry->d_name[0] == '.') continue;
            if (nl > 6 && strcmp(entry->d_name + nl - 6, ".track") == 0) continue;
            build_path(filepath, sizeof(filepath), ctx->server_cfg.shared_dir, entry->d_name);
            sz = get_file_size(filepath);
            if (sz <= 0) continue;
            snprintf(msg, sizeof(msg), "updatetracker %s %d %ld %s %d\n",
                     entry->d_name, 0, sz - 1, my_ip, ctx->server_cfg.listen_port);
            printf("%s: periodic update - %s\n", ctx->peer_name, msg);
            send_tracker_request(&ctx->tracker_info, msg);
        }
        closedir(dir);
    }
}

static void start_periodic_updater(const char *peer_name,
                                   const PeerClientConfig *tracker_info,
                                   const PeerServerConfig *server_cfg) {
    pthread_t t;
    strncpy(g_periodic.peer_name, peer_name, sizeof(g_periodic.peer_name) - 1);
    g_periodic.peer_name[sizeof(g_periodic.peer_name) - 1] = '\0';
    g_periodic.tracker_info = *tracker_info;
    g_periodic.server_cfg = *server_cfg;
    pthread_create(&t, NULL, periodic_update_thread, &g_periodic);
    pthread_detach(t);
}

static void resume_incomplete_downloads(const char *peer_name,
                                        const PeerClientConfig *tracker_info,
                                        const PeerServerConfig *server_cfg) {
    DIR *dir = opendir(server_cfg->shared_dir);
    struct dirent *entry;
    if (!dir) return;

    while ((entry = readdir(dir)) != NULL) {
        char track_path[PATHBUF], target[256], md5[33], target_path[PATHBUF];
        long filesize = 0, current;
        size_t nl = strlen(entry->d_name);
        if (nl <= 6 || strcmp(entry->d_name + nl - 6, ".track") != 0) continue;

        build_path(track_path, sizeof(track_path), server_cfg->shared_dir, entry->d_name);
        if (parse_tracker_file(track_path, target, &filesize, md5, NULL, 0) < 0 || filesize <= 0) continue;

        build_path(target_path, sizeof(target_path), server_cfg->shared_dir, target);
        current = get_file_size(target_path);
        if (current >= filesize) {
            remove(track_path);
            continue;
        }

        printf("%s: resuming incomplete download of %s (%ld/%ld bytes)\n",
               peer_name, target, current < 0 ? 0 : current, filesize);
        download_file_from_track(peer_name, tracker_info, server_cfg, track_path);
    }
    closedir(dir);
}

static void autodownload_all_files(const char *peer_name,
                                   const PeerClientConfig *tracker_info,
                                   const PeerServerConfig *server_cfg) {
    int sock;
    char line[MAXLINE];
    int in_list = 0;

    printf("%s: REQ LIST\n", peer_name);
    sock = open_tcp_connection(tracker_info->tracker_ip, tracker_info->tracker_port);
    if (sock < 0) {
        printf("%s: cannot connect to tracker\n", peer_name);
        return;
    }

    send_all(sock, "REQ LIST\n", 9);
    while (recv_line(sock, line, sizeof(line)) > 0) {
        trim_newline(line);
        if (strncmp(line, "REP LIST ", 9) == 0) {
            in_list = 1;
            continue;
        }
        if (strcmp(line, "REP LIST END") == 0) break;
        if (in_list) {
            char idx[16], fname[256], sz[64], md5[64];
            if (sscanf(line, "%15s %255s %63s %63s", idx, fname, sz, md5) >= 2) {
                char track_name[270];
                snprintf(track_name, sizeof(track_name), "%s.track", fname);
                fetch_tracker_file(peer_name, tracker_info, server_cfg, track_name);
            }
        }
    }
    close(sock);
}

int main(int argc, char **argv) {
    const char *peer_name, *client_cfg_path, *server_cfg_path, *command;
    PeerClientConfig tracker_info;
    PeerServerConfig server_cfg;

    if (argc < 5) {
        usage();
        return 1;
    }

    peer_name = argv[1];
    client_cfg_path = argv[2];
    server_cfg_path = argv[3];
    command = argv[4];

    if (load_peer_client_config(client_cfg_path, &tracker_info) != 0 ||
        load_peer_server_config(server_cfg_path, &server_cfg) != 0) {
        fprintf(stderr, "failed to load config\n");
        return 1;
    }

    if (strcmp(command, "serve") == 0) {
        start_periodic_updater(peer_name, &tracker_info, &server_cfg);
        resume_incomplete_downloads(peer_name, &tracker_info, &server_cfg);
        start_file_server(peer_name, &server_cfg);
        return 0;
    }

    if (strcmp(command, "autodownload") == 0) {
        pthread_t t;
        g_serve_cfg = server_cfg;
        strncpy(g_serve_name, peer_name, sizeof(g_serve_name) - 1);
        g_serve_name[sizeof(g_serve_name) - 1] = '\0';
        pthread_create(&t, NULL, serve_thread_trampoline, NULL);
        pthread_detach(t);
        sleep(1);
        start_periodic_updater(peer_name, &tracker_info, &server_cfg);
        resume_incomplete_downloads(peer_name, &tracker_info, &server_cfg);
        autodownload_all_files(peer_name, &tracker_info, &server_cfg);
        return 0;
    }

    if (strcmp(command, "list") == 0) {
        printf("%s: REQ LIST\n", peer_name);
        send_tracker_request(&tracker_info, "REQ LIST\n");
        return 0;
    }

    if (strcmp(command, "createtracker") == 0) {
        const char *filename = (argc > 5) ? argv[5] : NULL;
        const char *description = (argc > 6) ? argv[6] : "demo_file";
        if (!filename) {
            usage();
            return 1;
        }
        register_file_with_tracker(peer_name, &tracker_info, &server_cfg, filename, description);
        return 0;
    }

    if (strcmp(command, "updatetracker") == 0) {
        char msg[MAXLINE], my_ip[64];
        if (argc < 8) {
            usage();
            return 1;
        }
        strncpy(my_ip, "127.0.0.1", 63);
        my_ip[63] = '\0';
        if (get_local_ip_for_remote(tracker_info.tracker_ip, tracker_info.tracker_port, my_ip, sizeof(my_ip)) != 0) {
            strncpy(my_ip, "127.0.0.1", 63);
            my_ip[63] = '\0';
        }
        snprintf(msg, sizeof(msg), "updatetracker %s %ld %ld %s %d\n",
                 argv[5], atol(argv[6]), atol(argv[7]), my_ip, server_cfg.listen_port);
        printf("%s: %s", peer_name, msg);
        send_tracker_request(&tracker_info, msg);
        return 0;
    }

    if (strcmp(command, "gettrack") == 0) {
        if (argc < 6) {
            usage();
            return 1;
        }
        fetch_tracker_file(peer_name, &tracker_info, &server_cfg, argv[5]);
        return 0;
    }

    usage();
    return 1;
}
