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

/* ── Buffer sizes ──────────────────────────────────────────────── */
#define MAXLINE    4096   /* general-purpose read/write buffer      */
#define SMALLBUF    256   /* short strings (IPs, short messages)    */
#define PATHBUF     512   /* file-system paths                      */
#define MAX_CHUNK  1024   /* max bytes transferred in one GETFILE   */

/* ── Config structs ────────────────────────────────────────────── */

/* Settings loaded from the tracker's config file (sconfig). */
typedef struct {
    int  listen_port;               /* port the tracker binds to    */
    char torrents_dir[PATHBUF];     /* where .track files are kept  */
} TrackerConfig;

/* What a peer needs to reach the tracker (peer*_client.cfg). */
typedef struct {
    char tracker_ip[64];    /* tracker's IP address           */
    int  tracker_port;      /* tracker's port                 */
    int  refresh_interval;  /* how often to re-announce (sec) */
} PeerClientConfig;

/* What a peer needs to serve files to other peers (peer*_server.cfg). */
typedef struct {
    int  listen_port;           /* port this peer's file server binds to */
    char shared_dir[PATHBUF];   /* local folder holding shared files     */
} PeerServerConfig;

/* ── Function declarations ─────────────────────────────────────── */

/* Strip trailing \r or \n from a string in-place. */
void trim_newline(char *s);

/* Load each config type from a text file. Returns 0 on success, -1 on error. */
int load_tracker_config(const char *path, TrackerConfig *cfg);
int load_peer_client_config(const char *path, PeerClientConfig *cfg);
int load_peer_server_config(const char *path, PeerServerConfig *cfg);

/* Create a directory if it doesn't already exist. Returns 0 on success. */
int ensure_dir(const char *path);

/* Blocking send that keeps retrying until all `len` bytes are delivered. */
int send_all(int sock, const char *buf, size_t len);

/* Read one '\n'-terminated line from a socket into `buf`. Returns byte count. */
int recv_line(int sock, char *buf, size_t maxlen);

/* Return the size of a file in bytes, or -1 if it can't be opened. */
long get_file_size(const char *path);

/* Write "<dir>/<file>" into `out` (safe, size-bounded). */
void build_path(char *out, size_t n, const char *dir, const char *file);

/* Case-insensitive prefix check. Returns 1 if `s` starts with `prefix`. */
int starts_with_ci(const char *s, const char *prefix);

/* Figure out which local IP the OS would use to reach remote_ip:remote_port.
   Writes the result into out_ip.  Returns 0 on success, -1 on error. */
int get_local_ip_for_remote(const char *remote_ip, int remote_port,
                             char *out_ip, size_t out_len);

#endif /* COMMON_H */