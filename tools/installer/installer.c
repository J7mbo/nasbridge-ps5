/*
 * NASBridge installer payload.
 *
 * `make install` sends this through the PS5's ELF loader (port 9021). It then:
 *   1. listens on INSTALLER_PORT for ONE connection (gives up after 60 s),
 *   2. receives an app folder, verifying each file's SHA-256 as it arrives,
 *   3. writes it to a staging folder, then swaps it into place,
 *      unless that title is open on the PS5 (checked first and again just
 *      before the swap: replacing a running title's files has crashed the
 *      whole console),
 *   4. for a home-screen title, refreshes the PS5's own copy of its icon and
 *      backgrounds (/user/appmeta/<title ID>), which a reinstall otherwise
 *      leaves as it was,
 *   5. replies OK/ERR, closes the port and exits.
 * Or, for `make uninstall`, it removes an app: refuses while it's open,
 * deletes its folder from /data/homebrew (so ShadowMountPlus, which adds
 * every folder there to the home screen, doesn't add it back), then removes
 * it from the home screen with the system's own uninstall.
 *
 * Nothing keeps running afterwards. Writes are confined to fixed folders and
 * every incoming path is checked, so a client cannot write anywhere else.
 *
 * Release build (INSTALLER_EMBEDDED, `make release`): the same installer with
 * the app inside it, as the install stream below (made by tools/release/
 * pack.py, zlib-compressed and linked in by tools/release/blob.S). Sent to the
 * ELF loader on its own, it installs or updates the app without a computer:
 * no port is opened; it reads the stream from itself, installs it exactly as
 * above, asks ShadowMountPlus to scan, and says how it went in a notification.
 *
 * Wire format (little-endian), client -> installer:
 *   "NBI1"                 magic
 *   u8   root              0 = /data/homebrew, 1 = /data/nasbridge/selftest
 *   u16  name_len, name    destination folder name, e.g. "NASB00001"
 *   u32  file_count
 *   file_count times:
 *     u16 path_len, path   relative path inside the folder, '/'-separated
 *     u64 size
 *     u8  sha256[32]
 *     size bytes of data
 *   "END1"                 terminator
 * Installer -> client: one line, "OK <files> <bytes> <pictures refreshed>\n"
 * or "ERR <reason>\n".
 *
 * Uninstall, client -> installer: "NBU1", u8 root (0 only), u16 name_len,
 * name (the folder, which is the title ID). Reply: "OK uninstalled\n" or
 * "ERR <reason>\n".
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "sha256.h"

#ifdef INSTALLER_EMBEDDED
#include <zlib.h>
#ifndef NASBRIDGE_VERSION
#include "version.h" /* NASBRIDGE_VERSION, made by the build */
#endif
#endif

#ifndef INSTALLER_HOST_TEST
#include <dlfcn.h>
#include <ps5/kernel.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif
/* Host test build (INSTALLER_HOST_TEST): everything lives under
 * $NB_TEST_PREFIX instead of /, so the installer can be tested on any machine. */

#define INSTALLER_PORT_DEFAULT 9145

#ifndef ACCEPT_TIMEOUT_S
#define ACCEPT_TIMEOUT_S  60
#endif
#ifndef RECV_TIMEOUT_S
#define RECV_TIMEOUT_S    30
#endif
#define MAX_FILES         4096
#define MAX_PATH_LEN      255
#define MAX_NAME_LEN      32
#define MAX_FILE_SIZE     (8ULL * 1024 * 1024 * 1024)
#define CHUNK             (64 * 1024)
/* The PS5 only starts an app whose files are open to every user, so every
 * installed file and folder gets 0777 (as the native app template's release
 * ZIPs do). */
#define INSTALL_MODE      0777

static char g_prefix[256] = "";   /* "" on the PS5; a temp dir in host tests */
static char g_err[512];

static int fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof g_err, fmt, ap);
  va_end(ap);
  return -1;
}

/* ---------------------------------------------------------------- output */

#ifndef INSTALLER_HOST_TEST
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
static void notify(const char *text) { printf("[notify] %s\n", text); }
#endif

/* ------------------------------------------------------- running titles */

