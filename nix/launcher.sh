#!/usr/bin/env bash
set -euo pipefail
export FBBROWSER_POINTER_SPEED="${FBBROWSER_POINTER_SPEED:-@pointerSpeedDefault@}"
export PATH="@path@${PATH:+:$PATH}"

real_bin='@unwrapped@'
vt_reset='@vtReset@'
pointer_shim='@pointerShim@'
backend="${FBBROWSER_BACKEND:-@defaultBackend@}"
dry_run=0
debug=0

die()  { printf 'fbbrowser: error: %s\n' "$*" >&2; exit 1; }
warn() { printf 'fbbrowser: warning: %s\n' "$*" >&2; }
note() { printf 'fbbrowser: %s\n' "$*" >&2; }

usage() {
  cat <<EOF
Usage: fbbrowser [options] [URL]

Options:
  -b, --backend <auto|eglfs|fb|wayland>
                  Qt platform backend (default: $backend)
        auto     eglfs if a KMS-capable DRM node exists, else linuxfb
        eglfs    accelerated, needs a DRM node with display outputs
        fb       linuxfb. Qt6 WebEngine needs OpenGL, which linuxfb lacks,
                 so expect it NOT to render web content
        wayland  run as a normal window (for testing inside a compositor)
  -d, --debug     Enable Qt platform logging
  -n, --dry-run   Print the chosen backend and environment, don't start
  -h, --help      Show this help

Only one URL is accepted (the browser reads just argv[1]). Chromium flags
cannot be passed as arguments; use QTWEBENGINE_CHROMIUM_FLAGS instead.
Quit the browser with Ctrl+Q. Ctrl+Left / Ctrl+Right go back / forward.

Environment:
  FBBROWSER_BACKEND          Default for --backend
  FBBROWSER_CONFIG_DIR       Directory holding config.json
                             (default: \${XDG_CONFIG_HOME:-~/.config}/fbbrowser)
  FBBROWSER_DRM_DEVICE       DRM node for eglfs, e.g. /dev/dri/card2
                             (default: first node that has display outputs)
  FBBROWSER_FB_DEVICE        Framebuffer node for linuxfb (default: first usable /dev/fb*)
  FBBROWSER_KEYBOARD_DEV     Pin keyboard to an evdev node (eventN or full path)
  FBBROWSER_MOUSE_DEV        Pin mouse to an evdev node
  FBBROWSER_TOUCHSCREEN_DEV  Pin touchscreen to an evdev node
                             Setting any *_DEV switches Qt from libinput
                             (auto-discovery, the default) to plain evdev.
  FBBROWSER_HWCURSOR         Set to 1 to use the hardware cursor under eglfs. Off by
                             default: some drivers (e.g. Asahi) reject Qt's cursor
                             ioctls, spamming "Failed to move cursor on screen"
  FBBROWSER_POINTER_SPEED    libinput pointer speed, -1.0 .. 1.0 (0 = libinput default)
  QT_QPA_EGLFS_KMS_CONFIG    Your own Qt KMS JSON; stops the launcher generating one
                             (then hwcursor is whatever your JSON says)
  QT_QPA_PLATFORM            If set and backend is 'auto', used as-is (no detection)

If a crash leaves a text VT dead (blank, no keyboard), run from another VT
or over SSH:  sudo fbbrowser-vt-reset /dev/ttyN
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    -b|--backend)
      [ $# -ge 2 ] || die "$1 needs a value (auto, eglfs, fb or wayland)"
      backend=$2; shift 2 ;;
    --backend=*) backend=${1#*=}; shift ;;
    -d|--debug) debug=1; shift ;;
    -n|--dry-run) dry_run=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) die "unknown option '$1' (the browser would treat it as a URL; see --help)" ;;
    *) break ;;
  esac
done
[ $# -le 1 ] || die "at most one URL is accepted"
url=${1:-}

case "$backend" in
  auto|eglfs|fb|wayland) ;;
  linuxfb) backend=fb ;;
  *) die "invalid backend '$backend' (expected auto, eglfs, fb or wayland)" ;;
esac

# Character device among the arguments that we can read and write
usable_node() {
  local n
  for n in "$@"; do
    if [ -c "$n" ] && [ -r "$n" ] && [ -w "$n" ]; then
      printf '%s\n' "$n"
      return 0
    fi
  done
  return 1
}

any_node() {
  local n
  for n in "$@"; do
    if [ -c "$n" ]; then return 0; fi
  done
  return 1
}

# Only KMS (display) devices have connector entries in sysfs
has_connectors() {
  local c s
  for c in /sys/class/drm/"$1"-*; do
    [ -e "$c" ] || continue
    if [ "${2:-}" = connected ]; then
      s=""
      { read -r s <"$c/status"; } 2>/dev/null || true
      [ "$s" = connected ] || continue
    fi
    return 0
  done
  return 1
}

