# MT8183: eksperyment `vaCreateImage`/`vaPutImage` -> Direct PRIME NV12M

## Przyczyna

FFmpeg sprawdza `vaDeriveImage()` podczas inicjalizacji puli VAAPI, zanim powierzchnia jest
oznaczona `encoder_input`. Jeżeli test się powiedzie, FFmpeg mapuje obrazy przez
`vaDeriveImage()` i nie wywołuje `vaPutImage()`. W naszym backendzie ten tryb tworzy
pojedynczy `converted_buffer` NV12, blokujący Direct PRIME (dwa osobne obiekty DMA-BUF).

## Eksperyment

Ustawienie jednocześnie:

```
LIBVA_V4L2_ENC_DMABUF=1
LIBVA_V4L2_ENC_DIRECT=1
LIBVA_V4L2_ENC_FORCE_PUTIMAGE=1
```

sprawia, że `vaDeriveImage()` odpowiada `VA_STATUS_ERROR_UNIMPLEMENTED`. Już podczas
inicjalizacji puli FFmpeg rozpoznaje, że mapowanie bezpośrednie nie działa; zwykle
korzysta wtedy z `vaCreateImage()` i `vaPutImage()`. `vaPutImage()` alokuje dwa
bufory DRM, zapisuje do nich NV12 i `queue_input()` próbuje przekazać ich FD do
V4L2 przez `VIDIOC_QBUF` w trybie `V4L2_MEMORY_DMABUF`.

**Nie jest to zero-copy podczas `hwupload`**. Nadal następuje kopiowanie
`VAImage` -> dwa bufory DRM. Usuwamy natomiast *drugą* kopię (DRM staging -> V4L2),
jeżeli bezpośredni import powiedzie się. Bez flagi FORCE_PUTIMAGE wszystko działa jak
w poprzedniej wersji.

**Uwaga:** wyłączenie `vaDeriveImage()` jest globalne dla procesu, również dla
powierzchni dekodera. To jest celowo ograniczony eksperyment do kodowania z `lavfi`;
nie włączaj FORCE_PUTIMAGE podczas równoczesnego sprzętowego dekodowania.

## Test

W katalogu projektu:

```sh
meson setup build # tylko gdy katalog build jeszcze nie istnieje
ninja -C build

LIBVA_DRIVERS_PATH="$PWD/build/src" \
LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 \
LIBVA_V4L2_ENC_DIRECT=1 \
LIBVA_V4L2_ENC_FORCE_PUTIMAGE=1 \
LIBVA_V4L2_ENC_DEBUG=1 \
ffmpeg -hide_banner -loglevel verbose \
    -vaapi_device /dev/dri/renderD128 \
    -f lavfi -i 'testsrc2=size=1920x1080:rate=30' \
    -vf 'format=nv12,hwupload' -c:v h264_vaapi \
    -profile:v constrained_baseline -b:v 10M -bf 0 -g 30 \
    -frames:v 300 -y /tmp/mt8183-prime-putimage.mp4 \
    2>&1 | tee /tmp/mt8183-prime-putimage.log
```

Ważne wpisy logu:

- `vaDeriveImage deliberately disabled ...`: nasza opcjonalna zmiana jest aktywna.
- `Direct mapping disabled: deriving image does not work`: FFmpeg wykrył brak `derive`.
- `vaPutImage target selected: two-plane direct PRIME`: upload trafił do DRM NV12M.
- `VENC direct PRIME QBUF (no encoder staging copy)`: V4L2 zaakceptował DMA-BUF bez stagingu.
- `direct PRIME QBUF rejected ... MMAP fallback`: import się nie powiódł; plik może się mimo tego zakodować.

Jeśli FFmpeg zamiast tego zgłosi błąd mapowania obrazu, należy zachować log i użyć
sprawdzonego trybu (usunąć FORCE_PUTIMAGE lub ustawić go na `0`).

Sprawdzenie strumienia:

```sh
ffprobe -v error -show_entries stream=codec_name,profile,width,height,level \
    -of default=noprint_wrappers=1 /tmp/mt8183-prime-putimage.mp4
ffmpeg -v error -i /tmp/mt8183-prime-putimage.mp4 -f null -
```

Dla porównania warto uruchomić to samo polecenie bez FORCE_PUTIMAGE.

## Status testów

Sprawdzono składnię C++20 z nagłówkami libva 2.22.0. Test uruchomieniowy na
sprzęcie MT8183 pozostaje do wykonania.
