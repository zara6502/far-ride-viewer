# Far Ride Viewer

Модульный плагин просмотра изображений для Far Manager 3.x.

Основой окна просмотра, взаимодействия с Far Manager, асинхронной загрузки и управления изображением служит проверенная реализация из проекта [FarQoiViewer](https://github.com/zara6502/far-qoi-viewer). Декодирование вынесено во внешние DLL-модули через стабильный C ABI из `include/ride_codec_api.h`.

## Возможности

- Перехват `F3` по сигнатуре содержимого, а не по расширению файла.
- Независимые кодеки в DLL, расположенных в каталоге `Codecs` рядом с `FarRideViewer.dll`.
- Кодеки: `qoi.dll` (QOI/QOIMT), `pcx.dll` (PCX), `stb_image.dll` (BMP/TGA/JPEG).
- `stb_image.dll` поддерживает JPEG baseline и progressive; 12-битный JPEG и JPEG с арифметическим кодированием не поддерживаются библиотекой stb_image.
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
    |-- qoi_codec_test.exe
    |-- pcx_codec_test.exe
    \`-- stb_image_codec_test.exe
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
        \`-- stb_image.dll
```

Перезапусти Far Manager после установки.

## ABI декодера

Кодеки экспортируют четыре функции:

- `RideCodecGetInfo`
- `RideCodecProbe`
- `RideCodecDecode`
- `RideCodecFreeImage`

API v1 возвращает top-down RGBA8. Память изображения освобождается тем же модулем, который её выделил. Host проверяет версию ABI, наличие всех точек входа, формат пикселей, размеры, stride и объём буфера.