#ifndef INSTALLER_HOST_TEST
/* The system's per-process app record. The SDK's ps sample puts the title ID
 * at byte 20, but on 13.60 it starts at byte 16 ("PPSA62737" came back as
 * "62737"), so the ID is searched for anywhere in the record instead. */
typedef struct app_info {
  uint32_t app_id;
  unsigned char rest[0x7c];
} app_info_t;

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

/* Whether `name` appears in the record, as a whole NUL-ended string. */
static int record_has(const app_info_t *info, const char *name) {
  const unsigned char *bytes = (const unsigned char *)info;
  size_t n = strlen(name), i;
  for (i = 0; i + n < sizeof *info; ++i)
    if (memcmp(bytes + i, name, n) == 0 && bytes[i + n] == '\0' &&
        (i == 0 || bytes[i - 1] == '\0'))
      return 1;
  return 0;
}

/* 1 when a process belongs to the title `name` (e.g. "PPSA62737"), 0 when
 * none does, -1 when the process list can't be read. */
static int title_running(const char *name) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  char *buf, *ptr;
  int found = 0;

  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0) return -1;
  size += 16 * sizeof(struct kinfo_proc); /* room for processes started meanwhile */
  if (!(buf = malloc(size))) return -1;
  if (sysctl(mib, 4, buf, &size, NULL, 0) != 0) { free(buf); return -1; }
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
#else
/* Host tests pretend the title named in $NB_TEST_RUNNING is open. */
static int title_running(const char *name) {
  const char *running = getenv("NB_TEST_RUNNING");
  return running && strcmp(running, name) == 0;
}
#endif

/* The installed title's name from its param.json ("NASBridge"), or the
 * folder name when there is none. */
static void title_name(const char *name, char *out, size_t size) {
  char path[600], text[16384], *key, *p, *end;
  ssize_t n;
  int fd;
  snprintf(out, size, "%s", name);
  snprintf(path, sizeof path, "%s/data/homebrew/%s/sce_sys/param.json", g_prefix, name);
  fd = open(path, O_RDONLY);
  if (fd < 0) return;
  n = read(fd, text, sizeof text - 1);
  close(fd);
  if (n <= 0) return;
  text[n] = '\0';
  if (!(key = strstr(text, "\"titleName\""))) return;
  if (!(p = strchr(key + 11, ':')) || !(p = strchr(p, '"'))) return;
  ++p;
  if (!(end = strchr(p, '"')) || end == p || (size_t)(end - p) >= size) return;
  memcpy(out, p, (size_t)(end - p));
  out[end - p] = '\0';
}

/* Refuses (returns -1) when the title being replaced is open. */
static int refuse_if_running(uint8_t root_id, const char *name) {
  char shown[64];
  int r;
  if (root_id != 0) {
    return 0; /* the self-test folder is never a title */
  }
  r = title_running(name);
  title_name(name, shown, sizeof shown);
  if (r > 0) return fail("%s is open on the PS5: close it, then try again", shown);
  if (r < 0) return fail("cannot check whether %s is open, so it was not replaced", shown);
  return 0;
}

/* --------------------------------------------------------------- network */

#ifdef INSTALLER_EMBEDDED
/* The packed install stream, linked in by tools/release/blob.S. */
extern const unsigned char nb_app_blob[];
extern const unsigned char nb_app_blob_end[];

static z_stream g_zs;
static int g_zs_open = 0, g_zs_done = 0;

static int embedded_open(void) {
  memset(&g_zs, 0, sizeof g_zs);
  if (inflateInit(&g_zs) != Z_OK) return fail("couldn't start unpacking");
  g_zs_open = 1;
  g_zs.next_in = (Bytef *)nb_app_blob;
  g_zs.avail_in = (uInt)(nb_app_blob_end - nb_app_blob);
  return 0;
}

static void embedded_close(void) {
  if (g_zs_open) inflateEnd(&g_zs);
  g_zs_open = 0;
}

/* The next `len` bytes of the stream, unpacked. `fd` is unused: the stream
 * comes from this file, not a connection. */
