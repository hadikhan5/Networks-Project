#include "common.h"
#include <dirent.h>

/* Bundle passed to each per-connection worker thread. */
typedef struct {
    int          connection;   /* accepted socket fd for this client */
    TrackerConfig cfg;         /* snapshot of tracker settings       */
} ClientJob;

/* ── Small helper ──────────────────────────────────────────────── */

static void send_reply(int sock, const char *msg) {
    send_all(sock, msg, strlen(msg));
}

/* ── Protocol handlers ─────────────────────────────────────────── */

/* Handle "REQ LIST": scan the torrents directory for .track files,
   read their metadata, and reply with a numbered list. */
static void reply_with_file_list(int sock, const TrackerConfig *cfg) {
    DIR           *dir = opendir(cfg->torrents_dir);
    struct dirent *entry;
    int            total = 0, index = 1;
    char           response[8192] = "";
    char           line[1024], track_path[PATHBUF];

    if (!dir) {
        send_reply(sock, "REP LIST 0\nREP LIST END\n");
        return;
    }

    /* First pass: count how many .track files exist so we can emit the header. */
    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len > 6 && strcmp(entry->d_name + name_len - 6, ".track") == 0)
            total++;
    }
    rewinddir(dir);

    snprintf(line, sizeof(line), "REP LIST %d\n", total);
    strncat(response, line, sizeof(response) - strlen(response) - 1);

    /* Second pass: pull Filename, Filesize and MD5 from each .track file. */
    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        FILE  *fp;
        char   filename[256] = "", filesize[64] = "", md5[128] = "";

        if (!(name_len > 6 && strcmp(entry->d_name + name_len - 6, ".track") == 0))
            continue;

        build_path(track_path, sizeof(track_path), cfg->torrents_dir, entry->d_name);
        fp = fopen(track_path, "r");
        if (!fp) continue;

        while (fgets(line, sizeof(line), fp)) {
            trim_newline(line);
            if      (strncmp(line, "Filename: ", 10) == 0) strncpy(filename, line + 10, sizeof(filename) - 1);
            else if (strncmp(line, "Filesize: ", 10) == 0) strncpy(filesize, line + 10, sizeof(filesize) - 1);
            else if (strncmp(line, "MD5: ",       5) == 0) strncpy(md5,      line + 5,  sizeof(md5)      - 1);
        }
        fclose(fp);

        snprintf(line, sizeof(line), "%d %s %s %s\n",
                 index++, filename,
                 filesize[0] ? filesize : "0",
                 md5[0]      ? md5      : "dummy_md5");
        strncat(response, line, sizeof(response) - strlen(response) - 1);
    }
    closedir(dir);

    strncat(response, "REP LIST END\n", sizeof(response) - strlen(response) - 1);
    send_reply(sock, response);
}

/* Handle "createtracker <filename> <size> <desc> <md5> <ip> <port>":
   Create a new .track file in the torrents directory.
   The seeding peer's IP and port are recorded as the first known source. */
static void register_new_file(int sock, const TrackerConfig *cfg, char *raw_cmd) {
    char  filename[256], description[256], md5[128], peer_ip[64];
    long  filesize;
    int   peer_port;
    char  track_path[PATHBUF];
    FILE *fp;

    if (sscanf(raw_cmd, "createtracker %255s %ld %255s %127s %63s %d",
               filename, &filesize, description, md5, peer_ip, &peer_port) != 6) {
        send_reply(sock, "createtracker fail\n");
        return;
    }

    snprintf(track_path, sizeof(track_path), "%s/%s.track", cfg->torrents_dir, filename);

    /* Refuse to overwrite an existing entry. */
    if (access(track_path, F_OK) == 0) {
        send_reply(sock, "createtracker ferr\n");
        return;
    }

    fp = fopen(track_path, "w");
    if (!fp) {
        send_reply(sock, "createtracker fail\n");
        return;
    }

    /* Write the human-readable header block. */
    fprintf(fp, "Filename: %s\n",    filename);
    fprintf(fp, "Filesize: %ld\n",   filesize);
    fprintf(fp, "Description: %s\n", description);
    fprintf(fp, "MD5: %s\n",         md5);
    fprintf(fp, "#list of peers follows next\n");

    /* First peer entry: <ip>:<port>:<start_byte>:<end_byte>:<timestamp> */
    fprintf(fp, "%s:%d:%d:%ld:%ld\n",
            peer_ip, peer_port,
            0,
            filesize > 0 ? filesize - 1 : 0,
            (long)time(NULL));

    fclose(fp);
    send_reply(sock, "createtracker succ\n");
}

/* Handle "updatetracker <filename> <start> <end> <ip> <port>":
   Append a new peer entry to the .track file to record that this peer
   now holds bytes [start, end] of the file. */
