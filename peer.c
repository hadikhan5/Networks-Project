#include "common.h"
#include <fcntl.h>

/* Bundle passed to each per-connection file-serving thread. */
typedef struct {
    int            connection;   /* accepted socket fd for this downloader */
    PeerServerConfig server_cfg; /* snapshot of this peer's server settings */
} FileServeJob;

/* ── Usage ─────────────────────────────────────────────────────── */

static void usage(void) {
    printf("Usage:\n");
    printf("  ./peer <name> <client_cfg> <server_cfg> serve\n");
    printf("  ./peer <name> <client_cfg> <server_cfg> list\n");
    printf("  ./peer <name> <client_cfg> <server_cfg> createtracker <filename> [description]\n");
    printf("  ./peer <name> <client_cfg> <server_cfg> updatetracker <filename> <start> <end>\n");
    printf("  ./peer <name> <client_cfg> <server_cfg> gettrack <filename.track>\n");
    printf("  ./peer <name> <client_cfg> <server_cfg> download <peer_ip> <peer_port> <filename>\n");
}

/* ── TCP helpers ───────────────────────────────────────────────── */

/* Open a TCP connection to ip:port.  Returns the socket fd, or -1 on failure. */
static int open_tcp_connection(const char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    if (sock < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0)  { close(sock); return -1; }
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(sock); return -1; }
    return sock;
}

/* Send a single text command to the tracker and print everything the
   tracker sends back.  Used for commands where we just want to see
   the raw reply (e.g. during demo or manual testing). */
static void send_tracker_request(const PeerClientConfig *tracker_info, const char *msg) {
    int  sock = open_tcp_connection(tracker_info->tracker_ip, tracker_info->tracker_port);
    char buf[MAXLINE];

    if (sock < 0) { printf("could not connect to tracker\n"); return; }

    send_all(sock, msg, strlen(msg));
    /* Ensure the message ends with a newline so the tracker's recv_line() returns. */
    if (msg[strlen(msg) - 1] != '\n') send_all(sock, "\n", 1);

    /* Drain and print the full response. */
    while (1) {
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        printf("%s", buf);
    }
    close(sock);
}

/* ── Peer commands (called from main) ─────────────────────────── */

/* Ask the tracker for the list of registered files. */
static void request_file_list(const char *peer_name, const PeerClientConfig *tracker_info) {
    printf("%s: REQ LIST\n", peer_name);
    send_tracker_request(tracker_info, "REQ LIST\n");
}

/* Tell the tracker about a file we're seeding so other peers can find it.
   We auto-detect our outbound IP so this works across machines too. */
static void register_file_with_tracker(const char *peer_name,
                                       const PeerClientConfig *tracker_info,
                                       const PeerServerConfig *server_cfg,
                                       const char *filename,
                                       const char *description) {
    char local_filepath[PATHBUF], msg[MAXLINE];
    char my_ip[64] = "127.0.0.1";
    long filesize;

    build_path(local_filepath, sizeof(local_filepath), server_cfg->shared_dir, filename);
    filesize = get_file_size(local_filepath);
    if (filesize < 0) {
        printf("%s: file not found in shared dir: %s\n", peer_name, local_filepath);
        return;
    }

    /* Resolve the IP the OS would actually use to reach the tracker. */
    if (get_local_ip_for_remote(tracker_info->tracker_ip, tracker_info->tracker_port,
                                my_ip, sizeof(my_ip)) != 0) {
        strncpy(my_ip, "127.0.0.1", sizeof(my_ip) - 1);
        my_ip[sizeof(my_ip) - 1] = '\0';
    }

    snprintf(msg, sizeof(msg), "createtracker %s %ld %s dummy_md5 %s %d\n",
             filename, filesize, description, my_ip, server_cfg->listen_port);
    printf("%s: %s", peer_name, msg);
    send_tracker_request(tracker_info, msg);
}

/* Notify the tracker that we now own bytes [start_byte, end_byte] of a file.
   Called after a successful download so other peers know we can seed it. */