static int read_exact(int fd, void *buf, size_t len) {
  (void)fd;
  g_zs.next_out = buf;
  g_zs.avail_out = (uInt)len;
  while (g_zs.avail_out > 0) {
    int r;
    if (g_zs_done) return fail("the packed app is incomplete");
    r = inflate(&g_zs, Z_NO_FLUSH);
    if (r == Z_STREAM_END) g_zs_done = 1;
    else if (r != Z_OK) return fail("the packed app is damaged (zlib %d)", r);
  }
  return 0;
}
#else
static int read_exact(int fd, void *buf, size_t len) {
  uint8_t *p = buf;
  while (len > 0) {
    ssize_t n = recv(fd, p, len, 0);
    if (n == 0) return fail("connection closed early");
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return fail("timed out waiting for data");
      return fail("receive failed: %s", strerror(errno));
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}
#endif

static int read_u8(int fd, uint8_t *v) { return read_exact(fd, v, 1); }

static int read_u16(int fd, uint16_t *v) {
  uint8_t b[2];
  if (read_exact(fd, b, 2)) return -1;
  *v = (uint16_t)(b[0] | b[1] << 8);
  return 0;
}

static int read_u32(int fd, uint32_t *v) {
  uint8_t b[4];
  if (read_exact(fd, b, 4)) return -1;
  *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
  return 0;
}

static int read_u64(int fd, uint64_t *v) {
  uint32_t lo, hi;
  if (read_u32(fd, &lo) || read_u32(fd, &hi)) return -1;
  *v = (uint64_t)hi << 32 | lo;
  return 0;
}

#ifndef INSTALLER_EMBEDDED
static void send_line(int fd, const char *line) {
  size_t len = strlen(line);
  while (len > 0) {
    ssize_t n = send(fd, line, len, 0);
    if (n <= 0) return;
    line += n;
    len -= (size_t)n;
  }
}
#endif

/* ------------------------------------------------------------ validation */

static int valid_name(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n > MAX_NAME_LEN) return 0;
  for (; *s; s++) {
    char c = *s;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return 0;
  }
  return 1;
}

/* A relative path whose components are non-empty, not "." or "..", and use
 * only safe characters. This is what stops writes escaping the folder. */
static int valid_rel_path(const char *s) {
  const char *start = s;
  size_t comp = 0;

  if (*s == '\0' || *s == '/') return 0;
  for (;; s++) {
    char c = *s;
    if (c == '/' || c == '\0') {
      if (comp == 0) return 0;
      if (comp == 1 && s[-1] == '.') return 0;
      if (comp == 2 && s[-1] == '.' && s[-2] == '.') return 0;
      if (c == '\0') break;
      comp = 0;
      continue;
    }
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
          c == ' ' || c == '(' || c == ')'))
      return 0;
    comp++;
  }
  return (size_t)(s - start) <= MAX_PATH_LEN;
}

/* ------------------------------------------------------------ filesystem */

static int mkdir_p(const char *path) {
  char tmp[1024];
  size_t i, len = strlen(path);

  if (len >= sizeof tmp) return fail("path too long: %s", path);
  memcpy(tmp, path, len + 1);
  for (i = 1; i <= len; i++) {
    if (tmp[i] == '/' || tmp[i] == '\0') {
      char saved = tmp[i];
      tmp[i] = '\0';
      if (mkdir(tmp, INSTALL_MODE) == 0)
        chmod(tmp, INSTALL_MODE); /* mkdir's mode is reduced by the umask */
      else if (errno != EEXIST)
        return fail("cannot create %s: %s", tmp, strerror(errno));
      tmp[i] = saved;
    }
  }
  return 0;
}

static int rm_rf(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    struct dirent *e;
    char child[1024];
    if (!d) return -1;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      snprintf(child, sizeof child, "%s/%s", path, e->d_name);
      rm_rf(child);
    }
    closedir(d);
    return rmdir(path);
  }
  return unlink(path);
}

static int exists(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0;
}

/* ---------------------------------------------------------------- install */

