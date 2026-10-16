/*
 * Game Compressor - self-update from GitHub releases. See gc_update.h.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>
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
#define LATEST_BASE "https://github.com/" REPO "/releases/latest/download/"
#define ATOM_FEED "https://github.com/" REPO "/releases.atom"
#define API_NEWEST "https://api.github.com/repos/" REPO "/releases?per_page=1"
#define DOWNLOAD_BASE "https://github.com/" REPO "/releases/download/"
#define API_BASE "https://api.github.com/repos/" REPO "/"
#define APP_FILE "PPSA99290.ffpkg"
#define ZIP_FILE "PPSA99290-app.zip"
#define MAX_ZIP (160LL << 20)
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
         !strcmp(url, ATOM_FEED) ||
         !strncmp(url, LATEST_BASE, strlen(LATEST_BASE)) ||
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
  if(!strcmp(tag, "latest")) snprintf(out, size, LATEST_BASE "%s", name);
  else snprintf(out, size, DOWNLOAD_BASE "%s/%s", tag, name);
}

static int
do_check(void) {
  char err[200], err2[200] = "", err3[200] = "";
  char tag[96] = {0};
  int ok = 0;
  sink_t sink = {.fd = -1, .limit = 1 << 20};
  /* 0. the "latest" release alias on github.com: same host as the downloads,
   *    no API, no rate limit. */
  {
    char probe_url[300];
    char probe_err[200];
    sink_t probe = {.fd = -1, .limit = 16384};
    build_url(probe_url, sizeof(probe_url), "latest", "build-info.json");
    if(http_get(probe_url, &probe, probe_err, sizeof(probe_err)) == 0 && probe.data &&
       strstr(probe.data, "\"build\"")) {
      snprintf(tag, sizeof(tag), "latest");
      ok = 1;
    }
    free(probe.data);
  }
  /* 1. newest release of any kind through the API (branch builds are
   *    pre-releases, so /releases/latest alone is not enough). */
  if(!ok && http_get(API_NEWEST, &sink, err, sizeof(err)) == 0) {
    ok = sink.data && json_find(sink.data, "tag_name", tag, sizeof(tag)) == 0 && tag_safe(tag);
  }
  free(sink.data);
  /* 2. the public releases feed: not subject to the API rate limit. */
  if(!ok) {
    sink = (sink_t){.fd = -1, .limit = 1 << 20};
    if(http_get(ATOM_FEED, &sink, err2, sizeof(err2)) == 0 && sink.data) {
      const char *m = strstr(sink.data, "/releases/tag/");
      if(m) {
        m += strlen("/releases/tag/");
        size_t n = 0;
        while(m[n] && m[n] != '"' && m[n] != '<' && n + 1 < sizeof(tag)) { tag[n] = m[n]; n++; }
        tag[n] = 0;
        ok = tag_safe(tag);
      }
    }
    free(sink.data);
  }
  /* 3. last resort: the latest stable release. */
  if(!ok) {
    sink = (sink_t){.fd = -1, .limit = 1 << 20};
    if(http_get(API_LATEST, &sink, err3, sizeof(err3)) == 0)
      ok = sink.data && json_find(sink.data, "tag_name", tag, sizeof(tag)) == 0 && tag_safe(tag);
    free(sink.data);
  }
  if(!ok) {
    char msg[256];
    snprintf(msg, sizeof(msg), "no release found. API: %s | feed: %s | stable: %s",
             err[0] ? err : "bad answer", err2[0] ? err2 : "bad answer", err3[0] ? err3 : "-");
    fail(msg);
    return -1;
  }

  char url[300];
  build_url(url, sizeof(url), tag, "build-info.json");
  sink_t info = {.fd = -1, .limit = 16384};
  if(http_get(url, &info, err, sizeof(err)) != 0) {
    free(info.data);
    char msg[256];
    snprintf(msg, sizeof(msg), "%s: %s", tag, err);
    fail(msg);
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
    if(!*folder_copy && !stat(p, &s)) {
      *folder_copy = 1;
      snprintf(dir, size, "%s", k_app_dirs[i]);
    }
    snprintf(p, sizeof(p), "%s/" APP_FILE, k_app_dirs[i]);
    if(!*existing && !stat(p, &s) && S_ISREG(s.st_mode)) {
      *existing = 1;
      if(!*folder_copy) snprintf(dir, size, "%s", k_app_dirs[i]);
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


static void
mkdir_p(char *path) {
  for(char *c = path + 1; *c; c++) {
    if(*c != '/') continue;
    *c = 0;
    mkdir(path, 0755);
    *c = '/';
  }
}

static uint32_t rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

static int
file_crc_matches(const char *path, uint32_t size, uint32_t crc) {
  int fd = open(path, O_RDONLY);
  if(fd < 0) return 0;
  struct stat sb;
  int ok = 0;
  if(fstat(fd, &sb) == 0 && (uint64_t)sb.st_size == size) {
    static __thread unsigned char buf[1 << 15];
    uLong c = crc32(0, Z_NULL, 0);
    ssize_t n;
    while((n = read(fd, buf, sizeof(buf))) > 0) c = crc32(c, buf, (uInt)n);
    ok = n == 0 && (uint32_t)c == crc;
  }
  close(fd);
  return ok;
}

/* Extracts the release zip (PPSA99290/...) into <app_dir>. Each file is
 * written under a temporary name and renamed into place. Returns 0 on success. */
static int
extract_app_zip(const char *zip_path, const char *app_dir, char *err, size_t err_size,
                int *wrote, int *same) {
  int fd = open(zip_path, O_RDONLY);
  if(fd < 0) { snprintf(err, err_size, "cannot open %s: %s", zip_path, strerror(errno)); return -1; }
  off_t size = lseek(fd, 0, SEEK_END);
  unsigned char *z = size > 22 ? malloc((size_t)size) : NULL;
  if(!z || pread(fd, z, (size_t)size, 0) != size) {
    close(fd); free(z);
    snprintf(err, err_size, "cannot read the app zip");
    return -1;
  }
  close(fd);
  int rc = -1;
  unsigned char *iobuf = NULL;
  long eocd = -1;
  for(long i = (long)size - 22; i >= 0 && i > (long)size - 70000; i--) {
    if(rd32(z + i) == 0x06054b50u) { eocd = i; break; }
  }
  if(eocd < 0) { snprintf(err, err_size, "the app zip is damaged"); goto done; }
  unsigned count = rd16(z + eocd + 10);
  uint32_t pos = rd32(z + eocd + 16);
  iobuf = malloc(1 << 16);
  if(!iobuf) { snprintf(err, err_size, "out of memory"); goto done; }
  const uint32_t cd_start = pos;
  for(int pass = 0; pass < 2; pass++) {
  pos = cd_start;
  for(unsigned n = 0; n < count; n++) {
    if((int64_t)pos + 46 > size || rd32(z + pos) != 0x02014b50u) {
      snprintf(err, err_size, "the app zip is damaged"); goto done;
    }
    unsigned method = rd16(z + pos + 10);
    uint32_t crc = rd32(z + pos + 16), csize = rd32(z + pos + 20), usize = rd32(z + pos + 24);
    unsigned nlen = rd16(z + pos + 28), elen = rd16(z + pos + 30), clen = rd16(z + pos + 32);
    uint32_t lho = rd32(z + pos + 42);
    char name[256];
    if(nlen == 0 || nlen >= sizeof(name) || (int64_t)pos + 46 + nlen > size) {
      snprintf(err, err_size, "bad name in the app zip"); goto done;
    }
    memcpy(name, z + pos + 46, nlen);
    name[nlen] = 0;
    pos += 46 + nlen + elen + clen;
    if(strncmp(name, "PPSA99290/", 10) != 0 || strstr(name, "..") || name[nlen - 1] == '/') continue;
    /* eboot.bin goes last: it is the file the running app keeps busy. */
    int is_eboot = strcmp(name, "PPSA99290/eboot.bin") == 0;
    if((pass == 1) != is_eboot) continue;
    if((int64_t)lho + 30 > size || rd32(z + lho) != 0x04034b50u) {
      snprintf(err, err_size, "the app zip is damaged"); goto done;
    }
    int64_t data = (int64_t)lho + 30 + rd16(z + lho + 26) + rd16(z + lho + 28);
    if(data + csize > size || (method != 0 && method != 8)) {
      snprintf(err, err_size, "unsupported entry %s", name); goto done;
    }
    char dest[460], tmp[470];
    snprintf(dest, sizeof(dest), "%s/%s", app_dir, name);
    snprintf(tmp, sizeof(tmp), "%s.new", dest);
    mkdir_p(dest);
    if(file_crc_matches(dest, usize, crc)) { if(same) (*same)++; continue; } /* already up to date */
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(out < 0) { snprintf(err, err_size, "cannot write %s: %s", dest, strerror(errno)); goto done; }
    uLong crc_now = crc32(0, Z_NULL, 0);
    int ok = 1;
    if(method == 0) {
      ok = write(out, z + data, csize) == (ssize_t)csize;
      crc_now = crc32(crc_now, z + data, csize);
    } else {
      z_stream zs;
      memset(&zs, 0, sizeof(zs));
      if(inflateInit2(&zs, -15) != Z_OK) ok = 0;
      else {
        zs.next_in = z + data;
        zs.avail_in = csize;
        int r;
        do {
          zs.next_out = iobuf;
          zs.avail_out = sizeof(iobuf);
          r = inflate(&zs, Z_NO_FLUSH);
          size_t got = sizeof(iobuf) - zs.avail_out;
          if(got) {
            crc_now = crc32(crc_now, iobuf, (uInt)got);
            if(write(out, iobuf, got) != (ssize_t)got) { ok = 0; break; }
          }
        } while(r == Z_OK);
        if(r != Z_STREAM_END) ok = 0;
        if(zs.total_out != usize) ok = 0;
        inflateEnd(&zs);
      }
    }
    int werr = errno;
    if(ok) fsync(out);
    close(out);
    if(!ok || (uint32_t)crc_now != crc) {
      unlink(tmp);
      snprintf(err, err_size, "cannot write %s: %s", dest, ok ? "checksum mismatch" : strerror(werr));
      goto done;
    }
    if(rename(tmp, dest) != 0) {
      int e = errno;
      /* Some file systems refuse to replace a file in use: remove, then rename. */
      if(!(unlink(dest) == 0 && rename(tmp, dest) == 0)) {
        snprintf(err, err_size, "cannot replace %s: %s", dest, strerror(e));
        unlink(tmp);
        rc = 1; /* busy: the app is probably still running */
        goto done;
      }
    }
    if(wrote) (*wrote)++;
  }
  }
  rc = 0;
done:
  free(iobuf);
  free(z);
  return rc;
}

#define NOTE_FILE DATA_DIR "/app-update.txt"

/* One persistent line about the last app-files update (survives payload
 * restarts); shown in the TV app's Settings. */
static void
note_set(const char *fmt, ...) {
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  char stamp[32] = "";
  time_t now = time(NULL);
  struct tm tm;
  if(gmtime_r(&now, &tm)) strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M UTC", &tm);
  char out[460];
  int n = snprintf(out, sizeof(out), "%s - %s\n", stamp, line);
  int fd = open(NOTE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd >= 0) {
    if(n > 0 && write(fd, out, (size_t)n) < 0) { /* best effort */ }
    close(fd);
  }
  gc_log("update note: %s", line);
}

#define PENDING_ZIP DATA_DIR "/pending-app.zip"

/* Applies the zip to every app folder found. 0 ok, 1 busy (some file is in
 * use), -1 failed. updated lists the folders; err the last problem. */
static int
apply_zip_everywhere(const char *zip_path, char *updated, size_t updated_size, char *err,
                     size_t err_size, char *info, size_t info_size) {
  int copies = 0, rc = 0, wrote = 0, same = 0;
  char param_version[40] = "?";
  updated[0] = 0;
  info[0] = 0;
  for(int i = 0; k_app_dirs[i]; i++) {
    char probe[400];
    struct stat ps;
    snprintf(probe, sizeof(probe), "%s/PPSA99290/eboot.bin", k_app_dirs[i]);
    if(stat(probe, &ps) != 0) continue;
    char root[320];
    snprintf(root, sizeof(root), "%s", k_app_dirs[i]);
    int r = extract_app_zip(zip_path, root, err, err_size, &wrote, &same);
    if(r == 0) {
      copies++;
      snprintf(probe, sizeof(probe), "%s/PPSA99290/sce_sys/param.json", root);
      int pfd = open(probe, O_RDONLY);
      if(pfd >= 0) {
        char pj[2048] = {0};
        ssize_t pn = read(pfd, pj, sizeof(pj) - 1);
        close(pfd);
        if(pn > 0) json_find(pj, "contentVersion", param_version, sizeof(param_version));
      }
      size_t len = strlen(updated);
      snprintf(updated + len, updated_size - len, "%s%s/PPSA99290", len ? ", " : "", k_app_dirs[i]);
      gc_log("update: app folder updated: %s", root);
    } else {
      rc = r;
      break;
    }
  }
  if(rc == 0 && copies == 0) {
    snprintf(err, err_size, "no app folder found");
    return -1;
  }
  snprintf(info, info_size, "%d written, %d unchanged, param.json now %s", wrote, same,
           param_version);
  return rc;
}

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
static int pending_running;

static void *
pending_worker(void *arg) {
  (void)arg;
  for(int attempt = 0; attempt < 900; attempt++) { /* about an hour */
    sleep(4);
    if(access(PENDING_ZIP, F_OK) != 0) break;
    char updated[300], err[240] = "", info[120];
    int r = apply_zip_everywhere(PENDING_ZIP, updated, sizeof(updated), err, sizeof(err), info,
                                 sizeof(info));
    if(r == 1) continue; /* the app is still open */
    if(r == 0) {
      gc_log("update: pending app update applied: %s", updated);
      note_set("applied after the app closed: %s (%s)", updated, info);
      pthread_mutex_lock(&st.mutex);
      snprintf(st.app_status, sizeof(st.app_status), "%s", "folder-updated");
      snprintf(st.app_path, sizeof(st.app_path), "%s", updated);
      pthread_mutex_unlock(&st.mutex);
    } else {
      gc_log("update: pending app update failed: %s", err);
      note_set("failed: %s", err);
    }
    unlink(PENDING_ZIP);
    break;
  }
  pthread_mutex_lock(&pending_lock);
  pending_running = 0;
  pthread_mutex_unlock(&pending_lock);
  return NULL;
}

static void
pending_start(void) {
  pthread_mutex_lock(&pending_lock);
  if(!pending_running) {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if(pthread_create(&t, &attr, pending_worker, NULL) == 0) pending_running = 1;
    pthread_attr_destroy(&attr);
  }
  pthread_mutex_unlock(&pending_lock);
}

/* Called when the payload starts: finish an app update left waiting. */
void
gc_update_start(void) {
  if(access(PENDING_ZIP, F_OK) == 0) pending_start();
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
  char elf_hash[65], app_hash[65], zip_hash[65];
  int have_elf = sums_lookup(sums.data, ELF_FILE, elf_hash) == 0;
  int have_app = sums_lookup(sums.data, APP_FILE, app_hash) == 0;
  int have_zip = sums_lookup(sums.data, ZIP_FILE, zip_hash) == 0;
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
    /* App run from a folder (ShadowMount+ on /data/homebrew): replace its
     * files from the release zip. The payload is already saved above, so a
     * failure here only means the interface itself must be updated by hand. */
    char zip_part[400], zerr[240] = "app zip missing in the release";
    snprintf(zip_part, sizeof(zip_part), "%s/" ZIP_FILE ".part", DATA_DIR);
    int zrc = -1;
    if(have_zip && free_space_ok(DATA_DIR, 80LL << 20) &&
       download_verified(tag, ZIP_FILE, zip_hash, zip_part, MAX_ZIP) == 0) {
      set_phase("installing");
      char updated[300] = "", info[120] = "";
      zrc = apply_zip_everywhere(zip_part, updated, sizeof(updated), zerr, sizeof(zerr), info,
                                 sizeof(info));
      if(zrc == 0) note_set("updated %s (%s)", updated, info);
      else if(zrc == 1) note_set("waiting for the app to close: %s", zerr);
      else note_set("failed: %s", zerr);
      snprintf(app_dir, sizeof(app_dir), "%s", updated);
      if(zrc == 1) {
        /* The running app keeps eboot.bin busy. Keep the zip and apply it as
         * soon as the app is closed (a background retry, also resumed when
         * the payload restarts). */
        if(rename(zip_part, PENDING_ZIP) == 0) {
          pending_start();
          pthread_mutex_lock(&st.mutex);
          snprintf(st.app_status, sizeof(st.app_status), "%s", "folder-pending");
          snprintf(st.app_path, sizeof(st.app_path), "%s", zerr);
          st.error[0] = 0;
          snprintf(st.phase, sizeof(st.phase), "%s", "installed");
          pthread_mutex_unlock(&st.mutex);
          gc_log("update: app files busy, will retry when the app closes: %s", zerr);
          return 0;
        }
      }
      unlink(zip_part);
    }
    if(zrc != 0) {
      gc_log("update: app folder not updated: %s", zerr);
      pthread_mutex_lock(&st.mutex);
      snprintf(st.app_status, sizeof(st.app_status), "%s", "folder-copy");
      st.app_path[0] = 0;
      pthread_mutex_unlock(&st.mutex);
      fail(zerr); /* the payload is saved; tell why the interface was not */
      return -1;
    }
    pthread_mutex_lock(&st.mutex);
    snprintf(st.app_status, sizeof(st.app_status), "%s", "folder-updated");
    snprintf(st.app_path, sizeof(st.app_path), "%s", app_dir);
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
  char tag[200], ver[140], err[560], path[620], file[100], note[1000] = "";
  char body[3072];
  {
    char raw[420] = {0};
    int nfd = open(NOTE_FILE, O_RDONLY);
    if(nfd >= 0) {
      ssize_t rn = read(nfd, raw, sizeof(raw) - 1);
      close(nfd);
      if(rn > 0) {
        for(char *c = raw; *c; c++) if(*c == '\n' || *c == '\r') *c = 0;
        gc_json_escape_small(note, sizeof(note), raw);
      }
    }
  }
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
    "\"checkedAt\":%lld,\"appStatus\":\"%s\",\"appPath\":\"%s\",\"elfSaved\":%s,\"appNote\":\"%s\"}",
    st.busy ? "true" : "false", st.phase, err, (int)GC_BUILD_NUMBER,
    GAME_COMPRESSOR_VERSION, st.latest_build, ver, tag,
    st.latest_build > GC_BUILD_NUMBER ? "true" : "false", file,
    (long long)st.received, (long long)st.total, (long long)st.checked_at,
    st.app_status[0] ? st.app_status : "none", path,
    st.elf_saved ? "true" : "false", note);
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