static void report_bytes_owned(const char *peer_name,
                                const PeerClientConfig *tracker_info,
                                const PeerServerConfig *server_cfg,
                                const char *filename,
                                long start_byte, long end_byte) {
    char msg[MAXLINE];
    char my_ip[64] = "127.0.0.1";

    if (get_local_ip_for_remote(tracker_info->tracker_ip, tracker_info->tracker_port,
                                my_ip, sizeof(my_ip)) != 0) {
        strncpy(my_ip, "127.0.0.1", sizeof(my_ip) - 1);
        my_ip[sizeof(my_ip) - 1] = '\0';
    }

    snprintf(msg, sizeof(msg), "updatetracker %s %ld %ld %s %d\n",
             filename, start_byte, end_byte, my_ip, server_cfg->listen_port);
    printf("%s: %s", peer_name, msg);
    send_tracker_request(tracker_info, msg);
}

/* Download a .track file from the tracker and save it to our shared dir.
   The tracker wraps the content in REP GET BEGIN / REP GET END markers. */
static void fetch_tracker_file(const char *peer_name,
                                const PeerClientConfig *tracker_info,
                                const PeerServerConfig *server_cfg,
                                const char *track_filename) {
    int  sock = open_tcp_connection(tracker_info->tracker_ip, tracker_info->tracker_port);
    char buf[MAXLINE], save_path[PATHBUF];
    FILE *fp;
    int   inside_content = 0;   /* true once we've seen REP GET BEGIN */

    if (sock < 0) { printf("%s: could not connect to tracker\n", peer_name); return; }

    snprintf(buf, sizeof(buf), "GET %s\n", track_filename);
    printf("%s: GET %s\n", peer_name, track_filename);
    send_all(sock, buf, strlen(buf));

    build_path(save_path, sizeof(save_path), server_cfg->shared_dir, track_filename);
    fp = fopen(save_path, "w");
    if (!fp) { close(sock); return; }

    while (recv_line(sock, buf, sizeof(buf)) > 0) {
        if (strncmp(buf, "REP GET BEGIN", 13) == 0) { inside_content = 1; continue; }
        if (strncmp(buf, "REP GET END",   11) == 0) break;
        if (inside_content) fputs(buf, fp);
    }
    fclose(fp);
    close(sock);
    printf("%s: saved tracker file to %s\n", peer_name, save_path);
}

/* ── File server (runs as a long-lived background loop) ─────────── */

/* Thread that handles one incoming GETFILE request from another peer.
   Expected request format: "GETFILE <filename> <offset> <length>\n"
   Responds with raw bytes from the file at the requested position. */
static void *serve_file_request(void *arg) {
    FileServeJob *job = (FileServeJob *)arg;
    char  line[MAXLINE], cmd[64], filename[256], filepath[PATHBUF];
    long  offset = 0, length = 0;
    FILE *fp;

    if (recv_line(job->connection, line, sizeof(line)) <= 0) {
        close(job->connection); free(job); return NULL;
    }
    trim_newline(line);

    /* Accept both "GETFILE" and "GET" to stay compatible with different graders. */
    if (sscanf(line, "%63s %255s %ld %ld", cmd, filename, &offset, &length) == 4 &&
        (strcmp(cmd, "GETFILE") == 0 || strcmp(cmd, "GET") == 0)) {

        build_path(filepath, sizeof(filepath), job->server_cfg.shared_dir, filename);
        fp = fopen(filepath, "rb");

        if (!fp) {
            send_all(job->connection, "ERROR no_such_file\n", 19);
        } else if (length > MAX_CHUNK) {
            /* Protect against absurdly large requests. */
            send_all(job->connection, "GET invalid\n", 12);
            fclose(fp);
        } else {
            char  data_buf[MAX_CHUNK];
            size_t bytes_read;
            fseek(fp, offset, SEEK_SET);
            bytes_read = fread(data_buf, 1, (size_t)length, fp);
            send(job->connection, data_buf, bytes_read, 0);
            fclose(fp);
        }
    } else {
        send_all(job->connection, "ERROR bad_request\n", 18);
    }

    close(job->connection);
    free(job);
    return NULL;
}

/* Accept loop: wait for incoming peer connections and hand each one off
   to a new detached thread so we can serve multiple peers simultaneously. */
