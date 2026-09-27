#!/usr/bin/env bash
# Record two displays at once, stop cleanly on Enter or Ctrl+C.
# Usage: record-screens.sh <av-index-1> <name-1> <av-index-2> <name-2> [framerate]
# Example: record-screens.sh 4 m27up 3 dell
#
# Lessons baked in (each cost a debugging session):
# - ONE ffmpeg process with two avfoundation inputs. Two concurrent processes
#   (or two AVFoundation opens) race: the loser wedges before writing anything
#   instead of erroring. A single process opens both devices cleanly.
# - Hardware encode (h264_videotoolbox): software x264 of a 4K screen on a hot
#   Air is too slow to even start in reasonable time and fights the RDP client.
# - ffmpeg ignores stdin EOF quirks: detach recorder stdin via </dev/null.
# - A display that naps yields zero frames without an error: assert user
#   activity while recording, and fail loudly if an output stays empty.
set -uo pipefail

if [ "$#" -lt 4 ]; then
	echo "usage: $0 <av-index-1> <name-1> <av-index-2> <name-2> [framerate]" >&2
	exit 1
fi
idx1="$1"; name1="$2"; idx2="$3"; name2="$4"; fps="${5:-30}"

for idx in "$idx1" "$idx2"; do
	case "$idx" in
		'' | *[!0-9]*)
			echo "display index must be numeric, got '$idx'" >&2
			exit 1
			;;
	esac
done

command -v ffmpeg >/dev/null || {
	echo "ffmpeg not found" >&2
	exit 1
}

file1="$name1.mp4"
file2="$name2.mp4"
for f in "$file1" "$file2"; do
	if [ -e "$f" ]; then
		echo "refusing to overwrite existing $f" >&2
		exit 1
	fi
done

# Displays nap when idle and yield zero frames without an error; assert user
# activity for the recording window so both sides actually produce frames.
caffeinate -u -t 3600 &
caffeinate_pid=$!

ffmpeg -f avfoundation -framerate "$fps" -capture_cursor 1 -i "$idx1" \
	-f avfoundation -framerate "$fps" -capture_cursor 1 -i "$idx2" \
	-map 0:v -c:v h264_videotoolbox -realtime 1 "$file1" \
	-map 1:v -c:v h264_videotoolbox -realtime 1 "$file2" </dev/null &
ffpid=$!

# Readiness: both outputs must exist and grow (device open plus encoder
# startup takes several seconds; stopping earlier yields no files at all).
ready=0
for _ in $(seq 1 40); do
	if [ -s "$file1" ] && [ -s "$file2" ] && kill -0 "$ffpid" 2>/dev/null; then
		ready=1
		break
	fi
	kill -0 "$ffpid" 2>/dev/null || break
	sleep 1
done
if [ "$ready" -ne 1 ]; then
	echo "recorder produced no output - check device indexes and permissions" >&2
	kill -KILL "$ffpid" 2>/dev/null || true
	wait "$ffpid" 2>/dev/null || true
	kill "$caffeinate_pid" 2>/dev/null || true
	exit 1
fi
echo "recording display $idx1 -> $file1, display $idx2 -> $file2"
echo "press Enter or Ctrl+C to stop"

stopped=0
# wait_pid <pid> <tenths>: true if pid exits within ~tenths/10 seconds.
# A zombie (already dead, awaiting reap) counts as exited: kill -0 alone
# cannot tell them apart, so inspect state instead.
wait_pid() {
	local i state
	for i in $(seq 1 "$2"); do
		state="$(ps -o stat= -p "$1" 2>/dev/null)" || return 0
		case "$state" in
			*Z*) return 0 ;;
			'') return 0 ;;
		esac
		sleep 0.1
	done
	state="$(ps -o stat= -p "$1" 2>/dev/null)" || return 0
	case "$state" in
		*Z* | '') return 0 ;;
	esac
	return 1
}
stop_all() {
	[ "$stopped" -eq 1 ] && return 0
	stopped=1
	echo "stopping..."
	# SIGINT finalizes cleanly, but some AVFoundation teardowns ignore it:
	# escalate instead of hanging in wait forever (KILL may cost the moov).
	kill -INT "$ffpid" 2>/dev/null || true
	if kill -0 "$ffpid" 2>/dev/null && ! wait_pid "$ffpid" 50; then
		kill -TERM "$ffpid" 2>/dev/null || true
		if kill -0 "$ffpid" 2>/dev/null && ! wait_pid "$ffpid" 30; then
			echo "warning: recorder would not stop, SIGKILL (files may be unplayable)" >&2
			kill -KILL "$ffpid" 2>/dev/null || true
		fi
	fi
	wait "$ffpid" 2>/dev/null || true
	echo "saved $file1 $file2"
}
trap stop_all INT TERM

# shellcheck disable=SC2162
read dummy || true
stop_all
trap - INT TERM
kill "$caffeinate_pid" 2>/dev/null || true
