/*
 * Game Compressor - self-update from GitHub releases. See gc_update.h.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <openssl/evp.h>

#include "gc_ca.h"
#include "gc_diag.h"
#include "gc_json_escape.h"
#include "gc_update.h"

#ifndef GC_BUILD_NUMBER
#define GC_BUILD_NUMBER 0
#endif
#ifndef GAME_COMPRESSOR_VERSION
#define GAME_COMPRESSOR_VERSION "dev"
#endif

#define REPO "TOSanthony/PS5-Game-Compressor"
#define API_LATEST "https://api.github.com/repos/" REPO "/releases/latest"
#define API_NEWEST "https://api.github.com/repos/" REPO "/releases?per_page=1"
#define DOWNLOAD_BASE "https://github.com/" REPO "/releases/download/"
#define API_BASE "https://api.github.com/repos/" REPO "/"
#define APP_FILE "PPSA99290.ffpkg"
#define ELF_FILE "game-compressor.elf"
#define DATA_DIR "/data/GameCompressor" /* the payload's own, known-writable directory */
#define MAX_ELF (64LL << 20)
#define MAX_IMAGE (512LL << 20)
#define SPARE (64LL << 20)
#define UFS2_MAGIC_OFFSET (65536 + 1372)

static const char *const k_app_dirs[] = {
  "/data/homebrew", "/mnt/ext0/homebrew", "/mnt/ext1/homebrew",
  "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",
  "/mnt/usb3/homebrew", "/mnt/usb4/homebrew", "/mnt/usb5/homebrew",
  "/mnt/usb6/homebrew", NULL};

static struct {
  pthread_mutex_t mutex;
  int busy;
  int cancel;
  char phase[16];
  char error[256];
  char tag[96];
  char latest_version[96];
  long latest_build;
  int64_t received, total;
  char file[48];
  time_t checked_at;
  char app_status[24];   /* replaced | created | folder-copy | none */
  char app_path[420];
  int elf_saved;
  int curl_ready;
  int staged_elsewhere;
} st = {.mutex = PTHREAD_MUTEX_INITIALIZER, .phase = "idle"};

static void
set_phase(const char *phase) {
  pthread_mutex_lock(&st.mutex);
  snprintf(st.phase, sizeof(st.phase), "%s", phase);
  pthread_mutex_unlock(&st.mutex);
}

static void
fail(const char *message) {
  gc_log("update: %s", message);
  pthread_mutex_lock(&st.mutex);
  snprintf(st.error, sizeof(st.error), "%s", message);
  snprintf(st.phase, sizeof(st.phase), "%s", "error");
  pthread_mutex_unlock(&st.mutex);
}

static int
url_allowed(const char *url) {
  return !strncmp(url, API_BASE, strlen(API_BASE)) ||
         !strncmp(url, DOWNLOAD_BASE, strlen(DOWNLOAD_BASE));
}

typedef struct {
  int fd;                 /* -1: collect into data */
  char *data;
  size_t size, cap;
  int64_t limit, written;
  EVP_MD_CTX *digest;
  int failed;
} sink_t;

static size_t
write_cb(char *ptr, size_t size, size_t nmemb, void *user) {
  sink_t *sink = user;
  size_t n = size * nmemb;
  if(sink->limit > 0 && sink->written + (int64_t)n > sink->limit) {
    sink->failed = 1;
    return 0;
  }
  if(sink->fd >= 0) {
    size_t done = 0;
    while(done < n) {
      ssize_t w = write(sink->fd, ptr + done, n - done);
      if(w < 0 && errno == EINTR) continue;
      if(w <= 0) { sink->failed = 1; return 0; }
      done += (size_t)w;
    }
    if(sink->digest) EVP_DigestUpdate(sink->digest, ptr, n);
  } else {
    if(sink->size + n + 1 > sink->cap) {
      size_t cap = sink->cap ? sink->cap : 4096;
      while(cap < sink->size + n + 1) cap *= 2;
      char *grown = realloc(sink->data, cap);
      if(!grown) { sink->failed = 1; return 0; }
      sink->data = grown;
      sink->cap = cap;
    }
    memcpy(sink->data + sink->size, ptr, n);
    sink->size += n;
    sink->data[sink->size] = 0;
  }
  sink->written += (int64_t)n;
  return n;
}

static int
progress_cb(void *user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
            curl_off_t ulnow) {
  (void)user; (void)ultotal; (void)ulnow;
  pthread_mutex_lock(&st.mutex);
  int cancel = st.cancel;
  st.received = dlnow;
  st.total = dltotal;
  pthread_mutex_unlock(&st.mutex);
  return cancel ? 1 : 0;
}

