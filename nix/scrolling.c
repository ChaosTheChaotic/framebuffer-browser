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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef REL_WHEEL_HI_RES
#define REL_WHEEL_HI_RES 0x0b
#endif
#ifndef REL_HWHEEL_HI_RES
#define REL_HWHEEL_HI_RES 0x0c
#endif

#define MAX_SLOTS 32
#define MAX_DEVS 8
#define MAX_EVENTS 512
#define FRAME_BUF 64
#define NAME_LEN 256
#define MAX_CLAIMS 16

struct slot_state {
  int active;
  int pos_x;
  int pos_y;
};

struct dev_state {
  int fd, used;
  dev_t rdev;
  int can_count, has_mt, has_legacy;
  int has_wheel, has_hwheel, has_hi_res;

  int cur_slot;
  struct slot_state slots[MAX_SLOTS];
  int mt_fingers;

  int bt_finger, bt_touch, bt_double, bt_triple, bt_quad;

  int driver_scroll_known, driver_scroll;
  double scroll_accum;
  int last_x, last_y, has_last_xy;

  int prev_cnt;
  int jmp_max;

  int abs_y_min, abs_y_max;
  double threshold;

  int synth_active;
  int name_claim_checked;
  char name[NAME_LEN];
};

static struct dev_state g_devs[MAX_DEVS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_disabled, g_debug, g_verbose;
static int g_natural = 1;
static int g_mode = 0;     // 0=auto, 1=synth, 2=off
static int g_strip = 1;    // filter events to match faked caps
static int g_claim = 1;    // lazy name-claiming dedupe
static int g_force_fd = 0; // if >0, only that fd is active
static double g_threshold = 20.0;
static double g_threshold_frac = 0.01;

struct name_claim {
  char name[NAME_LEN];
  int fd;
};
static struct name_claim g_claims[MAX_CLAIMS];
static int g_claim_count;

static int raw_ioctl(int fd, unsigned long req, void *arg) {
  return (int)syscall(SYS_ioctl, fd, req, arg);
}

static int test_bit(int bit, const unsigned long *arr, size_t bytes) {
  size_t w = bit / (8 * sizeof(unsigned long));
  if (w * sizeof(unsigned long) >= bytes)
    return 0;
  return (arr[w] >> (bit % (8 * sizeof(unsigned long)))) & 1;
}

static int has_mt_caps_raw(int fd) {
  unsigned long absbits[(ABS_MAX / (8 * sizeof(unsigned long))) + 2] = {0};
  if (raw_ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0)
    return 0;
  return test_bit(ABS_MT_POSITION_X, absbits, sizeof absbits) &&
         test_bit(ABS_MT_POSITION_Y, absbits, sizeof absbits);
}

static int is_wheel_code(int c) {
  return c == REL_WHEEL || c == REL_HWHEEL || c == REL_WHEEL_HI_RES ||
         c == REL_HWHEEL_HI_RES;
}

static int is_touch_key(int code) {
  switch (code) {
  case BTN_TOUCH:
  case BTN_TOOL_FINGER:
  case BTN_TOOL_DOUBLETAP:
  case BTN_TOOL_TRIPLETAP:
  case BTN_TOOL_QUADTAP:
  case BTN_TOOL_QUINTTAP:
    return 1;
  }
  return 0;
}

static int on_main_thread(void) {
  return (pid_t)syscall(SYS_gettid) == getpid();
}

static int is_input_char(int fd) {
  struct stat st;
  return fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) &&
         major(st.st_rdev) == INPUT_MAJOR && minor(st.st_rdev) >= 64;
}