# First usable KMS node, preferring one with a connected display.
find_kms_node() {
  local pass n
  for pass in connected any; do
    for n in /dev/dri/card*; do
      if [ -c "$n" ] && has_connectors "${n##*/}" "$pass" && usable_node "$n" >/dev/null; then
        printf '%s\n' "$n"
        return 0
      fi
    done
  done
  return 1
}

explain_no_kms() {
  local n have_kms=0
  for n in /dev/dri/card*; do
    if [ -c "$n" ] && has_connectors "${n##*/}" any; then have_kms=1; fi
  done
  if [ "$have_kms" = 1 ]; then
    warn "a KMS-capable DRM device exists but $(id -un) cannot read/write it (groups: $(id -nG))."
    warn "  Add the user to the 'video' group, or log in on the local seat."
  elif any_node /dev/dri/card*; then
    warn "DRM nodes exist but none has display outputs (render-only GPU?)."
    warn "  If you know the right one, set FBBROWSER_DRM_DEVICE=/dev/dri/cardN."
  else
    warn "no DRM device found."
  fi
}

explain_no_fb() {
  if any_node /dev/fb*; then
    warn "framebuffer device exists but $(id -un) cannot read/write it (groups: $(id -nG))."
    warn "  Add the user to the 'video' group."
  else
    warn "no framebuffer device found."
  fi
}

