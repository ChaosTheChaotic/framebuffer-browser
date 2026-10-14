#define _GNU_SOURCE
#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif
#define _FORTIFY_SOURCE 0

#include <dlfcn.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/major.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

static pthread_once_t env_once = PTHREAD_ONCE_INIT;
static int disabled;
static int have_speed;
static double speed;
static double evdev_factor = 1.0;
static double residual[2];
static double abs_residual[64];
static int abs_last_raw[64];
static int abs_last_sent[64];
static int abs_has_last[64];
static int evdev_reported;

static int debug_enabled(void) {
  const char *d = getenv("QT_QPA_EGLFS_DEBUG");
  return d && *d && strcmp(d, "0") != 0;
}

static void parse_env(void) {
  const char *s = getenv("FBBROWSER_POINTER_SPEED");
  if (s && *s) {
    char *end;
    double v = strtod(s, &end);
    if (*end != '\0') {
      fprintf(stderr,
              "fbbrowser: ignoring invalid FBBROWSER_POINTER_SPEED '%s'\n", s);
    } else {
      if (v < -1.0)
        v = -1.0;
      if (v > 1.0)
        v = 1.0;
      speed = v;
      have_speed = 1;
      evdev_factor = 1.0 + v;
      if (evdev_factor < 0.01)
        evdev_factor = 0.01;
    }
  }
}

static void ensure_env(void) { pthread_once(&env_once, parse_env); }

static int is_chromium_child(void) {
  char buf[8192];
  size_t n, i;
  FILE *f = fopen("/proc/self/cmdline", "r");
  if (!f)
    return 0;
  n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = '\0';
  for (i = 0; i < n; i += strlen(buf + i) + 1)
    if (!strncmp(buf + i, "--type=", 7))
      return 1;
  return 0;
}

__attribute__((constructor)) static void shim_init(void) {
  disabled = is_chromium_child();
}

static void scale_events(struct input_event *ev, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (ev[i].type == EV_REL) {
      int axis;
      if (ev[i].code == REL_X)
        axis = 0;
      else if (ev[i].code == REL_Y)
        axis = 1;
      else
        continue;

      double x = residual[axis] + (double)ev[i].value * evdev_factor;
      int out = (int)x;
      residual[axis] = x - out;
      ev[i].value = out;
    } else if (ev[i].type == EV_ABS) {
      int code = ev[i].code;
      if (code < 0 || code >= 64)
        continue;
      if (code != ABS_X && code != ABS_Y && code != ABS_MT_POSITION_X &&
          code != ABS_MT_POSITION_Y)
        continue;

      if (!abs_has_last[code]) {
        abs_last_raw[code] = ev[i].value;
        abs_last_sent[code] = ev[i].value;
        abs_has_last[code] = 1;
        continue;
      }

      int raw_delta = ev[i].value - abs_last_raw[code];
      abs_last_raw[code] = ev[i].value;

      double x = abs_residual[code] + (double)raw_delta * evdev_factor;
      int scaled_delta = (int)x;
      abs_residual[code] = x - scaled_delta;

      abs_last_sent[code] += scaled_delta;
      ev[i].value = abs_last_sent[code];
    }
  }
}

static int is_pointer_device(int fd) {
#ifdef FBB_TEST_ASSUME_MOUSE
  (void)fd;
  return 1;
#else
  struct stat st;
  unsigned long rel = 0;
  unsigned long abs_bits = 0;

  if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode))
    return 0;
  if (major(st.st_rdev) != INPUT_MAJOR || minor(st.st_rdev) < 64)
    return 0;
  if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), &rel) >= 0) {
    if ((rel & (1UL << REL_X)) && (rel & (1UL << REL_Y)))
      return 1;
  }
  if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_bits), &abs_bits) >= 0) {
    if ((abs_bits & (1UL << ABS_X)) && (abs_bits & (1UL << ABS_Y)))
      return 1;
    if ((abs_bits & (1UL << ABS_MT_POSITION_X)) &&
        (abs_bits & (1UL << ABS_MT_POSITION_Y)))
      return 1;
  }
  return 0;
#endif
}

static void filter_read(int fd, void *buf, ssize_t len) {
  int saved_errno = errno;
  if (disabled || len <= 0 || (size_t)len % sizeof(struct input_event) != 0)
    return;
  ensure_env();
  if (evdev_factor == 1.0)
    return;
  if (is_pointer_device(fd)) {
    if (!evdev_reported) {
      evdev_reported = 1;
      if (debug_enabled())
        fprintf(stderr,
                "fbbrowser: pointer shim scaling evdev pointer/trackpad motion "
                "by %.3f\n",
                evdev_factor);
    }
    scale_events(buf, (size_t)len / sizeof(struct input_event));
  }
  errno = saved_errno;
}

typedef ssize_t (*read_fn)(int, void *, size_t);
typedef ssize_t (*read_chk_fn)(int, void *, size_t, size_t);
static read_fn real_read;
static read_chk_fn real_read_chk;

ssize_t read(int fd, void *buf, size_t count) {
  ssize_t r;
  if (!real_read) {
    real_read = (read_fn)dlsym(RTLD_NEXT, "read");
    if (!real_read) {
      errno = ENOSYS;
      return -1;
    }
  }
  r = real_read(fd, buf, count);
  if (r > 0)
    filter_read(fd, buf, r);
  return r;
}

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buflen) {
  ssize_t r;
  if (!real_read_chk) {
    real_read_chk = (read_chk_fn)dlsym(RTLD_NEXT, "__read_chk");
    if (!real_read_chk) {
      errno = ENOSYS;
      return -1;
    }
  }
  r = real_read_chk(fd, buf, count, buflen);
  if (r > 0)
    filter_read(fd, buf, r);
  return r;
}
