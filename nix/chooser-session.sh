#!/usr/bin/env bash
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
export PATH="$here:$PATH"
# Absolute path of yield so when a shell startup file has rewritten PATH, "$YIELD_BIN" <path> always works
export YIELD_BIN="$here/yield"

# Keep the browser's own environment out of whatever is started in here
unset LD_PRELOAD QT_LOGGING_RULES
for v in "${!QT_QPA_@}" "${!QTWEBENGINE_@}"; do
	[ -n "$v" ] && unset "$v"
done

cyan=$'\e[1;36m'
green=$'\e[1;32m'
red=$'\e[1;31m'
reset=$'\e[0m'

action=Open
target=File
limit=Single
[ "${YIELD_MODE_SAVE:-0}" = 1 ] && action=Save
[ "${YIELD_MODE_DIR:-0}" = 1 ] && target=Directory
[ "${YIELD_MODE_MULTIPLE:-0}" = 1 ] && limit=Multiple

printf '%sTerminal File Chooser%s\n' "$cyan" "$reset"
printf 'Action: %s | Target: %s | Selection: %s\n' "$action" "$target" "$limit"
[ -n "${YIELD_SUGGESTED:-}" ] && printf 'Suggested name: %s\n' "$YIELD_SUGGESTED"
[ -n "${YIELD_ACCEPT:-}" ] && printf 'Accepts: %s\n' "$YIELD_ACCEPT"
printf 'Confirm: %syield <path>%s  •  Cancel: %sexit%s (or Ctrl+D)\n\n' \
	"$green" "$reset" "$red" "$reset"

shell=${FBB_CHOOSER_SHELL:-${SHELL:-/bin/sh}}

# Say it here if yield cannot work, rather than as "command not found" later
if [ ! -x "$YIELD_BIN" ]; then
	printf '%sWarning: %s is not executable (chmod +x it)%s\n' "$red" "$YIELD_BIN" "$reset"
fi
for c in realpath dirname; do
	command -v "$c" >/dev/null || printf '%sWarning: %s not found, yield needs it%s\n' "$red" "$c" "$reset"
done

# exec keeps this pid, so it is also the id of the interactive shell,
# and that is the one yield hangs up once a selection is stored
export YIELD_SHELL_PID=$$
shopt -s execfail

# Shell startup files may rewrite PATH, which would drop the directory yield is
# in. For zsh and bash, start the shell through a small rc that runs your own
# startup files first, then puts the directory back on PATH and defines a yield
# function that calls $YIELD_BIN, so a prompt hook that edits PATH cannot lose it
# either. Other shells just get the PATH set above ($YIELD_BIN always works).
export FBB_CHOOSER_BIN=$here
rcbase=$(dirname -- "${YIELD_SAVE_PATH:-${TMPDIR:-/tmp}/x}")
rc=$(mktemp -d "$rcbase/rc.XXXXXX" 2>/dev/null) || rc=

if [ -n "$rc" ]; then
	case ${shell##*/} in
	zsh)
		# zsh reads .zshenv then .zshrc from $ZDOTDIR. Keep ZDOTDIR on this
		# directory between the two, and on yours while your files run.
		export FBB_USER_ZDOTDIR=${ZDOTDIR:-$HOME}
		cat >"$rc/.zshenv" <<'RC_END'
FBB_SHIM_ZDOTDIR=$ZDOTDIR
ZDOTDIR=$FBB_USER_ZDOTDIR
[ -f "$ZDOTDIR/.zshenv" ] && . "$ZDOTDIR/.zshenv"
FBB_USER_ZDOTDIR=$ZDOTDIR
ZDOTDIR=$FBB_SHIM_ZDOTDIR
RC_END
		cat >"$rc/.zshrc" <<'RC_END'
ZDOTDIR=$FBB_USER_ZDOTDIR
[ -f "$ZDOTDIR/.zshrc" ] && . "$ZDOTDIR/.zshrc"
PATH="$FBB_CHOOSER_BIN:$PATH"
yield() { "$YIELD_BIN" "$@"; }
unset FBB_SHIM_ZDOTDIR FBB_USER_ZDOTDIR
RC_END
		export ZDOTDIR=$rc
		exec "$shell" -i
		;;
	bash)
		cat >"$rc/bashrc" <<'RC_END'
if [ -f /etc/bash.bashrc ]; then . /etc/bash.bashrc; elif [ -f /etc/bashrc ]; then . /etc/bashrc; fi
[ -f ~/.bashrc ] && . ~/.bashrc
PATH="$FBB_CHOOSER_BIN:$PATH"
yield() { "$YIELD_BIN" "$@"; }
RC_END
		exec "$shell" --rcfile "$rc/bashrc" -i
		;;
	esac
fi
exec "$shell" -i

printf '%sCannot start %s%s\n' "$red" "$shell" "$reset" >&2
sleep 5
exit 127