static void log_msg(const char *fmt, ...) {
  if (!g_debug)
    return;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "fbbrowser-scroll: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

static int is_chromium_child(void) {
  char buf[8192];
  FILE *f = fopen("/proc/self/cmdline", "r");
  if (!f)
    return 0;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  for (size_t i = 0; i < n; i += strlen(buf + i) + 1)
    if (!strncmp(buf + i, "--type=", 7))
      return 1;
  return 0;
}

__attribute__((constructor)) static void scroll_init(void) {
  g_disabled = is_chromium_child();
  const char *d = getenv("QT_QPA_EGLFS_DEBUG");
  g_debug = d && *d && strcmp(d, "0") != 0;
  g_verbose =
      getenv("FBBROWSER_SCROLL_DEBUG") && *getenv("FBBROWSER_SCROLL_DEBUG");

  const char *t = getenv("FBBROWSER_SCROLL_THRESHOLD");
  if (t && *t) {
    char *e;
    double v = strtod(t, &e);
    if (*e == 0 && v > 0) {
      g_threshold = v;
      g_threshold_frac = 0;
    }
  }
  const char *nat = getenv("FBBROWSER_SCROLL_NATURAL");
  if (nat && strcmp(nat, "0") == 0)
    g_natural = 0;
  const char *m = getenv("FBBROWSER_SCROLL_MODE");
  if (m) {
    if (strcmp(m, "synth") == 0)
      g_mode = 1;
    else if (strcmp(m, "off") == 0)
      g_mode = 2;
  }
  const char *st = getenv("FBBROWSER_SCROLL_STRIP");
  if (st && strcmp(st, "0") == 0)
    g_strip = 0;
  const char *cl = getenv("FBBROWSER_SCROLL_CLAIM");
  if (cl && strcmp(cl, "0") == 0)
    g_claim = 0;
  const char *ff = getenv("FBBROWSER_SCROLL_PRIMARY_FD");
  if (ff && *ff)
    g_force_fd = atoi(ff);
}

static int claim_name(const char *name, int fd) {
  if (!g_claim)
    return 1;
  for (int i = 0; i < g_claim_count; i++)
    if (strcmp(g_claims[i].name, name) == 0)
      return g_claims[i].fd == fd;
  if (g_claim_count < MAX_CLAIMS) {
    snprintf(g_claims[g_claim_count].name, NAME_LEN, "%s", name);
    g_claims[g_claim_count].fd = fd;
    g_claim_count++;
  }
  return 1;
}

static void init_dev(struct dev_state *ds, int fd) {
  memset(ds, 0, sizeof *ds);
  ds->fd = fd;
  ds->used = 1;

  struct stat st;
  if (fstat(fd, &st) == 0)
    ds->rdev = st.st_rdev;

  unsigned long evbits[8] = {0};
  unsigned long keybits[(KEY_MAX / 64) + 2] = {0};
  unsigned long absbits[(ABS_MAX / 64) + 2] = {0};
  unsigned long relbits[(REL_MAX / 64) + 2] = {0};

  int has_abs = 0, has_rel = 0;
  if (raw_ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) >= 0) {
    has_abs = test_bit(EV_ABS, evbits, sizeof evbits);
    has_rel = test_bit(EV_REL, evbits, sizeof evbits);
  }
  int has_mt_pos = 0;
  if (has_abs &&
      raw_ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) >= 0) {
    int mx = test_bit(ABS_MT_POSITION_X, absbits, sizeof absbits);
    int my = test_bit(ABS_MT_POSITION_Y, absbits, sizeof absbits);
    has_mt_pos = mx && my;
  }
  int has_btn_finger = 0, has_btn_double = 0;
  if (raw_ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) >= 0) {
    has_btn_finger = test_bit(BTN_TOOL_FINGER, keybits, sizeof keybits);
    has_btn_double = test_bit(BTN_TOOL_DOUBLETAP, keybits, sizeof keybits);
  }
  if (has_rel &&
      raw_ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbits), relbits) >= 0) {
    ds->has_wheel = test_bit(REL_WHEEL, relbits, sizeof relbits);
    ds->has_hwheel = test_bit(REL_HWHEEL, relbits, sizeof relbits);
    ds->has_hi_res = test_bit(REL_WHEEL_HI_RES, relbits, sizeof relbits);
  }

  struct input_absinfo yi;
  if (raw_ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &yi) >= 0 &&
      yi.maximum > yi.minimum) {
    ds->abs_y_min = yi.minimum;
    ds->abs_y_max = yi.maximum;
  } else {
    ds->abs_y_min = 0;
    ds->abs_y_max = 1000;
  }

  if (g_threshold_frac > 0)
    ds->threshold = (ds->abs_y_max - ds->abs_y_min) * g_threshold_frac;
  else
    ds->threshold = g_threshold;
  if (ds->threshold < 1.0)
    ds->threshold = 1.0;

  ds->has_mt = has_mt_pos;
  ds->has_legacy = has_btn_finger && has_btn_double;
  ds->can_count = ds->has_mt || ds->has_legacy;

  if (ds->can_count && !ds->has_wheel) {
    ds->driver_scroll_known = 1;
    ds->driver_scroll = 0;
  }

  if (raw_ioctl(fd, EVIOCGNAME(sizeof ds->name), ds->name) < 0)
    ds->name[0] = 0;
  ds->name[NAME_LEN - 1] = 0;

  ds->synth_active = ds->has_mt ? 1 : 0;
  if (g_force_fd > 0 && fd != g_force_fd)
    ds->synth_active = 0;
	int main_thr = on_main_thread();
	if (ds->has_mt && !main_thr) {
		ds->synth_active = 0;
		ds->name_claim_checked = 1; // never takes part in name claiming
	}

  ds->prev_cnt = 0;
  ds->jmp_max = (ds->abs_y_max - ds->abs_y_min) / 2;
  if (ds->jmp_max < 100)
    ds->jmp_max = 100;

  log_msg("fd %d: mt=%d can_count=%d wheel=%d synth=%d rdev=%d:%d "
          "name='%s' range=[%d,%d] thr=%.1f jmp_max=%d "
          "strip=%d claim=%d force_fd=%d main_thread=%d",
          fd, ds->has_mt, ds->can_count, ds->has_wheel, ds->synth_active,
          (int)major(ds->rdev), (int)minor(ds->rdev), ds->name, ds->abs_y_min,
          ds->abs_y_max, ds->threshold, ds->jmp_max, g_strip, g_claim,
          g_force_fd, main_thr);
}