static int receive_file(int fd, const char *dir, uint64_t *total) {
  char rel[MAX_PATH_LEN + 1], full[1024], *slash;
  uint8_t want[32], got[32];
  uint8_t *buf;
  uint16_t plen;
  uint64_t size, left;
  sha256_ctx ctx;
  int out;

  if (read_u16(fd, &plen)) return -1;
  if (plen == 0 || plen > MAX_PATH_LEN) return fail("bad path length %u", plen);
  if (read_exact(fd, rel, plen)) return -1;
  rel[plen] = '\0';
  if (!valid_rel_path(rel)) return fail("refused unsafe path: %s", rel);
  if (read_u64(fd, &size) || read_exact(fd, want, 32)) return -1;
  if (size > MAX_FILE_SIZE) return fail("file too large: %s", rel);

  snprintf(full, sizeof full, "%s/%s", dir, rel);
  slash = strrchr(full, '/');
  *slash = '\0';
  if (mkdir_p(full)) return -1;
  *slash = '/';

  out = open(full, O_WRONLY | O_CREAT | O_TRUNC, INSTALL_MODE);
  if (out < 0) return fail("cannot write %s: %s", rel, strerror(errno));
  fchmod(out, INSTALL_MODE); /* open's mode is reduced by the umask */

  buf = malloc(CHUNK);
  if (!buf) { close(out); return fail("out of memory"); }

  sha256_init(&ctx);
  for (left = size; left > 0;) {
    size_t n = left < CHUNK ? (size_t)left : CHUNK;
    if (read_exact(fd, buf, n)) goto err;
    sha256_update(&ctx, buf, n);
    for (size_t off = 0; off < n;) {
      ssize_t w = write(out, buf + off, n - off);
      if (w < 0) { fail("write failed for %s: %s", rel, strerror(errno)); goto err; }
      off += (size_t)w;
    }
    left -= n;
  }
  free(buf);
  if (close(out) != 0) return fail("cannot finish %s: %s", rel, strerror(errno));

  sha256_final(&ctx, got);
  if (memcmp(want, got, 32) != 0) return fail("checksum mismatch: %s", rel);
  *total += size;
  return 0;

err:
  free(buf);
  close(out);
  return -1;
}

static int refresh_appmeta(const char *name, const char *dest);

/* ------------------------------------------------------------- uninstall */

#ifndef INSTALLER_HOST_TEST
/* As the delete helper (tools/helper/helper.c) does: the identity and
 * capabilities the system's uninstall needs, and its modules loaded at
 * runtime (a payload must not link system modules, or the loader refuses it). */
#define AUTHID_SHELL 0x4800000000000006ull
static int system_uninstall(const char *title_id) {
  typedef int (*user_init_t)(const void *);
  typedef int (*appinst_init_t)(void);
  typedef int (*uninstall_t)(const char *);
  void *user_h, *inst_h;
  appinst_init_t inst_init;
  uninstall_t app_uninstall;
  uint8_t caps[16];
  int rc;
  kernel_set_ucred_authid(getpid(), AUTHID_SHELL);
  memset(caps, 0xff, sizeof caps);
  kernel_set_ucred_caps(getpid(), caps);
  user_h = dlopen("/system/common/lib/libSceUserService.sprx", RTLD_LAZY);
  if (user_h) {
    user_init_t user_init = (user_init_t)dlsym(user_h, "sceUserServiceInitialize");
    if (user_init) user_init(NULL);
  }
  inst_h = dlopen("/system/common/lib/libSceAppInstUtil.sprx", RTLD_LAZY);
  if (!inst_h) return fail("couldn't load the uninstall module");
  inst_init = (appinst_init_t)dlsym(inst_h, "sceAppInstUtilInitialize");
  app_uninstall = (uninstall_t)dlsym(inst_h, "sceAppInstUtilAppUnInstall");
  if (!app_uninstall) return fail("couldn't find the uninstall function");
  if (inst_init) inst_init();
  rc = app_uninstall(title_id);
  if (rc != 0) return fail("the PS5 refused to remove it from the home screen (code 0x%08X)",
                           (unsigned)rc);
  return 0;
}
#else
/* Tests: records the uninstall in $NB_TEST_PREFIX/uninstalled.txt. */
static int system_uninstall(const char *title_id) {
  char path[600];
  FILE *f;
  if (getenv("NB_TEST_UNINSTALL_FAILS"))
    return fail("the PS5 refused to remove it from the home screen (code 0x80990001)");
  snprintf(path, sizeof path, "%s/uninstalled.txt", g_prefix);
  if (!(f = fopen(path, "a"))) return fail("cannot record the uninstall");
  fprintf(f, "%s\n", title_id);
  fclose(f);
  return 0;
}
#endif

