# v0.1.0-rc1 — Audit 02: DMA-BUF END error propagation

Baza: Audit 01 (sprawdzona diagnostyka `ENC_DEBUG=0/1`). Ten patch nie zmienia detilingu, konfiguracji V4L2 ani kolejności QBUF.

## Zakres

- `DmaBufCpuWriteGuard` (`h264_encode.cc`): jawne `end()` propaguje błąd `DMA_BUF_IOCTL_SYNC END`; destruktor `noexcept` sprząta i raportuje błąd na stderr. Wszystkie fds są zamykane również po błędzie jednej płaszczyzny.
- `SourcePrimeGuard` (`h264_encode.cc`): analogiczne jawne zakończenie synchronizacji CPU READ przed opuszczeniem ścieżki staging copy.
- `EncoderPlaneCpuAccess` (`image.cc`): jawne `finish()` propaguje błąd; destruktor awaryjny tylko sprząta i loguje.
- Wywołano jawne `end()`/`finish()` po operacjach CPU w direct detile, RAM copy, staging copy, inicjalizacji PRIME NV12M oraz `vaPutImage`.

## Zachowanie błędów

`END` jest podejmowane dla wszystkich otwartych płaszczyzn, tymczasowe duplikaty fd zawsze są zamykane, a pierwszy błąd przekazywany wyżej (do istniejącej obsługi wyjątków / fallbacku). Podczas zwijania stosu destruktory nie rzucają wyjątków. Gdy START zawiedzie w połowie, wcześniej rozpoczęte dostępy są kończone awaryjnie.

Uwaga: błąd `END` w direct MM21→DMA-BUF może uruchomić istniejący fallback konwersji. Przed oddaniem niezsynchronizowanej pamięci do VENC ścieżka staging ponownie synchronizuje zapis; jej błąd `END` również przerywa operację.

## Regresja sprzętowa MT8183

1. Zbudować: `meson setup build --buildtype=release -Db_lto=true && ninja -C build`.
2. Powtórzyć kodowanie 300 klatek z `ENC_DMABUF=0` oraz `ENC_DMABUF=1`, z `ENC_DEBUG=1`. Oczekiwane liczniki: odpowiednio `mmap=300` i `dmabuf_staging=300`.
3. Wykonać transkodowanie 300 klatek `DIRECT_CONVERT=1`, `PADDING_ONLY=1`, `MEMORY_MODE=direct`, `DETILE_ORDER=tile`, `PREFETCH=0` i sprawdzić SHA-256 strumienia H.264.
4. Sprawdzić brak komunikatu `CPU sync END ... failed`, poprawność dekodowania oraz zgodność SHA z bazą.

Kontrola składni na maszynie autora nie zastępuje testu rzeczywistej synchronizacji na MT8183. Nie prowokować błędów w sterowniku jądra podczas testu normalnej pracy.
