/*
 * Experimental launcher eboot for Game Compressor.
 *
 * A home-screen tile that has a real eboot.bin (instead of only a deeplinkUri)
 * gets a process of its own when opened. This eboot is intentionally tiny and
 * freestanding: it uses raw FreeBSD syscalls only (no libc, no dynamic
 * linking), connects to the Game Compressor payload on 127.0.0.1 and asks it
 * to open the web UI in the full system web browser, then exits.
 *
 * STATUS: builds, but is untested on a console.
 */

#ifndef GC_PORT
#define GC_PORT 5910
#endif

#define SYS_exit    1
#define SYS_read    3
#define SYS_write   4
#define SYS_close   6
#define SYS_socket  97
#define SYS_connect 98

#define AF_INET     2
#define SOCK_STREAM 1

struct gc_sockaddr_in {
  unsigned char  sin_len;
  unsigned char  sin_family;
  unsigned short sin_port;   /* network byte order */
  unsigned int   sin_addr;   /* network byte order */
  char           sin_zero[8];
};

static long
gc_syscall(long n, long a1, long a2, long a3) {
  unsigned long ret;
  char iserror;

  __asm__ __volatile__("syscall"
                       : "=a"(ret), "=@ccc"(iserror)
                       : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                       : "rcx", "r11", "memory");
  return iserror ? -(long)ret : (long)ret;
}

static unsigned
gc_len(const char *s) {
  unsigned n = 0;
  while(s[n]) n++;
  return n;
}

int
main(void) {
  static const char request[] =
      "GET /api/control/open-browser HTTP/1.0\r\n"
      "Host: 127.0.0.1\r\n"
      "Connection: close\r\n"
      "\r\n";
  struct gc_sockaddr_in addr;
  char buf[128];
  long fd, rc;
  unsigned i;

  for(i = 0; i < sizeof(addr); i++) ((char *)&addr)[i] = 0;
  addr.sin_len = sizeof(addr);
  addr.sin_family = AF_INET;
  addr.sin_port = (unsigned short)(((GC_PORT & 0xff) << 8) |
                                   ((GC_PORT >> 8) & 0xff));
  addr.sin_addr = 0x0100007fu; /* 127.0.0.1, network byte order */

  fd = gc_syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
  if(fd < 0) return 1;

  rc = gc_syscall(SYS_connect, fd, (long)&addr, sizeof(addr));
  if(rc < 0) {
    gc_syscall(SYS_close, fd, 0, 0);
    return 2;
  }

  rc = gc_syscall(SYS_write, fd, (long)request, gc_len(request));
  if(rc > 0) {
    gc_syscall(SYS_read, fd, (long)buf, sizeof(buf));
  }
  gc_syscall(SYS_close, fd, 0, 0);
  return rc > 0 ? 0 : 3;
}

void
_start(void) {
  gc_syscall(SYS_exit, main(), 0, 0);
  __builtin_trap();
}