evdev_path() { case "$1" in /*) printf '%s' "$1" ;; *) printf '/dev/input/%s' "$1" ;; esac; }

drm_node=${FBBROWSER_DRM_DEVICE:-}
if [ -z "$drm_node" ]; then drm_node=$(find_kms_node || true); fi
if [ -n "${FBBROWSER_FB_DEVICE:-}" ]; then
  fb_node=$FBBROWSER_FB_DEVICE
else
  fb_node=$(usable_node /dev/fb* || true)
fi

resolved=$backend
if [ "$backend" = auto ]; then
  if [ -n "${QT_QPA_PLATFORM:-}" ]; then
    resolved=env
    note "backend: using QT_QPA_PLATFORM=$QT_QPA_PLATFORM from the environment"
  elif [ -n "$drm_node" ]; then
    resolved=eglfs
  elif [ -n "$fb_node" ]; then
    resolved=fb
    note "backend: no usable KMS node, falling back to linuxfb ($fb_node)"
    explain_no_kms
  else
    explain_no_kms
    explain_no_fb
    die "no usable display device for either eglfs or linuxfb"
  fi
elif [ "$backend" = eglfs ] && [ -z "$drm_node" ]; then
	# Starting anyway causes a crash that ends up rendering the TTY unusable
  explain_no_kms
  die "no usable KMS node for eglfs (set FBBROWSER_DRM_DEVICE to override detection)"
elif [ "$backend" = fb ] && [ -z "$fb_node" ]; then
  explain_no_fb
  warn "continuing anyway because --backend fb was forced"
fi

# eglfs/fb take over a kernel VT; wayland and externally chosen platforms do not.
vt_owner=0
if [ "$resolved" = eglfs ] || [ "$resolved" = fb ]; then vt_owner=1; fi

if [ "$vt_owner" = 1 ]; then
  if [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
    warn "a graphical session is running (WAYLAND_DISPLAY/DISPLAY set); $resolved cannot take the"
    warn "  display from it. Switch to a text VT, or use --backend wayland to test."
  fi
  if [ -t 0 ]; then
    tty_name=$(tty || true)
    case "$tty_name" in
      /dev/tty[0-9]*) ;;
      *) warn "stdin is '$tty_name', not a kernel VT. Inside a terminal emulator (bcon, kmscon, ssh, ...)"
         warn "  the display is usually owned by someone else. Use a plain getty VT." ;;
    esac
  fi
  if ! usable_node /dev/input/event* >/dev/null; then
    warn "no readable /dev/input/event* nodes: keyboard and mouse will not work."
    warn "  Add the user to the 'input' group."
  fi
fi

if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
  warn "XDG_RUNTIME_DIR is not set (no logind session?): PipeWire/Bluetooth audio will not work."
fi

evdev=0
if [ -n "${FBBROWSER_KEYBOARD_DEV:-}" ]; then
  export QT_QPA_EVDEV_KEYBOARD_PARAMETERS="$(evdev_path "$FBBROWSER_KEYBOARD_DEV"):grab=1"
  evdev=1
fi
if [ -n "${FBBROWSER_MOUSE_DEV:-}" ]; then
  export QT_QPA_EVDEV_MOUSE_PARAMETERS="$(evdev_path "$FBBROWSER_MOUSE_DEV")"
  evdev=1
fi
if [ -n "${FBBROWSER_TOUCHSCREEN_DEV:-}" ]; then
  export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS="$(evdev_path "$FBBROWSER_TOUCHSCREEN_DEV")"
  evdev=1
fi

export QT_QPA_ENABLE_TERMINAL_KEYBOARD=0

if [ "$debug" = 1 ]; then
  export QT_LOGGING_RULES="${QT_LOGGING_RULES:+$QT_LOGGING_RULES;}qt.qpa.*=true"
  export QT_QPA_EGLFS_DEBUG=1
fi

# config.json lives in the working directory, so give it a stable one.
config_dir="${FBBROWSER_CONFIG_DIR:-${XDG_CONFIG_HOME:-${HOME:?HOME is not set}/.config}/fbbrowser}"

hwcursor=false
if [ "${FBBROWSER_HWCURSOR:-0}" = 1 ]; then hwcursor=true; fi

write_kms_json=0
case "$resolved" in
  eglfs)
    export QT_QPA_PLATFORM=eglfs
    export QT_QPA_EGLFS_INTEGRATION=eglfs_kms
    if [ "$evdev" = 1 ]; then export QT_QPA_EGLFS_NO_LIBINPUT=1; fi
    # Qt has no env var for the DRM node; it only reads "device" from this JSON.
    # "hwcursor": false makes Qt draw a software cursor instead.
    if [ -z "${QT_QPA_EGLFS_KMS_CONFIG:-}" ]; then
      export QT_QPA_EGLFS_KMS_CONFIG="$config_dir/kms.json"
      write_kms_json=1
    fi
    ;;
  fb)
    warn "linuxfb has no OpenGL and Qt6 WebEngine needs it: web content will most likely not render."
    export QT_QPA_PLATFORM="linuxfb:fb=${fb_node:-/dev/fb0}:nographicsmodeswitch=1"
    export QT_QPA_FB_FORCE_FULLSCREEN=1
    if [ "$evdev" = 1 ]; then export QT_QPA_FB_NO_LIBINPUT=1; fi
    case " ${QTWEBENGINE_CHROMIUM_FLAGS:-} " in
      *" --disable-gpu "*) ;;
      *) export QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu${QTWEBENGINE_CHROMIUM_FLAGS:+ $QTWEBENGINE_CHROMIUM_FLAGS}" ;;
    esac
    ;;
  wayland)
    export QT_QPA_PLATFORM=wayland
    ;;
esac

# Resolve ./relative URLs before we change directory.
case "$url" in ./*) url="file://$PWD/${url#./}" ;; esac

preload=""
if [ -n "${FBBROWSER_POINTER_SPEED:-}${FBBROWSER_POINTER_ACCEL:-}" ]; then
	if [ "$vt_owner" = 0 ]; then
		note "pointer speed/accel only apply to eglfs/linuxfb; under '$resolved' the compositor owns them"
	else
		preload=$pointer_shim
	fi
fi

cmd=("$real_bin")
if [ -n "$preload" ]; then
  cmd=(env "LD_PRELOAD=$preload${LD_PRELOAD:+:$LD_PRELOAD}" "$real_bin")
fi
if [ -n "$url" ]; then cmd+=("$url"); fi

if [ "$dry_run" = 1 ]; then
  printf 'backend:    %s\n' "$resolved"
  printf 'drm node:   %s\n' "${drm_node:-<none>}"
  printf 'config dir: %s\n' "$config_dir"
  while IFS= read -r name; do
    case "$name" in QT_QPA_*|QTWEBENGINE_*|QT_LOGGING_*) printf '%s=%s\n' "$name" "${!name}" ;; esac
  done < <(compgen -e | sort)
  if [ "$write_kms_json" = 1 ]; then printf 'kms.json:   {"device": "%s", "hwcursor": %s}\n' "$drm_node" "$hwcursor"; fi
  printf 'command:   '; printf ' %q' "${cmd[@]}"; printf '\n'
  exit 0
fi

mkdir -p "$config_dir"
if [ ! -e "$config_dir/config.json" ]; then
  cat >"$config_dir/config.json" <<'JSON'
{
  "url": "https://google.com",
  "wsServerPort": 0,
  "width": 0,
  "height": 0,
  "proxyHost": "",
  "proxyPort": 0
}
JSON
fi
if [ "$write_kms_json" = 1 ]; then
  printf '{\n  "device": "%s",\n  "hwcursor": %s\n}\n' "$drm_node" "$hwcursor" >"$QT_QPA_EGLFS_KMS_CONFIG"
fi
cd "$config_dir"

if [ "$vt_owner" = 0 ]; then
  exec "${cmd[@]}"
fi

# Clean up if crash
restore_vt() {
  if [ "$resolved" = fb ]; then
    if [ -t 1 ]; then setterm -cursor on || true; fi
    cat /dev/zero >"${fb_node:-/dev/fb0}" 2>/dev/null || true
  fi
  if [ -t 0 ]; then "$vt_reset" 2>/dev/null || true; fi
}
trap restore_vt EXIT
if [ "$resolved" = fb ] && [ -t 1 ]; then setterm -cursor off || true; fi
rc=0
"${cmd[@]}" || rc=$?
exit "$rc"
