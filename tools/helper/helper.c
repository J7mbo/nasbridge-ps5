/*
 * NASBridge delete helper payload.
 *
 * NASBridge itself runs in the game sandbox, and ShadowMountPlus refuses to
 * delete or uninstall anything while NASBridge is open (it counts NASBridge as
 * a running game). So when you delete a game, NASBridge sends this payload to
 * the PS5's own ELF loader (127.0.0.1:9021). Running with the loader's
 * privileges, it:
 *   1. listens on 127.0.0.1:HELPER_PORT for ONE connection (gives up after 15 s),
 *   2. reads one request: which game, and the folder or image to delete,
 *   3. checks it: the title isn't running, and the path is a game's folder or
 *      image on internal or extended storage (never anything else),
 *   4. deletes the files, reporting progress,
 *   5. removes the game from the home screen (the system's own uninstall,
 *      as ShadowMountPlus's uninstall does),
 *   6. replies, closes and exits. Nothing keeps running afterwards.
 *
 * Request (NASBridge -> helper), text lines:
 *   "NBD1\n" <title ID> "\n" <path, or "-" for none (an installed PKG)> "\n"
 * or, for a folder ShadowMountPlus has already removed from the home screen:
 *   "NBD2\n" <title ID> "\n" <path> "\n" "files_only threads=<n>\n"
 * which first renames the folder into <its parent>/.nasbridge-deleting/
 * (instant: the same drive), where ShadowMountPlus never looks (it skips
 * names starting with a dot), then deletes that folder (this game and
 * anything an earlier delete left there) n files at once: on the
 * PS5's internal storage each delete waits milliseconds on the drive, and
 * those waits overlap (a 49,000-file game: minutes one at a time). No
 * uninstall. Or, to finish what a delete cut short left there:
 *   "NBD3\n" "-\n" <a folder games go to, e.g. /data/homebrew> "\n" "threads=<n>\n"
 * which deletes everything in <that folder>/.nasbridge-deleting.
 * Replies (helper -> NASBridge), text lines:
 *   "T <total>"           what will be deleted: bytes (NBD1), or files
 *                         (NBD2, NBD3: the time goes on each file, not
 *                         its size, so a bar of files moves evenly)
 *   "P <done>"            progress in the same unit, a few times a second
 *   "OK"                  files deleted and the game uninstalled
 *   "ERR <stage> <why>"   stage: request, files or uninstall
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef HELPER_HOST_TEST
#include <dlfcn.h>
#include <ps5/kernel.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

#define HELPER_PORT_DEFAULT 9146
#define ACCEPT_TIMEOUT_S 40 /* the ELF loader can take a while to start us */
#define RECV_TIMEOUT_S 10
#define MAX_REQUEST 1024
#define MAX_PATH 1024
#define MAX_THREADS 32
/* Every thread gets this stack (the PS5's default is tight; big buffers are
 * on the heap), as in the copy helper. */
#define THREAD_STACK (256u * 1024u)
/* The identity and capabilities ShadowMountPlus and the SDK's own sample
 * raise themselves to before privileged system calls. */
#define AUTHID_SHELL 0x4800000000000006ull

/* Host test build (HELPER_HOST_TEST): paths live under $NB_TEST_PREFIX, the
 * uninstall is written to $NB_TEST_PREFIX/uninstalled.txt instead. */
static char g_prefix[256] = "";
static char g_err[512];
static int g_client = -1;

/* On-screen notifications, so a delete can be followed without a debugger.
 * The host build just prints them. */
#ifndef HELPER_HOST_TEST
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
#else
static void notify(const char *text) { fprintf(stderr, "[notify] %s\n", text); }
#endif

static int fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof g_err, fmt, ap);
  va_end(ap);
  return -1;
}

static void send_line(const char *fmt, ...) {
  char line[700];
  va_list ap;
  size_t len;
  const char *p = line;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  len = strlen(line);
  line[len++] = '\n';
  while (len > 0) {
    ssize_t n = send(g_client, p, len, 0);
    if (n <= 0) return;
    p += n;
    len -= (size_t)n;
  }
}

static uint64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* ------------------------------------------------------- running titles */