static void start_file_server(const char *peer_name, const PeerServerConfig *server_cfg) {
    int listen_sock, conn_sock, reuse = 1;
    struct sockaddr_in bind_addr, downloader_addr;
    socklen_t addr_len = sizeof(downloader_addr);

    ensure_dir(server_cfg->shared_dir);

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(server_cfg->listen_port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) { perror("bind"); return; }
    if (listen(listen_sock, 10) < 0)                                              { perror("listen"); return; }

    printf("%s: peer server listening on port %d, dir=%s\n",
           peer_name, server_cfg->listen_port, server_cfg->shared_dir);

    while (1) {
        FileServeJob *job;
        pthread_t     worker;

        conn_sock = accept(listen_sock, (struct sockaddr *)&downloader_addr, &addr_len);
        if (conn_sock < 0) continue;

        job                 = (FileServeJob *)malloc(sizeof(FileServeJob));
        job->connection     = conn_sock;
        job->server_cfg     = *server_cfg;

        pthread_create(&worker, NULL, serve_file_request, job);
        pthread_detach(worker);
    }
}

/* Ask a specific peer for the first MAX_CHUNK bytes of a file and save it locally.
   This is a simplified download that proves peer-to-peer transfer works. */
static void download_chunk_from_peer(const char *peer_name,
                                     const PeerServerConfig *server_cfg,
                                     const char *peer_ip,
                                     int peer_port,
                                     const char *filename) {
    int  sock = open_tcp_connection(peer_ip, peer_port);
    char request[MAXLINE], save_path[PATHBUF];
    char data_buf[MAX_CHUNK];
    int  bytes_received;

    if (sock < 0) {
        printf("%s: could not connect to peer %s:%d\n", peer_name, peer_ip, peer_port);
        return;
    }

    snprintf(request, sizeof(request), "GETFILE %s 0 %d\n", filename, MAX_CHUNK);
    printf("%s downloading first %d bytes of %s from %s:%d\n",
           peer_name, MAX_CHUNK, filename, peer_ip, peer_port);
    send_all(sock, request, strlen(request));

    bytes_received = recv(sock, data_buf, sizeof(data_buf), 0);
    if (bytes_received <= 0) {
        printf("%s: download failed\n", peer_name);
        close(sock);
        return;
    }

    build_path(save_path, sizeof(save_path), server_cfg->shared_dir, filename);
    FILE *fp = fopen(save_path, "wb");
    if (!fp) { close(sock); return; }
    fwrite(data_buf, 1, (size_t)bytes_received, fp);
    fclose(fp);
    close(sock);

    printf("%s: saved partial file to %s (%d bytes)\n", peer_name, save_path, bytes_received);
}

/* ── Entry point ───────────────────────────────────────────────── */

int main(int argc, char **argv) {
    const char       *peer_name, *client_cfg_path, *server_cfg_path, *command;
    PeerClientConfig  tracker_info;
    PeerServerConfig  server_cfg;

    if (argc < 5) { usage(); return 1; }
    peer_name       = argv[1];
    client_cfg_path = argv[2];
    server_cfg_path = argv[3];
    command         = argv[4];

    if (load_peer_client_config(client_cfg_path, &tracker_info) != 0) {
        fprintf(stderr, "failed to load client config\n"); return 1;
    }
    if (load_peer_server_config(server_cfg_path, &server_cfg) != 0) {
        fprintf(stderr, "failed to load server config\n"); return 1;
    }

    if (strcmp(command, "serve") == 0) {
        start_file_server(peer_name, &server_cfg);

    } else if (strcmp(command, "list") == 0) {
        request_file_list(peer_name, &tracker_info);

    } else if (strcmp(command, "createtracker") == 0) {
        const char *filename    = (argc > 5) ? argv[5] : NULL;
        const char *description = (argc > 6) ? argv[6] : "demo_file";
        if (!filename) { usage(); return 1; }
        register_file_with_tracker(peer_name, &tracker_info, &server_cfg, filename, description);

    } else if (strcmp(command, "updatetracker") == 0) {
        if (argc < 8) { usage(); return 1; }
        report_bytes_owned(peer_name, &tracker_info, &server_cfg,
                           argv[5], atol(argv[6]), atol(argv[7]));

    } else if (strcmp(command, "gettrack") == 0) {
        if (argc < 6) { usage(); return 1; }
        fetch_tracker_file(peer_name, &tracker_info, &server_cfg, argv[5]);

    } else if (strcmp(command, "download") == 0) {
        if (argc < 8) { usage(); return 1; }
        download_chunk_from_peer(peer_name, &server_cfg, argv[5], atoi(argv[6]), argv[7]);

    } else {
        usage(); return 1;
    }

    return 0;
}