/*
 * NASBridge copy helper payload.
 *
 * NASBridge runs in the game sandbox. Writing a big file from there collapses
 * to a few MB/s a few GB in, while the same drive takes a steady stream from a
 * payload (ps5upload's receiver is one). So NASBridge hands each file it
 * copies to this payload, sent to the PS5's own ELF loader (127.0.0.1:9021)
 * the first time it's needed. Running with the jailbreak's privileges, it:
 *   1. listens on 127.0.0.1:COPIER_PORT (only NASBridge, on this PS5),
 *   2. raises itself out of the sandbox as ps5upload's payload does,
 *   3. copies one file per connection: NASBridge sends the NAS's address and
 *      the GET request (sign-in and all; over SMB, the sign-in and the file:
 *      see below); this payload works out where the
 *      file got to and fetches the rest, either
 *        - over one connection, received and written on two threads, or
 *        - over two (NASBridge asks), each fetching the next CHUNK of the file
 *          in turn, a single writer putting chunks down strictly in order:
 *          the file still only grows front to back, never sparse or pre-sized,
 *          and the NAS reads two places CHUNK apart, not two far-off ones,
 *      flushing to the drive every SYNC_MS or SYNC_BYTES,
 *   4. reports progress a few times a second, and stops as soon as NASBridge
 *      says so or goes away,
 *   5. exits once nothing has asked it for IDLE_EXIT_S.
 * Either way the file's length says how far it got, which is all a resume
 * needs (the last REFETCH bytes are fetched again).
 *
 * Per connection, text lines (the request bytes aside):
 *   helper  -> "NBC1"                                  ready
 *   NASBridge -> "NBC1\t<ip>\t<port>\t<size>\t<file>\t<request length>[\t<streams>]\n"
 *                then the HTTP request (ends "\r\n\r\n"; no Range header)
 *   helper  -> "S <HTTP status>"                       the NAS answered
 *              "P <bytes in the file> <write ms> <ms> <connections>" progress
 *              "OK" | "STOPPED" | "ERR <kind> <why>"   the end
 *   NASBridge -> "STOP\n" (or closing the connection)  stop now
 * ERR kinds: request, file, write (the drive), connect, drop, timeout (the
 * network: NASBridge tries again), http <status>, chunked, changed, and smb
 * <why> (the NAS refused over SMB: sign-in, no share, not found, denied).
 *
 * Over SMB (through libsmb2) the helper signs in itself:
 *   NASBridge -> "NBS1\t<ip>\t<port>\t<size>\t<file>\t<sign-in length>[\t<streams>]\n"
 *                then the share, the path in it, the user, the domain and the
 *                password, each ended by a NUL
 * and the replies are as above (no "S" line). The file comes over <streams>
 * sign-ins (1 when left out), each fetching the next CHUNK in turn as over
 * HTTP, with several reads in flight (SMB_IN_FLIGHT) for each chunk.
 *
 * Over NFS (NFSv3, through libnfs) the helper connects itself the same way:
 *   NASBridge -> "NBN1\t<ip>\t<port>\t<size>\t<file>\t<target length>[\t<streams>]\n"
 *                then the export, the path in it, the MOUNT port (0: ask the
 *                portmapper), the user ID and the group ID, each ended by a NUL
 *   helper  -> "N <local port>"  the port its connection came from (below
 *                                1024: a privileged one), once it's connected
 * ERR kind nfs <why>: the NAS refused (this PS5, not in an export, not
 * found, denied), in the words of NASBridge's nfs_failure_brief.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/smb2-errors.h>

#include <nfsc/libnfs.h>

#ifndef COPIER_HOST_TEST
#include <ps5/kernel.h>
#endif

#define COPIER_PORT_DEFAULT 9147
#define PROTO "NBC1"
#define IDLE_EXIT_S 120   /* no copy asked for this long: exit */
#define REQUEST_S 10      /* NASBridge sends its request at once */
#define NAS_CONNECT_S 5
#define NAS_REPLY_S 30    /* how long the NAS may go quiet mid-file */
#define BUF_SIZE (4u << 20)
#define NBUF 8            /* one connection: 32 MiB between the network and the drive */
#define CHUNK (8ull << 20) /* two connections: what each asks for at a time */
#define WINDOW 4          /* two connections: chunks fetched ahead of the writer (32 MiB) */
/* Every thread gets this stack, as ps5upload's do: the PS5's default thread
 * stack is tight (a 64 KiB buffer on it crashes the payload), and big buffers
 * here are all on the heap. */
#define THREAD_STACK (256u * 1024u)
#define MAX_STREAMS 4 /* the Settings screen: 1..4 */
#define CHUNK_TRIES 3     /* a dropped chunk is asked for again this often */
#define REFETCH (4ull << 20) /* a file's last bytes may not have reached the disk */
#define SYNC_MS 250
#define SYNC_BYTES (64ull << 20)
#define MAX_HEADER (64 * 1024)
#define MAX_REQUEST (64 * 1024)
#define MAX_FILE 2048
#define MAX_SIGN_IN 4096
/* Game files are open to every user, as an FTP copy or the installer leaves
 * them: a game whose files lack the execute bit shows "Can't Start Game"
 * (issue #4), and NASBridge, as another user, moves and deletes them later. */
#define FILE_MODE 0777
#define PROTO_SMB "NBS1"
#define SMB_READ (1u << 20) /* one SMB read (the NAS may allow less) */
#define SMB_IN_FLIGHT 8     /* SMB reads asked for before the first answers */
#define PROTO_NFS "NBN1"
#define NFS_IN_FLIGHT 8     /* NFS reads asked for before the first answers */
#define UNKNOWN_LEN UINT64_MAX
/* ps5upload's identity for its payload (the debugger authid). */
#define AUTHID_SYSTEM 0x4800000000000006ull

/* ----------------------------------------------------------------- basics */