/* GET url into memory (fd < 0) or into the open file descriptor. */
static int
http_get(const char *url, sink_t *sink, char *err, size_t err_size) {
  if(!url_allowed(url)) {
    snprintf(err, err_size, "url not allowed");
    return -1;
  }
  CURL *curl = curl_easy_init();
  if(!curl) {
    snprintf(err, err_size, "curl init failed");
    return -1;
  }
  struct curl_blob ca = {(void *)gc_ca_bundle, sizeof(gc_ca_bundle) - 1,
                         CURL_BLOB_COPY};
  char curl_err[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "GameCompressor/" GAME_COMPRESSOR_VERSION);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1000L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 40L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, sink);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
  struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if(rc != CURLE_OK || sink->failed) {
    snprintf(err, err_size, "download failed: %s (HTTP %ld)",
             curl_err[0] ? curl_err : curl_easy_strerror(rc), status);
    return -1;
  }
  return 0;
}

static int
tag_safe(const char *tag) {
  if(!tag[0] || strlen(tag) > 90) return 0;
  for(const char *p = tag; *p; p++) {
    if(!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_') return 0;
  }
  return 1;
}

/* Value of "key" in a small JSON document: a string (copied) or a number. */
static int
json_find(const char *json, const char *key, char *out, size_t out_size) {
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if(!p) return -1;
  p += strlen(pat);
  while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if(*p != ':') return -1;
  p++;
  while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  size_t n = 0;
  if(*p == '"') {
    p++;
    while(*p && *p != '"' && n + 1 < out_size) out[n++] = *p++;
  } else {
    while((isdigit((unsigned char)*p) || *p == '.' || *p == '-') && n + 1 < out_size) out[n++] = *p++;
  }
  out[n] = 0;
  return n ? 0 : -1;
}

static void
build_url(char *out, size_t size, const char *tag, const char *name) {
  snprintf(out, size, DOWNLOAD_BASE "%s/%s", tag, name);
}

static int
do_check(void) {
  char err[200];
  sink_t sink = {.fd = -1, .limit = 1 << 20};
  if(http_get(API_LATEST, &sink, err, sizeof(err)) != 0) {
    /* No stable release yet (only pre-releases of branch builds): use the
     * newest release of any kind. */
    free(sink.data);
    sink = (sink_t){.fd = -1, .limit = 1 << 20};
    if(http_get(API_NEWEST, &sink, err, sizeof(err)) != 0) {
      free(sink.data);
      fail(err);
      return -1;
    }
  }
  char tag[96] = {0};
  int ok = sink.data && json_find(sink.data, "tag_name", tag, sizeof(tag)) == 0 &&
           tag_safe(tag);
  free(sink.data);
  if(!ok) { fail("no release found"); return -1; }

  char url[300];
  build_url(url, sizeof(url), tag, "build-info.json");
  sink_t info = {.fd = -1, .limit = 16384};
  if(http_get(url, &info, err, sizeof(err)) != 0) {
    free(info.data);
    fail("the latest release has no build-info.json");
    return -1;
  }
  char number[32] = {0}, version[64] = {0};
  int have = info.data && json_find(info.data, "build", number, sizeof(number)) == 0;
  if(info.data) json_find(info.data, "version", version, sizeof(version));
  free(info.data);
  if(!have) { fail("bad build-info.json"); return -1; }

  pthread_mutex_lock(&st.mutex);
  snprintf(st.tag, sizeof(st.tag), "%s", tag);
  snprintf(st.latest_version, sizeof(st.latest_version), "%s", version[0] ? version : tag);
  st.latest_build = atol(number);
  st.checked_at = time(NULL);
  st.error[0] = 0;
  snprintf(st.phase, sizeof(st.phase), "%s",
           st.latest_build > GC_BUILD_NUMBER ? "available" : "uptodate");
  pthread_mutex_unlock(&st.mutex);
  gc_log("update: latest tag=%s build=%ld current=%d", tag, atol(number), (int)GC_BUILD_NUMBER);
  return 0;
}

static int
hex_of(const unsigned char *digest, unsigned len, char *out) {
  static const char *hex = "0123456789abcdef";
  for(unsigned i = 0; i < len; i++) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 15];
  }
  out[len * 2] = 0;
  return 0;
}

