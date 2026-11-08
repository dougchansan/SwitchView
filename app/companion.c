#include "companion.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct companion {
  char cmd[1024];
  int columns;
  pthread_t th;
  atomic_int quit;
  pid_t pid;
  pthread_mutex_t lock;
  struct comp_line lines[COMP_LINES];
  int head, count;
  atomic_uint seq;
  // parser state carried across reads
  int color, bold;
};

// Append one raw line (with ANSI escapes) as segments.
static void add_line(struct companion *c, const char *raw)
{
  struct comp_line L;
  memset(&L, 0, sizeof L);
  size_t n = 0;
  int color = c->color, bold = c->bold;
  L.seg[0].color = (uint8_t)color;
  L.seg[0].bold = (uint8_t)bold;
  L.nseg = 1;
  for (const char *p = raw; *p && n + 1 < sizeof L.text;) {
    if (*p == 27 && p[1] == '[') {
      const char *q = p + 2;
      int codes[8], nc = 0, v = 0, have = 0;
      while (*q && *q != 'm' && (*q == ';' || (*q >= '0' && *q <= '9'))) {
        if (*q == ';') { if (nc < 8) codes[nc++] = have ? v : 0; v = 0; have = 0; }
        else { v = v * 10 + (*q - '0'); have = 1; }
        q++;
      }
      if (*q != 'm') { p = *q ? q + 1 : q; continue; }  // non-colour escape: drop
      if (nc < 8) codes[nc++] = have ? v : 0;
      for (int i = 0; i < nc; i++) {
        int k = codes[i];
        if (k == 0) { color = CC_DEFAULT; bold = 0; }
        else if (k == 1) bold = 1;
        else if (k == 2 || k == 90) color = CC_GREY;
        else if (k == 31) color = CC_RED;
        else if (k == 32) color = CC_GREEN;
        else if (k == 33) color = CC_YELLOW;
        else if (k == 34) color = CC_BLUE;
        else if (k == 35) color = CC_MAGENTA;
        else if (k == 36) color = CC_CYAN;
        else if (k == 39) color = CC_DEFAULT;
      }
      p = q + 1;
      struct comp_seg *cur = &L.seg[L.nseg - 1];
      if (cur->len == 0) { cur->color = (uint8_t)color; cur->bold = (uint8_t)bold; }
      else if (L.nseg < COMP_SEGS && (cur->color != color || cur->bold != bold)) {
        struct comp_seg *s = &L.seg[L.nseg++];
        s->start = (uint8_t)(n > 255 ? 255 : n);
        s->len = 0;
        s->color = (uint8_t)color;
        s->bold = (uint8_t)bold;
      }
      continue;
    }
    unsigned char ch = (unsigned char)*p;
    if (ch == '\r' || ch == '\t') ch = ' ';
    if (ch < 0x20) { p++; continue; }
    L.text[n++] = (char)ch;
    if (L.seg[L.nseg - 1].len < 255) L.seg[L.nseg - 1].len++;
    p++;
  }
  L.text[n] = 0;
  c->color = color;
  c->bold = bold;
  pthread_mutex_lock(&c->lock);
  int idx = (c->head + c->count) % COMP_LINES;
  if (c->count == COMP_LINES) c->head = (c->head + 1) % COMP_LINES;
  else c->count++;
  c->lines[idx] = L;
  pthread_mutex_unlock(&c->lock);
  atomic_fetch_add(&c->seq, 1);
}

static pid_t spawn(struct companion *c, int *out_fd)
{
  int fds[2];
  if (pipe(fds) < 0) return -1;
  pid_t pid = fork();
  if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
  if (pid == 0) {
    setpgid(0, 0);  // own group, so stop kills tail/python children too
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[0]);
    close(fds[1]);
    char cols[16];
    snprintf(cols, sizeof cols, "%d", c->columns);
    setenv("COLUMNS", cols, 1);
    setenv("PYTHONUNBUFFERED", "1", 1);
    execl("/bin/sh", "sh", "-c", c->cmd, (char *)NULL);
    _exit(127);
  }
  close(fds[1]);
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
  *out_fd = fds[0];
  return pid;
}

static void *run(void *arg)
{
  struct companion *c = arg;
  char buf[4096], line[2048];
  size_t ln = 0;
  while (!atomic_load(&c->quit)) {
    int fd = -1;
    c->pid = spawn(c, &fd);
    if (c->pid < 0) { sleep(2); continue; }
    for (;;) {
      if (atomic_load(&c->quit)) break;
      struct pollfd pf = {.fd = fd, .events = POLLIN};
      int r = poll(&pf, 1, 200);
      if (r <= 0) continue;
      ssize_t k = read(fd, buf, sizeof buf);
      if (k == 0) break;  // watcher exited
      if (k < 0) { if (errno == EAGAIN || errno == EINTR) continue; break; }
      for (ssize_t i = 0; i < k; i++) {
        if (buf[i] == '\n') { line[ln] = 0; add_line(c, line); ln = 0; }
        else if (ln + 1 < sizeof line) line[ln++] = buf[i];
      }
    }
    close(fd);
    kill(-c->pid, SIGTERM);
    waitpid(c->pid, NULL, 0);
    c->pid = 0;
    if (!atomic_load(&c->quit)) {
      add_line(c, "\033[90m(watcher exited; restarting in 3 s)\033[0m");
      for (int i = 0; i < 30 && !atomic_load(&c->quit); i++) usleep(100000);
    }
  }
  return NULL;
}

struct companion *companion_start(const char *cmd, int columns)
{
  struct companion *c = calloc(1, sizeof *c);
  snprintf(c->cmd, sizeof c->cmd, "%s", cmd);
  c->columns = columns;
  pthread_mutex_init(&c->lock, NULL);
  if (pthread_create(&c->th, NULL, run, c) != 0) { free(c); return NULL; }
  return c;
}

void companion_stop(struct companion *c)
{
  if (!c) return;
  atomic_store(&c->quit, 1);
  if (c->pid > 0) kill(-c->pid, SIGTERM);
  pthread_join(c->th, NULL);
  pthread_mutex_destroy(&c->lock);
  free(c);
}

int companion_lines(struct companion *c, struct comp_line *out, int max)
{
  pthread_mutex_lock(&c->lock);
  int n = c->count < max ? c->count : max;
  int start = c->head + c->count - n;
  for (int i = 0; i < n; i++) out[i] = c->lines[(start + i) % COMP_LINES];
  pthread_mutex_unlock(&c->lock);
  return n;
}

unsigned companion_seq(struct companion *c) { return atomic_load(&c->seq); }
const char *companion_command(struct companion *c) { return c->cmd; }
