# v0.1.0-rc1 — Audit 03: częściowa inicjalizacja kolejek V4L2

Baza: Audit 02, który przeszedł regresję enkodowania MMAP/DMA-BUF i transkodowania 3300 klatek.

## Potwierdzona luka w obsłudze błędów

`H264EncoderContext` najpierw wywołuje `VIDIOC_REQBUFS` i mapuje bufory OUTPUT, a następnie przydziela CAPTURE. Jeżeli alokacja/mapowanie CAPTURE rzuci wyjątek, destruktor `~H264EncoderContext()` nie zostanie wywołany (nieukończona konstrukcja obiektu C++). Same składniki klasy są niszczone, ale dotychczasowa ścieżka `release_queues()` nie była wywoływana w tym miejscu. Mogło to pozostawić kolejki sterownika jądra aktywne do zamknięcia urządzenia.

## Zmiana

Dodano `try/catch (...)` wyłącznie wokół końcowego przydziału kolejek MMAP / CAPTURE. Przy wyjątku wywołuje się istniejące `release_queues() noexcept`, a następnie ten sam wyjątek jest przekazywany dalej. Nie zmieniono normalnej ścieżki inicjalizacji, `QBUF`, `STREAMON`, DMA-BUF sync, przetwarzania obrazu ani wartości przełączników.

## Pozostałe obszary do przyszłej kontroli

- `stop_streaming()` i `release_queues()` ignorują błędy `STREAMOFF` i `REQBUFS(0)` zgodnie z dotychczasowym sprzątaniem `noexcept`. Można dodać ograniczone raportowanie błędów, ale nie należy zmieniać ich semantyki bez testów.
- Jeśli konfiguracja DMA-BUF zawiedzie *przed* rozpoczęciem końcowego przydziału kolejek, obsługuje to osobny dotychczasowy fallback. W szczególności wynik jego `REQBUFS(0)` wciąż nie jest sprawdzany. To osobny przypadek; nie został zmieniony w tym patchu.
- Należy w przyszłości rozważyć test fault-injection `VIDIOC_QUERYBUF` lub `mmap` na kontrolowanej instancji, a nie poprzez psucie realnego VENC.

## Regresja na MT8183

1. Ponownie zbudować źródła Audit 03.
2. Powtórzyć krótkie kodowanie `testsrc2` 300 klatek przy `ENC_DMABUF=0` i `1`; sprawdzić dekodowanie wyników i liczniki VENC.
3. Powtórzyć 300 klatek H.264 High → Baseline przy direct-convert; sprawdzić brak fallbacków i pełne dekodowanie.
4. Nie trzeba ponownie tworzyć dużych plików 3300-klatkowych w `/tmp`; krótka regresja jest adekwatna do tej zmiany.

Patch nie dowodzi jeszcze odporności na wszystkie awarie inicjalizacji. Testy sprzętowe są po stronie urządzenia MT8183.