/* Expected SHA-256 of name from "hash  name" lines. */
static int
sums_lookup(const char *sums, const char *name, char *hash) {
  for(const char *line = sums; line && *line;) {
    const char *end = strchr(line, '\n');
    size_t len = end ? (size_t)(end - line) : strlen(line);
    if(len > 66) {
      const char *n = line + 64;
      while(n < line + len && (*n == ' ' || *n == '*')) n++;
      size_t nl = (size_t)(line + len - n);
      while(nl && (n[nl - 1] == '\r' || n[nl - 1] == ' ')) nl--;
      if(nl == strlen(name) && !strncmp(n, name, nl)) {
        memcpy(hash, line, 64);
        hash[64] = 0;
        for(int i = 0; i < 64; i++) hash[i] = (char)tolower((unsigned char)hash[i]);
        return 0;
      }
    }
    line = end ? end + 1 : NULL;
  }
  return -1;
}

static int
download_verified(const char *tag, const char *name, const char *expected,
                  const char *part, int64_t limit) {
  char url[300], err[200];
  build_url(url, sizeof(url), tag, name);
  pthread_mutex_lock(&st.mutex);
  snprintf(st.file, sizeof(st.file), "%s", name);
  st.received = st.total = 0;
  pthread_mutex_unlock(&st.mutex);
  set_phase("downloading");
  int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd < 0) {
    char msg[256];
    snprintf(msg, sizeof(msg), "cannot write %s: %s", part, strerror(errno));
    fail(msg);
    return -1;
  }
  sink_t sink = {.fd = fd, .limit = limit, .digest = EVP_MD_CTX_new()};
  if(!sink.digest || EVP_DigestInit_ex(sink.digest, EVP_sha256(), NULL) != 1) {
    close(fd); unlink(part); fail("sha256 init failed"); return -1;
  }
  int rc = http_get(url, &sink, err, sizeof(err));
  if(rc == 0) fsync(fd);
  close(fd);
  unsigned char digest[32];
  unsigned dlen = 0;
  EVP_DigestFinal_ex(sink.digest, digest, &dlen);
  EVP_MD_CTX_free(sink.digest);
  if(rc != 0) { unlink(part); fail(err); return -1; }
  set_phase("verifying");
  char hex[65];
  hex_of(digest, dlen, hex);
  if(strcmp(hex, expected)) {
    unlink(part);
    fail("checksum mismatch, download discarded");
    return -1;
  }
  return 0;
}

static int
file_prefix_ok(const char *path, int64_t offset, const unsigned char *want, size_t n) {
  unsigned char buf[16];
  int fd = open(path, O_RDONLY);
  if(fd < 0 || n > sizeof(buf)) { if(fd >= 0) close(fd); return 0; }
  ssize_t r = pread(fd, buf, n, (off_t)offset);
  close(fd);
  return r == (ssize_t)n && !memcmp(buf, want, n);
}

static int
free_space_ok(const char *dir, int64_t need) {
  struct statvfs vfs;
  if(statvfs(dir, &vfs) != 0) return 1;
  return (int64_t)vfs.f_bavail * (int64_t)vfs.f_frsize >= need + SPARE;
}

static void
find_app_dir(char *dir, size_t size, int *folder_copy, int *existing) {
  *folder_copy = 0;
  *existing = 0;
  snprintf(dir, size, "%s", k_app_dirs[0]);
  for(int i = 0; k_app_dirs[i]; i++) {
    char p[400];
    struct stat s;
    snprintf(p, sizeof(p), "%s/PPSA99290/eboot.bin", k_app_dirs[i]);
    if(!stat(p, &s)) *folder_copy = 1;
    snprintf(p, sizeof(p), "%s/" APP_FILE, k_app_dirs[i]);
    if(!*existing && !stat(p, &s) && S_ISREG(s.st_mode)) {
      *existing = 1;
      snprintf(dir, size, "%s", k_app_dirs[i]);
    }
  }
}

/* Copies src over dest (through a temporary name next to dest) when rename()
 * cannot cross file systems. Returns 1 on success. */
static int
copy_over(const char *src, const char *dest) {
  char tmp[460];
  snprintf(tmp, sizeof(tmp), "%s.new", dest);
  int in = open(src, O_RDONLY);
  int out = in >= 0 ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
  int ok = 0;
  if(in >= 0 && out >= 0) {
    static char buf[1 << 16];
    ssize_t n;
    ok = 1;
    while(ok && (n = read(in, buf, sizeof(buf))) > 0) {
      for(ssize_t done = 0; done < n;) {
        ssize_t w = write(out, buf + done, (size_t)(n - done));
        if(w <= 0) { ok = 0; break; }
        done += w;
      }
    }
    if(n < 0) ok = 0;
    if(ok) fsync(out);
  }
  if(in >= 0) close(in);
  if(out >= 0) close(out);
  if(ok && rename(tmp, dest) == 0) return 1;
  unlink(tmp);
  return 0;
}