#ifndef HELPER_HOST_TEST
/* The system's per-process app record; the title ID is searched for anywhere
 * in it (its offset differs between firmwares). See the installer. */
typedef struct app_info {
  uint32_t app_id;
  unsigned char rest[0x7c];
} app_info_t;

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

/* The uninstall lives in libSceAppInstUtil, and its init in libSceUserService.
 * These are loaded at runtime with dlopen (as ShadowMountPlus does) rather than
 * linked: a payload sent to the ELF loader must not depend on system modules at
 * load time, or the loader refuses to run it at all. */
static const char *const kUserServiceSprx =
    "/system/common/lib/libSceUserService.sprx";
static const char *const kAppInstUtilSprx =
    "/system/common/lib/libSceAppInstUtil.sprx";

static int record_has(const app_info_t *info, const char *name) {
  const unsigned char *bytes = (const unsigned char *)info;
  size_t n = strlen(name), i;
  for (i = 0; i + n < sizeof *info; ++i)
    if (memcmp(bytes + i, name, n) == 0 && bytes[i + n] == '\0' &&
        (i == 0 || bytes[i - 1] == '\0'))
      return 1;
  return 0;
}

/* 1 when a process belongs to the title, 0 when none does, -1 when the
 * process list can't be read. */
static int title_running(const char *name) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  char *buf, *ptr;
  int found = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0) return -1;
  size += 16 * sizeof(struct kinfo_proc);
  if (!(buf = malloc(size))) return -1;
  if (sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
    free(buf);
    return -1;
  }
  for (ptr = buf; ptr < buf + size && !found;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
    app_info_t info;
    if (ki->ki_structsize <= 0) break;
    ptr += ki->ki_structsize;
    memset(&info, 0, sizeof info);
    if (sceKernelGetAppInfo(ki->ki_pid, &info) == 0 && record_has(&info, name)) found = 1;
  }
  free(buf);
  return found;
}

static int uninstall(const char *title_id) {
  /* The identity the system's uninstall needs, as ShadowMountPlus raises
   * itself before calling it. Done here, not at start-up, so that if anything
   * faults the files are already deleted and the port is already open to
   * report it. */
  typedef int (*user_init_t)(const void *);
  typedef int (*appinst_init_t)(void);
  typedef int (*uninstall_t)(const char *);
  void *user_h, *inst_h;
  appinst_init_t inst_init;
  uninstall_t app_uninstall;
  int rc;

  notify("NASBridge delete helper: escalating");
  kernel_set_ucred_authid(getpid(), AUTHID_SHELL);
  {
    /* Full capabilities, as the SDK's privilege sample does; the uninstall
     * needs more than the authid alone. */
    uint8_t caps[16];
    memset(caps, 0xff, sizeof caps);
    kernel_set_ucred_caps(getpid(), caps);
  }

  /* User service first (the app-install module expects it), then the uninstall
   * module, both loaded from disk at runtime. */
  notify("NASBridge delete helper: loading modules");
  user_h = dlopen(kUserServiceSprx, RTLD_LAZY);
  if (user_h) {
    user_init_t user_init = (user_init_t)dlsym(user_h, "sceUserServiceInitialize");
    if (user_init) user_init(NULL);
  }
  inst_h = dlopen(kAppInstUtilSprx, RTLD_LAZY);
  if (!inst_h) return fail("couldn't load the uninstall module (%s)", dlerror());
  inst_init = (appinst_init_t)dlsym(inst_h, "sceAppInstUtilInitialize");
  app_uninstall = (uninstall_t)dlsym(inst_h, "sceAppInstUtilAppUnInstall");
  if (!app_uninstall) return fail("couldn't find the uninstall function");
  if (inst_init) inst_init();
  notify("NASBridge delete helper: calling uninstall");
  rc = app_uninstall(title_id);
  if (rc != 0) return fail("the PS5 refused (code 0x%08X)", (unsigned)rc);
  return 0;
}
#else
static int title_running(const char *name) {
  const char *running = getenv("NB_TEST_RUNNING");
  return running && strcmp(running, name) == 0;
}

