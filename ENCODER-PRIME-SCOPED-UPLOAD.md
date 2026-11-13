# MT8183: Scoped Direct PRIME upload (eksperyment)

Bazą jest wersja `prime-qbuf-verified` z potwierdzonym `direct_prime=3300/3300` przy `FORCE_PUTIMAGE=1`.

## Zmiana

`vaCreateSurfaces2` przechowuje `VASurfaceAttribUsageHint`. Powierzchnia staje się kandydatem do uploadu enkodera, jeśli (1) ma jawny `VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER` bez flagi DECODER albo (2) ma zwykły hint, lecz w chwili tworzenia istnieje konfiguracja EncSlice, a nie istnieje konfiguracja VLD. Importowane powierzchnie pozostają poza automatyczną klasyfikacją. `vaDeriveImage` odmawia tylko dla takiego kandydata przy `LIBVA_V4L2_ENC_DMABUF=1` i `LIBVA_V4L2_ENC_DIRECT=1`; `vaPutImage` obsługuje wstępny upload nawet przed powiązaniem z kontekstem enkodera. Dotychczasowy `FORCE_PUTIMAGE=1` pozostaje tylko jako wyraźny tryb porównawczy.

**Ograniczenie:** VA API nie przekazuje VAConfigID do `vaCreateSurfaces2`, a FFmpeg może nie ustawiać UsageHint. Gdy dekoder i enkoder współistnieją, domyślny hint jest niejednoznaczny i nie wyłączamy `vaDeriveImage`. Nie należy interpretować prawidłowego MP4 jako dowodu użycia Direct PRIME: sprawdzić liczniki QBUF.

## Test 1: tylko enkoder, bez FORCE_PUTIMAGE

```sh
LIBVA_DRIVERS_PATH="$PWD/build/src" LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 LIBVA_V4L2_ENC_DIRECT=1 LIBVA_V4L2_ENC_DEBUG=1 \
ffmpeg -hide_banner -loglevel verbose -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i 'testsrc2=size=1920x1080:rate=30' \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi -profile:v constrained_baseline \
  -b:v 10M -bf 0 -g 30 -frames:v 300 -y /tmp/mt8183-prime-scoped.mp4 \
  2>&1 | tee /tmp/mt8183-prime-scoped.log
```

Szukaj komunikatów `surface purpose`, `vaDeriveImage select PutImage`, `VENC first successful direct PRIME QBUF` i `VENC input statistics`.

Jeśli brak `candidate=1`, bieżąca wersja FFmpeg nie dostarczyła wystarczającego sygnału. Nie wymuszaj globalnej blokady derive podczas równoczesnego dekodowania: wróć do stagingu, a klasyfikację dostosujemy na podstawie logu.

## Test 2: dekodowanie i kodowanie razem

Użyj **istniejącego, lokalnego pliku H.264** i przetestuj ten sam backend bez `FORCE_PUTIMAGE`:

```sh
LIBVA_DRIVERS_PATH="$PWD/build/src" LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 LIBVA_V4L2_ENC_DIRECT=1 LIBVA_V4L2_ENC_DEBUG=1 \
ffmpeg -hide_banner -loglevel verbose \
  -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -hwaccel_output_format vaapi \
  -i /tmp/mt8183-prime-verified.mp4 \
  -c:v h264_vaapi -profile:v constrained_baseline -b:v 10M -bf 0 -g 30 \
  -frames:v 300 -y /tmp/mt8183-transcode-scoped.mp4 \
  2>&1 | tee /tmp/mt8183-transcode-scoped.log
```

Ta komenda jest **testem kompatybilności**, a nie gwarancją sprzętowego transkodowania. Dekoder może produkować powierzchnie o formacie/pamięci niezgodnej z NV12M; wtedy test może się nie udać lub backend użyje fallbacku.

Weryfikacja plików:

```sh
ffprobe -v error -show_entries stream=profile,width,height,level -of default=noprint_wrappers=1 /tmp/mt8183-prime-scoped.mp4
ffmpeg -v error -i /tmp/mt8183-prime-scoped.mp4 -f null -
```

Nie zmieniano kolejek dekodera ani algorytmu synchronizacji i zwalniania DMA-BUF. Test składni na nagłówkach libva 2.22.0 to nie test działania na MT8183.