/* The rest of an "NBU1" request (the magic is read). The folder goes first:
 * if the home-screen removal then fails, deleting the tile by hand finishes
 * the job, where the other order would let ShadowMountPlus add it back. */
static int run_uninstall(int fd) {
  char name[MAX_NAME_LEN + 1], dest[600];
  uint8_t root_id;
  uint16_t nlen;
  if (read_u8(fd, &root_id)) return -1;
  if (root_id != 0) return fail("only apps in /data/homebrew can be uninstalled");
  if (read_u16(fd, &nlen)) return -1;
  if (nlen == 0 || nlen > MAX_NAME_LEN) return fail("bad folder name length");
  if (read_exact(fd, name, nlen)) return -1;
  name[nlen] = '\0';
  if (!valid_name(name)) return fail("refused folder name: %s", name);
  if (refuse_if_running(root_id, name)) return -1;
  snprintf(dest, sizeof dest, "%s/data/homebrew/%s", g_prefix, name);
  rm_rf(dest);
  if (exists(dest)) return fail("couldn't delete %s: %s", dest, strerror(errno));
  return system_uninstall(name);
}

static int g_uninstalling = 0; /* this connection asked to uninstall */
static int g_replaced = 0;     /* the install replaced an earlier version */

static int run_install(int fd, uint32_t *files_out, uint64_t *bytes_out, int *pictures_out) {
  char magic[4], name[MAX_NAME_LEN + 1];
  char root[512], staging_root[512], staging[600], dest[600], old[600];
  uint8_t root_id;
  uint16_t nlen;
  uint32_t count, i;
  uint64_t total = 0;

  if (read_exact(fd, magic, 4)) return -1;
  if (memcmp(magic, "NBU1", 4) == 0) {
    g_uninstalling = 1;
    return run_uninstall(fd);
  }
  if (memcmp(magic, "NBI1", 4) != 0) return fail("not a NASBridge install stream");
  if (read_u8(fd, &root_id)) return -1;
  if (root_id > 1) return fail("unknown destination %u", root_id);
  if (read_u16(fd, &nlen)) return -1;
  if (nlen == 0 || nlen > MAX_NAME_LEN) return fail("bad folder name length");
  if (read_exact(fd, name, nlen)) return -1;
  name[nlen] = '\0';
  if (!valid_name(name)) return fail("refused folder name: %s", name);
  if (read_u32(fd, &count)) return -1;
  if (count == 0 || count > MAX_FILES) return fail("bad file count %u", count);
  if (refuse_if_running(root_id, name)) return -1;

  snprintf(root, sizeof root, "%s%s", g_prefix,
           root_id == 0 ? "/data/homebrew" : "/data/nasbridge/selftest");
  snprintf(staging_root, sizeof staging_root, "%s/data/nasbridge/staging", g_prefix);
  snprintf(staging, sizeof staging, "%s/%s", staging_root, name);
  snprintf(old, sizeof old, "%s/%s.old", staging_root, name);
  snprintf(dest, sizeof dest, "%s/%s", root, name);

  /* Everything is written to a staging folder on the same drive first, so a
   * failed or interrupted install never leaves a half-written app behind. */
  rm_rf(staging);
  if (mkdir_p(staging) || mkdir_p(root)) return -1;

  for (i = 0; i < count; i++) {
    if (receive_file(fd, staging, &total)) { rm_rf(staging); return -1; }
  }
  if (read_exact(fd, magic, 4) || memcmp(magic, "END1", 4) != 0) {
    rm_rf(staging);
    return g_err[0] ? -1 : fail("missing end marker");
  }

  /* The title may have been opened while the files were arriving. */
  if (refuse_if_running(root_id, name)) { rm_rf(staging); return -1; }

  /* Swap: move any existing version aside, move the new one in, then delete
   * the old one. If the move-in fails, put the old version back. */
  rm_rf(old);
  g_replaced = exists(dest);
  if (exists(dest) && rename(dest, old) != 0) {
    rm_rf(staging);
    return fail("cannot replace %s: %s", dest, strerror(errno));
  }
  if (rename(staging, dest) != 0) {
    int e = errno;
    if (exists(old)) rename(old, dest);
    rm_rf(staging);
    return fail("cannot move into %s: %s", dest, strerror(e));
  }
  rm_rf(old);

  *files_out = count;
  *bytes_out = total;
  *pictures_out = root_id == 0 ? refresh_appmeta(name, dest) : 0;
  return 0;
}