static struct dev_state *find_dev(int fd) {
  for (int i = 0; i < MAX_DEVS; i++)
    if (g_devs[i].used && g_devs[i].fd == fd)
      return &g_devs[i];
  if (!is_input_char(fd))
    return NULL;

  unsigned long relbits[(REL_MAX / 64) + 2] = {0};
  unsigned long absbits[(ABS_MAX / 64) + 2] = {0};
  int ptr = 0;
  if (raw_ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbits), relbits) >= 0) {
    if (test_bit(REL_X, relbits, sizeof relbits) &&
        test_bit(REL_Y, relbits, sizeof relbits))
      ptr = 1;
  }
  if (!ptr && raw_ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) >= 0) {
    if ((test_bit(ABS_X, absbits, sizeof absbits) &&
         test_bit(ABS_Y, absbits, sizeof absbits)) ||
        (test_bit(ABS_MT_POSITION_X, absbits, sizeof absbits) &&
         test_bit(ABS_MT_POSITION_Y, absbits, sizeof absbits)))
      ptr = 1;
  }
  if (!ptr)
    return NULL;

  for (int i = 0; i < MAX_DEVS; i++)
    if (!g_devs[i].used) {
      init_dev(&g_devs[i], fd);
      return &g_devs[i];
    }
  return NULL;
}

static int fingers_now(struct dev_state *ds) {
  int n = ds->mt_fingers, lf = 0;
  if (ds->bt_finger)
    lf = 1;
  if (ds->bt_touch && lf == 0)
    lf = 1;
  if (ds->bt_double)
    lf = 2;
  if (ds->bt_triple)
    lf = 3;
  if (ds->bt_quad)
    lf = 4;
  return lf > n ? lf : n;
}

static void avg_xy(struct dev_state *ds, int *x, int *y, int *cnt) {
  long sx = 0, sy = 0;
  int c = 0;
  for (int i = 0; i < MAX_SLOTS; i++)
    if (ds->slots[i].active) {
      sx += ds->slots[i].pos_x;
      sy += ds->slots[i].pos_y;
      c++;
    }
  *cnt = c;
  *x = c ? (int)(sx / c) : 0;
  *y = c ? (int)(sy / c) : 0;
}

static void update_state(struct dev_state *ds, struct input_event *e) {
  if (e->type == EV_ABS) {
    if (e->code == ABS_MT_SLOT) {
      ds->cur_slot = e->value;
    } else if (e->code == ABS_MT_TRACKING_ID) {
      if (ds->cur_slot >= 0 && ds->cur_slot < MAX_SLOTS) {
        ds->slots[ds->cur_slot].active = (e->value >= 0);
        ds->mt_fingers = 0;
        for (int s = 0; s < MAX_SLOTS; s++)
          if (ds->slots[s].active)
            ds->mt_fingers++;
      }
    } else if (e->code == ABS_MT_POSITION_X) {
      if (ds->cur_slot >= 0 && ds->cur_slot < MAX_SLOTS)
        ds->slots[ds->cur_slot].pos_x = e->value;
    } else if (e->code == ABS_MT_POSITION_Y) {
      if (ds->cur_slot >= 0 && ds->cur_slot < MAX_SLOTS)
        ds->slots[ds->cur_slot].pos_y = e->value;
    }
  } else if (e->type == EV_KEY) {
    if (e->code == BTN_TOOL_FINGER)
      ds->bt_finger = e->value;
    else if (e->code == BTN_TOOL_DOUBLETAP)
      ds->bt_double = e->value;
    else if (e->code == BTN_TOOL_TRIPLETAP)
      ds->bt_triple = e->value;
    else if (e->code == BTN_TOOL_QUADTAP)
      ds->bt_quad = e->value;
    else if (e->code == BTN_TOUCH)
      ds->bt_touch = e->value;
  }
}

