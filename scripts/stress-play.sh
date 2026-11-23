#!/usr/bin/env bash
# Slice 1's disk check: play a directory of FLAC files through the default device while
# other processes hammer the disk the files are on, and report the underruns.
#
#   scripts/stress-play.sh MUSIC_DIR [--seconds N] [--next N] [--load-gb N] [--no-load]
#
#   --seconds N   how long to play (90)
#   --next N      skip to the next file every N seconds, so a file is read under the load
#                 every N seconds and not only at the start (off)
#   --load-gb N   the size of the file the load reads and writes, on the same disk (2)
#   --no-load     the same run with the disk left alone, for comparison
#
# The load: three readers of the load file with O_DIRECT (sequential and at random
# offsets), and a writer that rewrites part of it with O_DIRECT and fsync, all looping
# until the run ends. On a filesystem without O_DIRECT (exFAT), the readers drop the
# page cache behind them (dd's nocache) and the writer fsyncs, so the load still reaches
# the disk. The music is evicted from the page cache first (posix_fadvise, no
# root needed), so the player's reads go to the disk. The load file is in a directory of
# its own beside the music (MUSIC_DIR/.blackwing-stress), removed at the end.
#
# Needs the built player (build/linux-dev/bin/blackwing), script(1), dd, python3, and
# iostat (sysstat) for the disk's utilisation. Prints the player's lines and a summary.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
player="${BLACKWING:-$here/build/linux-dev/bin/blackwing}"
seconds=90 next=0 load_gb=2 load=1 dir=""
while [ $# -gt 0 ]; do
  case "$1" in
    --seconds) seconds="$2"; shift 2 ;;
    --next)    next="$2"; shift 2 ;;
    --load-gb) load_gb="$2"; shift 2 ;;
    --no-load) load=0; shift ;;
    -*)        echo "stress-play: unknown option $1" >&2; exit 2 ;;
    *)         dir="$1"; shift ;;
  esac
done
[ -n "$dir" ] && [ -d "$dir" ] || { echo "usage: stress-play.sh MUSIC_DIR [--seconds N] [--next N] [--load-gb N] [--no-load]" >&2; exit 2; }
[ -x "$player" ] || { echo "stress-play: no player at $player (run kormake)" >&2; exit 2; }