static int uninstall(const char *title_id) {
  char path[600];
  FILE *f;
  if (getenv("NB_TEST_UNINSTALL_FAILS")) return fail("the PS5 refused (code 0x80990001)");
  snprintf(path, sizeof path, "%s/uninstalled.txt", g_prefix);
  if (!(f = fopen(path, "a"))) return fail("cannot record the uninstall");
  fprintf(f, "%s\n", title_id);
  fclose(f);
  return 0;
}
#endif

/* ------------------------------------------------------------ validation */

static int valid_title_id(const char *id) {
  size_t i;
  if (strlen(id) != 9) return 0;
  for (i = 0; i < 4; ++i)
    if (id[i] < 'A' || id[i] > 'Z') return 0;
  for (i = 4; i < 9; ++i)
    if (id[i] < '0' || id[i] > '9') return 0;
  return 1;
}

/* The length of the storage root `path` starts with ("/data", "/mnt/ext0",
 * "/mnt/usb1"), or 0 when it starts with none. Mount points such as
 * /mnt/shadowmnt (a running game's view of its files) are never roots. */
static size_t storage_root(const char *path) {
  if (strncmp(path, "/data/", 6) == 0) return 5;
  if ((strncmp(path, "/mnt/ext", 8) == 0 || strncmp(path, "/mnt/usb", 8) == 0) &&
      path[8] >= '0' && path[8] <= '9' && path[9] == '/')
    return 9;
  return 0;
}

