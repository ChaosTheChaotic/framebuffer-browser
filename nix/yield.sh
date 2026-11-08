#!/usr/bin/env bash
set -u

if [ -z "${YIELD_SAVE_PATH:-}" ]; then
	echo "yield: only works inside the fbbrowser file chooser terminal" >&2
	exit 2
fi

if [ -t 2 ]; then
	red=$'\e[1;31m'
	reset=$'\e[0m'
else
	red=''
	reset=''
fi
err() { printf '%s%s%s\n' "$red" "$*" "$reset" >&2; }

mode_save=${YIELD_MODE_SAVE:-0}
mode_dir=${YIELD_MODE_DIR:-0}
mode_multi=${YIELD_MODE_MULTIPLE:-0}

paths=()
errors=0

process_path() {
	local target=$1 abs parent

	if [ "$mode_save" = 0 ] && [ ! -e "$target" ]; then
		err "Error: '$target' does not exist."
		errors=$((errors + 1))
		return
	fi
	if [ "$mode_dir" = 1 ] && [ -e "$target" ] && [ ! -d "$target" ]; then
		err "Error: '$target' is not a directory. A directory is required."
		errors=$((errors + 1))
		return
	fi
	if [ "$mode_dir" = 0 ] && [ -d "$target" ]; then
		err "Error: '$target' is a directory. A file is required."
		errors=$((errors + 1))
		return
	fi

	# -s keeps symlinks as picked (the page sees the name you chose, as with a normal dialog)
	# -m lets a file that is about to be created resolve
	if [ "$mode_save" = 1 ]; then
		abs=$(realpath -s -m -- "$target")
		parent=$(dirname -- "$abs")
		if [ ! -d "$parent" ]; then
			err "Error: '$parent' does not exist."
			errors=$((errors + 1))
			return
		fi
	else
		abs=$(realpath -s -- "$target")
	fi
	paths+=("$abs")
}

for arg in "$@"; do
	process_path "$arg"
done

if [ ! -t 0 ]; then
	while IFS= read -r line || [ -n "$line" ]; do
		[ -n "$line" ] && process_path "$line"
	done
fi

if [ "$mode_multi" = 0 ] && [ "${#paths[@]}" -gt 1 ]; then
	err "Error: Multiple paths provided, but only a single selection is allowed."
	errors=$((errors + 1))
fi

if [ "$errors" -gt 0 ] || [ "${#paths[@]}" -eq 0 ]; then
	if [ "$errors" -eq 0 ]; then
		err "Error: No valid paths provided."
	fi
	# A bad selection must not leave a half valid one behind
	: >"$YIELD_SAVE_PATH"
	exit 1 # the terminal stays open
fi

printf '%s\0' "${paths[@]}" >"$YIELD_SAVE_PATH"

# We are about to be hung up together with the shell
trap '' HUP TERM
if [ -n "${YIELD_SHELL_PID:-}" ] && kill -HUP "$YIELD_SHELL_PID" 2>/dev/null; then
	exit 0
fi
echo "Selection stored. Type exit to return to the page."
exit 0