/* ------------------------------------------------- home-screen pictures */

/* Copies one file over another, through a temporary name. 0 on success. */
static int copy_file(const char *from, const char *to) {
  char tmp[700], buf[CHUNK];
  ssize_t n;
  int in, out, ok = 1;
  snprintf(tmp, sizeof tmp, "%s.nbtmp", to);
  in = open(from, O_RDONLY);
  if (in < 0) return -1;
  out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, INSTALL_MODE);
  if (out < 0) { close(in); return -1; }
  while ((n = read(in, buf, sizeof buf)) > 0)
    if (write(out, buf, (size_t)n) != n) { ok = 0; break; }
  if (n < 0) ok = 0;
  fchmod(out, INSTALL_MODE);
  close(in);
  if (close(out) != 0) ok = 0;
  if (!ok || rename(tmp, to) != 0) { unlink(tmp); return -1; }
  return 0;
}

/* The home screen shows a title's icon and backgrounds (and plays its
 * selection music) from its own copies
 * in /user/appmeta/<title ID>, made when the title was first registered; a
 * reinstall doesn't update them. Only an existing folder for this title is
 * touched. Returns how many pictures were copied. */
static int refresh_appmeta(const char *name, const char *dest) {
  static const char *const always[] = {"icon0.png", "pic0.dds", "pic1.dds", "snd0.at9"};
  static const char *const if_there[] = {"pic0.png", "pic1.png"};
  char meta[600], from[700], to[700];
  unsigned i;
  int copied = 0;
  snprintf(meta, sizeof meta, "%s/user/appmeta/%s", g_prefix, name);
  if (!exists(meta)) return 0;
  for (i = 0; i < sizeof always / sizeof always[0]; i++) {
    snprintf(from, sizeof from, "%s/sce_sys/%s", dest, always[i]);
    snprintf(to, sizeof to, "%s/%s", meta, always[i]);
    if (exists(from) && copy_file(from, to) == 0) copied++;
  }
  for (i = 0; i < sizeof if_there / sizeof if_there[0]; i++) {
    snprintf(from, sizeof from, "%s/sce_sys/%s", dest, if_there[i]);
    snprintf(to, sizeof to, "%s/%s", meta, if_there[i]);
    if (exists(from) && exists(to) && copy_file(from, to) == 0) copied++;
  }
  return copied;
}

/* ------------------------------------------------------------------- main */

#ifdef INSTALLER_EMBEDDED
/* ShadowMountPlus's API (127.0.0.1:10101): a scan adds the app to the home
 * screen now instead of at its next look. Best effort: without the API, it
 * still finds the app on its own. */
static int g_smp_port = 10101;

static void ask_smp_scan(void) {
  static const char request[] =
      "POST /api/v1/scan HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Content-Type: application/json\r\nContent-Length: 2\r\n"
      "Connection: close\r\n\r\n{}";
  struct sockaddr_in addr;
  struct timeval tv = {5, 0};
  char sink[512];
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)g_smp_port);
  if (connect(s, (struct sockaddr *)&addr, sizeof addr) == 0 &&
      send(s, request, sizeof request - 1, 0) == (ssize_t)(sizeof request - 1)) {
    while (recv(s, sink, sizeof sink, 0) > 0) {
    }
  }
  close(s);
}