static int has_suffix(const char *s, const char *suffix) {
  size_t n = strlen(s), m = strlen(suffix);
  return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* A game's folder (with its sce_sys/param.json naming this title) or a game
 * image, at least two folders below a storage root, with no "." or ".."
 * parts: nothing else is ever deleted. */
static int check_path(const char *path, const char *title_id) {
  const char *p;
  int parts = 0;
  size_t root = storage_root(path);
  char full[MAX_PATH + 300], param[16384];
  struct stat st;
  if (root == 0) return fail("%s isn't on internal or extended storage", path);
  for (p = path + root; *p == '/';) {
    const char *end = strchr(p + 1, '/');
    size_t len = end ? (size_t)(end - p - 1) : strlen(p + 1);
    if (len == 0 || (len == 1 && p[1] == '.') || (len == 2 && p[1] == '.' && p[2] == '.'))
      return fail("%s isn't a plain path", path);
    ++parts;
    if (!end) break;
    p = end;
  }
  if (parts < 2) return fail("%s is a storage folder, not a game", path);
  snprintf(full, sizeof full, "%s%s", g_prefix, path);
  if (lstat(full, &st) != 0) return fail("%s isn't there (%s)", path, strerror(errno));
  if (S_ISREG(st.st_mode)) {
    if (has_suffix(path, ".exfat") || has_suffix(path, ".ffpfs") ||
        has_suffix(path, ".ffpfsc") || has_suffix(path, ".ffpkg"))
      return 0;
    return fail("%s isn't a game image", path);
  }
  if (!S_ISDIR(st.st_mode)) return fail("%s isn't a game folder or image", path);
  {
    int fd;
    ssize_t n;
    snprintf(full, sizeof full, "%s%s/sce_sys/param.json", g_prefix, path);
    fd = open(full, O_RDONLY);
    if (fd < 0) return fail("%s has no sce_sys/param.json", path);
    n = read(fd, param, sizeof param - 1);
    close(fd);
    if (n <= 0) return fail("%s has an unreadable param.json", path);
    param[n] = '\0';
    if (!strstr(param, title_id)) return fail("%s isn't %s", path, title_id);
  }
  return 0;
}

/* ------------------------------------------------------------- deleting */

static uint64_t g_total, g_done, g_reported_at;

static void measure(const char *path) {
  struct stat st;
  DIR *dir;
  struct dirent *e;
  char child[MAX_PATH + 300];
  if (lstat(path, &st) != 0) return;
  if (!S_ISDIR(st.st_mode)) {
    g_total += (uint64_t)st.st_size;
    return;
  }
  if (!(dir = opendir(path))) return;
  while ((e = readdir(dir)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    measure(child);
  }
  closedir(dir);
}

static void progress(void) {
  uint64_t now = now_ms();
  if (now - g_reported_at >= 250) {
    g_reported_at = now;
    send_line("P %llu", (unsigned long long)g_done);
  }
}

/* Depth first; symbolic links are removed, never followed. */
static int remove_tree(const char *path) {
  struct stat st;
  DIR *dir;
  struct dirent *e;
  char child[MAX_PATH + 300];
  if (lstat(path, &st) != 0) {
    if (errno == ENOENT) return 0;
    return fail("cannot read %s (%s)", path + strlen(g_prefix), strerror(errno));
  }
  if (!S_ISDIR(st.st_mode)) {
    if (unlink(path) != 0 && errno != ENOENT)
      return fail("cannot delete %s (%s)", path + strlen(g_prefix), strerror(errno));
    g_done += (uint64_t)st.st_size;
    progress();
    return 0;
  }
  if (!(dir = opendir(path)))
    return fail("cannot open %s (%s)", path + strlen(g_prefix), strerror(errno));
  while ((e = readdir(dir)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    if (remove_tree(child) != 0) {
      closedir(dir);
      return -1;
    }
  }
  closedir(dir);
  if (rmdir(path) != 0 && errno != ENOENT)
    return fail("cannot delete %s (%s)", path + strlen(g_prefix), strerror(errno));
  return 0;
}

/* ---------------------------------------------------- several at once */

/* The tree's files and folders, listed first (folders in the order they
 * were found: parents before children). */
typedef struct tree {
  char **files;
  size_t nfiles, cap_files;
  char **dirs;
  size_t ndirs, cap_dirs;
} tree_t;

static int push(char ***list, size_t *n, size_t *cap, const char *path) {
  if (*n == *cap) {
    size_t grow = *cap ? *cap * 2 : 1024;
    char **more = realloc(*list, grow * sizeof **list);
    if (!more) return -1;
    *list = more;
    *cap = grow;
  }
  if (!((*list)[*n] = strdup(path))) return -1;
  ++*n;
  return 0;
}

static int list_tree(tree_t *t, const char *path) {
  struct stat st;
  DIR *dir;
  struct dirent *e;
  char child[MAX_PATH + 300];
  if (lstat(path, &st) != 0) {
    if (errno == ENOENT) return 0;
    return fail("cannot read %s (%s)", path + strlen(g_prefix), strerror(errno));
  }
  if (!S_ISDIR(st.st_mode)) {
    if (push(&t->files, &t->nfiles, &t->cap_files, path) != 0) return fail("out of memory");
    return 0;
  }
  if (push(&t->dirs, &t->ndirs, &t->cap_dirs, path) != 0) return fail("out of memory");
  if (!(dir = opendir(path)))
    return fail("cannot open %s (%s)", path + strlen(g_prefix), strerror(errno));
  while ((e = readdir(dir)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    if (list_tree(t, child) != 0) {
      closedir(dir);
      return -1;
    }
  }
  closedir(dir);
  return 0;
}

/* Deals the files out one folder at a time, round the folders like cards:
 * the PS5's file system locks a folder while a file in it is deleted, so
 * threads deleting side by side in one folder mostly wait on each other
 * (49,439 files, 8 at once in listing order: 202 files/s, barely more than
 * one at a time). Spread over different folders, the deletes overlap. Files
 * were listed folder by folder, so each folder's files are runs side by
 * side. Leaves t->files untouched if there's no memory for the new order. */
static size_t parent_length(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? (size_t)(slash - path) : 0;
}

static void interleave_folders(tree_t *t) {
  size_t *start, *left, nruns = 0, active, i, out = 0;
  char **dealt;
  if (t->nfiles < 2) return;
  start = malloc(t->nfiles * sizeof *start);
  left = malloc(t->nfiles * sizeof *left);
  dealt = malloc(t->nfiles * sizeof *dealt);
  if (!start || !left || !dealt) {
    free(start);
    free(left);
    free(dealt);
    return;
  }
  for (i = 0; i < t->nfiles; ++i) {
    size_t n = parent_length(t->files[i]);
    if (nruns > 0) {
      const char *prev = t->files[start[nruns - 1]];
      if (parent_length(prev) == n && strncmp(prev, t->files[i], n) == 0) {
        ++left[nruns - 1];
        continue;
      }
    }
    start[nruns] = i;
    left[nruns] = 1;
    ++nruns;
  }
  /* One file from each folder still holding some, round and round; an
   * emptied folder drops out of the round. */
  active = nruns;
  while (active > 0) {
    size_t kept = 0;
    for (i = 0; i < active; ++i) {
      dealt[out++] = t->files[start[i]++];
      if (--left[i] > 0) {
        start[kept] = start[i];
        left[kept] = left[i];
        ++kept;
      }
    }
    active = kept;
  }
  free(t->files);
  t->files = dealt;
  t->cap_files = t->nfiles;
  free(start);
  free(left);
}

typedef struct deleting {
  tree_t *t;
  pthread_mutex_t mu;
  size_t next;
  int failed;
} deleting_t;

static void *delete_worker(void *arg) {
  deleting_t *d = arg;
  for (;;) {
    size_t i;
    pthread_mutex_lock(&d->mu);
    if (d->failed || d->next >= d->t->nfiles) {
      pthread_mutex_unlock(&d->mu);
      break;
    }
    i = d->next++;
    pthread_mutex_unlock(&d->mu);
#ifdef HELPER_HOST_TEST
    { /* Tests: $NB_TEST_DELETE_FAILS names a file that can't be deleted. */
      const char *bad = getenv("NB_TEST_DELETE_FAILS");
      if (bad && *bad && strstr(d->t->files[i], bad)) {
        errno = EACCES;
        goto failed;
      }
    }
#endif
    if (unlink(d->t->files[i]) != 0 && errno != ENOENT) {
#ifdef HELPER_HOST_TEST
    failed:
#endif
      pthread_mutex_lock(&d->mu);
      if (!d->failed) {
        d->failed = 1;
        fail("cannot delete %s (%s)", d->t->files[i] + strlen(g_prefix), strerror(errno));
      }
      pthread_mutex_unlock(&d->mu);
      break;
    }
    pthread_mutex_lock(&d->mu);
    g_done += 1;
    pthread_mutex_unlock(&d->mu);
  }
  return NULL;
}

/* How the last parallel delete went, for the "done" notification: where
 * the time went tells a slow listing from slow deletes. */
static uint64_t g_list_ms, g_files_ms, g_dirs_ms;
static size_t g_nfiles, g_ndirs;
static int g_started;

static void notify_done(void) {
  char text[300];
  if (g_nfiles == 0 && g_ndirs == 0) {
    notify("NASBridge delete helper: done");
    return;
  }
  snprintf(text, sizeof text,
           "NASBridge delete helper: done. %zu files, %d at once: listing %llu s, "
           "files %llu s (%llu/s), %zu folders %llu s",
           g_nfiles, g_started, (unsigned long long)(g_list_ms / 1000),
           (unsigned long long)(g_files_ms / 1000),
           (unsigned long long)(g_files_ms ? g_nfiles * 1000ull / g_files_ms : 0), g_ndirs,
           (unsigned long long)(g_dirs_ms / 1000));
  notify(text);
}

static void free_tree(tree_t *t) {
  size_t i;
  for (i = 0; i < t->nfiles; ++i) free(t->files[i]);
  for (i = 0; i < t->ndirs; ++i) free(t->dirs[i]);
  free(t->files);
  free(t->dirs);
}

/* The folder `path` (prefix included) and everything in it, `threads` files
 * at a time, reporting progress from this thread. Symbolic links are
 * removed, never followed. */
static int remove_tree_parallel(const char *path, int threads) {
  tree_t t;
  deleting_t d;
  pthread_t tid[MAX_THREADS];
  int started = 0, i, rc = 0;
  memset(&t, 0, sizeof t);
  memset(&d, 0, sizeof d);
  g_list_ms = now_ms();
  if (list_tree(&t, path) != 0) {
    free_tree(&t);
    return -1;
  }
  /* Counted in files: deleting a small file takes as long as a big one,
   * so a bar of bytes would crawl through a game's thousands of small
   * files and race through its few big ones. */
  interleave_folders(&t);
  g_list_ms = now_ms() - g_list_ms;
  g_files_ms = now_ms();
  g_nfiles = t.nfiles;
  g_ndirs = t.ndirs;
  g_total = (uint64_t)t.nfiles;
  g_done = 0;
  send_line("T %llu", (unsigned long long)g_total);
  d.t = &t;
  pthread_mutex_init(&d.mu, NULL);
  if (threads < 1) threads = 1;
  if (threads > MAX_THREADS) threads = MAX_THREADS;
  for (i = 0; i < threads; ++i) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK);
    if (pthread_create(&tid[started], &attr, delete_worker, &d) == 0) ++started;
    pthread_attr_destroy(&attr);
  }
  if (started == 0) {
    delete_worker(&d); /* no threads to be had: one at a time, here */
  } else {
    for (;;) { /* progress until the workers have taken every file */
      int busy;
      struct timespec pause = {0, 250 * 1000000L};
      pthread_mutex_lock(&d.mu);
      busy = !d.failed && d.next < t.nfiles;
      send_line("P %llu", (unsigned long long)g_done);
      pthread_mutex_unlock(&d.mu);
      if (!busy) break;
      nanosleep(&pause, NULL);
    }
    for (i = 0; i < started; ++i) pthread_join(tid[i], NULL);
  }
  g_started = started > 0 ? started : 1;
  g_files_ms = now_ms() - g_files_ms;
  g_dirs_ms = now_ms();
  if (d.failed) {
    rc = -1;
  } else {
    /* Folders last, deepest first (they were listed parents first). */
    size_t k;
    for (k = t.ndirs; k-- > 0;)
      if (rmdir(t.dirs[k]) != 0 && errno != ENOENT) {
        rc = fail("cannot delete %s (%s)", t.dirs[k] + strlen(g_prefix), strerror(errno));
        break;
      }
  }
  g_dirs_ms = now_ms() - g_dirs_ms;
  pthread_mutex_destroy(&d.mu);
  free_tree(&t);
  return rc;
}

#define DELETING_DIR ".nasbridge-deleting"

/* Moves the folder `full` (prefix included) to <its parent>/.nasbridge-
 * deleting/<its name> (with " 2", " 3"... when that's taken), out of
 * ShadowMountPlus's sight. The same drive, so it's one step whatever the
 * size. The new path goes in `hidden`. */
static int hide_folder(const char *full, char *hidden, size_t size) {
  char parent[MAX_PATH + 300], dir[MAX_PATH + 340];
  const char *slash = strrchr(full, '/');
  const char *name;
  int n;
  if (!slash || slash == full) return fail("cannot find the folder's parent");
  snprintf(parent, sizeof parent, "%.*s", (int)(slash - full), full);
  name = slash + 1;
  snprintf(dir, sizeof dir, "%s/%s", parent, DELETING_DIR);
  if (mkdir(dir, 0777) != 0 && errno != EEXIST)
    return fail("cannot make %s (%s)", dir + strlen(g_prefix), strerror(errno));
  for (n = 1; n < 1000; ++n) {
    struct stat st;
    if (n == 1)
      snprintf(hidden, size, "%s/%s", dir, name);
    else
      snprintf(hidden, size, "%s/%s %d", dir, name, n);
    if (lstat(hidden, &st) != 0 && errno == ENOENT) break;
  }
  if (rename(full, hidden) != 0)
    return fail("cannot move %s aside (%s)", full + strlen(g_prefix), strerror(errno));
  return 0;
}

/* A folder games go to ("/data/homebrew", "/mnt/ext0/games"): on internal or
 * extended storage, a plain path. Only its .nasbridge-deleting is touched. */
static int check_root(const char *path) {
  const char *p;
  size_t root = storage_root(path);
  if (root == 0) return fail("%s isn't on internal or extended storage", path);
  for (p = path + root; *p == '/';) {
    const char *end = strchr(p + 1, '/');
    size_t len = end ? (size_t)(end - p - 1) : strlen(p + 1);
    if (len == 0 || (len == 1 && p[1] == '.') || (len == 2 && p[1] == '.' && p[2] == '.'))
      return fail("%s isn't a plain path", path);
    if (!end) break;
    p = end;
  }
  return 0;
}

/* --------------------------------------------------------------- network */

/* Open the port (no privilege needed), so NASBridge can connect right away. */
static int bind_listen(int port) {
  struct sockaddr_in addr;
  int one = 1, srv;
  srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) return -1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* only NASBridge, on this PS5 */
  addr.sin_port = htons((uint16_t)port);
  if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(srv, 1) != 0) {
    close(srv);
    return -1;
  }
  return srv;
}