#ifndef COPIER_HOST_TEST
typedef struct notify_request {
  char reserved[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
static void notify(const char *text) {
  notify_request_t req;
  memset(&req, 0, sizeof req);
  snprintf(req.message, sizeof req.message, "%s", text);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
#endif

static uint64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static uint64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void sleep_ms(unsigned ms) {
  struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
  nanosleep(&t, NULL);
}

static int start_thread(pthread_t *t, void *(*fn)(void *), void *arg) {
  pthread_attr_t attr;
  int rc;
  if (pthread_attr_init(&attr) != 0) return -1;
  (void)pthread_attr_setstacksize(&attr, THREAD_STACK);
  rc = pthread_create(t, &attr, fn, arg);
  pthread_attr_destroy(&attr);
  return rc;
}

static int send_all(int fd, const char *p, size_t len) {
  while (len > 0) {
    ssize_t n = send(fd, p, len, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static void send_line(int fd, const char *fmt, ...) {
  char line[512];
  size_t len;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  len = strlen(line);
  line[len++] = '\n';
  (void)send_all(fd, line, len);
}

/* --------------------------------------------------------- the privileges */

#ifndef COPIER_HOST_TEST
/* As ps5upload's runtime_apply_ucred_jailbreak: root, every capability, the
 * system-process attribute and authid, and out of the sandbox (root and jail
 * directories at the real root), so /data and /mnt are the drives' own. */
static int elevate(void) {
  static const uint8_t caps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  static const uint8_t attrs[32] = {0x00, 0x00, 0x00, 0x80};
  const pid_t pid = getpid();
  intptr_t root;
  int rc = 0;
  rc |= kernel_set_ucred_uid(pid, 0);
  rc |= kernel_set_ucred_ruid(pid, 0);
  rc |= kernel_set_ucred_svuid(pid, 0);
  rc |= kernel_set_ucred_rgid(pid, 0);
  rc |= kernel_set_ucred_svgid(pid, 0);
  rc |= kernel_set_ucred_caps(pid, caps);
  rc |= kernel_set_ucred_attrs(pid, attrs);
  rc |= kernel_set_ucred_authid(pid, AUTHID_SYSTEM);
  root = kernel_get_root_vnode();
  if (root != 0) {
    rc |= kernel_set_proc_rootdir(pid, root);
    rc |= kernel_set_proc_jaildir(pid, root);
  } else {
    rc |= -1;
  }
  return rc;
}
#endif

/* -------------------------------------------------------- talking to the NAS */

static int connect_nas(const char *ip, int port) {
  struct sockaddr_in addr;
  struct timeval tv;
  int fd, flags, rc, size = 4 << 20;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return -1;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  /* Before connecting, so the window it advertises can grow. */
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
  flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
  if (rc != 0 && errno == EINPROGRESS) {
    struct pollfd p = {fd, POLLOUT, 0};
    int err = 0;
    socklen_t elen = sizeof err;
    if (poll(&p, 1, NAS_CONNECT_S * 1000) == 1 &&
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0)
      rc = 0;
  }
  if (rc != 0) {
    close(fd);
    return -1;
  }
  fcntl(fd, F_SETFL, flags);
  tv.tv_sec = NAS_REPLY_S;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return fd;
}

/* NASBridge's request with a Range added: bytes from..to (to < 0: to the end).
 * The caller frees it. */
static char *with_range(const char *request, size_t reqlen, uint64_t from, int64_t to,
                        size_t *len) {
  char *out = malloc(reqlen + 80);
  int n;
  if (!out) return NULL;
  if (to >= 0)
    n = snprintf(out, reqlen + 80, "%.*sRange: bytes=%llu-%lld\r\n\r\n", (int)(reqlen - 2), request,
                 (unsigned long long)from, (long long)to);
  else if (from > 0)
    n = snprintf(out, reqlen + 80, "%.*sRange: bytes=%llu-\r\n\r\n", (int)(reqlen - 2), request,
                 (unsigned long long)from);
  else
    n = snprintf(out, reqlen + 80, "%s", request);
  *len = (size_t)n;
  return out;
}

/* The value of header `name` in `head`, or NULL. */
static const char *header(const char *head, const char *name, char *out, size_t size) {
  const char *p = head;
  size_t n = strlen(name);
  while ((p = strstr(p, "\r\n")) != NULL) {
    p += 2;
    if (strncasecmp(p, name, n) == 0 && p[n] == ':') {
      const char *v = p + n + 1, *end;
      size_t len;
      while (*v == ' ' || *v == '\t') ++v;
      end = strstr(v, "\r\n");
      len = end ? (size_t)(end - v) : strlen(v);
      if (len >= size) len = size - 1;
      memcpy(out, v, len);
      out[len] = '\0';
      return out;
    }
  }
  return NULL;
}

/* A reply's header, into head (NUL-terminated). Returns 0, or 1 when the NAS
 * dropped the connection and 2 when it went quiet; *status, *body_at set. */
static int read_head(int nas, char *head, size_t *hlen, size_t *body_at, int *status) {
  *hlen = 0;
  *body_at = 0;
  *status = 0;
  while (*hlen < MAX_HEADER) {
    ssize_t n = recv(nas, head + *hlen, MAX_HEADER - *hlen, 0);
    char *end;
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? 2 : 1;
    *hlen += (size_t)n;
    head[*hlen] = '\0';
    end = strstr(head, "\r\n\r\n");
    if (end) {
      *body_at = (size_t)(end - head) + 4;
      if (strncmp(head, "HTTP/", 5) == 0 && strchr(head, ' ')) *status = atoi(strchr(head, ' ') + 1);
      return 0;
    }
  }
  return 1;
}

/* ------------------------------------------------------------ the drive */

/* What every way of copying shares: the file, the stop flag and the flushing. */
typedef struct job {
  int ctl;
  int fd;
  uint64_t size;
  const char *ip;
  int port;
  const char *request;
  size_t reqlen;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int abort;          /* stop: NASBridge asked, or something failed */
  int write_err;      /* errno */
  uint64_t start;     /* where this pass started in the file */
  uint64_t written;   /* bytes written this pass (all in order, from start) */
  uint64_t write_ns;  /* time spent writing and flushing */
  uint64_t last_sync; /* now_ms() of the last fsync */
  uint64_t unsynced;
  int active;         /* connections bringing data in now */
  int nas[MAX_STREAMS]; /* their sockets, to cut them off on a stop (-1: none) */
  int nfs_port;       /* over NFS: the local port a connection came from */
  int nfs_port_sent;
} job_t;

/* Writes len bytes at the file's next position, flushing every SYNC_MS or
 * SYNC_BYTES (the PS5 otherwise lets unflushed writes pile up to its
 * dirty-buffer limit, then throttles hard). 0, or errno. Writer thread only. */
static int put(job_t *j, const char *p, size_t len) {
  uint64_t t0 = now_ns(), now;
  uint64_t at = j->start + j->written;
  size_t done = 0;
  int err = 0;
  while (done < len) {
    ssize_t n = pwrite(j->fd, p + done, len - done, (off_t)(at + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      err = n == 0 ? EIO : errno;
      break;
    }
    done += (size_t)n;
  }
  j->unsynced += done;
  now = now_ms();
  if (!err && (j->unsynced >= SYNC_BYTES || now - j->last_sync >= SYNC_MS)) {
    if (fsync(j->fd) != 0 && errno != EINVAL && errno != ENOTSUP) err = errno;
    j->last_sync = now;
    j->unsynced = 0;
  }
  pthread_mutex_lock(&j->mu);
  j->written += done;
  j->write_ns += now_ns() - t0;
  pthread_mutex_unlock(&j->mu);
  return err;
}

/* Stops every connection at once (a stop asked for, or a failure). */
static void stop_all(job_t *j) {
  int i;
  pthread_mutex_lock(&j->mu);
  j->abort = 1;
  for (i = 0; i < MAX_STREAMS; ++i)
    if (j->nas[i] >= 0) shutdown(j->nas[i], SHUT_RDWR);
  pthread_cond_broadcast(&j->cv);
  pthread_mutex_unlock(&j->mu);
}

/* While the threads work: progress lines, and NASBridge's STOP (or its going
 * away) stops everything. Returns when done() says the threads are finished. */
static void watch(job_t *j, int (*done)(void *), void *arg) {
  uint64_t started = now_ms();
  for (;;) {
    struct pollfd p = {j->ctl, POLLIN, 0};
    uint64_t written, write_ns;
    int active, finished;
    if (poll(&p, 1, 250) > 0) {
      char cmd[64];
      ssize_t n = recv(j->ctl, cmd, sizeof cmd - 1, 0);
      if (n > 0) cmd[n] = '\0';
      if (n <= 0 || strstr(cmd, "STOP")) stop_all(j);
    }
    pthread_mutex_lock(&j->mu);
    written = j->written;
    write_ns = j->write_ns;
    active = j->active;
    finished = done(arg);
    if (j->nfs_port != j->nfs_port_sent) {
      j->nfs_port_sent = j->nfs_port;
      send_line(j->ctl, "N %d", j->nfs_port);
    }
    pthread_mutex_unlock(&j->mu);
    send_line(j->ctl, "P %llu %llu %llu %d", (unsigned long long)(j->start + written),
              (unsigned long long)(write_ns / 1000000u), (unsigned long long)(now_ms() - started),
              active);
    if (finished) return;
  }
}

/* ---------------------------------------------------- over one connection */

typedef struct single {
  job_t *j;
  uint64_t expected; /* body bytes the NAS will send, or UNKNOWN_LEN */
  uint64_t received;
  char *buf[NBUF];
  size_t len[NBUF];
  int full[NBUF], full_head, full_n; /* filled, in order */
  int free_[NBUF], free_head, free_n;
  int reader_done, writer_done;
  int read_err; /* 0, 1 drop, 2 timeout */
  char read_why[160];
} single_t;

static void push(int *q, int *head, int *n, int idx) { q[(*head + *n) % NBUF] = idx; ++*n; }
static int pop(int *q, int *head, int *n) {
  int idx = q[*head];
  *head = (*head + 1) % NBUF;
  --*n;
  return idx;
}

/* The network side: fills buffers until the body is in, the NAS stops, or the
 * job stops. */
static void *single_reader(void *arg) {
  single_t *s = arg;
  job_t *j = s->j;
  int done = 0;
  while (!done) {
    int idx;
    size_t len = 0;
    pthread_mutex_lock(&j->mu);
    while (s->free_n == 0 && !j->abort) pthread_cond_wait(&j->cv, &j->mu);
    if (j->abort) {
      pthread_mutex_unlock(&j->mu);
      break;
    }
    idx = pop(s->free_, &s->free_head, &s->free_n);
    pthread_mutex_unlock(&j->mu);
    while (len < BUF_SIZE) {
      size_t want = BUF_SIZE - len;
      ssize_t n;
      if (s->expected != UNKNOWN_LEN) {
        if (s->received >= s->expected) {
          done = 1;
          break;
        }
        if (want > s->expected - s->received) want = (size_t)(s->expected - s->received);
      }
      n = recv(j->nas[0], s->buf[idx] + len, want, 0);
      if (n > 0) {
        len += (size_t)n;
        s->received += (uint64_t)n;
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      done = 1;
      if (n == 0) {
        if (s->expected != UNKNOWN_LEN && s->received < s->expected) {
          s->read_err = 1;
          snprintf(s->read_why, sizeof s->read_why, "the NAS ended the connection early");
        }
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        s->read_err = 2;
        snprintf(s->read_why, sizeof s->read_why, "the NAS went quiet");
      } else {
        s->read_err = 1;
        snprintf(s->read_why, sizeof s->read_why, "%s", strerror(errno));
      }
      break;
    }
    pthread_mutex_lock(&j->mu);
    if (len > 0) {
      s->len[idx] = len;
      push(s->full, &s->full_head, &s->full_n, idx);
    } else {
      push(s->free_, &s->free_head, &s->free_n, idx);
    }
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
  }
  pthread_mutex_lock(&j->mu);
  s->reader_done = 1;
  j->active = 0;
  pthread_cond_broadcast(&j->cv);
  pthread_mutex_unlock(&j->mu);
  return NULL;
}

/* The drive side: writes buffers in order. */
static void *single_writer(void *arg) {
  single_t *s = arg;
  job_t *j = s->j;
  for (;;) {
    int idx, err;
    pthread_mutex_lock(&j->mu);
    while (s->full_n == 0 && !s->reader_done && !j->abort) pthread_cond_wait(&j->cv, &j->mu);
    if (j->abort || s->full_n == 0) {
      pthread_mutex_unlock(&j->mu);
      break;
    }
    idx = pop(s->full, &s->full_head, &s->full_n);
    pthread_mutex_unlock(&j->mu);
    err = put(j, s->buf[idx], s->len[idx]);
    pthread_mutex_lock(&j->mu);
    push(s->free_, &s->free_head, &s->free_n, idx);
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
    if (err) {
      pthread_mutex_lock(&j->mu);
      j->write_err = err;
      pthread_mutex_unlock(&j->mu);
      stop_all(j);
      break;
    }
  }
  pthread_mutex_lock(&j->mu);
  s->writer_done = 1;
  pthread_cond_broadcast(&j->cv);
  pthread_mutex_unlock(&j->mu);
  return NULL;
}

static int single_done(void *arg) {
  single_t *s = arg;
  return s->reader_done && s->writer_done;
}

/* The buffers between the network and the drive, all free. 0, or -1 having
 * said so. */
static int single_buffers(job_t *j, single_t *s) {
  int i;
  for (i = 0; i < NBUF; ++i) {
    s->buf[i] = malloc(BUF_SIZE);
    if (!s->buf[i]) {
      send_line(j->ctl, "ERR request out of memory");
      return -1;
    }
    push(s->free_, &s->free_head, &s->free_n, i);
  }
  return 0;
}

/* Runs `reader` (the network) and the writer (the drive) until the file is
 * in or it stops, then sends the last line. */
static void single_pump(job_t *j, single_t *s, void *(*reader_main)(void *)) {
  pthread_t reader, writer;
  j->active = 1;
  if (start_thread(&reader, reader_main, s) != 0) {
    send_line(j->ctl, "ERR request couldn't start a thread");
    return;
  }
  if (start_thread(&writer, single_writer, s) != 0) {
    stop_all(j);
    pthread_join(reader, NULL);
    send_line(j->ctl, "ERR request couldn't start a thread");
    return;
  }
  watch(j, single_done, s);
  pthread_join(reader, NULL);
  pthread_join(writer, NULL);
  (void)fsync(j->fd);
  if (j->write_err)
    send_line(j->ctl, "ERR write %s", strerror(j->write_err));
  else if (j->abort)
    send_line(j->ctl, "STOPPED");
  else if (s->read_err)
    send_line(j->ctl, "ERR %s %s", s->read_err == 2 ? "timeout" : "drop", s->read_why);
  else if (j->start + j->written == j->size)
    send_line(j->ctl, "OK");
  else
    send_line(j->ctl, "ERR drop the NAS sent %llu of %llu bytes",
              (unsigned long long)(j->start + j->written), (unsigned long long)j->size);
}

/* The rest of the file from j->start over one connection. Sends the last line. */
static void single_pass(job_t *j) {
  char *head = malloc(MAX_HEADER + 1), value[64];
  size_t hlen, body_at, len;
  char *request;
  int status, rc, i;
  single_t s;

  memset(&s, 0, sizeof s);
  s.j = j;
  request = head ? with_range(j->request, j->reqlen, j->start, -1, &len) : NULL;
  if (!request) {
    free(head);
    send_line(j->ctl, "ERR request out of memory");
    return;
  }
  j->nas[0] = connect_nas(j->ip, j->port);
  if (j->nas[0] < 0) {
    free(request);
    free(head);
    send_line(j->ctl, "ERR connect couldn't reach the NAS");
    return;
  }
  rc = send_all(j->nas[0], request, len);
  free(request);
  if (rc != 0) {
    send_line(j->ctl, "ERR drop couldn't send the request");
    goto out;
  }
  rc = read_head(j->nas[0], head, &hlen, &body_at, &status);
  if (rc != 0) {
    send_line(j->ctl, rc == 2 ? "ERR timeout the NAS didn't answer" : "ERR drop the NAS closed the connection");
    goto out;
  }
  send_line(j->ctl, "S %d", status);
  if (status == 200 && j->start > 0) {
    /* The NAS ignored the range: the whole file, from the start. */
    if (ftruncate(j->fd, 0) != 0) {
      send_line(j->ctl, "ERR file %s", strerror(errno));
      goto out;
    }
    j->start = 0;
    send_line(j->ctl, "P 0 0 0 1");
  } else if (!(status == 206 && j->start > 0) && status != 200) {
    send_line(j->ctl, "ERR http %d", status);
    goto out;
  }
  if (header(head, "transfer-encoding", value, sizeof value) && strstr(value, "chunked")) {
    send_line(j->ctl, "ERR chunked the NAS sends this file in chunks");
    goto out;
  }
  s.expected = UNKNOWN_LEN;
  if (header(head, "content-length", value, sizeof value)) {
    s.expected = strtoull(value, NULL, 10);
    if (j->start + s.expected != j->size) {
      send_line(j->ctl, "ERR changed the file on the NAS is a different size now");
      goto out;
    }
  }

  if (single_buffers(j, &s) != 0) goto out;
  if (hlen > body_at) { /* what came with the header is the body's start */
    size_t extra = hlen - body_at;
    int idx = pop(s.free_, &s.free_head, &s.free_n);
    memcpy(s.buf[idx], head + body_at, extra);
    s.len[idx] = extra;
    s.received = extra;
    push(s.full, &s.full_head, &s.full_n, idx);
  }
  single_pump(j, &s, single_reader);

out:
  for (i = 0; i < NBUF; ++i) free(s.buf[i]);
  free(head);
  pthread_mutex_lock(&j->mu);
  if (j->nas[0] >= 0) close(j->nas[0]);
  j->nas[0] = -1;
  pthread_mutex_unlock(&j->mu);
}

/* ---------------------------------------------------- over two connections */

/* Each connection asks for the next CHUNK of the file; finished chunks wait
 * (at most WINDOW of them) for the writer, which puts them down in order. */
typedef struct multi {
  job_t *j;
  int streams;
  uint64_t chunks;     /* chunks from j->start to the end */
  uint64_t next_claim; /* the next chunk a connection takes */
  uint64_t next_write; /* the next chunk the writer puts down */
  char *pool[WINDOW];
  int pool_free[WINDOW], nfree;
  uint64_t ready_chunk[WINDOW]; /* fetched, waiting for the writer */
  char *ready_buf[WINDOW];
  size_t ready_len[WINDOW];
  int nready;
  int fetchers_left, writer_done;
  int no_ranges;  /* the NAS answered a range with the whole file */
  int net_err;    /* 1 drop, 2 timeout, 3 http, 4 changed, 7 smb or nfs (refused) */
  const struct smb_sign_in *smb; /* over SMB: each connection signs in with this */
  const struct nfs_target *nfs;  /* over NFS: each connection mounts this */
  int http_status;
  char why[160];
} multi_t;

static int multi_done(void *arg) {
  multi_t *m = arg;
  return m->fetchers_left == 0 && m->writer_done;
}

/* One chunk into buf. 0, or the failure (net_err codes; 5: the NAS ignores
 * ranges; 6: stopped). */
static int fetch_chunk(multi_t *m, int slot, uint64_t chunk, char *buf, size_t *got, int *status,
                       char *why, size_t why_size) {
  job_t *j = m->j;
  uint64_t from = j->start + chunk * CHUNK, to = from + CHUNK;
  char value[96], expect[64];
  char *head; /* MAX_HEADER: on the heap, never this thread's stack */
  size_t hlen = 0, body_at = 0, len, have = 0, want;
  char *request;
  int nas, rc;
  if (to > j->size) to = j->size;
  want = (size_t)(to - from);
  head = malloc(MAX_HEADER + 1);
  request = head ? with_range(j->request, j->reqlen, from, (int64_t)to - 1, &len) : NULL;
  if (!request) {
    free(head);
    snprintf(why, why_size, "out of memory");
    return 1;
  }
  nas = connect_nas(j->ip, j->port);
  if (nas < 0) {
    free(request);
    free(head);
    snprintf(why, why_size, "couldn't reach the NAS");
    return 1;
  }
  pthread_mutex_lock(&j->mu);
  if (j->abort) {
    pthread_mutex_unlock(&j->mu);
    close(nas);
    free(request);
    free(head);
    return 6;
  }
  j->nas[slot] = nas;
  pthread_mutex_unlock(&j->mu);
  rc = send_all(nas, request, len) == 0 ? read_head(nas, head, &hlen, &body_at, status) : 1;
  free(request);
  if (rc != 0) {
    snprintf(why, why_size, rc == 2 ? "the NAS didn't answer" : "the NAS closed the connection");
    goto done;
  }
  if (*status == 200) {
    rc = 5;
    goto done;
  }
  if (*status != 206) {
    rc = 3;
    goto done;
  }
  /* The part asked for, from the right place (a NAS that sends past the end
   * asked for is fine: only `want` bytes are read). */
  snprintf(expect, sizeof expect, "bytes %llu-", (unsigned long long)from);
  if (!header(head, "content-range", value, sizeof value) ||
      strncmp(value, expect, strlen(expect)) != 0 ||
      (header(head, "content-length", value, sizeof value) && strtoull(value, NULL, 10) < want)) {
    snprintf(why, why_size, "the NAS sent a different part of the file");
    rc = 4;
    goto done;
  }
  if (hlen > body_at) {
    have = hlen - body_at;
    if (have > want) have = want;
    memcpy(buf, head + body_at, have);
  }
  while (have < want) {
    ssize_t n = recv(nas, buf + have, want - have, 0);
    if (n > 0) {
      have += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      snprintf(why, why_size, "the NAS went quiet");
      rc = 2;
    } else {
      snprintf(why, why_size, "the NAS ended the connection early");
      rc = 1;
    }
    break;
  }
done:
  pthread_mutex_lock(&j->mu);
  j->nas[slot] = -1;
  if (j->abort && rc != 0) rc = 6;
  pthread_mutex_unlock(&j->mu);
  close(nas);
  free(head);
  *got = have;
  return rc;
}

typedef struct fetcher {
  multi_t *m;
  int slot;
  struct smb2_context *smb2; /* over SMB: this connection's sign-in, kept chunk to chunk */
  struct smb2fh *fh;
  struct nfs_context *nfs; /* over NFS: this connection's mount, kept chunk to chunk */
  struct nfsfh *nfh;
} fetcher_t;

static int smb_fetch_chunk(fetcher_t *f, uint64_t chunk, char *buf, size_t *got, char *why,
                           size_t why_size);
static void smb_fetcher_end(fetcher_t *f);
static int nfs_fetch_chunk(fetcher_t *f, uint64_t chunk, char *buf, size_t *got, char *why,
                           size_t why_size);
static void nfs_fetcher_end(fetcher_t *f);

static void *fetcher_main(void *arg) {
  fetcher_t *f = arg;
  multi_t *m = f->m;
  job_t *j = m->j;
  for (;;) {
    uint64_t chunk;
    char *buf;
    size_t got = 0;
    int b, rc = 0, tries, status = 0;
    char why[160] = "";
    pthread_mutex_lock(&j->mu);
    while (!j->abort && m->next_claim < m->chunks &&
           (m->nfree == 0 || m->next_claim - m->next_write >= WINDOW))
      pthread_cond_wait(&j->cv, &j->mu);
    if (j->abort || m->next_claim >= m->chunks) {
      pthread_mutex_unlock(&j->mu);
      break;
    }
    chunk = m->next_claim++;
    b = m->pool_free[--m->nfree];
    buf = m->pool[b];
    ++j->active;
    pthread_mutex_unlock(&j->mu);
    for (tries = 0; tries < CHUNK_TRIES; ++tries) {
      rc = m->nfs   ? nfs_fetch_chunk(f, chunk, buf, &got, why, sizeof why)
           : m->smb ? smb_fetch_chunk(f, chunk, buf, &got, why, sizeof why)
                    : fetch_chunk(m, f->slot, chunk, buf, &got, &status, why, sizeof why);
      if (rc != 1 && rc != 2) break; /* only the network's hiccups are retried */
      sleep_ms(500u * (unsigned)(tries + 1));
    }
    pthread_mutex_lock(&j->mu);
    --j->active;
    if (rc == 0) {
      m->ready_chunk[m->nready] = chunk;
      m->ready_buf[m->nready] = buf;
      m->ready_len[m->nready] = got;
      ++m->nready;
    } else {
      m->pool_free[m->nfree++] = b;
      if (rc != 6 && !m->no_ranges && !m->net_err) {
        if (rc == 5) {
          m->no_ranges = 1;
        } else {
          m->net_err = rc;
          m->http_status = status;
          snprintf(m->why, sizeof m->why, "%s", why);
        }
      }
    }
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
    if (rc != 0) {
      stop_all(j);
      break;
    }
  }
  if (m->smb) smb_fetcher_end(f);
  if (m->nfs) nfs_fetcher_end(f);
  pthread_mutex_lock(&j->mu);
  --m->fetchers_left;
  pthread_cond_broadcast(&j->cv);
  pthread_mutex_unlock(&j->mu);
  return NULL;
}

static void *multi_writer(void *arg) {
  multi_t *m = arg;
  job_t *j = m->j;
  while (m->next_write < m->chunks) {
    int i, at = -1, err;
    char *buf;
    size_t len;
    pthread_mutex_lock(&j->mu);
    for (;;) {
      for (i = 0; i < m->nready; ++i)
        if (m->ready_chunk[i] == m->next_write) at = i;
      if (at >= 0 || j->abort || m->fetchers_left == 0) break;
      pthread_cond_wait(&j->cv, &j->mu);
    }
    if (at < 0 || j->abort) {
      pthread_mutex_unlock(&j->mu);
      break;
    }
    buf = m->ready_buf[at];
    len = m->ready_len[at];
    m->ready_chunk[at] = m->ready_chunk[m->nready - 1];
    m->ready_buf[at] = m->ready_buf[m->nready - 1];
    m->ready_len[at] = m->ready_len[m->nready - 1];
    --m->nready;
    pthread_mutex_unlock(&j->mu);
    err = put(j, buf, len);
    pthread_mutex_lock(&j->mu);
    for (i = 0; i < WINDOW; ++i)
      if (m->pool[i] == buf) m->pool_free[m->nfree++] = i;
    ++m->next_write;
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
    if (err) {
      pthread_mutex_lock(&j->mu);
      j->write_err = err;
      pthread_mutex_unlock(&j->mu);
      stop_all(j);
      break;
    }
  }
  pthread_mutex_lock(&j->mu);
  m->writer_done = 1;
  pthread_cond_broadcast(&j->cv);
  pthread_mutex_unlock(&j->mu);
  return NULL;
}

/* The rest of the file from j->start over `streams` connections. Sends the
 * last line and returns 0, or returns 1 (nothing sent) when the NAS turned
 * out not to take ranges: the caller carries on over one connection. Over
 * SMB (`smb` set) each connection is a sign-in of its own, reading its chunks
 * with several reads in flight; a NAS can serve each sign-in slowly but
 * several side by side (an Unraid NAS: about 17 MB/s over one, 40 over four
 * by WebDAV). Over NFS (`nfs` set) likewise, each connection a mount of
 * its own. */
static int multi_pass(job_t *j, int streams, const struct smb_sign_in *smb,
                      const struct nfs_target *nfs) {
  multi_t m;
  fetcher_t f[MAX_STREAMS];
  pthread_t ft[MAX_STREAMS], writer;
  int i, rc = 0, started = 0;

  memset(&m, 0, sizeof m);
  m.j = j;
  m.streams = streams;
  m.smb = smb;
  m.nfs = nfs;
  m.chunks = (j->size - j->start + CHUNK - 1) / CHUNK;
  for (i = 0; i < WINDOW; ++i) {
    m.pool[i] = malloc(CHUNK);
    if (!m.pool[i]) {
      send_line(j->ctl, "ERR request out of memory");
      goto out;
    }
    m.pool_free[m.nfree++] = i;
  }
  if (!smb && !nfs) send_line(j->ctl, "S 206");
  memset(f, 0, sizeof f);
  for (i = 0; i < streams; ++i) {
    f[i].m = &m;
    f[i].slot = i;
    pthread_mutex_lock(&j->mu);
    ++m.fetchers_left;
    pthread_mutex_unlock(&j->mu);
    if (start_thread(&ft[i], fetcher_main, &f[i]) != 0) {
      pthread_mutex_lock(&j->mu);
      --m.fetchers_left;
      pthread_mutex_unlock(&j->mu);
      break;
    }
  }
  started = i;
  if (started == 0 || start_thread(&writer, multi_writer, &m) != 0) {
    stop_all(j);
    for (i = 0; i < started; ++i) pthread_join(ft[i], NULL);
    send_line(j->ctl, "ERR request couldn't start a thread");
    goto out;
  }
  watch(j, multi_done, &m);
  for (i = 0; i < started; ++i) pthread_join(ft[i], NULL);
  pthread_join(writer, NULL);
  (void)fsync(j->fd);
  if (m.no_ranges && !j->write_err) {
    rc = 1; /* over one connection instead, from where the writer got to */
  } else if (j->write_err) {
    send_line(j->ctl, "ERR write %s", strerror(j->write_err));
  } else if (m.net_err == 3) {
    send_line(j->ctl, "ERR http %d", m.http_status);
  } else if (m.net_err == 4) {
    send_line(j->ctl, "ERR changed %s", m.why);
  } else if (m.net_err == 7) {
    send_line(j->ctl, "ERR %s %s", nfs ? "nfs" : "smb", m.why);
  } else if (m.net_err) {
    send_line(j->ctl, "ERR %s %s", m.net_err == 2 ? "timeout" : "drop", m.why);
  } else if (j->abort) {
    send_line(j->ctl, "STOPPED");
  } else if (j->start + j->written == j->size) {
    send_line(j->ctl, "OK");
  } else {
    send_line(j->ctl, "ERR drop the NAS sent %llu of %llu bytes",
              (unsigned long long)(j->start + j->written), (unsigned long long)j->size);
  }
out:
  for (i = 0; i < WINDOW; ++i) free(m.pool[i]);
  return rc;
}

/* ------------------------------------------------------------ one file */

/* Reads exactly len bytes (the request after the job line). */
static int recv_exact(int fd, char *p, size_t len) {
  while (len > 0) {
    ssize_t n = recv(fd, p, len, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

/* The job line, a byte at a time (it's short), up to '\n'. */
static int recv_line(int fd, char *line, size_t size) {
  size_t have = 0;
  while (have + 1 < size) {
    char c;
    ssize_t n = recv(fd, &c, 1, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    if (c == '\n') {
      line[have] = '\0';
      return 0;
    }
    line[have++] = c;
  }
  return -1;
}

/* Where the file got to, made the start of this pass: its last bytes may not
 * have reached the disk before a crash or a power cut, so they're fetched
 * again. 0, or errno. */
static int resume_point(job_t *j) {
  struct stat st;
  uint64_t have, offset;
  if (fstat(j->fd, &st) != 0) return errno;
  have = (uint64_t)st.st_size;
  if (have > j->size) have = 0; /* bigger than on the NAS (it changed): again */
  offset = have == j->size ? have : have > REFETCH ? have - REFETCH : 0;
  if (offset < j->size) offset -= offset % (1u << 20);
  if (ftruncate(j->fd, (off_t)offset) != 0) return errno;
  pthread_mutex_lock(&j->mu);
  j->start = offset;
  j->written = 0;
  pthread_mutex_unlock(&j->mu);
  return 0;
}

/* ------------------------------------------------------------ over SMB */

/* The sign-in NASBridge sends: NUL-ended fields. */
typedef struct smb_sign_in {
  char *share, *path, *user, *domain, *password;
} smb_sign_in_t;

/* Splits `len` bytes into `n` NUL-ended fields. 0, or -1 when they aren't. */
static int smb_fields(char *bytes, size_t len, char **field, int n) {
  size_t at = 0;
  int k;
  for (k = 0; k < n; ++k) {
    char *end;
    if (at >= len) return -1;
    end = memchr(bytes + at, '\0', len - at);
    if (!end) return -1;
    field[k] = bytes + at;
    at = (size_t)(end - bytes) + 1;
  }
  return at == len ? 0 : -1;
}

/* What an SMB failure was, as an ERR kind and a few words. */
static const char *smb_failure(struct smb2_context *smb2, char *why, size_t size) {
  const char *text = smb2 ? smb2_get_error(smb2) : NULL;
  switch (smb2 ? smb2_get_nterror(smb2) : 0) {
  case SMB2_STATUS_LOGON_FAILURE:
  case SMB2_STATUS_WRONG_PASSWORD:
  case SMB2_STATUS_NO_SUCH_USER:
  case SMB2_STATUS_ACCOUNT_RESTRICTION:
  case SMB2_STATUS_ACCOUNT_DISABLED:
  case SMB2_STATUS_ACCOUNT_LOCKED_OUT:
  case SMB2_STATUS_PASSWORD_EXPIRED:
    snprintf(why, size, "sign-in refused");
    return "smb";
  case SMB2_STATUS_BAD_NETWORK_NAME:
    snprintf(why, size, "no such share");
    return "smb";
  case SMB2_STATUS_OBJECT_NAME_NOT_FOUND:
  case SMB2_STATUS_OBJECT_PATH_NOT_FOUND:
  case SMB2_STATUS_NO_SUCH_FILE:
    snprintf(why, size, "not found");
    return "smb";
  case SMB2_STATUS_ACCESS_DENIED:
  case SMB2_STATUS_NETWORK_ACCESS_DENIED:
    snprintf(why, size, "access denied");
    return "smb";
  default:
    break;
  }
  snprintf(why, size, "%s", text && *text ? text : "the NAS stopped answering");
  if (text && strstr(text, "onnect failed")) return "connect";
  if (text && strstr(text, "imeout")) return "timeout";
  return "drop";
}

/* A sign-in to the share, or NULL with *kind and why set. */
static struct smb2_context *smb_connect(const char *ip, int port, const smb_sign_in_t *in,
                                        const char **kind, char *why, size_t size) {
  struct smb2_context *smb2 = smb2_init_context();
  char server[96];
  if (!smb2) {
    *kind = "request";
    snprintf(why, size, "out of memory");
    return NULL;
  }
  smb2_set_timeout(smb2, NAS_REPLY_S);
  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  if (in->domain[0]) smb2_set_domain(smb2, in->domain);
  smb2_set_password(smb2, in->password);
  snprintf(server, sizeof server, "%s:%d", ip, port);
  if (smb2_connect_share(smb2, server, in->share, in->user) < 0) {
    *kind = smb_failure(smb2, why, size);
    smb2_destroy_context(smb2);
    return NULL;
  }
  return smb2;
}

/* Reads [from, from + want) of an open file into buf, up to SMB_IN_FLIGHT
 * reads in flight. 0, or -1 with the sign-in's error. */
typedef struct smb_span {
  int in_flight;
  int failed;
  size_t short_at[SMB_IN_FLIGHT * 64]; /* where short answers ended, and how much is missing */
  uint32_t short_len[SMB_IN_FLIGHT * 64];
  int shorts;
} smb_span_t;

typedef struct smb_span_read {
  smb_span_t *span;
  size_t at;
  uint32_t want;
} smb_span_read_t;

static void smb_span_answer(struct smb2_context *smb2, int status, void *data, void *cb_data) {
  smb_span_read_t *r = cb_data;
  smb_span_t *span = r->span;
  (void)smb2;
  (void)data;
  --span->in_flight;
  if (status <= 0) {
    if (!span->failed) span->failed = status < 0 ? -status : EIO;
  } else if ((uint32_t)status < r->want) {
    if (span->shorts < (int)(sizeof span->short_at / sizeof span->short_at[0])) {
      span->short_at[span->shorts] = r->at + (size_t)status;
      span->short_len[span->shorts++] = r->want - (uint32_t)status;
    } else if (!span->failed) {
      span->failed = EIO;
    }
  }
  free(r);
}

static int smb_read_span(struct smb2_context *smb2, struct smb2fh *fh, uint64_t from, size_t want,
                         char *buf) {
  smb_span_t span;
  uint32_t most = smb2_get_max_read_size(smb2);
  size_t asked = 0;
  uint64_t heard = now_ms();
  int i;
  memset(&span, 0, sizeof span);
  if (most == 0 || most > SMB_READ) most = SMB_READ;
  while (!span.failed && (asked < want || span.in_flight > 0)) {
    struct pollfd p;
    int before = span.in_flight;
    while (!span.failed && span.in_flight < SMB_IN_FLIGHT && asked < want) {
      smb_span_read_t *r = malloc(sizeof *r);
      uint32_t n = want - asked > most ? most : (uint32_t)(want - asked);
      if (!r) {
        span.failed = ENOMEM;
        break;
      }
      r->span = &span;
      r->at = asked;
      r->want = n;
      if (smb2_pread_async(smb2, fh, (uint8_t *)buf + asked, n, from + asked, smb_span_answer, r) <
          0) {
        free(r);
        span.failed = EIO;
        break;
      }
      ++span.in_flight;
      asked += n;
    }
    if (span.failed || span.in_flight == 0) break;
    p.fd = smb2_get_fd(smb2);
    p.events = (short)smb2_which_events(smb2);
    p.revents = 0;
    if (poll(&p, 1, 250) < 0 && errno != EINTR) {
      span.failed = errno;
      break;
    }
    if (p.revents != 0 && smb2_service(smb2, p.revents) < 0) {
      if (!span.failed) span.failed = EIO;
      break;
    }
    if (span.in_flight < before) heard = now_ms();
    if (now_ms() - heard > NAS_REPLY_S * 1000u) {
      span.failed = ETIMEDOUT;
      break;
    }
  }
  if (span.failed || span.in_flight > 0) return -1; /* the caller drops the sign-in */
  /* Short answers (the NAS's credits ran low): the rest, one read at a time. */
  for (i = 0; i < span.shorts; ++i) {
    size_t at = span.short_at[i];
    uint32_t left = span.short_len[i];
    while (left > 0) {
      int n = smb2_pread(smb2, fh, (uint8_t *)buf + at, left, from + at);
      if (n <= 0) return -1;
      at += (size_t)n;
      left -= (uint32_t)n;
    }
  }
  return 0;
}

/* One chunk of the file over this connection's own sign-in (made on its
 * first chunk, and again after a failure). 0, or multi's codes: 1 drop and 2
 * timeout (tried again), 4 changed, 6 stopped, 7 refused (sign-in, share,
 * not found, denied). */
static int smb_fetch_chunk(fetcher_t *f, uint64_t chunk, char *buf, size_t *got, char *why,
                           size_t why_size) {
  multi_t *m = f->m;
  job_t *j = m->j;
  uint64_t from = j->start + chunk * CHUNK, to = from + CHUNK;
  const char *kind = "drop";
  int rc = 0;
  *got = 0;
  if (to > j->size) to = j->size;
  if (!f->smb2) {
    struct smb2_stat_64 st;
    f->smb2 = smb_connect(j->ip, j->port, m->smb, &kind, why, why_size);
    if (!f->smb2) return strcmp(kind, "smb") == 0 ? 7 : strcmp(kind, "timeout") == 0 ? 2 : 1;
    f->fh = smb2_open(f->smb2, m->smb->path, O_RDONLY);
    if (!f->fh || smb2_fstat(f->smb2, f->fh, &st) < 0) {
      kind = smb_failure(f->smb2, why, why_size);
      smb_fetcher_end(f);
      return strcmp(kind, "smb") == 0 ? 7 : strcmp(kind, "timeout") == 0 ? 2 : 1;
    }
    if (st.smb2_size != j->size) {
      snprintf(why, why_size, "the file on the NAS is a different size now");
      smb_fetcher_end(f);
      return 4;
    }
    pthread_mutex_lock(&j->mu);
    j->nas[f->slot] = smb2_get_fd(f->smb2); /* a stop cuts it off */
    rc = j->abort;
    pthread_mutex_unlock(&j->mu);
    if (rc) return 6;
  }
  if (smb_read_span(f->smb2, f->fh, from, (size_t)(to - from), buf) != 0) {
    pthread_mutex_lock(&j->mu);
    rc = j->abort;
    pthread_mutex_unlock(&j->mu);
    if (rc) return 6;
    kind = smb_failure(f->smb2, why, why_size);
    smb_fetcher_end(f); /* the next try signs in again */
    return strcmp(kind, "smb") == 0 ? 7 : strcmp(kind, "timeout") == 0 ? 2 : 1;
  }
  *got = (size_t)(to - from);
  return 0;
}

/* Signs this connection out (reads still out answer as shut down). */
static void smb_fetcher_end(fetcher_t *f) {
  job_t *j = f->m->j;
  if (!f->smb2) return;
  pthread_mutex_lock(&j->mu);
  j->nas[f->slot] = -1;
  pthread_mutex_unlock(&j->mu);
  smb2_destroy_context(f->smb2); /* closes the file too */
  f->smb2 = NULL;
  f->fh = NULL;
}

/* Copies one file over SMB (the NBS1 line, then the sign-in). Every path out
 * sends its final line. */
static void run_smb_job(job_t *j, char *line) {
  char *field[7], *save = NULL, *bytes = NULL, *sign[5], ip[64], path[MAX_FILE];
  smb_sign_in_t in;
  size_t len;
  int k, err, streams;
  field[0] = strtok_r(line, "\t", &save);
  for (k = 1; k < 7; ++k) field[k] = strtok_r(NULL, "\t", &save);
  if (!field[5]) {
    send_line(j->ctl, "ERR request not a NASBridge copy request");
    return;
  }
  snprintf(ip, sizeof ip, "%s", field[1]);
  j->ip = ip;
  j->port = atoi(field[2]);
  j->size = strtoull(field[3], NULL, 10);
  snprintf(path, sizeof path, "%s", field[4]);
  len = (size_t)strtoull(field[5], NULL, 10);
  streams = field[6] ? atoi(field[6]) : 1;
  if (streams < 1) streams = 1;
  if (streams > MAX_STREAMS) streams = MAX_STREAMS;
  if (len < 5 || len > MAX_SIGN_IN || path[0] != '/') {
    send_line(j->ctl, "ERR request bad request");
    return;
  }
  bytes = malloc(len);
  if (!bytes || recv_exact(j->ctl, bytes, len) != 0 || smb_fields(bytes, len, sign, 5) != 0) {
    send_line(j->ctl, "ERR request bad sign-in");
    free(bytes);
    return;
  }
  in.share = sign[0];
  in.path = sign[1];
  in.user = sign[2];
  in.domain = sign[3];
  in.password = sign[4];
  j->fd = open(path, O_RDWR | O_CREAT, FILE_MODE);
  if (j->fd < 0) {
    send_line(j->ctl, "ERR file %s", strerror(errno));
    free(bytes);
    return;
  }
  (void)fchmod(j->fd, FILE_MODE); /* open's mode is reduced by the umask */
  if ((err = resume_point(j)) != 0) {
    send_line(j->ctl, "ERR file %s", strerror(err));
  } else {
    send_line(j->ctl, "P %llu 0 0 0", (unsigned long long)j->start);
    j->last_sync = now_ms();
    if (j->start == j->size)
      send_line(j->ctl, "OK");
    else
      multi_pass(j, streams, &in, NULL);
  }
  memset(bytes, 0, len); /* the password */
  free(bytes);
}

/* ------------------------------------------------------------ over NFS */

/* The export NASBridge sends, and who to read as. */
typedef struct nfs_target {
  char *exported, *path;
  int mount_port, uid, gid;
} nfs_target_t;

/* The fields after the path (or after the export, with no path). */
static void nfs_numbers(nfs_target_t *t, char **field) {
  t->mount_port = atoi(field[0]);
  t->uid = atoi(field[1]);
  t->gid = atoi(field[2]);
}

/* What an NFS failure was, as an ERR kind and a few words: libnfs gives a
 * negative errno and its own words (a refused connection only shows in
 * those). `mounting`: a failure to mount the export. */
static const char *nfs_failure(struct nfs_context *nfs, int rc, int mounting, char *why,
                               size_t size) {
  const char *text = nfs ? nfs_get_error(nfs) : NULL;
  int err = rc < 0 ? -rc : rc;
  if (text && (strstr(text, "AUTH") || strstr(text, "auth error"))) {
    snprintf(why, size, "the NAS refused this PS5");
    return "nfs";
  }
  if (text && (strstr(text, "imeout") || strstr(text, "timed out"))) err = ETIMEDOUT;
  else if (text && (strstr(text, "refused") || strstr(text, "unreachable"))) err = ECONNREFUSED;
  else if (text && mounting && strstr(text, "POLLHUP")) err = ECONNREFUSED; /* how a refusal shows */
  switch (err) {
  case ENOENT:
  case ENOTDIR:
    snprintf(why, size, mounting ? "not in an NFS export" : "not found");
    return "nfs";
  case EACCES:
  case EPERM:
    snprintf(why, size, mounting ? "the NAS refused this PS5" : "access denied");
    return "nfs";
  default:
    break;
  }
  snprintf(why, size, "%s", text && *text ? text : "the NAS stopped answering");
  if (err == ETIMEDOUT) return "timeout";
  if (err == ECONNREFUSED || err == EHOSTUNREACH || err == ENETUNREACH) return "connect";
  return "drop";
}

/* The local port of the context's connection (0 when unknown). */
static int nfs_local_port(struct nfs_context *nfs) {
  struct sockaddr_in a;
  socklen_t len = sizeof a;
  memset(&a, 0, sizeof a);
  if (getsockname(nfs_get_fd(nfs), (struct sockaddr *)&a, &len) != 0) return 0;
  return ntohs(a.sin_port);
}

/* A mount of the export, or NULL with *kind and why set. */
static struct nfs_context *nfs_connect(const char *ip, int port, const nfs_target_t *t,
                                       const char **kind, char *why, size_t size) {
  struct nfs_context *nfs = nfs_init_context();
  int rc;
  if (!nfs) {
    *kind = "request";
    snprintf(why, size, "out of memory");
    return NULL;
  }
  nfs_set_timeout(nfs, NAS_REPLY_S * 1000);
  nfs_set_uid(nfs, t->uid);
  nfs_set_gid(nfs, t->gid);
  nfs_set_version(nfs, 3); /* NFSv3 (NFS_V3 is in a header the helper leaves out) */
  nfs_set_autoreconnect(nfs, 0); /* a lost connection fails the read: tried again here */
  nfs_set_nfsport(nfs, port);
  if (t->mount_port > 0) nfs_set_mountport(nfs, t->mount_port);
  rc = nfs_mount(nfs, ip, t->exported);
  if (rc < 0) {
    *kind = nfs_failure(nfs, rc, 1, why, size);
    nfs_destroy_context(nfs);
    return NULL;
  }
  return nfs;
}

/* Reads [from, from + want) of an open file into buf, up to NFS_IN_FLIGHT
 * reads in flight. 0, or -1 (the caller drops the mount). */
typedef struct nfs_span {
  int in_flight;
  int failed; /* errno */
  size_t short_at[NFS_IN_FLIGHT * 64]; /* where short answers ended, and how much is missing */
  uint32_t short_len[NFS_IN_FLIGHT * 64];
  int shorts;
} nfs_span_t;

typedef struct nfs_span_read {
  nfs_span_t *span;
  size_t at;
  uint32_t want;
} nfs_span_read_t;

static void nfs_span_answer(int status, struct nfs_context *nfs, void *data, void *cb_data) {
  nfs_span_read_t *r = cb_data;
  nfs_span_t *span = r->span;
  (void)nfs;
  (void)data;
  --span->in_flight;
  if (status <= 0) {
    if (!span->failed) span->failed = status < 0 ? -status : EIO;
  } else if ((uint32_t)status < r->want) {
    if (span->shorts < (int)(sizeof span->short_at / sizeof span->short_at[0])) {
      span->short_at[span->shorts] = r->at + (size_t)status;
      span->short_len[span->shorts++] = r->want - (uint32_t)status;
    } else if (!span->failed) {
      span->failed = EIO;
    }
  }
  free(r);
}

static int nfs_read_span(struct nfs_context *nfs, struct nfsfh *fh, uint64_t from, size_t want,
                         char *buf) {
  nfs_span_t span;
  size_t most = nfs_get_readmax(nfs), asked = 0;
  uint64_t heard = now_ms();
  int i;
  memset(&span, 0, sizeof span);
  if (most == 0 || most > SMB_READ) most = SMB_READ;
  while (!span.failed && (asked < want || span.in_flight > 0)) {
    struct pollfd p;
    int before = span.in_flight;
    while (!span.failed && span.in_flight < NFS_IN_FLIGHT && asked < want) {
      nfs_span_read_t *r = malloc(sizeof *r);
      uint32_t n = want - asked > most ? (uint32_t)most : (uint32_t)(want - asked);
      if (!r) {
        span.failed = ENOMEM;
        break;
      }
      r->span = &span;
      r->at = asked;
      r->want = n;
      if (nfs_pread_async(nfs, fh, buf + asked, n, from + asked, nfs_span_answer, r) != 0) {
        free(r);
        span.failed = EIO;
        break;
      }
      ++span.in_flight;
      asked += n;
    }
    if (span.failed || span.in_flight == 0) break;
    p.fd = nfs_get_fd(nfs);
    p.events = (short)nfs_which_events(nfs);
    p.revents = 0;
    if (poll(&p, 1, 250) < 0 && errno != EINTR) {
      span.failed = errno;
      break;
    }
    /* Called even when nothing came in: libnfs times requests out here. */
    if (nfs_service(nfs, p.revents) < 0) {
      if (!span.failed) span.failed = EIO;
      break;
    }
    if (span.in_flight < before) heard = now_ms();
    if (now_ms() - heard > NAS_REPLY_S * 1000u) {
      span.failed = ETIMEDOUT;
      break;
    }
  }
  if (span.failed || span.in_flight > 0) return -1;
  /* Short answers (the NAS sent less than asked): the rest, one read at a time. */
  for (i = 0; i < span.shorts; ++i) {
    size_t at = span.short_at[i];
    uint32_t left = span.short_len[i];
    while (left > 0) {
      int n = nfs_pread(nfs, fh, buf + at, left, from + at);
      if (n <= 0) return -1;
      at += (size_t)n;
      left -= (uint32_t)n;
    }
  }
  return 0;
}

static int nfs_code(const char *kind) {
  return strcmp(kind, "nfs") == 0 ? 7 : strcmp(kind, "timeout") == 0 ? 2 : 1;
}

/* One chunk of the file over this connection's own mount (made on its first
 * chunk, and again after a failure). 0, or multi's codes: 1 drop and 2
 * timeout (tried again), 4 changed, 6 stopped, 7 refused. */
static int nfs_fetch_chunk(fetcher_t *f, uint64_t chunk, char *buf, size_t *got, char *why,
                           size_t why_size) {
  multi_t *m = f->m;
  job_t *j = m->j;
  uint64_t from = j->start + chunk * CHUNK, to = from + CHUNK;
  const char *kind = "drop";
  int rc = 0;
  *got = 0;
  if (to > j->size) to = j->size;
  if (!f->nfs) {
    struct nfs_stat_64 st;
    int port;
    f->nfs = nfs_connect(j->ip, j->port, m->nfs, &kind, why, why_size);
    if (!f->nfs) return nfs_code(kind);
    port = nfs_local_port(f->nfs);
    rc = nfs_open(f->nfs, m->nfs->path, O_RDONLY, &f->nfh);
    if (rc == 0) rc = nfs_fstat64(f->nfs, f->nfh, &st);
    if (rc < 0) {
      kind = nfs_failure(f->nfs, rc, 0, why, why_size);
      nfs_fetcher_end(f);
      return nfs_code(kind);
    }
    if (st.nfs_size != j->size) {
      snprintf(why, why_size, "the file on the NAS is a different size now");
      nfs_fetcher_end(f);
      return 4;
    }
    pthread_mutex_lock(&j->mu);
    j->nas[f->slot] = nfs_get_fd(f->nfs); /* a stop cuts it off */
    if (port) j->nfs_port = port;
    rc = j->abort;
    pthread_mutex_unlock(&j->mu);
    if (rc) return 6;
  }
  if (nfs_read_span(f->nfs, f->nfh, from, (size_t)(to - from), buf) != 0) {
    pthread_mutex_lock(&j->mu);
    rc = j->abort;
    pthread_mutex_unlock(&j->mu);
    if (rc) return 6;
    kind = nfs_failure(f->nfs, -EIO, 0, why, why_size);
    nfs_fetcher_end(f); /* the next try mounts again */
    return nfs_code(kind);
  }
  *got = (size_t)(to - from);
  return 0;
}

/* Lets go of this connection's mount (reads still out answer as cancelled). */
static void nfs_fetcher_end(fetcher_t *f) {
  job_t *j = f->m->j;
  if (!f->nfs) return;
  pthread_mutex_lock(&j->mu);
  j->nas[f->slot] = -1;
  pthread_mutex_unlock(&j->mu);
  nfs_destroy_context(f->nfs); /* closes the file too */
  f->nfs = NULL;
  f->nfh = NULL;
}

/* Copies one file over NFS (the NBN1 line, then the target). Every path out
 * sends its final line. */
static void run_nfs_job(job_t *j, char *line) {
  char *field[7], *save = NULL, *bytes = NULL, *part[5], ip[64], path[MAX_FILE];
  nfs_target_t t;
  size_t len;
  int k, err, streams;
  field[0] = strtok_r(line, "\t", &save);
  for (k = 1; k < 7; ++k) field[k] = strtok_r(NULL, "\t", &save);
  if (!field[5]) {
    send_line(j->ctl, "ERR request not a NASBridge copy request");
    return;
  }
  snprintf(ip, sizeof ip, "%s", field[1]);
  j->ip = ip;
  j->port = atoi(field[2]);
  j->size = strtoull(field[3], NULL, 10);
  snprintf(path, sizeof path, "%s", field[4]);
  len = (size_t)strtoull(field[5], NULL, 10);
  streams = field[6] ? atoi(field[6]) : 1;
  if (streams < 1) streams = 1;
  if (streams > MAX_STREAMS) streams = MAX_STREAMS;
  if (len < 5 || len > MAX_SIGN_IN || path[0] != '/') {
    send_line(j->ctl, "ERR request bad request");
    return;
  }
  bytes = malloc(len);
  if (!bytes || recv_exact(j->ctl, bytes, len) != 0 || smb_fields(bytes, len, part, 5) != 0) {
    send_line(j->ctl, "ERR request bad target");
    free(bytes);
    return;
  }
  t.exported = part[0];
  t.path = part[1];
  nfs_numbers(&t, part + 2);
  j->fd = open(path, O_RDWR | O_CREAT, FILE_MODE);
  if (j->fd < 0) {
    send_line(j->ctl, "ERR file %s", strerror(errno));
    free(bytes);
    return;
  }
  (void)fchmod(j->fd, FILE_MODE); /* open's mode is reduced by the umask */
  if ((err = resume_point(j)) != 0) {
    send_line(j->ctl, "ERR file %s", strerror(err));
  } else {
    send_line(j->ctl, "P %llu 0 0 0", (unsigned long long)j->start);
    j->last_sync = now_ms();
    if (j->start == j->size)
      send_line(j->ctl, "OK");
    else
      multi_pass(j, streams, NULL, &t);
  }
  free(bytes);
}

/* ------------------------------------------------- many small files at once */

/* A folder game can be tens of thousands of small files. One connection per
 * file (above) spends nearly all its time setting up, so for those NASBridge
 * streams the list over one connection instead:
 *   NASBridge -> "NBB1\t<ip>\t<port>\t<workers>\n"
 *                then per file "F\t<index>\t<size>\t<file>\t<request length>\n"
 *                and the GET request (keep-alive), and "E\n" after the last;
 *                "STOP\n" (or closing) stops it
 *   helper  -> "D <index>"            that file is in (written and closed)
 *              "Y"                    a save to the drive finished (sync() every
 *                                     BATCH_SYNC_MS; NASBridge resumes by file
 *                                     size, so it needs no more than that)
 *              "P <bytes> <files> <busy> <saving ms> <ms>" progress: the time
 *                                     spent saving (sync), and in all
 *              "OK" | "STOPPED" | "ERR <index> <kind> <why>"   the end
 * BATCH_WORKERS workers each keep a connection to the NAS open and take the
 * next file; a file is always written from its start (O_TRUNC), so one that
 * was cut off is simply done again.
 *
 * Over SMB the same, with "NBT1\t<ip>\t<port>\t<workers>\t<sign-in length>\n"
 * and the sign-in (share, user, domain, password, each ended by a NUL) first,
 * and each file's path in the share where the GET request would be. Each
 * worker signs in once and keeps that connection.
 *
 * Over NFS likewise, with "NBM1\t<ip>\t<port>\t<workers>\t<target length>\n"
 * and the target (export, MOUNT port, user ID, group ID, each ended by a
 * NUL) first, each file's path in the export, and an "N <local port>" line
 * once a worker has mounted it. */
#define BATCH_PROTO "NBB1"
#define BATCH_PROTO_SMB "NBT1"
#define BATCH_PROTO_NFS "NBM1"
#define BATCH_WORKERS 16 /* the Settings screen: 1..16 (a NAS with one disk answered fastest at 16) */
#define BATCH_QUEUE 64
#define BATCH_BUF (512u << 10) /* per worker: 16 of them stay within the payload's few MB */
#define BATCH_SYNC_MS 2000
#define BATCH_TRIES 3

typedef struct bfile {
  long long index;
  uint64_t size;
  char *path;
  char *request;
  size_t reqlen;
} bfile_t;

typedef struct batch {
  int ctl;
  const char *ip;
  int port;
  int workers;
  uint64_t sync_ms; /* spent in sync(), for the progress line */
  pthread_mutex_t mu;
  pthread_cond_t cv;
  pthread_mutex_t out; /* one line at a time to NASBridge; sync() under it */
  bfile_t queue[BATCH_QUEUE];
  int qhead, qn;
  int ended;  /* NASBridge sent the whole list */
  int abort;  /* stop: asked, failed, or NASBridge went away */
  int failed; /* an ERR line went out */
  int finished;
  int busy;   /* workers on a file */
  uint64_t bytes;
  uint64_t files;
  int nas[BATCH_WORKERS];
  int smb;                                /* NBT1: over SMB */
  smb_sign_in_t sign_in;                  /* ... with this (path unused) */
  struct smb2_context *smb2[BATCH_WORKERS]; /* ... each worker's sign-in */
  int nfs;                                /* NBM1: over NFS */
  nfs_target_t target;                    /* ... this export (path unused) */
  struct nfs_context *nfsc[BATCH_WORKERS]; /* ... each worker's mount */
  int nfs_port, nfs_port_sent;            /* ... the local port one came from */
} batch_t;

typedef struct bworker {
  batch_t *b;
  int w;
} bworker_t;

static void bsend(batch_t *b, const char *fmt, ...) {
  char line[640];
  size_t len;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  len = strlen(line);
  line[len++] = '\n';
  pthread_mutex_lock(&b->out);
  (void)send_all(b->ctl, line, len);
  pthread_mutex_unlock(&b->out);
}

static void batch_stop(batch_t *b) {
  int i;
  pthread_mutex_lock(&b->mu);
  b->abort = 1;
  for (i = 0; i < BATCH_WORKERS; ++i)
    if (b->nas[i] >= 0) shutdown(b->nas[i], SHUT_RDWR);
  pthread_cond_broadcast(&b->cv);
  pthread_mutex_unlock(&b->mu);
}

/* The first failure is reported; everything stops. Always returns -1. */
static int batch_fail(batch_t *b, long long index, const char *kind, const char *why) {
  int first;
  pthread_mutex_lock(&b->mu);
  first = !b->failed && !b->abort;
  if (first) b->failed = 1;
  pthread_mutex_unlock(&b->mu);
  if (first) bsend(b, "ERR %lld %s %s", index, kind, why);
  batch_stop(b);
  return -1;
}

static int batch_aborted(batch_t *b) {
  int a;
  pthread_mutex_lock(&b->mu);
  a = b->abort;
  pthread_mutex_unlock(&b->mu);
  return a;
}

static void set_nas(batch_t *b, int w, int fd) {
  pthread_mutex_lock(&b->mu);
  if (b->nas[w] >= 0 && b->nas[w] != fd) close(b->nas[w]);
  b->nas[w] = fd;
  if (b->abort && fd >= 0) shutdown(fd, SHUT_RDWR);
  pthread_mutex_unlock(&b->mu);
}

static int write_all(int fd, const char *p, size_t len) {
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return n == 0 ? EIO : errno;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

/* One file, over this worker's connection (made again when the NAS drops it).
 * 0, or -1 once it has failed or the batch stopped. */
static int batch_one(batch_t *b, int w, const bfile_t *f, char *buf, char *head) {
  char value[64], why[200];
  int fd, tries;
  fd = open(f->path, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE);
  if (fd < 0) return batch_fail(b, f->index, "file", strerror(errno));
  (void)fchmod(fd, FILE_MODE); /* open's mode is reduced by the umask */
  if (f->size == 0) goto done;
  for (tries = 0;; ++tries) {
    size_t hlen, body_at;
    uint64_t expected, got = 0;
    int status, rc, keep = 1, nas = b->nas[w], err = 0, net = 0;
    if (batch_aborted(b)) goto stopped;
    if (tries > 0) {
      set_nas(b, w, -1);
      nas = -1;
      if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return batch_fail(b, f->index, "file", strerror(errno));
      }
      sleep_ms(500u * (unsigned)tries);
    }
    if (nas < 0) {
      nas = connect_nas(b->ip, b->port);
      if (nas < 0) {
        if (tries + 1 < BATCH_TRIES) continue;
        close(fd);
        return batch_fail(b, f->index, "connect", "couldn't reach the NAS");
      }
      set_nas(b, w, nas);
    }
    if (send_all(nas, f->request, f->reqlen) != 0) {
      net = 1;
      snprintf(why, sizeof why, "couldn't send the request");
    } else if ((rc = read_head(nas, head, &hlen, &body_at, &status)) != 0) {
      net = rc;
      snprintf(why, sizeof why, rc == 2 ? "the NAS didn't answer" : "the NAS closed the connection");
    }
    if (net) {
      if (batch_aborted(b)) goto stopped;
      if (tries + 1 < BATCH_TRIES) continue;
      close(fd);
      return batch_fail(b, f->index, net == 2 ? "timeout" : "drop", why);
    }
    if (status != 200) {
      snprintf(why, sizeof why, "%d", status);
      close(fd);
      return batch_fail(b, f->index, "http", why);
    }
    if (header(head, "transfer-encoding", value, sizeof value) && strstr(value, "chunked")) {
      close(fd);
      return batch_fail(b, f->index, "chunked", "the NAS sends this file in chunks");
    }
    if (header(head, "connection", value, sizeof value)) {
      char *c;
      for (c = value; *c; ++c)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
      if (strstr(value, "close")) keep = 0;
    }
    expected = UNKNOWN_LEN;
    if (header(head, "content-length", value, sizeof value)) {
      expected = strtoull(value, NULL, 10);
      if (expected != f->size) {
        close(fd);
        return batch_fail(b, f->index, "changed", "the file on the NAS is a different size now");
      }
    } else {
      keep = 0; /* it ends when the NAS closes */
    }
    if (hlen > body_at) {
      size_t extra = hlen - body_at;
      if (expected != UNKNOWN_LEN && extra > expected) extra = (size_t)expected;
      err = write_all(fd, head + body_at, extra);
      got = extra;
    }
    while (!err && (expected == UNKNOWN_LEN || got < expected)) {
      size_t want = BATCH_BUF;
      ssize_t n;
      if (expected != UNKNOWN_LEN && want > expected - got) want = (size_t)(expected - got);
      n = recv(nas, buf, want, 0);
      if (n > 0) {
        err = write_all(fd, buf, (size_t)n);
        got += (uint64_t)n;
        pthread_mutex_lock(&b->mu);
        b->bytes += (uint64_t)n;
        pthread_mutex_unlock(&b->mu);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n == 0 && expected == UNKNOWN_LEN) break; /* the end, by closing */
      net = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? 2 : 1;
      break;
    }
    if (err) {
      close(fd);
      return batch_fail(b, f->index, "write", strerror(err));
    }
    if (net) {
      if (batch_aborted(b)) goto stopped;
      if (tries + 1 < BATCH_TRIES) continue;
      close(fd);
      return batch_fail(b, f->index, net == 2 ? "timeout" : "drop",
                        net == 2 ? "the NAS went quiet" : "the NAS ended the connection early");
    }
    if (got != f->size) {
      close(fd);
      return batch_fail(b, f->index, "changed", "the file on the NAS is a different size now");
    }
    if (!keep) set_nas(b, w, -1);
    break;
  }
done:
  if (close(fd) != 0) return batch_fail(b, f->index, "write", strerror(errno));
  pthread_mutex_lock(&b->mu);
  b->files++;
  pthread_mutex_unlock(&b->mu);
  bsend(b, "D %lld", f->index);
  return 0;
stopped:
  close(fd);
  return -1;
}

/* Lets go of worker w's SMB sign-in (its socket is libsmb2's to close). */
static void smb_drop(batch_t *b, int w) {
  struct smb2_context *smb2;
  pthread_mutex_lock(&b->mu);
  b->nas[w] = -1;
  smb2 = b->smb2[w];
  b->smb2[w] = NULL;
  pthread_mutex_unlock(&b->mu);
  if (smb2) {
    smb2_disconnect_share(smb2);
    smb2_destroy_context(smb2);
  }
}

/* One file over SMB, as batch_one. */
static int batch_one_smb(batch_t *b, int w, const bfile_t *f, char *buf) {
  char why[200];
  int fd, tries;
  fd = open(f->path, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE);
  if (fd < 0) return batch_fail(b, f->index, "file", strerror(errno));
  (void)fchmod(fd, FILE_MODE); /* open's mode is reduced by the umask */
  if (f->size == 0) goto done;
  for (tries = 0;; ++tries) {
    struct smb2_context *smb2 = b->smb2[w];
    struct smb2fh *fh;
    struct smb2_stat_64 st;
    const char *kind = "drop";
    uint64_t got = 0;
    uint32_t most;
    int err = 0, net = 0;
    if (batch_aborted(b)) goto stopped;
    if (tries > 0) {
      smb_drop(b, w);
      smb2 = NULL;
      if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return batch_fail(b, f->index, "file", strerror(errno));
      }
      sleep_ms(500u * (unsigned)tries);
    }
    if (!smb2) {
      smb2 = smb_connect(b->ip, b->port, &b->sign_in, &kind, why, sizeof why);
      if (!smb2) {
        if (strcmp(kind, "smb") != 0 && tries + 1 < BATCH_TRIES) continue;
        close(fd);
        return batch_fail(b, f->index, kind, why);
      }
      pthread_mutex_lock(&b->mu);
      b->smb2[w] = smb2;
      b->nas[w] = smb2_get_fd(smb2);
      if (b->abort) shutdown(b->nas[w], SHUT_RDWR);
      pthread_mutex_unlock(&b->mu);
    }
    fh = smb2_open(smb2, f->request, O_RDONLY);
    if (!fh || smb2_fstat(smb2, fh, &st) < 0) {
      kind = smb_failure(smb2, why, sizeof why);
      if (fh) smb2_close(smb2, fh);
      if (strcmp(kind, "smb") == 0) {
        close(fd);
        return batch_fail(b, f->index, kind, why);
      }
      if (batch_aborted(b)) goto stopped;
      if (tries + 1 < BATCH_TRIES) continue;
      close(fd);
      return batch_fail(b, f->index, kind, why);
    }
    if (st.smb2_size != f->size) {
      smb2_close(smb2, fh);
      close(fd);
      return batch_fail(b, f->index, "changed", "the file on the NAS is a different size now");
    }
    most = smb2_get_max_read_size(smb2);
    if (most == 0 || most > BATCH_BUF) most = BATCH_BUF;
    while (!err && got < f->size) {
      uint64_t want = f->size - got;
      int n;
      if (want > most) want = most;
      n = smb2_pread(smb2, fh, (uint8_t *)buf, (uint32_t)want, got);
      if (n <= 0) {
        if (n == 0) {
          snprintf(why, sizeof why, "the file ended early");
          kind = "drop";
        } else {
          kind = smb_failure(smb2, why, sizeof why);
        }
        net = 1;
        break;
      }
      err = write_all(fd, buf, (size_t)n);
      got += (uint64_t)n;
      pthread_mutex_lock(&b->mu);
      b->bytes += (uint64_t)n;
      pthread_mutex_unlock(&b->mu);
    }
    smb2_close(smb2, fh);
    if (err) {
      close(fd);
      return batch_fail(b, f->index, "write", strerror(err));
    }
    if (net) {
      if (batch_aborted(b)) goto stopped;
      if (strcmp(kind, "smb") != 0 && tries + 1 < BATCH_TRIES) continue;
      close(fd);
      return batch_fail(b, f->index, kind, why);
    }
    break;
  }
done:
  if (close(fd) != 0) return batch_fail(b, f->index, "write", strerror(errno));
  pthread_mutex_lock(&b->mu);
  b->files++;
  pthread_mutex_unlock(&b->mu);
  bsend(b, "D %lld", f->index);
  return 0;
stopped:
  close(fd);
  return -1;
}

/* Lets go of worker w's NFS mount (its socket is libnfs's to close). */
static void nfs_drop(batch_t *b, int w) {
  struct nfs_context *nfs;
  pthread_mutex_lock(&b->mu);
  b->nas[w] = -1;
  nfs = b->nfsc[w];
  b->nfsc[w] = NULL;
  pthread_mutex_unlock(&b->mu);
  if (nfs) nfs_destroy_context(nfs);
}

/* One file over NFS, as batch_one. */
static int batch_one_nfs(batch_t *b, int w, const bfile_t *f, char *buf) {
  char why[200];
  int fd, tries;
  fd = open(f->path, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE);
  if (fd < 0) return batch_fail(b, f->index, "file", strerror(errno));
  (void)fchmod(fd, FILE_MODE); /* open's mode is reduced by the umask */
  if (f->size == 0) goto done;
  for (tries = 0;; ++tries) {
    struct nfs_context *nfs = b->nfsc[w];
    struct nfsfh *fh = NULL;
    struct nfs_stat_64 st;
    const char *kind = "drop";
    uint64_t got = 0;
    size_t most;
    int err = 0, net = 0, rc;
    if (batch_aborted(b)) goto stopped;
    if (tries > 0) {
      nfs_drop(b, w);
      nfs = NULL;
      if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return batch_fail(b, f->index, "file", strerror(errno));
      }
      sleep_ms(500u * (unsigned)tries);
    }
    if (!nfs) {
      int port;
      nfs = nfs_connect(b->ip, b->port, &b->target, &kind, why, sizeof why);
      if (!nfs) {
        if (strcmp(kind, "nfs") != 0 && tries + 1 < BATCH_TRIES) continue;
        close(fd);
        return batch_fail(b, f->index, kind, why);
      }
      port = nfs_local_port(nfs);
      pthread_mutex_lock(&b->mu);
      b->nfsc[w] = nfs;
      b->nas[w] = nfs_get_fd(nfs);
      if (port) b->nfs_port = port;
      if (b->abort) shutdown(b->nas[w], SHUT_RDWR);
      pthread_mutex_unlock(&b->mu);
    }
    rc = nfs_open(nfs, f->request, O_RDONLY, &fh);
    if (rc == 0) rc = nfs_fstat64(nfs, fh, &st);
    if (rc < 0) {
      kind = nfs_failure(nfs, rc, 0, why, sizeof why);
      if (fh) nfs_close(nfs, fh);
      if (strcmp(kind, "nfs") == 0) {
        close(fd);
        return batch_fail(b, f->index, kind, why);
      }
      if (batch_aborted(b)) goto stopped;
      if (tries + 1 < BATCH_TRIES) continue;
      close(fd);
      return batch_fail(b, f->index, kind, why);
    }
    if (st.nfs_size != f->size) {
      nfs_close(nfs, fh);
      close(fd);
      return batch_fail(b, f->index, "changed", "the file on the NAS is a different size now");
    }
    most = nfs_get_readmax(nfs);
    if (most == 0 || most > BATCH_BUF) most = BATCH_BUF;
    while (!err && got < f->size) {
      uint64_t want = f->size - got;
      int n;
      if (want > most) want = most;
      n = nfs_pread(nfs, fh, buf, (size_t)want, got);
      if (n <= 0) {
        if (n == 0) {
          snprintf(why, sizeof why, "the file ended early");
          kind = "drop";
        } else {
          kind = nfs_failure(nfs, n, 0, why, sizeof why);
        }
        net = 1;
        break;
      }
      err = write_all(fd, buf, (size_t)n);
      got += (uint64_t)n;
      pthread_mutex_lock(&b->mu);
      b->bytes += (uint64_t)n;
      pthread_mutex_unlock(&b->mu);
    }
    nfs_close(nfs, fh);
    if (err) {
      close(fd);
      return batch_fail(b, f->index, "write", strerror(err));
    }
    if (net) {
      if (batch_aborted(b)) goto stopped;
      if (strcmp(kind, "nfs") != 0 && tries + 1 < BATCH_TRIES) continue;
      close(fd);
      return batch_fail(b, f->index, kind, why);
    }
    break;
  }
done:
  if (close(fd) != 0) return batch_fail(b, f->index, "write", strerror(errno));
  pthread_mutex_lock(&b->mu);
  b->files++;
  pthread_mutex_unlock(&b->mu);
  bsend(b, "D %lld", f->index);
  return 0;
stopped:
  close(fd);
  return -1;
}

static void free_bfile(bfile_t *f) {
  free(f->path);
  free(f->request);
  f->path = f->request = NULL;
}

static void *batch_worker(void *arg) {
  bworker_t *me = arg;
  batch_t *b = me->b;
  char *buf = malloc(BATCH_BUF), *head = malloc(MAX_HEADER + 1);
  if (!buf || !head) {
    batch_fail(b, -1, "request", "out of memory");
    goto out;
  }
  for (;;) {
    bfile_t f;
    int rc;
    pthread_mutex_lock(&b->mu);
    while (b->qn == 0 && !b->ended && !b->abort) pthread_cond_wait(&b->cv, &b->mu);
    if (b->abort || b->qn == 0) {
      pthread_mutex_unlock(&b->mu);
      break;
    }
    f = b->queue[b->qhead];
    b->qhead = (b->qhead + 1) % BATCH_QUEUE;
    b->qn--;
    b->busy++;
    pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->mu);
    rc = b->nfs   ? batch_one_nfs(b, me->w, &f, buf)
         : b->smb ? batch_one_smb(b, me->w, &f, buf)
                  : batch_one(b, me->w, &f, buf, head);
    free_bfile(&f);
    pthread_mutex_lock(&b->mu);
    b->busy--;
    pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->mu);
    if (rc != 0) break;
  }
out:
  free(buf);
  free(head);
  if (b->smb)
    smb_drop(b, me->w);
  else if (b->nfs)
    nfs_drop(b, me->w);
  else
    set_nas(b, me->w, -1);
  return NULL;
}

/* NASBridge's side of the connection, buffered (the list can be long). */
typedef struct breader {
  int fd;
  char buf[16384];
  size_t at, len;
} breader_t;

/* One byte; 0, or -1 when NASBridge closed (a quiet spell is waited out). */
static int bgetc(batch_t *b, breader_t *r, char *c) {
  while (r->at == r->len) {
    ssize_t n = recv(r->fd, r->buf, sizeof r->buf, 0);
    if (n > 0) {
      r->at = 0;
      r->len = (size_t)n;
      break;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (batch_aborted(b)) return -1;
      continue;
    }
    return -1;
  }
  *c = r->buf[r->at++];
  return 0;
}

static int bline(batch_t *b, breader_t *r, char *line, size_t size) {
  size_t have = 0;
  char c;
  while (have + 1 < size) {
    if (bgetc(b, r, &c) != 0) return -1;
    if (c == '\n') {
      line[have] = '\0';
      return 0;
    }
    line[have++] = c;
  }
  return -1;
}

static void *batch_reader(void *arg) {
  batch_t *b = arg;
  breader_t *r = malloc(sizeof *r);
  char line[MAX_FILE + 128];
  if (!r) {
    batch_fail(b, -1, "request", "out of memory");
    return NULL;
  }
  r->fd = b->ctl;
  r->at = r->len = 0;
  for (;;) {
    char *field[5], *save = NULL;
    bfile_t f;
    size_t i;
    int k;
    if (bline(b, r, line, sizeof line) != 0) {
      /* NASBridge went away, or closed its side once all was done. */
      pthread_mutex_lock(&b->mu);
      k = b->finished;
      pthread_mutex_unlock(&b->mu);
      if (!k) batch_stop(b);
      break;
    }
    if (strcmp(line, "STOP") == 0) {
      batch_stop(b);
      break;
    }
    if (strcmp(line, "E") == 0) {
      pthread_mutex_lock(&b->mu);
      b->ended = 1;
      pthread_cond_broadcast(&b->cv);
      pthread_mutex_unlock(&b->mu);
      continue;
    }
    field[0] = strtok_r(line, "\t", &save);
    for (k = 1; k < 5; ++k) field[k] = strtok_r(NULL, "\t", &save);
    if (!field[4] || strcmp(field[0], "F") != 0 || field[3][0] != '/') {
      batch_fail(b, -1, "request", "bad file record");
      break;
    }
    memset(&f, 0, sizeof f);
    f.index = strtoll(field[1], NULL, 10);
    f.size = strtoull(field[2], NULL, 10);
    f.reqlen = (size_t)strtoull(field[4], NULL, 10);
    f.path = strdup(field[3]);
    f.request = f.reqlen >= (b->smb || b->nfs ? 1u : 4u) && f.reqlen <= MAX_REQUEST ? malloc(f.reqlen + 1)
                                                                          : NULL;
    if (!f.path || !f.request) {
      free_bfile(&f);
      batch_fail(b, f.index, "request", "bad request");
      break;
    }
    for (i = 0; i < f.reqlen; ++i)
      if (bgetc(b, r, &f.request[i]) != 0) break;
    if (i < f.reqlen) {
      free_bfile(&f);
      batch_stop(b);
      break;
    }
    f.request[f.reqlen] = '\0';
    pthread_mutex_lock(&b->mu);
    while (b->qn == BATCH_QUEUE && !b->abort) pthread_cond_wait(&b->cv, &b->mu);
    if (b->abort) {
      pthread_mutex_unlock(&b->mu);
      free_bfile(&f);
      break;
    }
    b->queue[(b->qhead + b->qn) % BATCH_QUEUE] = f;
    b->qn++;
    pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->mu);
  }
  free(r);
  return NULL;
}

/* Copies the files NASBridge lists until it says that's all. Every path out
 * sends its final line. */
static void run_batch(int ctl, char *line) {
  char *field[5], *save = NULL, ip[64], *bytes = NULL, *sign[4];
  bworker_t me[BATCH_WORKERS];
  pthread_t workers[BATCH_WORKERS], reader;
  int k, started = 0, reader_started = 0;
  size_t sign_len = 0;
  uint64_t last_sync, started_ms;
  batch_t *b = calloc(1, sizeof *b);
  if (!b) {
    send_line(ctl, "ERR -1 request out of memory");
    return;
  }
  field[0] = strtok_r(line, "\t", &save);
  for (k = 1; k < 5; ++k) field[k] = strtok_r(NULL, "\t", &save);
  b->smb = strcmp(field[0], BATCH_PROTO_SMB) == 0;
  b->nfs = strcmp(field[0], BATCH_PROTO_NFS) == 0;
  if (!field[3] || ((b->smb || b->nfs) && !field[4])) {
    send_line(ctl, "ERR -1 request not a NASBridge batch request");
    free(b);
    return;
  }
  if (b->smb) {
    /* The sign-in comes before the list. */
    sign_len = (size_t)strtoull(field[4], NULL, 10);
    bytes = sign_len >= 4 && sign_len <= MAX_SIGN_IN ? malloc(sign_len) : NULL;
    if (!bytes || recv_exact(ctl, bytes, sign_len) != 0 || smb_fields(bytes, sign_len, sign, 4) != 0) {
      send_line(ctl, "ERR -1 request bad sign-in");
      free(bytes);
      free(b);
      return;
    }
    b->sign_in.share = sign[0];
    b->sign_in.path = NULL;
    b->sign_in.user = sign[1];
    b->sign_in.domain = sign[2];
    b->sign_in.password = sign[3];
  } else if (b->nfs) {
    /* The target comes before the list. */
    sign_len = (size_t)strtoull(field[4], NULL, 10);
    bytes = sign_len >= 4 && sign_len <= MAX_SIGN_IN ? malloc(sign_len) : NULL;
    if (!bytes || recv_exact(ctl, bytes, sign_len) != 0 || smb_fields(bytes, sign_len, sign, 4) != 0) {
      send_line(ctl, "ERR -1 request bad target");
      free(bytes);
      free(b);
      return;
    }
    b->target.exported = sign[0];
    b->target.path = NULL;
    nfs_numbers(&b->target, sign + 1);
  }
  snprintf(ip, sizeof ip, "%s", field[1]);
  b->ctl = ctl;
  b->ip = ip;
  b->port = atoi(field[2]);
  b->workers = atoi(field[3]);
  if (b->workers < 1) b->workers = 1;
  if (b->workers > BATCH_WORKERS) b->workers = BATCH_WORKERS;
  for (k = 0; k < BATCH_WORKERS; ++k) b->nas[k] = -1;
  pthread_mutex_init(&b->mu, NULL);
  pthread_cond_init(&b->cv, NULL);
  pthread_mutex_init(&b->out, NULL);

  if (start_thread(&reader, batch_reader, b) != 0) {
    send_line(ctl, "ERR -1 request couldn't start a thread");
    goto out;
  }
  reader_started = 1;
  for (k = 0; k < b->workers; ++k) {
    me[k].b = b;
    me[k].w = k;
    if (start_thread(&workers[k], batch_worker, &me[k]) != 0) break;
    ++started;
  }
  if (started == 0) {
    batch_fail(b, -1, "request", "couldn't start a thread");
    goto join;
  }
  last_sync = now_ms();
  started_ms = now_ms();
  for (;;) {
    uint64_t bytes, files, now;
    int busy, done, aborted, port;
    sleep_ms(250);
    pthread_mutex_lock(&b->mu);
    bytes = b->bytes;
    files = b->files;
    busy = b->busy;
    aborted = b->abort;
    done = b->ended && b->qn == 0 && b->busy == 0;
    port = b->nfs_port != b->nfs_port_sent ? b->nfs_port : 0;
    b->nfs_port_sent = b->nfs_port;
    pthread_mutex_unlock(&b->mu);
    if (port) bsend(b, "N %d", port);
    if (aborted) break;
    now = now_ms();
    if (done || now - last_sync >= BATCH_SYNC_MS) {
      /* Bounds what's waiting to reach the drive (the PS5 throttles hard
       * once a lot is). Without any lock: the workers carry on meanwhile (a
       * save of a few thousand small files takes over a second, and holding
       * them for it once left them idle most of the time). */
      sync();
      last_sync = now_ms();
      b->sync_ms += last_sync - now;
      bsend(b, "Y");
    }
    bsend(b, "P %llu %llu %d %llu %llu", (unsigned long long)bytes, (unsigned long long)files,
          busy, (unsigned long long)b->sync_ms, (unsigned long long)(now_ms() - started_ms));
    if (done) break;
  }

join:
  for (k = 0; k < started; ++k) pthread_join(workers[k], NULL);
  pthread_mutex_lock(&b->mu);
  b->finished = 1;
  k = b->abort;
  pthread_mutex_unlock(&b->mu);
  if (!b->failed) send_line(ctl, k ? "STOPPED" : "OK");
  shutdown(ctl, SHUT_RD); /* the reader's wait ends */
  batch_stop(b);
out:
  if (reader_started) pthread_join(reader, NULL);
  pthread_mutex_lock(&b->mu);
  while (b->qn > 0) {
    free_bfile(&b->queue[b->qhead]);
    b->qhead = (b->qhead + 1) % BATCH_QUEUE;
    b->qn--;
  }
  pthread_mutex_unlock(&b->mu);
  pthread_mutex_destroy(&b->mu);
  pthread_cond_destroy(&b->cv);
  pthread_mutex_destroy(&b->out);
  if (bytes) {
    memset(bytes, 0, sign_len); /* the password */
    free(bytes);
  }
  free(b);
}

/* Copies one file. Every path out sends its final line. */
static void run_job(int ctl) {
  char line[MAX_FILE + 128], *field[7], *save = NULL, *request = NULL;
  char ip[64], path[MAX_FILE];
  int k, streams = 1, err;
  size_t reqlen;
  job_t j;

  memset(&j, 0, sizeof j);
  j.ctl = ctl;
  j.fd = -1;
  for (k = 0; k < MAX_STREAMS; ++k) j.nas[k] = -1;
  pthread_mutex_init(&j.mu, NULL);
  pthread_cond_init(&j.cv, NULL);
  send_line(ctl, PROTO);
  if (recv_line(ctl, line, sizeof line) != 0) goto out;
#ifdef COPIER_HOST_TEST
  { /* Tests: $NB_TEST_PREFIX/no_batch makes this an older helper, without
     * batches (it refuses them as one would). */
    const char *prefix = getenv("NB_TEST_PREFIX");
    char flag[600];
    snprintf(flag, sizeof flag, "%s/no_batch", prefix ? prefix : "");
    if (prefix && access(flag, F_OK) == 0 && strncmp(line, BATCH_PROTO, 4) == 0) {
      send_line(ctl, "ERR request not a NASBridge copy request");
      goto out;
    }
  }
#endif
  if (strncmp(line, BATCH_PROTO "\t", sizeof BATCH_PROTO) == 0 ||
      strncmp(line, BATCH_PROTO_SMB "\t", sizeof BATCH_PROTO_SMB) == 0 ||
      strncmp(line, BATCH_PROTO_NFS "\t", sizeof BATCH_PROTO_NFS) == 0) {
    run_batch(ctl, line);
    goto out;
  }
  if (strncmp(line, PROTO_SMB "\t", sizeof PROTO_SMB) == 0) {
    run_smb_job(&j, line);
    goto out;
  }
  if (strncmp(line, PROTO_NFS "\t", sizeof PROTO_NFS) == 0) {
    run_nfs_job(&j, line);
    goto out;
  }
  field[0] = strtok_r(line, "\t", &save);
  for (k = 1; k < 7; ++k) field[k] = strtok_r(NULL, "\t", &save);
  if (!field[5] || strcmp(field[0], PROTO) != 0) {
    send_line(ctl, "ERR request not a NASBridge copy request");
    goto out;
  }
  snprintf(ip, sizeof ip, "%s", field[1]);
  j.ip = ip;
  j.port = atoi(field[2]);
  j.size = strtoull(field[3], NULL, 10);
  snprintf(path, sizeof path, "%s", field[4]);
  reqlen = (size_t)strtoull(field[5], NULL, 10);
  if (field[6]) streams = atoi(field[6]);
  if (streams < 1) streams = 1;
  if (streams > MAX_STREAMS) streams = MAX_STREAMS;
  if (reqlen < 4 || reqlen > MAX_REQUEST || path[0] != '/') {
    send_line(ctl, "ERR request bad request");
    goto out;
  }
  request = malloc(reqlen + 1);
  if (!request || recv_exact(ctl, request, reqlen) != 0) goto out;
  request[reqlen] = '\0';
  if (memcmp(request + reqlen - 4, "\r\n\r\n", 4) != 0) {
    send_line(ctl, "ERR request the HTTP request doesn't end its header");
    goto out;
  }
  j.request = request;
  j.reqlen = reqlen;

  j.fd = open(path, O_RDWR | O_CREAT, FILE_MODE);
  if (j.fd < 0) {
    send_line(ctl, "ERR file %s", strerror(errno));
    goto out;
  }
  (void)fchmod(j.fd, FILE_MODE); /* open's mode is reduced by the umask */
  if ((err = resume_point(&j)) != 0) {
    send_line(ctl, "ERR file %s", strerror(err));
    goto out;
  }
  send_line(ctl, "P %llu 0 0 0", (unsigned long long)j.start);
  if (j.start == j.size) {
    send_line(ctl, "OK");
    goto out;
  }
  j.last_sync = now_ms();
#ifdef COPIER_HOST_TEST
  { /* Tests: $NB_TEST_PREFIX/crash_multi makes a two-connection copy crash,
     * as an overflowing thread stack once did on the PS5. */
    const char *prefix = getenv("NB_TEST_PREFIX");
    char flag[600];
    if (prefix && streams > 1) {
      snprintf(flag, sizeof flag, "%s/crash_multi", prefix);
      if (access(flag, F_OK) == 0) _exit(3);
    }
  }
#endif
  /* Two connections pay off on a big rest; a small one goes in one. */
  if (streams > 1 && j.size - j.start >= 2 * CHUNK) {
    if (multi_pass(&j, streams, NULL, NULL) == 0) goto out;
    /* The NAS doesn't take ranges: over one connection, from where it got. */
    j.abort = 0;
    if ((err = resume_point(&j)) != 0) {
      send_line(ctl, "ERR file %s", strerror(err));
      goto out;
    }
  }
  single_pass(&j);

out:
  free(request);
  if (j.fd >= 0) close(j.fd);
  pthread_mutex_destroy(&j.mu);
  pthread_cond_destroy(&j.cv);
}

/* ------------------------------------------------------------------ main */

static int bind_listen(int port) {
  struct sockaddr_in addr;
  int one = 1, srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) return -1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* only NASBridge, on this PS5 */
  addr.sin_port = htons((uint16_t)port);
  if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(srv, 4) != 0) {
    close(srv);
    return -1;
  }
  return srv;
}

int main(void) {
  int port = COPIER_PORT_DEFAULT, srv, idle_s = IDLE_EXIT_S;
#ifdef COPIER_HOST_TEST
  const char *pp = getenv("NB_TEST_PORT");
  const char *idle = getenv("NB_TEST_IDLE");
  if (pp) port = atoi(pp);
  if (idle) idle_s = atoi(idle);
#endif
  /* NASBridge going away mid-copy must not kill us with SIGPIPE. */
  signal(SIGPIPE, SIG_IGN);

  /* Open the port first: if another copy helper already has it, that one
   * does the copying and this one leaves quietly. */
  srv = bind_listen(port);
  if (srv < 0) return 0;
#ifndef COPIER_HOST_TEST
  if (elevate() != 0)
    notify("NASBridge copy helper: couldn't get full access (is the jailbreak fully loaded?)");
#endif

  for (;;) {
    struct timeval tv = {idle_s, 0};
    fd_set set;
    int cli;
    FD_ZERO(&set);
    FD_SET(srv, &set);
    if (select(srv + 1, &set, NULL, NULL, &tv) <= 0) break; /* idle: done */
    cli = accept(srv, NULL, NULL);
    if (cli < 0) continue;
    tv.tv_sec = REQUEST_S;
    tv.tv_usec = 0;
    setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    run_job(cli);
    close(cli);
  }
  close(srv);
  return 0;
}