static int wants_synth(struct dev_state *ds) {
  if (g_mode == 1)
    return 1;
  if (g_mode == 2)
    return 0;
  if (!ds->can_count)
    return 0;
  if (ds->driver_scroll_known && !ds->driver_scroll)
    return 1;
  return 0;
}

static int should_drop_wheel(struct dev_state *ds) {
  if (g_mode == 2)
    return 0;
  if (!ds->can_count)
    return 0;
  if (fingers_now(ds) < 2)
    return 1;
  if (g_mode == 1)
    return 1;
  if (ds->driver_scroll_known && !ds->driver_scroll)
    return 1;
  return 0;
}

// Filter events to match what is advertised
// Mouse has no ABS, no BTN_TOUCH, no BTN_TOOL_*
static int should_strip_out(struct dev_state *ds, struct input_event *e) {
  if (!g_strip)
    return 0;
  if (!ds->has_mt)
    return 0; // non-mt devices should be left alone
  if (e->type == EV_ABS)
    return 1;
  if (e->type == EV_KEY && is_touch_key(e->code))
    return 1;
  return 0;
}

static ssize_t process(int fd, void *buf, size_t got_bytes, size_t max_bytes) {
  if (got_bytes % sizeof(struct input_event) != 0)
    return got_bytes;
  struct input_event *ev = buf;
  size_t n = got_bytes / sizeof(struct input_event);
  size_t max_n = max_bytes / sizeof(struct input_event);

  pthread_mutex_lock(&g_lock);
  struct dev_state *ds = find_dev(fd);
  if (!ds) {
    pthread_mutex_unlock(&g_lock);
    return got_bytes;
  }

  // Duped node which has been alreaady deactivated
  // Just consume silently with bare SYN_REPORT as to not block
  if (ds->has_mt && !ds->synth_active) {
    if (max_bytes >= sizeof(struct input_event)) {
      struct input_event *o = buf;
      memset(&o[0], 0, sizeof o[0]);
      o[0].type = EV_SYN;
      o[0].code = SYN_REPORT;
      pthread_mutex_unlock(&g_lock);
      return sizeof(struct input_event);
    }
    pthread_mutex_unlock(&g_lock);
    return got_bytes;
  }

  struct input_event out[MAX_EVENTS];
  size_t out_n = 0;
  struct input_event frame[FRAME_BUF];
  size_t frame_n = 0;

  for (size_t i = 0; i < n && i < MAX_EVENTS; i++) {
    update_state(ds, &ev[i]);

    if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
      int x, y, cnt;
      avg_xy(ds, &x, &y, &cnt);

      if (cnt != ds->prev_cnt) {
        if (g_verbose)
          log_msg("fd %d: cnt %d -> %d, resetting baselines", ds->fd,
                  ds->prev_cnt, cnt);
        ds->has_last_xy = 0;
        ds->scroll_accum = 0;
      }
      ds->prev_cnt = cnt;

      int fingers = fingers_now(ds);
      int motion = (cnt == 2 && ds->has_last_xy) ? abs(y - ds->last_y) : 0;
      int drop = should_drop_wheel(ds);
      int synth = wants_synth(ds);

      // Lazy name claim: first fd to actually touch the pad wins.
      if (ds->synth_active && !ds->name_claim_checked && fingers >= 1) {
        if (!claim_name(ds->name, ds->fd)) {
          ds->synth_active = 0;
          log_msg("fd %d: name '%s' already claimed, deactivating", ds->fd,
                  ds->name);
        }
        ds->name_claim_checked = 1;
      }

      int wheels_kept = 0;
      for (size_t j = 0; j < frame_n; j++)
        if (frame[j].type == EV_REL && is_wheel_code(frame[j].code) && !drop)
          wheels_kept++;

      if (!ds->driver_scroll_known) {
        if (fingers == 2 && motion > 2 && wheels_kept > 0) {
          ds->driver_scroll = 1;
          ds->driver_scroll_known = 1;
          log_msg("fd %d: driver scroll detected", ds->fd);
        } else if (fingers == 2 && motion > 2 && wheels_kept == 0) {
          // never hits here because we set known=1 at init when no wheel
        }
      }

      // Emit unstripped, not dropped actual events from frame
      for (size_t j = 0; j < frame_n; j++) {
        if (should_strip_out(ds, &frame[j]))
          continue;
        int is_w = (frame[j].type == EV_REL && is_wheel_code(frame[j].code));
        if (is_w && drop)
          continue;
        if (out_n < max_n && out_n < MAX_EVENTS)
          out[out_n++] = frame[j];
      }

      // Create a 1 finger cursor motion from mt position change
      if (ds->synth_active && fingers == 1 && cnt == 1 && ds->has_last_xy) {
        int dx = x - ds->last_x, dy = y - ds->last_y;

        if (ds->jmp_max > 0 &&
            (abs(dx) > ds->jmp_max || abs(dy) > ds->jmp_max)) {
          if (g_verbose)
            log_msg("fd %d: 1F implausible jump dx=%d dy=%d, dropped", ds->fd,
                    dx, dy);
          dx = dy = 0;
        }

        if (dx || dy) {
          if (out_n < max_n && out_n < MAX_EVENTS)
            out[out_n++] = (struct input_event){
                .type = EV_REL, .code = REL_X, .value = dx};
          if (out_n < max_n && out_n < MAX_EVENTS)
            out[out_n++] = (struct input_event){
                .type = EV_REL, .code = REL_Y, .value = dy};
          if (g_verbose)
            log_msg("fd %d: 1F dx=%d dy=%d", ds->fd, dx, dy);
        }
      }

      // Create 2 finger wheel from mt y change
      if (synth && ds->synth_active && fingers == 2 && cnt == 2 &&
          ds->has_last_xy)
        ds->scroll_accum += (double)(y - ds->last_y) * (g_natural ? 1.0 : -1.0);

      if (synth && ds->synth_active && fingers == 2) {
        while (ds->scroll_accum >= ds->threshold && out_n + 2 <= max_n &&
               out_n + 2 <= MAX_EVENTS) {
          out[out_n++] = (struct input_event){
              .type = EV_REL, .code = REL_WHEEL, .value = 1};
          out[out_n++] =
              (struct input_event){.type = EV_SYN, .code = SYN_REPORT};
          ds->scroll_accum -= ds->threshold;
          if (g_verbose)
            log_msg("fd %d: +wheel acc=%.1f", ds->fd, ds->scroll_accum);
        }
        while (ds->scroll_accum <= -ds->threshold && out_n + 2 <= max_n &&
               out_n + 2 <= MAX_EVENTS) {
          out[out_n++] = (struct input_event){
              .type = EV_REL, .code = REL_WHEEL, .value = -1};
          out[out_n++] =
              (struct input_event){.type = EV_SYN, .code = SYN_REPORT};
          ds->scroll_accum += ds->threshold;
          if (g_verbose)
            log_msg("fd %d: -wheel acc=%.1f", ds->fd, ds->scroll_accum);
        }
      }

      // Frame terminating SYN_REPORT
      if (out_n < max_n && out_n < MAX_EVENTS)
        out[out_n++] = ev[i];

      if (cnt >= 1) {
        ds->last_x = x;
        ds->last_y = y;
        ds->has_last_xy = 1;
      } else {
        ds->has_last_xy = 0;
        ds->scroll_accum = 0;
      }

      if (g_verbose)
        log_msg("fd %d: f=%d cnt=%d x=%d y=%d motion=%d drop=%d synth=%d "
                "active=%d kept=%d acc=%.1f",
                ds->fd, fingers, cnt, x, y, motion, drop, synth,
                ds->synth_active, wheels_kept, ds->scroll_accum);

      frame_n = 0;
    } else {
      if (frame_n < FRAME_BUF)
        frame[frame_n++] = ev[i];
      else if (out_n < max_n && out_n < MAX_EVENTS)
        out[out_n++] = ev[i];
    }
  }

  memcpy(ev, out, out_n * sizeof(struct input_event));
  pthread_mutex_unlock(&g_lock);
  return out_n * sizeof(struct input_event);
}