/* The one connection NASBridge makes, with a read timeout. */
static int accept_one(int srv) {
  struct timeval tv;
  fd_set set;
  int cli;
  FD_ZERO(&set);
  FD_SET(srv, &set);
  tv.tv_sec = ACCEPT_TIMEOUT_S;
  tv.tv_usec = 0;
  if (select(srv + 1, &set, NULL, NULL, &tv) <= 0) return -1;
  cli = accept(srv, NULL, NULL);
  if (cli < 0) return -1;
  tv.tv_sec = RECV_TIMEOUT_S;
  tv.tv_usec = 0;
  setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return cli;
}

/* The request lines, into title_id and path (and, for NBD2, its options). */
static int read_request(char *title_id, size_t id_size, char *path, size_t path_size,
                        int *files_only, int *threads) {
  char buf[MAX_REQUEST + 1];
  size_t have = 0;
  char *line[4];
  int count = 0, want = 3;
  char *p;
  *files_only = 0;
  *threads = 1;
  while (have < MAX_REQUEST) {
    ssize_t n = recv(g_client, buf + have, MAX_REQUEST - have, 0);
    if (n <= 0) break;
    have += (size_t)n;
    buf[have] = '\0';
    want = strncmp(buf, "NBD2\n", 5) == 0 || strncmp(buf, "NBD3\n", 5) == 0 ? 4 : 3;
    count = 0;
    for (p = buf; *p; ++p)
      if (*p == '\n') ++count;
    if (count >= want) break;
  }
  buf[have] = '\0';
  if (count < want) return fail("incomplete request");
  line[0] = strtok(buf, "\n");
  line[1] = strtok(NULL, "\n");
  line[2] = strtok(NULL, "\n");
  line[3] = want == 4 ? strtok(NULL, "\n") : NULL;
  if (!line[0] || !line[1] || !line[2] ||
      (strcmp(line[0], "NBD1") != 0 && strcmp(line[0], "NBD2") != 0 &&
       strcmp(line[0], "NBD3") != 0))
    return fail("not a NASBridge request");
  if (want == 4) {
    const char *t;
    if (!line[3]) return fail("incomplete request");
    *files_only = strcmp(line[0], "NBD3") == 0 ? 2 : strstr(line[3], "files_only") != NULL;
    if ((t = strstr(line[3], "threads=")) != NULL) *threads = atoi(t + 8);
    if (*threads < 1) *threads = 1;
    if (*threads > MAX_THREADS) *threads = MAX_THREADS;
    if (!*files_only || strcmp(line[2], "-") == 0) return fail("NBD2 deletes a folder's files only");
    if (*files_only == 2) { /* NBD3: no title, a folder games go to */
      if (strlen(line[2]) >= path_size) return fail("too long");
      snprintf(title_id, id_size, "-");
      snprintf(path, path_size, "%s", line[2]);
      return 0;
    }
  }
  if (!valid_title_id(line[1])) return fail("not a title ID");
  if (strlen(line[2]) >= path_size || strlen(line[1]) >= id_size) return fail("too long");
  snprintf(title_id, id_size, "%s", line[1]);
  snprintf(path, path_size, "%s", line[2]);
  return 0;
}

