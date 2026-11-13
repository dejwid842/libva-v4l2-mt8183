#!/bin/sh
# Decodes the first frames of a video three times (software, hardware, hardware again) and compares them frame by
# frame with ffmpeg's PSNR filter. It tells which frames the hardware decoder gets wrong, and whether it does so
# reproducibly (hw1 against hw2) or differently every time (which points to a race or cache problem).
#
#   LIBVA_DRIVERS_PATH=$PWD/build/src LIBVA_DRIVER_NAME=v4l2 test/compare-with-software.sh video.mkv [frames] [dir]
#
# Extra environment variables (LIBVA_V4L2_TRACE, LIBVA_V4L2_QUIRKS, ...) apply to the hardware runs.

set -e

video="$1"
frames="${2:-300}"
dir="${3:-/tmp/compare}"

if [ -z "$video" ]; then
    echo "usage: $0 video [frames] [outdir]" >&2
    exit 1
fi

rm -rf "$dir"
mkdir -p "$dir/sw" "$dir/hw1" "$dir/hw2"

decode() { # hwdec outdir
    mpv --no-config --no-audio --no-sub --vo=image --vo-image-format=jpg --vo-image-jpeg-quality=95 \
        --vo-image-outdir="$2" --frames="$frames" --hwdec="$1" "$video" >"$2/mpv.log" 2>&1 || true
}

echo "software..."; decode no "$dir/sw"
echo "hardware (1)..."; decode vaapi-copy "$dir/hw1"
echo "hardware (2)..."; decode vaapi-copy "$dir/hw2"

compare() { # a b name
    ffmpeg -v error -i "$dir/$1/%08d.jpg" -i "$dir/$2/%08d.jpg" -lavfi "psnr=stats_file=$dir/$3.log" -f null - 2>&1 || true
    # frame number and PSNR of every frame below 40 dB
    awk '{ n = $1; sub("n:", "", n); p = $0; sub(".*psnr_avg:", "", p); sub(" .*", "", p);
           total++; if (p != "inf" && p + 0 < 40) { bad++; list = list " " n "(" int(p) ")" } }
         END { printf "%s: %d of %d frames below 40 dB%s\n", "'"$3"'", bad, total, (bad ? ":" list : "") }' "$dir/$3.log"
}

echo
compare sw hw1 hw1_vs_software
compare hw1 hw2 hw1_vs_hw2
echo
echo "Logs: $dir/hw1_vs_software.log, $dir/hw1_vs_hw2.log"
