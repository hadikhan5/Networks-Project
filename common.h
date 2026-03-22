#ifndef COMMON_H
#define COMMON_H

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAXLINE 4096
#define SMALLBUF 256
#define PATHBUF 512
#define MAX_CHUNK 1024

typedef struct {
    int port;
    char torrents_dir[PATHBUF];
} TrackerConfig;

typedef struct {
    char tracker_ip[64];
    int tracker_port;
    int refresh_interval;
} PeerClientConfig;

typedef struct {
    int listen_port;
    char shared_dir[PATHBUF];
} PeerServerConfig;

void trim_newline(char *s);
int load_tracker_config(const char *path, TrackerConfig *cfg);
int load_peer_client_config(const char *path, PeerClientConfig *cfg);
int load_peer_server_config(const char *path, PeerServerConfig *cfg);
int ensure_dir(const char *path);
int send_all(int sock, const char *buf, size_t len);
int recv_line(int sock, char *buf, size_t maxlen);
long get_file_size(const char *path);
void build_path(char *out, size_t n, const char *dir, const char *file);
int starts_with_ci(const char *s, const char *prefix);
/* Resolve the local IPv4 address the OS would use to reach `remote_ip:remote_port`. */
int get_local_ip_for_remote(const char *remote_ip, int remote_port, char *out_ip, size_t out_len);

#endif