mapfile -t files < <(find "$dir" -maxdepth 1 -type f -iname '*.flac' | sort)
[ ${#files[@]} -gt 0 ] || { echo "stress-play: no .flac files in $dir" >&2; exit 2; }

source_dev="$(findmnt -no SOURCE -T "$dir")"
disk="$(lsblk -no PKNAME "$source_dev" 2>/dev/null | head -1)"
[ -n "$disk" ] || disk="$(basename "$source_dev")"
model="$(lsblk -dno MODEL,TRAN "/dev/$disk" 2>/dev/null | xargs || true)"

work="$dir/.blackwing-stress"
out="$(mktemp -d)"
pids=()
cleanup() {
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -rf "$work" "$out"
}
trap cleanup EXIT

echo "stress-play: ${#files[@]} files in $dir"
echo "stress-play: disk $disk ($model), ${seconds}s, load $([ $load = 1 ] && echo "on, ${load_gb} GB" || echo off), next every ${next}s (0: never)"

# Evict the music from the page cache, so the player reads from the disk.
python3 -I - "${files[@]}" <<'EOF'
import os, sys
for p in sys.argv[1:]:
    fd = os.open(p, os.O_RDONLY)
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    finally:
        os.close(fd)
EOF

if [ $load = 1 ]; then
  mkdir -p "$work"
  # Both ways: exFAT takes an O_DIRECT write and refuses the read.
  if dd if=/dev/zero of="$work/probe" bs=1M count=1 oflag=direct status=none 2>/dev/null &&
     dd if="$work/probe" of=/dev/null bs=1M iflag=direct status=none 2>/dev/null; then
    iflags=direct oflags=direct how="O_DIRECT"
  else
    iflags=nocache oflags=nocache how="no O_DIRECT here: buffered, dropping the cache"
  fi
  rm -f "$work/probe"
  echo "stress-play: writing the ${load_gb} GB load file ($how)"
  dd if=/dev/urandom of="$work/load" bs=1M count=$((load_gb * 1024)) oflag=$oflags conv=fsync status=none
  blocks=$((load_gb * 1024))
  reader() { # $1: sequential or random
    local n=0
    while :; do
      if [ "$1" = random ]; then
        dd if="$work/load" of=/dev/null bs=1M count=16 skip=$(((RANDOM * 32768 + RANDOM) % (blocks - 16))) iflag=$iflags status=none 2>/dev/null || true
        n=$((n + 16))
      else
        dd if="$work/load" of=/dev/null bs=1M iflag=$iflags status=none || true
        n=$((n + blocks))
      fi
      echo "$n" > "$out/read.$1.$BASHPID"
    done
  }
  writer() {
    local n=0
    while :; do
      dd if=/dev/zero of="$work/load" bs=1M count=256 seek=$((RANDOM % (blocks - 256))) oflag=$oflags conv=notrunc,fsync status=none || true
      n=$((n + 256))
      echo "$n" > "$out/write.$BASHPID"
    done
  }
  reader sequential & pids+=($!)
  reader random & pids+=($!)
  reader random & pids+=($!)
  writer & pids+=($!)
  sleep 2 # let the load settle before the first read
fi

if command -v iostat >/dev/null; then
  iostat -dxy "$disk" 5 > "$out/iostat" 2>&1 & pids+=($!)
fi

# The player, on a pseudo-terminal so it takes commands: next every N seconds, then the
# status (which has the underrun count) and quit.
feed() {
  local t=0
  while [ "$t" -lt "$seconds" ]; do
    local step=$(( next > 0 && next < seconds - t ? next : seconds - t ))
    sleep "$step"; t=$((t + step))
    if [ "$t" -lt "$seconds" ]; then echo "player next"; fi
  done
  echo "player status"
  sleep 0.5
  echo "quit"
}
cmd="/usr/bin/time -f 'stress-play: player cpu %U s user %S s sys, %M KB max resident' $(printf '%q' "$player") play"
for f in "${files[@]}"; do cmd+=" $(printf '%q' "$f")"; done
start=$(date +%s.%N)
feed | script -q -c "$cmd" /dev/null 2>&1 | tr -d '\r' | grep -v -E '^(player (next|status)|quit)$' | tee "$out/player"
end=$(date +%s.%N)

for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
wait 2>/dev/null || true
pids=()

underruns="$(grep -oE '[0-9]+ underruns' "$out/player" | tail -1 | grep -oE '^[0-9]+' || echo "?")"
played="$(grep -c '^playing ' "$out/player" || true)"
failed="$(grep -c '^cannot play ' "$out/player" || true)"
echo "stress-play: $(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.0f", b - a }') s; $played files started, $failed failed; underruns: $underruns"
if [ $load = 1 ]; then
  r=0; w=0
  for f in "$out"/read.*; do [ -f "$f" ] && r=$((r + $(cat "$f"))); done
  for f in "$out"/write.*; do [ -f "$f" ] && w=$((w + $(cat "$f"))); done
  echo "stress-play: the load read at least ${r} MB and wrote at least ${w} MB (completed passes only)"
fi
if [ -s "$out/iostat" ]; then
  # Columns by name: sysstat's -x layout differs between versions.
  awk -v d="$disk" '$1 == "Device" { for (i = 1; i <= NF; ++i) col[$i] = i; next }
                    $1 == d && ("%util" in col) { u += $col["%util"]; r += $col["r/s"]; w += $col["w/s"]; rk += $col["rkB/s"]; wk += $col["wkB/s"]; n++ }
                    END { if (n) printf "stress-play: %s busy %.0f%% on average: %.0f reads/s (%.0f MB/s), %.0f writes/s (%.0f MB/s)\n", d, u / n, r / n, rk / n / 1024, w / n, wk / n / 1024 }' "$out/iostat"
fi
[ "$underruns" = 0 ]
