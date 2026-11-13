# MT8183 Direct PRIME — diagnostyka ścieżki uploadu

Ta wersja NIE wymusza zero-copy ani nie zmienia wyboru buforów. Dodaje diagnostykę uruchamianą przez `LIBVA_V4L2_ENC_DEBUG=1`; pierwsze 96 zdarzeń w `image.cc` jest logowanych, żeby ograniczyć rozmiar logu.

Test 30 klatek do MP4 (w katalogu projektu):

```sh
LIBVA_DRIVERS_PATH="$PWD/build/src" LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 LIBVA_V4L2_ENC_DIRECT=1 LIBVA_V4L2_ENC_DEBUG=1 \
ffmpeg -hide_banner -loglevel verbose \
  -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i 'testsrc2=size=1920x1080:rate=30' \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi \
  -profile:v constrained_baseline -b:v 10M -bf 0 -g 30 \
  -frames:v 30 -y /tmp/mt8183-prime-diag.mp4 \
  2>&1 | tee /tmp/mt8183-prime-diag.log
```

Kluczowe linie:

- `createSurfaces2 encoder surface=...`: alokacja VA.
- `vaDeriveImage ... converted=...`: czy aplikacja wyprowadza mapowalny obraz powierzchni.
- `vaDeriveImage allocating single-buffer NV12 ...`: jednobuforowe NV12 blokuje późniejszą alokację dwóch DRM PRIME.
- `vaPutImage ...`: jak FFmpeg wgrywa klatki.
- `PRIME allocation skipped ...`: który istniejący bufor uniemożliwia próbę Direct PRIME.
- `vaPutImage target selected ...`: czy celem jest dwupłaszczyznowy Direct PRIME czy staging.
- `VENC input selection ...`: faktyczny wybór przed V4L2 QBUF.
- `VENC direct PRIME QBUF ...`: potwierdzenie importu bez kopiowania staging.

Prześlij fragment logu zawierający powyższe linie. Brak komunikatu `direct PRIME QBUF` oznacza, że ta ścieżka nie została udowodniona, nawet gdy MP4 jest poprawny.
