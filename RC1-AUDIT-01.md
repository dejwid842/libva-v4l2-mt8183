# v0.1.0-rc1 — audyt 01: diagnostyka i ścieżki awaryjne

**Baza:** transcode-prefetch-experiment. **Zakres zmiany:** tylko interpretacja `LIBVA_V4L2_ENC_DEBUG`, bez zmian algorytmu MM21, kodowania ani zarządzania DMA-BUF. To nie jest jeszcze wydanie rc1.

## Naprawione w tej poprawce

* Ujednolicono `LIBVA_V4L2_ENC_DEBUG`: niezdefiniowana, pusta i `0` oznaczają wyłączone logowanie; pozostałe niepuste wartości włączają debug. Wcześniej destruktor i część `image.cc` rozpoznawały `0`, lecz kilkanaście pozostałych warunków traktowało samo ustawienie zmiennej jako aktywację.
* Nie zmieniono zachowania przy `LIBVA_V4L2_ENC_DEBUG=1`, które dotychczas służyło benchmarkom.

## Znaleziska wymagające decyzji przed rc1 (bez ingerencji w tej zmianie)

1. **Obsługa błędów DMA_BUF_IOCTL_SYNC END.** `DmaBufCpuWriteGuard::end()` wywołuje `ioctl(...DMA_BUF_SYNC_END...)`, ale ignoruje wynik. W razie błędu synchronizacji dane mogłyby zostać przekazane urządzeniu bez prawidłowo zakończonego dostępu CPU. Trzeba zaprojektować rozróżnienie jawnego `finish()` z propagacją błędów i destruktora `noexcept` służącego wyłącznie do sprzątania. Nie naprawiać przez `throw` z destruktora.
2. **Przejścia DMA-BUF→MMAP.** `switch_input_to_mmap()` zwalnia buforowanie OUTPUT przed uruchomieniem streamingu. Weryfikacja: odrzucenie pierwszego QBUF i ponowny encode powinny dawać poprawny strumień oraz zero wycieków fd/mappingów.
3. **Zgłaszanie profilu H.264.** `VAProfileH264ConstrainedBaseline` mapuje się na `V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE`, a pomiar SPS pokazał profile_idc=66, constraint_set0_flag=0, constraint_set1_flag=0. Przed rc1 udokumentować lub skorygować ogłaszaną funkcjonalność, lecz nie przepisywać SPS bez dowodu zgodności.
4. **Zweryfikować logowanie `DIRECT CONVERT` bez ENC_DEBUG.** Jednorazowe potwierdzenie uruchomienia ścieżki jest w tej chwili bezwarunkowe; w finalnej wersji można przenieść je pod debug, bez zmiany działania.
5. **Niejednoznaczności środowiskowe.** Inne zmienne (`LIBVA_V4L2_TRACE`, `TRANSCODE_PROBE`) wykorzystują test obecności — osobny przegląd, bez ryzyka zmiany semantyki wszystkich opcji naraz.

## Minimalne testy dla tej poprawki

```sh
meson setup build --buildtype=release -Db_lto=true
ninja -C build
# W trzech uruchomieniach tego samego testu:
# unset LIBVA_V4L2_ENC_DEBUG; ...
# LIBVA_V4L2_ENC_DEBUG=0 ...
# LIBVA_V4L2_ENC_DEBUG=1 ...
# Dla 0/unset: brak diagnostycznych TRANSCODE MEMORY BENCH; dla 1: liczniki obecne.
# Dla każdej konfiguracji: poprawne kodowanie 300/3300 klatek i identyczny SHA-256 H.264.
```

**Status:** przegląd statyczny i drobny patch. Weryfikacja sprzętowa MT8183 i całościowy audyt zasobów nadal otwarte.