static int
do_install(void) {
  if(do_check() != 0) return -1;
  pthread_mutex_lock(&st.mutex);
  long latest = st.latest_build;
  char tag[96];
  snprintf(tag, sizeof(tag), "%s", st.tag);
  pthread_mutex_unlock(&st.mutex);
  if(latest <= GC_BUILD_NUMBER) return 0;

  char err[200], url[300];
  build_url(url, sizeof(url), tag, "SHA256SUMS");
  sink_t sums = {.fd = -1, .limit = 65536};
  set_phase("checking");
  if(http_get(url, &sums, err, sizeof(err)) != 0 || !sums.data) {
    free(sums.data);
    fail("the release has no SHA256SUMS");
    return -1;
  }
  char elf_hash[65], app_hash[65];
  int have_elf = sums_lookup(sums.data, ELF_FILE, elf_hash) == 0;
  int have_app = sums_lookup(sums.data, APP_FILE, app_hash) == 0;
  free(sums.data);
  if(!have_elf || !have_app) { fail("release assets are missing"); return -1; }

  char app_dir[300];
  int folder_copy, existing;
  find_app_dir(app_dir, sizeof(app_dir), &folder_copy, &existing);
  if(mkdir(DATA_DIR, 0755) != 0 && errno != EEXIST) {
    char msg[256];
    snprintf(msg, sizeof(msg), "cannot create %s: %s", DATA_DIR, strerror(errno));
    fail(msg);
    return -1;
  }
  if(!free_space_ok(DATA_DIR, 80LL << 20) ||
     (!folder_copy && !free_space_ok(app_dir, 200LL << 20))) {
    fail("not enough free space for the update");
    return -1;
  }

  const char *elf_part = DATA_DIR "/game-compressor.elf.part";
  char app_part[400];
  snprintf(app_part, sizeof(app_part), "%s/." APP_FILE ".part", app_dir);
  if(download_verified(tag, ELF_FILE, elf_hash, elf_part, MAX_ELF) != 0) return -1;
  static const unsigned char elf_magic[6] = {0x7f, 'E', 'L', 'F', 2, 1};
  static const unsigned char machine[2] = {62, 0};
  if(!file_prefix_ok(elf_part, 0, elf_magic, 6) || !file_prefix_ok(elf_part, 18, machine, 2)) {
    unlink(elf_part);
    fail("downloaded payload is not an x86-64 ELF");
    return -1;
  }
  if(!folder_copy) {
    /* Prefer a hidden file next to the destination (atomic rename). If that
     * directory refuses new files, stage in our own directory and copy. */
    int staged_elsewhere = 0;
    {
      int probe = open(app_part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if(probe >= 0) {
        close(probe);
        unlink(app_part);
      } else {
        gc_log("update: cannot stage in %s: %s", app_dir, strerror(errno));
        snprintf(app_part, sizeof(app_part), "%s/" APP_FILE ".part", DATA_DIR);
        staged_elsewhere = 1;
      }
    }
    pthread_mutex_lock(&st.mutex);
    st.staged_elsewhere = staged_elsewhere;
    pthread_mutex_unlock(&st.mutex);
    if(download_verified(tag, APP_FILE, app_hash, app_part, MAX_IMAGE) != 0) {
      unlink(elf_part);
      return -1;
    }
    static const unsigned char ufs2[4] = {0x19, 0x01, 0x54, 0x19};
    if(!file_prefix_ok(app_part, UFS2_MAGIC_OFFSET, ufs2, 4)) {
      unlink(elf_part);
      unlink(app_part);
      fail("downloaded app image is not a UFS2 image");
      return -1;
    }
  }

  set_phase("installing");
  if(rename(elf_part, DATA_DIR "/" ELF_FILE) != 0) {
    unlink(elf_part);
    if(!folder_copy) unlink(app_part);
    fail("cannot save the new payload");
    return -1;
  }
  char build_text[32];
  int bn = snprintf(build_text, sizeof(build_text), "%ld\n", latest);
  int vfd = open(DATA_DIR "/saved-version.txt.part", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(vfd >= 0) {
    if(write(vfd, build_text, (size_t)bn) == bn) {
      close(vfd);
      rename(DATA_DIR "/saved-version.txt.part", DATA_DIR "/saved-version.txt");
    } else {
      close(vfd);
    }
  }
  pthread_mutex_lock(&st.mutex);
  st.elf_saved = 1;
  pthread_mutex_unlock(&st.mutex);

  if(folder_copy) {
    pthread_mutex_lock(&st.mutex);
    snprintf(st.app_status, sizeof(st.app_status), "%s", "folder-copy");
    st.app_path[0] = 0;
    pthread_mutex_unlock(&st.mutex);
  } else {
    char dest[400];
    snprintf(dest, sizeof(dest), "%s/" APP_FILE, app_dir);
    if(rename(app_part, dest) != 0) {
      int saved_errno = errno;
      if(!copy_over(app_part, dest)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot replace %s: %s", dest, strerror(saved_errno));
        unlink(app_part);
        fail(msg);
        return -1;
      }
      unlink(app_part);
    }
    pthread_mutex_lock(&st.mutex);
    snprintf(st.app_status, sizeof(st.app_status), "%s", existing ? "replaced" : "created");
    snprintf(st.app_path, sizeof(st.app_path), "%s", dest);
    pthread_mutex_unlock(&st.mutex);
  }
  pthread_mutex_lock(&st.mutex);
  st.error[0] = 0;
  snprintf(st.phase, sizeof(st.phase), "%s", "installed");
  pthread_mutex_unlock(&st.mutex);
  gc_log("update: installed build=%ld app=%s", latest, st.app_status);
  return 0;
}

static void *
worker(void *arg) {
  int install = arg != NULL;
  if(install) do_install(); else do_check();
  pthread_mutex_lock(&st.mutex);
  st.busy = 0;
  st.cancel = 0;
  pthread_mutex_unlock(&st.mutex);
  return NULL;
}

static int
start(int install) {
  pthread_mutex_lock(&st.mutex);
  if(st.busy) {
    pthread_mutex_unlock(&st.mutex);
    return -1;
  }
  if(!st.curl_ready) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    st.curl_ready = 1;
  }
  st.busy = 1;
  st.cancel = 0;
  st.error[0] = 0;
  snprintf(st.phase, sizeof(st.phase), "%s", "checking");
  pthread_mutex_unlock(&st.mutex);
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1u << 20);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&thread, &attr, worker, install ? (void *)1 : NULL);
  pthread_attr_destroy(&attr);
  if(rc != 0) {
    pthread_mutex_lock(&st.mutex);
    st.busy = 0;
    snprintf(st.phase, sizeof(st.phase), "%s", "idle");
    pthread_mutex_unlock(&st.mutex);
    return -1;
  }
  return 0;
}