typedef int (*ioctl_t)(int, unsigned long, ...);
static ioctl_t real_ioctl;

int ioctl(int fd, unsigned long req, ...) {
  va_list ap;
  va_start(ap, req);
  void *arg = va_arg(ap, void *);
  va_end(ap);

  if (!real_ioctl) {
    real_ioctl = (ioctl_t)dlsym(RTLD_NEXT, "ioctl");
    if (!real_ioctl) {
      errno = ENOSYS;
      return -1;
    }
  }

  if (!g_disabled && _IOC_TYPE(req) == 'E' && _IOC_DIR(req) == _IOC_READ &&
      _IOC_NR(req) >= 0x20 && _IOC_NR(req) <= 0x3f && _IOC_SIZE(req) > 0 &&
      is_input_char(fd) && has_mt_caps_raw(fd)) {

    int ret = real_ioctl(fd, req, arg);
    if (ret < 0)
      return ret;

    int ev_type = _IOC_NR(req) - 0x20;
    unsigned long *bits = arg;
    size_t nbits = _IOC_SIZE(req) * 8;

    if (ev_type == 0) {
      if (EV_REL < nbits)
        bits[EV_REL / (8 * sizeof(unsigned long))] |=
            1UL << (EV_REL % (8 * sizeof(unsigned long)));
      if (EV_ABS < nbits)
        bits[EV_ABS / (8 * sizeof(unsigned long))] &=
            ~(1UL << (EV_ABS % (8 * sizeof(unsigned long))));
    } else if (ev_type == EV_REL) {
      if (REL_X < nbits)
        bits[REL_X / (8 * sizeof(unsigned long))] |=
            1UL << (REL_X % (8 * sizeof(unsigned long)));
      if (REL_Y < nbits)
        bits[REL_Y / (8 * sizeof(unsigned long))] |=
            1UL << (REL_Y % (8 * sizeof(unsigned long)));
    } else if (ev_type == EV_ABS) {
      int hide[] = {ABS_MT_SLOT,        ABS_MT_TOUCH_MAJOR, ABS_MT_TOUCH_MINOR,
                    ABS_MT_WIDTH_MAJOR, ABS_MT_WIDTH_MINOR, ABS_MT_ORIENTATION,
                    ABS_MT_POSITION_X,  ABS_MT_POSITION_Y,  ABS_MT_TOOL_TYPE,
                    ABS_MT_BLOB_ID,     ABS_MT_TRACKING_ID, ABS_MT_PRESSURE};
      for (size_t k = 0; k < sizeof(hide) / sizeof(hide[0]); k++) {
        int c = hide[k];
        if (c < (int)nbits)
          bits[c / (8 * sizeof(unsigned long))] &=
              ~(1UL << (c % (8 * sizeof(unsigned long))));
      }
    } else if (ev_type == EV_KEY) {
      int hide[] = {BTN_TOUCH,          BTN_TOOL_FINGER,  BTN_TOOL_DOUBLETAP,
                    BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP};
      for (size_t k = 0; k < sizeof(hide) / sizeof(hide[0]); k++) {
        int c = hide[k];
        if (c < (int)nbits)
          bits[c / (8 * sizeof(unsigned long))] &=
              ~(1UL << (c % (8 * sizeof(unsigned long))));
      }
    }
    return ret;
  }

  return real_ioctl(fd, req, arg);
}

typedef ssize_t (*read_fn)(int, void *, size_t);
static read_fn real_read;

ssize_t read(int fd, void *buf, size_t count) {
  if (!real_read) {
    real_read = (read_fn)dlsym(RTLD_NEXT, "read");
    if (!real_read) {
      errno = ENOSYS;
      return -1;
    }
  }
  ssize_t r = real_read(fd, buf, count);
  if (r > 0 && !g_disabled && (size_t)r % sizeof(struct input_event) == 0)
    r = process(fd, buf, r, count);
  return r;
}

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buflen) {
  static ssize_t (*real)(int, void *, size_t, size_t);
  if (!real) {
    real = dlsym(RTLD_NEXT, "__read_chk");
    if (!real) {
      errno = ENOSYS;
      return -1;
    }
  }
  ssize_t r = real(fd, buf, count, buflen);
  if (r > 0 && !g_disabled && (size_t)r % sizeof(struct input_event) == 0)
    r = process(fd, buf, r, count);
  return r;
}