int main(void) {
  int port = HELPER_PORT_DEFAULT;
  char title_id[16], path[MAX_PATH], full[MAX_PATH + 300];
  int running, files_only = 0, threads = 1;

#ifdef HELPER_HOST_TEST
  const char *prefix = getenv("NB_TEST_PREFIX");
  const char *pp = getenv("NB_TEST_PORT");
  if (!prefix || !*prefix) {
    fprintf(stderr, "NB_TEST_PREFIX not set\n");
    return 2;
  }
  snprintf(g_prefix, sizeof g_prefix, "%s", prefix);
  if (pp) port = atoi(pp);
#endif

  /* NASBridge closing mid-delete must not kill us halfway through the files:
   * the delete finishes, it just can't be reported. */
  signal(SIGPIPE, SIG_IGN);
  notify("NASBridge delete helper: starting");

  /* Open the port before any privileged call, so NASBridge can always connect
   * and be told what happened, even if a later step faults. */
  {
    const int srv = bind_listen(port);
    if (srv < 0) {
      notify("NASBridge delete helper: couldn't open its port");
      return 1;
    }
    notify("NASBridge delete helper: ready");
    g_client = accept_one(srv);
    close(srv); /* one connection only */
  }
  if (g_client < 0) {
    notify("NASBridge delete helper: NASBridge didn't connect");
    return 1;
  }

  if (read_request(title_id, sizeof title_id, path, sizeof path, &files_only, &threads) != 0) {
    send_line("ERR request %s", g_err);
    close(g_client);
    return 1;
  }
  {
    char line[128];
    snprintf(line, sizeof line, "NASBridge delete helper: deleting %s", title_id);
    notify(line);
  }

#ifndef HELPER_HOST_TEST
  /* Payloads start inside the app sandbox; step out to the real filesystem
   * root so /mnt and /data are reachable, as the installer does. */
  kernel_set_proc_rootdir(getpid(), kernel_get_root_vnode());
#endif

  if (files_only == 2) {
    /* NBD3: finish what earlier deletes left in .nasbridge-deleting. */
    char dir[MAX_PATH + 340];
    struct stat st;
    if (check_root(path) != 0) {
      send_line("ERR request %s", g_err);
      close(g_client);
      return 1;
    }
    snprintf(dir, sizeof dir, "%s%s/%s", g_prefix, path, DELETING_DIR);
    if (lstat(dir, &st) == 0 && remove_tree_parallel(dir, threads) != 0) {
      send_line("ERR files %s", g_err);
      close(g_client);
      return 1;
    }
    send_line("P %llu", (unsigned long long)g_done);
    send_line("OK");
    notify_done();
    close(g_client);
    return 0;
  }

  running = title_running(title_id);
  if (running != 0) {
    send_line("ERR request %s", running > 0 ? "the game is open: close it first"
                                            : "can't tell whether the game is open");
    close(g_client);
    return 1;
  }

  /* 1. Its files (an installed PKG has none of its own: "-"). */
  if (strcmp(path, "-") != 0) {
    if (check_path(path, title_id) != 0) {
      send_line("ERR request %s", g_err);
      close(g_client);
      return 1;
    }
    snprintf(full, sizeof full, "%s%s", g_prefix, path);
    if (files_only) {
      /* ShadowMountPlus has removed it from the home screen already. Out of
       * its sight first, then deleted where it is now. */
      char hidden[MAX_PATH + 400];
      if (hide_folder(full, hidden, sizeof hidden) != 0) {
        send_line("ERR request %s", g_err);
        close(g_client);
        return 1;
      }
      {
        /* The whole hidden folder: this game, and anything an earlier
         * delete left there. Once it's gone, nothing is left to finish. */
        char *slash = strrchr(hidden, '/');
        if (slash) *slash = '\0';
      }
      if (remove_tree_parallel(hidden, threads) != 0) {
        send_line("ERR files %s", g_err);
        close(g_client);
        return 1;
      }
      send_line("P %llu", (unsigned long long)g_done);
      send_line("OK");
      notify_done();
      close(g_client);
      return 0;
    }
    measure(full);
    send_line("T %llu", (unsigned long long)g_total);
    if (remove_tree(full) != 0) {
      send_line("ERR files %s", g_err);
      close(g_client);
      return 1;
    }
    send_line("P %llu", (unsigned long long)g_done);
  }

  /* 2. Its home-screen entry. */
  if (uninstall(title_id) != 0) {
    send_line("ERR uninstall %s", g_err);
    notify("NASBridge delete helper: files deleted, but uninstall failed");
    close(g_client);
    return 1;
  }
  send_line("OK");
  notify("NASBridge delete helper: done");
  close(g_client);
  return 0;
}
