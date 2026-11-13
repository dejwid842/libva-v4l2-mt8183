# Direct PRIME — pomiar QBUF i poprawka wstępnego uploadu

Wersja bazowa: `libva-v4l2-mt8183-encoder-prime-putimage-experiment.zip`.

Zmiany:
- W trybie `LIBVA_V4L2_ENC_DMABUF=1 LIBVA_V4L2_ENC_DIRECT=1 LIBVA_V4L2_ENC_FORCE_PUTIMAGE=1` `vaPutImage` przyjmuje również pierwszy, wstępny upload NV12 na powierzchnię, która nie została jeszcze oznaczona jako wejście enkodera. Nie zmienia to zachowania innych trybów.
- Liczniki po udanym `VIDIOC_QBUF`: `direct_prime`, `dmabuf_staging`, `mmap`. Liczone są również niezgodne layouty i odrzucone próby importu.
- Pod koniec życia kontekstu (przy `LIBVA_V4L2_ENC_DEBUG=1`) wypisywany jest jeden wiersz `VENC input statistics`, z liczbą zakodowanych klatek i sumami QBUF. Wiersz może pojawić się po podsumowaniu FFmpeg.
- Komunikat `VENC first successful direct PRIME QBUF` jest drukowany przy pierwszym *rzeczywistym* sukcesie, a nie tylko gdy `frame_number==0`.

Uruchom test:

```sh
LIBVA_DRIVERS_PATH="$PWD/build/src" \
LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 \
LIBVA_V4L2_ENC_DIRECT=1 \
LIBVA_V4L2_ENC_FORCE_PUTIMAGE=1 \
LIBVA_V4L2_ENC_DEBUG=1 \
ffmpeg -hide_banner -loglevel verbose \
  -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i 'testsrc2=size=1920x1080:rate=30' \
  -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -profile:v constrained_baseline \
  -b:v 10M -bf 0 -g 30 -frames:v 300 \
  -y /tmp/mt8183-prime-verified.mp4 \
  2>&1 | tee /tmp/mt8183-prime-verified.log
```

Sprawdź `grep -E 'pre-encoder upload|first successful direct|VENC input statistics|Failed to write image' /tmp/mt8183-prime-verified.log`.

**Interpretacja:** `direct_prime` liczy udane przekazania dwóch istniejących płaszczyzn powierzchni do V4L2 (bez kopiowania staging); `dmabuf_staging` liczy QBUF po kopiowaniu do buforów DRM, a `mmap` po kopiowaniu do MMAP. Suma powinna odpowiadać liczbie zakodowanych klatek w nieprzerwanym teście. W trybie Direct PRIME upload `vaPutImage` wciąż kopiuje obraz do powierzchni; to nie jest zero-copy end-to-end.

Zachowaj wcześniejszą działającą wersję jako punkt odniesienia. Test wykonany lokalnie dotyczy kompilacji/składni, nie urządzenia MT8183.