int
gc_update_status_request(const http_request_t *req) {
  char tag[200], ver[140], err[560], path[620], file[100];
  char body[2048];
  pthread_mutex_lock(&st.mutex);
  gc_json_escape_small(tag, sizeof(tag), st.tag);
  gc_json_escape_small(ver, sizeof(ver), st.latest_version);
  gc_json_escape_small(err, sizeof(err), st.error);
  gc_json_escape_small(path, sizeof(path), st.app_path);
  gc_json_escape_small(file, sizeof(file), st.file);
  int n = snprintf(body, sizeof(body),
    "{\"ok\":true,\"busy\":%s,\"phase\":\"%s\",\"error\":\"%s\","
    "\"currentBuild\":%d,\"currentVersion\":\"%s\","
    "\"latestBuild\":%ld,\"latestVersion\":\"%s\",\"tag\":\"%s\","
    "\"available\":%s,\"file\":\"%s\",\"received\":%lld,\"total\":%lld,"
    "\"checkedAt\":%lld,\"appStatus\":\"%s\",\"appPath\":\"%s\",\"elfSaved\":%s}",
    st.busy ? "true" : "false", st.phase, err, (int)GC_BUILD_NUMBER,
    GAME_COMPRESSOR_VERSION, st.latest_build, ver, tag,
    st.latest_build > GC_BUILD_NUMBER ? "true" : "false", file,
    (long long)st.received, (long long)st.total, (long long)st.checked_at,
    st.app_status[0] ? st.app_status : "none", path,
    st.elf_saved ? "true" : "false");
  pthread_mutex_unlock(&st.mutex);
  if(n < 0 || (size_t)n >= sizeof(body)) {
    return websrv_send_error_json(req->fd, 500, "status too large");
  }
  return websrv_send(req->fd, 200, "application/json", body, (size_t)n);
}

int
gc_update_action_request(const http_request_t *req) {
  int install = !strcmp(req->path, "/api/gc/update/install");
  if(!install && strcmp(req->path, "/api/gc/update/check")) {
    return websrv_send_error_json(req->fd, 404, "not found");
  }
  if(start(install) != 0) {
    return websrv_send_error_json(req->fd, 409, "an update task is already running");
  }
  return websrv_send(req->fd, 202, "application/json", "{\"ok\":true,\"started\":true}", 26);
}
