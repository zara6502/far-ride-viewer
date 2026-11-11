# Far Ride Viewer

Модульный плагин просмотра изображений для Far Manager 3.x.

Основой окна просмотра, взаимодействия с Far Manager, асинхронной загрузки и управления изображением служит проверенная реализация из проекта [FarQoiViewer](https://github.com/zara6502/far-qoi-viewer). Декодирование вынесено во внешние DLL-модули через стабильный C ABI из `include/ride_codec_api.h`.

## Возможности

- Перехват `F3` по сигнатуре содержимого; ZX Spectrum SCR определяется по точному размеру файла.
- Независимые кодеки в DLL, расположенных в каталоге `Codecs` рядом с `FarRideViewer.dll`.
- Кодеки: `qoi.dll` (QOI/QOIMT), `pcx.dll` (PCX), `stb_image.dll` (BMP/TGA/JPEG/PNG/GIF), `zx_spectrum.dll` (ZX Spectrum SCR).
- `stb_image.dll` поддерживает BMP, TGA, JPEG baseline/progressive, PNG и GIF. Для анимированного GIF отображается только первый кадр; воспроизведение анимации не реализовано. Изображения выдаются в RGBA8 с сохранением прозрачности.
- В основе `stb_image.dll` используется `stb_image` v2.31 (upstream `nothings/stb`, commit `68b75304a3c6e5b18472ced74d096fef571c2e33`).

- `zx_spectrum.dll` декодирует SCR в 256×192: 6144 байта — чёрно-белый bitmap; 6912 байт — bitmap и 768 байт атрибутов с 16-цветной палитрой. Распознавание строго по размеру, сигнатуры в файле нет.
- 12-битный JPEG и JPEG с арифметическим кодированием не поддерживаются библиотекой stb_image.
- Чтение файлов и декодирование в фоновом потоке.
- Сохранение пропорций изображения.
- Режимы вписывания в окно и 1:1.
- Масштабирование колёсиком мыши и клавишами `+` / `-`.
- Панорамирование стрелками клавиатуры.
- Прозрачность RGBA с корректным альфа-композитингом поверх фона.
- Выбор фона: шахматка, чёрный или белый.
- Диагностика ошибок чтения, декодирования и структуры результата кодека.
- Одновременно поддерживается одно окно просмотра; повторный вызов переводит его на передний план.
- Файлы, для которых нет подходящего кодека, остаются в обработке Far Manager.

## Управление

| Клавиша / действие | Результат |
|---|---|
| `0` | Вписать изображение в окно |
| `1` | Масштаб 100% (1:1) |
| `+` / `-` | Увеличить / уменьшить масштаб |
| Колёсико мыши | Изменить масштаб |
| Стрелки | Переместить изображение |
| `Home` | Центрировать изображение |
| `Space` / левая кнопка мыши | Переключить Fit / zoom |
| `Esc` | Закрыть окно |
| `w` | Белый фон |
| `b` | Чёрный фон |
| `c` | Шахматный фон |

## Команда Far

Префикс плагина — `ride:`:

```text
ride: C:\\path\\to\\image.qoi
ride: "C:\\path with spaces\\image.qoi"
```

Также можно выбрать поддерживаемый файл в панели Far и нажать `F3`.

## Сборка в MSYS2 UCRT64

Требуются MinGW-w64 GCC, CMake 3.20+, Ninja, Git и исходники Far Manager с `far/plugin.hpp`.

Если каталоги расположены рядом:

```bash
git clone --depth 1 https://github.com/FarGroup/FarManager.git ../FarManager

cmake -S . -B build -G Ninja \\
  -DCMAKE_BUILD_TYPE=Release \\
  -DFAR_SOURCE_DIR="$PWD/../FarManager"

cmake --build build
ctest --test-dir build --output-on-failure
```

Ожидаемые файлы:

```text
build/
|-- FarRideViewer.dll
\`-- Codecs/
    |-- qoi.dll
    |-- pcx.dll
    |-- stb_image.dll
    |-- zx_spectrum.dll
    |-- qoi_codec_test.exe
    |-- pcx_codec_test.exe
    |-- stb_image_codec_test.exe
    \`-- zx_spectrum_codec_test.exe
```

## Установка

Скопируй DLL с сохранением структуры:

```text
Plugins/
\`-- FarRideViewer/
    |-- FarRideViewer.dll
    \`-- Codecs/
        |-- qoi.dll
        |-- pcx.dll
        |-- stb_image.dll
        \`-- zx_spectrum.dll
```

Перезапусти Far Manager после установки.

## ABI декодера

Кодеки экспортируют четыре функции:

- `RideCodecGetInfo`
- `RideCodecProbe`
- `RideCodecDecode`
- `RideCodecFreeImage`

API v1 возвращает top-down RGBA8. Память изображения освобождается тем же модулем, который её выделил. Host проверяет версию ABI, наличие всех точек входа, формат пикселей, размеры, stride и объём буфера.

## Разработка

- Руководство по созданию и подключению сторонних кодеков: [docs/CODEC_DEVELOPER_GUIDE.md](docs/CODEC_DEVELOPER_GUIDE.md).
- Полный пример отдельной DLL, CMake-сборки и теста: [examples/ppm_codec/](examples/ppm_codec/).
- План работ и проверки перед публикацией: [docs/ROADMAP.md](docs/ROADMAP.md).