int main(void) {
  uint32_t files = 0;
  uint64_t bytes = 0;
  int pictures = 0, rc;
  char line[1024];

#ifdef INSTALLER_HOST_TEST
  const char *p = getenv("NB_TEST_PREFIX");
  const char *sp = getenv("NB_TEST_SMP_PORT");
  if (!p || !*p) { fprintf(stderr, "NB_TEST_PREFIX not set\n"); return 2; }
  snprintf(g_prefix, sizeof g_prefix, "%s", p);
  if (sp) g_smp_port = atoi(sp);
#else
  kernel_set_proc_rootdir(getpid(), kernel_get_root_vnode());
#endif

  notify("Installing NASBridge " NASBRIDGE_VERSION "...");
  g_err[0] = '\0';
  rc = embedded_open();
  if (rc == 0) rc = run_install(-1, &files, &bytes, &pictures);
  embedded_close();
  snprintf(line, sizeof line, "%s/data/nasbridge/staging", g_prefix);
  rmdir(line);

  if (rc == 0) {
    ask_smp_scan();
    if (g_replaced)
      snprintf(line, sizeof line, "NASBridge updated to " NASBRIDGE_VERSION);
    else
      snprintf(line, sizeof line,
               "NASBridge " NASBRIDGE_VERSION " installed. It appears on the home screen shortly.");
  } else {
    snprintf(line, sizeof line, "NASBridge " NASBRIDGE_VERSION " wasn't installed: %s",
             g_err[0] ? g_err : "unknown error");
  }
  notify(line);
  printf("%s %u %llu %d\n", rc == 0 ? "OK" : "ERR", files, (unsigned long long)bytes, pictures);
  return rc == 0 ? 0 : 1;
}
#else
/* After a refusal the client may still be sending. Closing with its data
 * unread would reset the connection and lose the ERR line, so stop sending,
 * then read and discard until the client closes (or goes quiet). */
static void drain(int fd) {
  char sink[4096];
  struct timeval tv = {5, 0};
  shutdown(fd, SHUT_WR);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  while (recv(fd, sink, sizeof sink, 0) > 0) {
  }
}

static int listen_once(int port) {
  struct sockaddr_in addr;
  struct timeval tv;
  fd_set set;
  int one = 1, srv, cli;

  srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) return -1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(srv, 1) != 0) {
    close(srv);
    return -1;
  }

  FD_ZERO(&set);
  FD_SET(srv, &set);
  tv.tv_sec = ACCEPT_TIMEOUT_S;
  tv.tv_usec = 0;
  if (select(srv + 1, &set, NULL, NULL, &tv) <= 0) {
    close(srv);
    return -2;
  }
  cli = accept(srv, NULL, NULL);
  close(srv); /* one connection only: stop listening straight away */
  if (cli < 0) return -1;

  tv.tv_sec = RECV_TIMEOUT_S;
  tv.tv_usec = 0;
  setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return cli;
}

int main(void) {
  int port = INSTALLER_PORT_DEFAULT, fd, rc;
  uint32_t files = 0;
  uint64_t bytes = 0;
  int pictures = 0;
  char line[2048];

#ifdef INSTALLER_HOST_TEST
  const char *p = getenv("NB_TEST_PREFIX");
  const char *pp = getenv("NB_TEST_PORT");
  if (!p || !*p) { fprintf(stderr, "NB_TEST_PREFIX not set\n"); return 2; }
  snprintf(g_prefix, sizeof g_prefix, "%s", p);
  if (pp) port = atoi(pp);
#else
  /* Payloads start inside the app sandbox; switch to the real filesystem
   * root so /data is reachable, as the SDK's own file samples do. */
  kernel_set_proc_rootdir(getpid(), kernel_get_root_vnode());
#endif

  fd = listen_once(port);
  if (fd == -2) {
    notify("NASBridge installer: nothing connected, closed");
    return 1;
  }
  if (fd < 0) {
    notify("NASBridge installer: could not open its port");
    return 1;
  }

  g_err[0] = '\0';
  rc = run_install(fd, &files, &bytes, &pictures);

  /* Tidy up: remove the staging folder once it's empty (rmdir refuses if
   * anything is still inside, so this can never delete real data). */
  snprintf(line, sizeof line, "%s/data/nasbridge/staging", g_prefix);
  rmdir(line);
  if (rc == 0 && g_uninstalling) {
    send_line(fd, "OK uninstalled\n");
    notify("NASBridge uninstalled");
  } else if (rc == 0) {
    snprintf(line, sizeof line, "OK %u %llu %d\n", files, (unsigned long long)bytes, pictures);
    send_line(fd, line);
    notify("NASBridge installed");
  } else {
    snprintf(line, sizeof line, "ERR %s\n", g_err[0] ? g_err : "unknown error");
    send_line(fd, line);
    drain(fd);
    snprintf(line, sizeof line, "%s failed: %s", g_uninstalling ? "Uninstall" : "Install", g_err);
    notify(line);
  }
  close(fd);
  return rc == 0 ? 0 : 1;
}
#endif