static void update_peer_progress(int sock, const TrackerConfig *cfg, char *raw_cmd) {
    char  filename[256], peer_ip[64], track_path[PATHBUF];
    long  start_byte, end_byte;
    int   peer_port;
    FILE *fp;

    if (sscanf(raw_cmd, "updatetracker %255s %ld %ld %63s %d",
               filename, &start_byte, &end_byte, peer_ip, &peer_port) != 5) {
        send_reply(sock, "updatetracker fail\n");
        return;
    }

    snprintf(track_path, sizeof(track_path), "%s/%s.track", cfg->torrents_dir, filename);

    if (access(track_path, F_OK) != 0) {
        /* File not registered — can't update something that doesn't exist. */
        char err[SMALLBUF];
        snprintf(err, sizeof(err), "updatetracker %s ferr\n", filename);
        send_reply(sock, err);
        return;
    }

    fp = fopen(track_path, "a");   /* append — don't overwrite existing peers */
    if (!fp) {
        char err[SMALLBUF];
        snprintf(err, sizeof(err), "updatetracker %s fail\n", filename);
        send_reply(sock, err);
        return;
    }

    fprintf(fp, "%s:%d:%ld:%ld:%ld\n",
            peer_ip, peer_port, start_byte, end_byte, (long)time(NULL));
    fclose(fp);

    char ok[SMALLBUF];
    snprintf(ok, sizeof(ok), "updatetracker %s succ\n", filename);
    send_reply(sock, ok);
}

/* Handle "GET <filename.track>":
   Stream the entire .track file back to the requester, wrapped in
   REP GET BEGIN / REP GET END markers so the peer knows where it ends. */
static void send_tracker_file(int sock, const TrackerConfig *cfg, const char *track_filename) {
    char  full_path[PATHBUF], line[MAXLINE];
    char  md5[128] = "dummy_md5";
    FILE *fp;

    snprintf(full_path, sizeof(full_path), "%s/%s", cfg->torrents_dir, track_filename);
    fp = fopen(full_path, "r");
    if (!fp) {
        send_reply(sock, "REP GET BEGIN\nERROR file_not_found\nREP GET END dummy_md5\n");
        return;
    }

    send_reply(sock, "REP GET BEGIN\n");

    while (fgets(line, sizeof(line), fp)) {
        /* Remember the MD5 value so we can echo it in the END marker. */
        if (strncmp(line, "MD5: ", 5) == 0) {
            char *value = line + 5;
            trim_newline(value);
            strncpy(md5, value, sizeof(md5) - 1);
            md5[sizeof(md5) - 1] = '\0';
        }
        send_reply(sock, line);
    }
    fclose(fp);

    snprintf(line, sizeof(line), "REP GET END %s\n", md5[0] ? md5 : "dummy_md5");
    send_reply(sock, line);
}

/* ── Worker thread ─────────────────────────────────────────────── */

/* Each accepted connection gets its own thread.  We read one command line,
   dispatch to the appropriate handler, then close the socket and exit. */
static void *handle_client_connection(void *arg) {
    ClientJob *job = (ClientJob *)arg;
    char       buf[MAXLINE];
    int        bytes_read = recv_line(job->connection, buf, sizeof(buf));

    if (bytes_read > 0) {
        trim_newline(buf);
        printf("[tracker] received: %s\n", buf);

        if (strcmp(buf, "REQ LIST") == 0 || strcmp(buf, "<REQ LIST>") == 0) {
            reply_with_file_list(job->connection, &job->cfg);

        } else if (starts_with_ci(buf, "createtracker ")) {
            register_new_file(job->connection, &job->cfg, buf);

        } else if (starts_with_ci(buf, "updatetracker ")) {
            update_peer_progress(job->connection, &job->cfg, buf);

        } else if (starts_with_ci(buf, "GET ")) {
            char track_name[256];
            if (sscanf(buf, "GET %255s", track_name) == 1)
                send_tracker_file(job->connection, &job->cfg, track_name);
            else
                send_reply(job->connection, "GET invalid\n");

        } else {
            send_reply(job->connection, "invalid command\n");
        }
    }

    close(job->connection);
    free(job);
    return NULL;
}

/* ── Entry point ───────────────────────────────────────────────── */

int main(int argc, char **argv) {
    TrackerConfig     cfg;
    int               listen_sock, conn_sock, reuse = 1;
    struct sockaddr_in bind_addr, peer_addr;
    socklen_t          peer_len = sizeof(peer_addr);

    const char *config_path = (argc > 1) ? argv[1] : "config/sconfig";
    if (load_tracker_config(config_path, &cfg) != 0) {
        fprintf(stderr, "failed to load tracker config: %s\n", config_path);
        return 1;
    }
    ensure_dir(cfg.torrents_dir);

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(cfg.listen_port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listen_sock, 10) < 0) {
        perror("listen"); return 1;
    }

    printf("[tracker] listening on port %d, dir=%s\n", cfg.listen_port, cfg.torrents_dir);

    /* Accept loop: spawn a detached thread for every incoming connection. */
    while (1) {
        pthread_t  worker;
        ClientJob *job;

        conn_sock = accept(listen_sock, (struct sockaddr *)&peer_addr, &peer_len);
        if (conn_sock < 0) continue;

        job             = (ClientJob *)malloc(sizeof(ClientJob));
        job->connection = conn_sock;
        job->cfg        = cfg;

        pthread_create(&worker, NULL, handle_client_connection, job);
        pthread_detach(worker);   /* let the OS reclaim resources when done */
    }
    return 0;
}