#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

using string = std::wstring;

#include "plugin.hpp"
#include "ride_codec_api.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

static PluginStartupInfo g_far{};
static FarStandardFunctions g_fsf{};

static const GUID PluginGuid = {
    0x9bba2c10,
    0x2f13,
    0x4c41,
    {0x9a, 0x77, 0x52, 0x49, 0x44, 0x45, 0x56, 0x31}};

static const GUID MenuGuid = {0x9bba2c11,
                              0x2f13,
                              0x4c41,
                              {0x9a, 0x77, 0x52, 0x49, 0x44, 0x45, 0x56, 0x31}};

static const wchar_t *const kClassName = L"FarRideViewerWindow";
enum class BackgroundMode { Checkers, Black, White };
static constexpr UINT WM_APP_IMAGE_LOADED = WM_APP + 17;

struct Codec {
  HMODULE module = nullptr;
  RideCodecInfo info{};
  RideCodecProbeFn probe = nullptr;
  RideCodecDecodeFn decode = nullptr;
  RideCodecFreeImageFn release = nullptr;
};

struct ViewerImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t channels = 4;
  std::vector<uint8_t> rgba;
};

static std::vector<Codec> g_codecs;
static bool g_codecsLoaded = false;

static std::wstring module_directory() {
  std::vector<wchar_t> path(512);
  for (;;) {
    const DWORD length = GetModuleFileNameW(
        reinterpret_cast<HMODULE>(&__ImageBase), path.data(),
        static_cast<DWORD>(path.size()));
    if (!length)
      return L".";
    if (length < path.size() - 1) {
      path.resize(length);
      std::wstring result(path.begin(), path.end());
      const size_t slash = result.find_last_of(L"\\/");
      return slash == std::wstring::npos ? L"." : result.substr(0, slash);
    }
    if (path.size() >= 32768)
      return L".";
    path.resize(path.size() * 2);
  }
}

static std::wstring utf8_to_wide(const char *text) {
  if (!text || !*text)
    return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         text, -1, nullptr, 0);
  if (length <= 0)
    return L"(invalid UTF-8 error text)";
  std::wstring result(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                      result.data(), length);
  if (!result.empty() && result.back() == L'\0')
    result.pop_back();
  return result;
}

template <typename Function>
static Function get_codec_symbol(HMODULE module, const char *name) {
  const FARPROC address = GetProcAddress(module, name);
  Function function{};
  static_assert(sizeof(function) == sizeof(address),
                "Windows function pointers must have equal sizes");
  std::memcpy(&function, &address, sizeof(function));
  return function;
}

static void load_codecs() {
  if (g_codecsLoaded)
    return;
  g_codecsLoaded = true;

  const std::wstring directory = module_directory() + L"\\Codecs";
  WIN32_FIND_DATAW found{};
  HANDLE search = FindFirstFileW((directory + L"\\*.dll").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE)
    return;

  do {
    const std::wstring path = directory + L"\\" + found.cFileName;
    HMODULE module = LoadLibraryW(path.c_str());
    if (!module)
      continue;

    const auto getInfo =
        get_codec_symbol<RideCodecGetInfoFn>(module, "RideCodecGetInfo");
    const auto probe =
        get_codec_symbol<RideCodecProbeFn>(module, "RideCodecProbe");
    const auto decode =
        get_codec_symbol<RideCodecDecodeFn>(module, "RideCodecDecode");
    const auto release =
        get_codec_symbol<RideCodecFreeImageFn>(module, "RideCodecFreeImage");

    if (!getInfo || !probe || !decode || !release) {
      FreeLibrary(module);
      continue;
    }

    Codec codec{};
    codec.module = module;
    codec.info.struct_size = sizeof(codec.info);
    if (!getInfo(&codec.info) ||
        codec.info.struct_size < sizeof(RideCodecInfo) ||
        codec.info.api_version != RIDE_CODEC_API_VERSION) {
      FreeLibrary(module);
      continue;
    }

    codec.info.name[sizeof(codec.info.name) - 1] = '\0';
    codec.info.extensions[sizeof(codec.info.extensions) - 1] = '\0';
    codec.probe = probe;
    codec.decode = decode;
    codec.release = release;
    try {
      g_codecs.push_back(codec);
    } catch (...) {
      FreeLibrary(module);
    }
  } while (FindNextFileW(search, &found));

  FindClose(search);

  // Raw SCR is identified only by exact file size. Check it before heuristic
  // headerless formats such as TGA, which may otherwise match by chance.
  std::stable_sort(g_codecs.begin(), g_codecs.end(),
                   [](const Codec &a, const Codec &b) {
                     const bool a_scr = std::strcmp(a.info.extensions, "scr") == 0;
                     const bool b_scr = std::strcmp(b.info.extensions, "scr") == 0;
                     return a_scr && !b_scr;
                   });
}

static bool is_scr_path(const std::wstring &path) {
  const size_t slash = path.find_last_of(L"\\/");
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos ||
      (slash != std::wstring::npos && dot < slash))
    return false;

  return CompareStringOrdinal(path.c_str() + dot, -1, L".scr", -1, TRUE) ==
         CSTR_EQUAL;
}

static bool probe_file(const std::wstring &path) {
  load_codecs();
  if (g_codecs.empty())
    return false;

  // SCR has no signature. Route .scr files to its decoder, where the
  // authoritative 6144/6912-byte size check is performed on the full input.
  if (is_scr_path(path)) {
    for (const auto &codec : g_codecs) {
      if (std::strcmp(codec.info.extensions, "scr") == 0)
        return true;
    }
    return false;
  }

  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;

  uint8_t header[8192]{};
  DWORD read = 0;
  const BOOL ok = ReadFile(file, header, sizeof(header), &read, nullptr);
  CloseHandle(file);
  if (!ok || !read)
    return false;

  for (const auto &codec : g_codecs) {
    if (codec.probe && codec.probe(header, read))
      return true;
  }
  return false;
}

struct CodecImageGuard {
  Codec *codec = nullptr;
  RideImage *image = nullptr;
  ~CodecImageGuard() {
    if (codec && image)
      codec->release(image);
  }
};

struct LoadContext {
  std::mutex mutex;

  bool done = false;
  bool success = false;

  ViewerImage image;
  std::vector<uint8_t> bgra;
  std::wstring error{};

  HBITMAP dib = nullptr;
  HDC dibDC = nullptr;
  HGDIOBJ previousDibObject = nullptr;

  ~LoadContext() {
    if (dibDC) {
      if (previousDibObject && previousDibObject != HGDI_ERROR) {
        SelectObject(dibDC, previousDibObject);
      }

      DeleteDC(dibDC);
      dibDC = nullptr;
    }

    if (dib) {
      DeleteObject(dib);
      dib = nullptr;
    }

    previousDibObject = nullptr;
  }
};

struct LoadJob {
  std::shared_ptr<LoadContext> context;
  std::wstring filename;
  HWND hwnd = nullptr;
};
struct ViewerState {
  bool loading = true;
  bool loadFailed = false;
  std::wstring loadError;
  std::shared_ptr<LoadContext> loadContext;
  ViewerImage image;
  std::wstring filename;

  HWND hwnd = nullptr;
  HANDLE loadThread = nullptr;

  double zoom = 1.0;
  double panX = 0.0;
  double panY = 0.0;

  bool fitToWindow = true;
  BackgroundMode background = BackgroundMode::Checkers;

  /*
     QOI RGBA -> Windows BGRA.
     Prepared once after loading.
  */
  std::vector<uint8_t> bgra;
  HBITMAP dib = nullptr;
  HDC dibDC = nullptr;
  HGDIOBJ previousDibObject = nullptr;
};
static ViewerState *g_state = nullptr;
static HBRUSH g_checkerBrush = nullptr;

// Защищает состояние окна, поток и флаг завершения.
static SRWLOCK g_stateLock = SRWLOCK_INIT;
static HANDLE g_viewerThread = nullptr;
static bool g_shuttingDown = false;

static HBRUSH get_checker_brush() {
  if (g_checkerBrush)
    return g_checkerBrush;

  constexpr int cs = 12;
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = cs * 2;
  bmi.bmiHeader.biHeight = -(cs * 2);
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void *bits = nullptr;
  HBITMAP dib =
      CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits)
    return nullptr;

  auto *px = static_cast<uint32_t *>(bits);
  for (int y = 0; y < cs * 2; ++y) {
    for (int x = 0; x < cs * 2; ++x) {
      const bool dark = ((x / cs) + (y / cs)) & 1;
      px[y * (cs * 2) + x] = dark ? 0x00DCDCDC : 0x00F5F5F5;
    }
  }

  g_checkerBrush = CreatePatternBrush(dib);
  DeleteObject(dib);
  return g_checkerBrush;
}

static void release_gdi_resources(ViewerState &state) {
  // Сначала восстанавливаем предыдущий объект DC.
  if (state.dibDC) {
    if (state.previousDibObject && state.previousDibObject != HGDI_ERROR) {
      SelectObject(state.dibDC, state.previousDibObject);
    }

    DeleteDC(state.dibDC);
    state.dibDC = nullptr;
  }

  // DIB удаляется после того, как он перестал
  // быть выбранным в DC.
  if (state.dib) {
    DeleteObject(state.dib);
    state.dib = nullptr;
  }

  state.previousDibObject = nullptr;
}

// Вызывать только при захваченном g_stateLock.
static void reap_finished_viewer_locked() {
  if (g_viewerThread &&
      WaitForSingleObject(g_viewerThread, 0) == WAIT_OBJECT_0) {
    CloseHandle(g_viewerThread);
    g_viewerThread = nullptr;
    g_state = nullptr;
  }
}

static bool focus_existing_viewer_if_busy() {
  HWND hwnd = nullptr;
  bool busy = false;

  AcquireSRWLockExclusive(&g_stateLock);

  if (g_shuttingDown) {
    busy = true;
  } else {
    reap_finished_viewer_locked();

    if (g_viewerThread) {
      busy = true;

      if (g_state)
        hwnd = g_state->hwnd;
    }
  }

  ReleaseSRWLockExclusive(&g_stateLock);

  if (hwnd)
    SetForegroundWindow(hwnd);

  return busy;
}

static bool load_file(const wchar_t *name, ViewerImage &image,
                      std::wstring &error) {
  error.clear();

  HANDLE file = CreateFileW(
      name, GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error = L"Stage: File I/O\nReason: Cannot open file.";
    return false;
  }

  LARGE_INTEGER fileSize{};
  if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0) {
    CloseHandle(file);
    error = L"Stage: File I/O\nReason: Cannot determine file size or file is empty.";
    return false;
  }
  if (static_cast<unsigned long long>(fileSize.QuadPart) > SIZE_MAX) {
    CloseHandle(file);
    error = L"Stage: File I/O\nReason: File is too large for this process.";
    return false;
  }

  std::vector<uint8_t> data;
  try {
    data.resize(static_cast<size_t>(fileSize.QuadPart));
  } catch (...) {
    CloseHandle(file);
    error = L"Stage: Memory\nReason: Unable to allocate the input buffer.";
    return false;
  }

  size_t done = 0;
  while (done < data.size()) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 20));
    DWORD got = 0;
    if (!ReadFile(file, data.data() + done, chunk, &got, nullptr) || got == 0) {
      CloseHandle(file);
      error = L"Stage: File I/O\nByte offset: " +
              std::to_wstring(static_cast<unsigned long long>(done)) +
              L"\nReason: File read failed.";
      return false;
    }
    done += got;
  }
  CloseHandle(file);

  load_codecs();
  Codec *selected = nullptr;

  // The SCR format has no signature, so the filename extension selects its
  // decoder. The decoder validates the complete input size (6144 or 6912).
  if (is_scr_path(name)) {
    for (auto &codec : g_codecs) {
      if (std::strcmp(codec.info.extensions, "scr") == 0) {
        selected = &codec;
        break;
      }
    }
  } else {
    for (auto &codec : g_codecs) {
      if (codec.probe && codec.probe(data.data(), data.size())) {
        selected = &codec;
        break;
      }
    }
  }

  if (!selected) {
    error = L"Stage: Codec selection\nReason: No installed codec recognizes this file.";
    return false;
  }

  RideImage decoded{};
  decoded.struct_size = sizeof(decoded);
  RideCodecError codecError{};
  const uint32_t decodedOk =
      selected->decode(data.data(), data.size(), &decoded, &codecError);
  CodecImageGuard imageGuard{selected, &decoded};
  codecError.message[sizeof(codecError.message) - 1] = '\0';

  if (!decodedOk) {
    error = L"Codec: " + utf8_to_wide(selected->info.name) +
            L"\nStage: Decode\nError code: " +
            std::to_wstring(codecError.code) + L"\nByte offset: " +
            std::to_wstring(static_cast<unsigned long long>(codecError.byte_offset)) +
            L"\nReason: " + utf8_to_wide(codecError.message);
    return false;
  }

  const uint64_t rowBytes64 = uint64_t(decoded.width) * 4u;
  if (decoded.struct_size < sizeof(RideImage) || !decoded.pixels ||
      decoded.pixel_format != 1 || !decoded.width || !decoded.height ||
      rowBytes64 > UINT32_MAX || decoded.stride < rowBytes64 ||
      decoded.width > static_cast<uint32_t>(std::numeric_limits<LONG>::max()) ||
      decoded.height > static_cast<uint32_t>(std::numeric_limits<LONG>::max())) {
    error = L"Codec: " + utf8_to_wide(selected->info.name) +
            L"\nStage: Output validation\nReason: Codec returned an unsupported image layout.";
    return false;
  }

  const uint64_t rowsBeforeLast = uint64_t(decoded.height) - 1u;
  if (rowsBeforeLast &&
      uint64_t(decoded.stride) >
          (UINT64_MAX - rowBytes64) / rowsBeforeLast) {
    error = L"Stage: Output validation\nReason: Decoded image size overflows.";
    return false;
  }
  const uint64_t requiredBytes =
      uint64_t(decoded.stride) * rowsBeforeLast + rowBytes64;
  const uint64_t outputBytes = rowBytes64 * uint64_t(decoded.height);
  if (requiredBytes > decoded.data_size ||
      outputBytes > std::numeric_limits<size_t>::max()) {
    error = L"Codec: " + utf8_to_wide(selected->info.name) +
            L"\nStage: Output validation\nReason: Decoded pixel buffer is smaller than its dimensions require.";
    return false;
  }

  try {
    image.rgba.resize(static_cast<size_t>(outputBytes));
  } catch (...) {
    error = L"Stage: Memory\nReason: Unable to allocate the decoded image buffer.";
    return false;
  }

  image.width = decoded.width;
  image.height = decoded.height;
  image.channels = 4;
  for (uint32_t y = 0; y < decoded.height; ++y) {
    std::memcpy(image.rgba.data() + size_t(y) * static_cast<size_t>(rowBytes64),
                decoded.pixels + size_t(y) * decoded.stride,
                static_cast<size_t>(rowBytes64));
  }
  return true;
}

static bool prepare_bgra(ViewerState &s) {
  const uint32_t width = s.image.width;
  const uint32_t height = s.image.height;

  if (width == 0 || height == 0 || s.image.rgba.empty())
    return false;

  const uint64_t pixelCount = uint64_t(width) * uint64_t(height);

  if (pixelCount > std::numeric_limits<size_t>::max() / 4) {
    return false;
  }

  const size_t expectedBytes = static_cast<size_t>(pixelCount) * 4;

  if (s.image.rgba.size() != expectedBytes)
    return false;

  if (width > static_cast<uint32_t>(std::numeric_limits<LONG>::max()) ||
      height > static_cast<uint32_t>(std::numeric_limits<LONG>::max())) {
    return false;
  }

  release_gdi_resources(s);

  try {
    s.bgra.resize(expectedBytes);
  } catch (...) {
    return false;
  }

  // QOI RGBA -> premultiplied BGRA для AlphaBlend.
  for (size_t i = 0; i < expectedBytes; i += 4) {
    const uint8_t r = s.image.rgba[i + 0];
    const uint8_t g = s.image.rgba[i + 1];
    const uint8_t b = s.image.rgba[i + 2];
    const uint8_t a = s.image.rgba[i + 3];

    s.bgra[i + 0] = static_cast<uint8_t>((uint32_t(b) * a) / 255);

    s.bgra[i + 1] = static_cast<uint8_t>((uint32_t(g) * a) / 255);

    s.bgra[i + 2] = static_cast<uint8_t>((uint32_t(r) * a) / 255);

    s.bgra[i + 3] = a;
  }

  BITMAPINFO bmi{};

  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = static_cast<LONG>(width);
  bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  s.dibDC = CreateCompatibleDC(nullptr);

  if (!s.dibDC) {
    release_gdi_resources(s);
    return false;
  }

  void *bits = nullptr;

  s.dib = CreateDIBSection(s.dibDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);

  if (!s.dib || !bits) {
    release_gdi_resources(s);
    return false;
  }

  HGDIOBJ previous = SelectObject(s.dibDC, s.dib);

  if (!previous || previous == HGDI_ERROR) {
    release_gdi_resources(s);
    return false;
  }

  s.previousDibObject = previous;

  std::memcpy(bits, s.bgra.data(), expectedBytes);

  return true;
}

static void publish_load_failure(ViewerState &state,
                                 const std::wstring &error) {
  const auto context = state.loadContext;
  if (!context)
    return;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    context->success = false;
    context->error = error;
    context->done = true;
  }
  if (state.hwnd)
    PostMessageW(state.hwnd, WM_APP_IMAGE_LOADED, 0, 0);
}

static DWORD WINAPI image_load_thread(LPVOID param) {
  std::unique_ptr<LoadJob> job(static_cast<LoadJob *>(param));
  if (!job || !job->context)
    return 1;

  ViewerState temporary;
  std::wstring error;
  bool success = false;
  try {
    success = load_file(job->filename.c_str(), temporary.image, error);
    if (success) {
      success = prepare_bgra(temporary);
      if (!success)
        error = L"Stage: Memory\nReason: Unable to prepare the display bitmap.";
    }
  } catch (...) {
    success = false;
    error = L"Stage: Decode\nReason: Unexpected exception while loading the image.";
  }

  if (!success && error.empty())
    error = L"Stage: Decode\nReason: Image loading failed without additional diagnostics.";

  {
    std::lock_guard<std::mutex> lock(job->context->mutex);
    job->context->success = success;
    job->context->error = error;
    if (success) {
      job->context->image = std::move(temporary.image);
      job->context->bgra = std::move(temporary.bgra);
      job->context->dib = temporary.dib;
      job->context->dibDC = temporary.dibDC;
      job->context->previousDibObject = temporary.previousDibObject;
      temporary.dib = nullptr;
      temporary.dibDC = nullptr;
      temporary.previousDibObject = nullptr;
    }
    job->context->done = true;
  }

  release_gdi_resources(temporary);
  PostMessageW(job->hwnd, WM_APP_IMAGE_LOADED, 0, 0);
  return 0;
}

static void start_image_load(ViewerState &state) {
  LoadJob *job = nullptr;
  try {
    job = new LoadJob{state.loadContext, state.filename, state.hwnd};
  } catch (...) {
    publish_load_failure(state, L"Stage: Memory\nReason: Unable to allocate the image loading task.");
    return;
  }

  HANDLE thread = CreateThread(nullptr, 0, image_load_thread, job, 0, nullptr);
  if (!thread) {
    delete job;
    publish_load_failure(state, L"Stage: Thread\nReason: Unable to start the image loading thread.");
    return;
  }
  state.loadThread = thread;
}

static void checker(HDC dc, const RECT &r) {
  HBRUSH brush = get_checker_brush();
  if (!brush) {
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));
    return;
  }
  FillRect(dc, &r, brush);
}

static void draw_background(HDC dc, const RECT &r, BackgroundMode mode) {
  switch (mode) {
  case BackgroundMode::Black:
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    break;
  case BackgroundMode::White:
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    break;
  case BackgroundMode::Checkers:
  default:
    checker(dc, r);
    break;
  }
}

/*
   Calculate the exact rectangle occupied by the image.
*/
static bool get_image_rect(HWND hwnd, const ViewerState &s, RECT &out) {
  RECT rc{};

  GetClientRect(hwnd, &rc);

  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;

  if (cw <= 0 || ch <= 0 || s.image.width == 0 || s.image.height == 0) {
    SetRectEmpty(&out);
    return false;
  }

  const double fit =
      std::min(static_cast<double>(cw) / static_cast<double>(s.image.width),
               static_cast<double>(ch) / static_cast<double>(s.image.height));

  if (fit <= 0.0) {
    SetRectEmpty(&out);
    return false;
  }

  const double scale = s.fitToWindow ? fit : s.zoom;

  const int dw = std::max(1, static_cast<int>(std::lround(
                                 static_cast<double>(s.image.width) * scale)));

  const int dh = std::max(1, static_cast<int>(std::lround(
                                 static_cast<double>(s.image.height) * scale)));

  const int x = static_cast<int>(std::lround((cw - dw) * 0.5 + s.panX));

  const int y = static_cast<int>(std::lround((ch - dh) * 0.5 + s.panY));

  out.left = x;
  out.top = y;
  out.right = x + dw;
  out.bottom = y + dh;

  return true;
}

static double get_fit_scale(HWND hwnd, const ViewerState &s) {
  RECT rc{};

  if (!GetClientRect(hwnd, &rc))
    return s.zoom > 0.0 ? s.zoom : 1.0;

  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;

  if (cw <= 0 || ch <= 0 || s.image.width == 0 || s.image.height == 0) {
    return s.zoom > 0.0 ? s.zoom : 1.0;
  }

  return std::min(static_cast<double>(cw) / s.image.width,
                  static_cast<double>(ch) / s.image.height);
}

static double get_effective_scale(HWND hwnd, const ViewerState &s) {
  const double scale = s.fitToWindow ? get_fit_scale(hwnd, s) : s.zoom;

  return scale > 0.0 ? scale : 1.0;
}

/*
   Invalidate the complete area that can have changed.

   IMPORTANT:

   RGN_XOR is NOT correct for scaling because the
   overlapping area contains different pixels after
   resampling.

   We therefore invalidate the UNION:

       old image + new image

   No background erase is requested. The old image
   remains visible until the new image is actually
   painted over it.
*/
static void invalidate_image_change(HWND hwnd, const RECT &oldRect,
                                    const RECT &newRect) {
  if (EqualRect(&oldRect, &newRect))
    return;

  RECT changed{};

  if (IsRectEmpty(&oldRect)) {
    changed = newRect;
  } else if (IsRectEmpty(&newRect)) {
    changed = oldRect;
  } else {
    changed.left = std::min(oldRect.left, newRect.left);

    changed.top = std::min(oldRect.top, newRect.top);

    changed.right = std::max(oldRect.right, newRect.right);

    changed.bottom = std::max(oldRect.bottom, newRect.bottom);
  }

  InvalidateRect(hwnd, &changed, FALSE);
}

/*
   Draw only the current paint region.

   BeginPaint clips the DC to ps.rcPaint, so there is
   no need to redraw the whole client area.
*/
static void draw_image(HWND hwnd, HDC dc, ViewerState &s,
                       const RECT &paintRect) {
  if (s.loading || s.loadFailed) {
    draw_background(dc, paintRect, s.background);

    RECT textRect{};
    GetClientRect(hwnd, &textRect);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(40, 40, 40));

    const wchar_t *text =
        s.loading ? L"Loading image..." : s.loadError.c_str();

    DrawTextW(dc, text, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    return;
  }

  RECT imageRect{};
  if (!get_image_rect(hwnd, s, imageRect)) {
    draw_background(dc, paintRect, s.background);
    return;
  }

  if (!s.dibDC || !s.dib) {
    draw_background(dc, paintRect, s.background);
    return;
  }

  const int dw = imageRect.right - imageRect.left;
  const int dh = imageRect.bottom - imageRect.top;

  BLENDFUNCTION bf{};
  bf.BlendOp = AC_SRC_OVER;
  bf.BlendFlags = 0;
  bf.SourceConstantAlpha = 255;
  bf.AlphaFormat = AC_SRC_ALPHA;

  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, nullptr);

  // Keep the original main-branch path for opaque RGB QOI images.
  if (s.image.channels != 4) {
    HRGN paintRegion = CreateRectRgnIndirect(&paintRect);
    HRGN imageRegion = CreateRectRgnIndirect(&imageRect);
    HRGN backgroundRegion = CreateRectRgn(0, 0, 0, 0);
    HRGN imagePaintRegion = CreateRectRgn(0, 0, 0, 0);

    if (!paintRegion || !imageRegion || !backgroundRegion ||
        !imagePaintRegion) {
      if (paintRegion) DeleteObject(paintRegion);
      if (imageRegion) DeleteObject(imageRegion);
      if (backgroundRegion) DeleteObject(backgroundRegion);
      if (imagePaintRegion) DeleteObject(imagePaintRegion);
      return;
    }

    CombineRgn(backgroundRegion, paintRegion, imageRegion, RGN_DIFF);
    CombineRgn(imagePaintRegion, paintRegion, imageRegion, RGN_AND);

    SelectClipRgn(dc, backgroundRegion);
    draw_background(dc, paintRect, s.background);
    SelectClipRgn(dc, imagePaintRegion);

    AlphaBlend(dc, imageRect.left, imageRect.top, dw, dh, s.dibDC, 0, 0,
               static_cast<int>(s.image.width),
               static_cast<int>(s.image.height), bf);

    SelectClipRgn(dc, nullptr);
    DeleteObject(paintRegion);
    DeleteObject(imageRegion);
    DeleteObject(backgroundRegion);
    DeleteObject(imagePaintRegion);
    return;
  }

  // RGBA path: compose into an off-screen surface. The checkerboard and
  // alpha blend are completed in memory, then copied to the window once.
  // This prevents transparent pixels from blending with stale window pixels
  // and avoids a separate erase/clear pass that could visibly flicker.
  const int paintWidth = paintRect.right - paintRect.left;
  const int paintHeight = paintRect.bottom - paintRect.top;
  if (paintWidth <= 0 || paintHeight <= 0)
    return;

  HDC memoryDC = CreateCompatibleDC(dc);
  if (!memoryDC)
    return;

  HBITMAP memoryBitmap = CreateCompatibleBitmap(dc, paintWidth, paintHeight);
  if (!memoryBitmap) {
    DeleteDC(memoryDC);
    return;
  }

  HGDIOBJ oldBitmap = SelectObject(memoryDC, memoryBitmap);
  if (!oldBitmap || oldBitmap == HGDI_ERROR) {
    DeleteObject(memoryBitmap);
    DeleteDC(memoryDC);
    return;
  }

  // Match the checker pattern's origin to the window's client coordinates.
  SetBrushOrgEx(memoryDC, -paintRect.left, -paintRect.top, nullptr);
  RECT memoryRect{0, 0, paintWidth, paintHeight};
  draw_background(memoryDC, memoryRect, s.background);

  SetStretchBltMode(memoryDC, HALFTONE);
  SetBrushOrgEx(memoryDC, 0, 0, nullptr);

  AlphaBlend(memoryDC, imageRect.left - paintRect.left,
              imageRect.top - paintRect.top, dw, dh, s.dibDC, 0, 0,
              static_cast<int>(s.image.width),
              static_cast<int>(s.image.height), bf);

  // BeginPaint clips the destination DC to the invalidated paint region.
  BitBlt(dc, paintRect.left, paintRect.top, paintWidth, paintHeight,
         memoryDC, 0, 0, SRCCOPY);

  SelectObject(memoryDC, oldBitmap);
  DeleteObject(memoryBitmap);
  DeleteDC(memoryDC);
}

static void update_title(HWND hwnd, const ViewerState &s) {

  if (s.loading) {
    const std::wstring title = s.filename + L" - Loading...";
    SetWindowTextW(hwnd, title.c_str());
    return;
  }

  if (s.loadFailed) {
    const std::wstring title = s.filename + L" - Load failed";
    SetWindowTextW(hwnd, title.c_str());
    return;
  }

  wchar_t mode[64]{};

  if (s.fitToWindow) {
    wcscpy_s(mode, L"Fit");
  } else {
    const int pct = static_cast<int>(std::lround(s.zoom * 100.0));
    swprintf_s(mode, L"%d%%", pct);
  }

  wchar_t title[512]{};

  swprintf_s(title,
             L"%s - %ux%u  %s  |  %s  |  "
             L"1 100%%  0 fit  +/- zoom  Space/LMB switch  Esc close",
             s.filename.c_str(), s.image.width, s.image.height,
             L"RGBA8", mode);

  SetWindowTextW(hwnd, title);
}

static void change_zoom(HWND hwnd, ViewerState &s, double newZoom) {
  RECT oldRect{};
  get_image_rect(hwnd, s, oldRect);

  s.fitToWindow = false;
  s.zoom = std::clamp(newZoom, 0.01, 32.0);

  RECT newRect{};
  get_image_rect(hwnd, s, newRect);

  invalidate_image_change(hwnd, oldRect, newRect);

  update_title(hwnd, s);
}

static void set_fit_mode(HWND hwnd, ViewerState &s) {
  RECT oldRect{};
  get_image_rect(hwnd, s, oldRect);

  s.fitToWindow = true;
  s.panX = 0;
  s.panY = 0;

  RECT newRect{};
  get_image_rect(hwnd, s, newRect);

  invalidate_image_change(hwnd, oldRect, newRect);

  update_title(hwnd, s);
}

static void set_100_percent(HWND hwnd, ViewerState &s) {
  RECT oldRect{};
  get_image_rect(hwnd, s, oldRect);

  s.fitToWindow = false;
  s.zoom = 1.0;
  s.panX = 0;
  s.panY = 0;

  RECT newRect{};
  get_image_rect(hwnd, s, newRect);

  invalidate_image_change(hwnd, oldRect, newRect);

  update_title(hwnd, s);
}

static void toggle_view_mode(HWND hwnd, ViewerState &s) {
  RECT oldRect{};
  get_image_rect(hwnd, s, oldRect);

  s.fitToWindow = !s.fitToWindow;

  s.panX = 0;
  s.panY = 0;

  RECT newRect{};
  get_image_rect(hwnd, s, newRect);

  invalidate_image_change(hwnd, oldRect, newRect);

  update_title(hwnd, s);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  ViewerState *s =
      reinterpret_cast<ViewerState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (msg) {
  case WM_CREATE: {
    auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);

    s = reinterpret_cast<ViewerState *>(cs->lpCreateParams);

    if (!s)
      return -1;

    AcquireSRWLockExclusive(&g_stateLock);

    // FAR уже завершает работу: не создаём новое окно.
    if (g_shuttingDown) {
      ReleaseSRWLockExclusive(&g_stateLock);
      return -1;
    }

    s->hwnd = hwnd;
    g_state = s;

    ReleaseSRWLockExclusive(&g_stateLock);

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));

    update_title(hwnd, *s);

    return 0;
  }

  case WM_APP_IMAGE_LOADED: {
    if (!s || !s->loadContext)
      return 0;

    const auto context = s->loadContext;

    {
      std::lock_guard<std::mutex> lock(context->mutex);

      if (!context->done)
        return 0;

      s->loading = false;
      s->loadFailed = !context->success;

      if (context->success) {
        s->image = std::move(context->image);
        s->bgra = std::move(context->bgra);

        // Принимаем GDI-объекты от рабочего потока.
        s->dib = context->dib;
        s->dibDC = context->dibDC;
        s->previousDibObject = context->previousDibObject;

        // Теперь ресурсами владеет ViewerState.
        context->dib = nullptr;
        context->dibDC = nullptr;
        context->previousDibObject = nullptr;

        s->loadError.clear();
      } else {
        s->loadError = context->error;
      }
    }

    update_title(hwnd, *s);
    InvalidateRect(hwnd, nullptr, FALSE);

    return 0;
  }

  case WM_PAINT: {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);

    if (s) {
      draw_image(hwnd, dc, *s, ps.rcPaint);
    } else {
      FillRect(dc, &ps.rcPaint,
               static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }

    EndPaint(hwnd, &ps);
    return 0;
  }

  case WM_ERASEBKGND:
    /*
       Never erase the background.
       This is what prevents the visible
       checkerboard -> image flicker.
    */
    return 1;

  case WM_SIZE:
    /*
       Resize changes the fit geometry.
       Let Windows repaint the affected area,
       but never erase it first.
    */
    InvalidateRect(hwnd, nullptr, FALSE);

    return 0;

  case WM_MOUSEWHEEL: {
    if (!s)
      return 0;

    const int delta = GET_WHEEL_DELTA_WPARAM(wp);

    if (delta == 0)
      return 0;

    const double currentScale = get_effective_scale(hwnd, *s);

    const double steps = static_cast<double>(delta) / WHEEL_DELTA;

    const double factor = std::pow(1.20, steps);

    change_zoom(hwnd, *s, currentScale * factor);

    return 0;
  }

  case WM_LBUTTONDOWN: {
    if (!s)
      return 0;

    /*
       No mouse dragging.
       Left click toggles Fit / explicit zoom.
    */
    toggle_view_mode(hwnd, *s);

    return 0;
  }

  case WM_KEYDOWN: {
    if (!s)
      break;

    switch (wp) {
    case 'B':
      s->background = BackgroundMode::Black;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case 'W':
      s->background = BackgroundMode::White;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case 'C':
      s->background = BackgroundMode::Checkers;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case VK_ESCAPE:
      DestroyWindow(hwnd);
      return 0;

    case VK_SPACE:
      toggle_view_mode(hwnd, *s);
      return 0;

    case VK_HOME: {
      RECT oldRect{};
      get_image_rect(hwnd, *s, oldRect);

      s->panX = 0;
      s->panY = 0;

      RECT newRect{};
      get_image_rect(hwnd, *s, newRect);

      invalidate_image_change(hwnd, oldRect, newRect);

      return 0;
    }

    case '1':
      set_100_percent(hwnd, *s);
      return 0;

    case '0':
      set_fit_mode(hwnd, *s);
      return 0;

    case VK_ADD:
    case VK_OEM_PLUS: {
      const double currentScale = get_effective_scale(hwnd, *s);

      change_zoom(hwnd, *s, currentScale * 1.20);

      return 0;
    }

    case VK_SUBTRACT:
    case VK_OEM_MINUS: {
      const double currentScale = get_effective_scale(hwnd, *s);

      change_zoom(hwnd, *s, currentScale / 1.20);

      return 0;
    }

    case VK_LEFT: {
      RECT oldRect{};
      get_image_rect(hwnd, *s, oldRect);

      s->panX += 32;

      RECT newRect{};
      get_image_rect(hwnd, *s, newRect);

      invalidate_image_change(hwnd, oldRect, newRect);

      return 0;
    }

    case VK_RIGHT: {
      RECT oldRect{};
      get_image_rect(hwnd, *s, oldRect);

      s->panX -= 32;

      RECT newRect{};
      get_image_rect(hwnd, *s, newRect);

      invalidate_image_change(hwnd, oldRect, newRect);

      return 0;
    }

    case VK_UP: {
      RECT oldRect{};
      get_image_rect(hwnd, *s, oldRect);

      s->panY += 32;

      RECT newRect{};
      get_image_rect(hwnd, *s, newRect);

      invalidate_image_change(hwnd, oldRect, newRect);

      return 0;
    }

    case VK_DOWN: {
      RECT oldRect{};
      get_image_rect(hwnd, *s, oldRect);

      s->panY -= 32;

      RECT newRect{};
      get_image_rect(hwnd, *s, newRect);

      invalidate_image_change(hwnd, oldRect, newRect);

      return 0;
    }
    }

    break;
  }

  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;

  case WM_DESTROY: {
    if (s) {
      AcquireSRWLockExclusive(&g_stateLock);

      if (s->hwnd == hwnd)
        s->hwnd = nullptr;

      ReleaseSRWLockExclusive(&g_stateLock);
    }

    PostQuitMessage(0);
    return 0;
  }
  }

  return DefWindowProcW(hwnd, msg, wp, lp);
}

static void finish_viewer_thread(ViewerState &state, HWND hwnd,
                                 HINSTANCE instance, bool unregisterClass) {
  // Вызывать не удерживая g_stateLock:
  // DestroyWindow отправляет WM_DESTROY.
  if (hwnd && IsWindow(hwnd))
    DestroyWindow(hwnd);
  
  // Дожидаемся фоновой загрузки до завершения потока плагина.
  if (state.loadThread) {
    WaitForSingleObject(state.loadThread, INFINITE);
    CloseHandle(state.loadThread);
    state.loadThread = nullptr;
  }

  release_gdi_resources(state);

  if (unregisterClass)
    UnregisterClassW(kClassName, instance);

  AcquireSRWLockExclusive(&g_stateLock);

  state.hwnd = nullptr;

  if (g_state == &state)
    g_state = nullptr;

  ReleaseSRWLockExclusive(&g_stateLock);
}

static DWORD WINAPI viewer_thread(LPVOID param) {
  std::unique_ptr<ViewerState> state(reinterpret_cast<ViewerState *>(param));

  const HINSTANCE instance = GetModuleHandleW(nullptr);

  AcquireSRWLockExclusive(&g_stateLock);

  if (g_shuttingDown) {
    ReleaseSRWLockExclusive(&g_stateLock);
    release_gdi_resources(*state);
    return 1;
  }

  g_state = state.get();

  ReleaseSRWLockExclusive(&g_stateLock);

  WNDCLASSW wc{};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = instance;
  wc.lpszClassName = kClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;

  bool registeredClass = false;

  WNDCLASSW existing{};

  if (GetClassInfoW(instance, kClassName, &existing)) {
    // Не используем чужой класс с тем же именем.
    if (existing.lpfnWndProc != wndproc) {
      finish_viewer_thread(*state, nullptr, instance, false);
      return 1;
    }
  } else if (RegisterClassW(&wc)) {
    registeredClass = true;
  } else {
    const DWORD error = GetLastError();

    // Допускаем уже зарегистрированный экземпляр
    // только при условии, что он принадлежит этому коду.
    if (error != ERROR_CLASS_ALREADY_EXISTS ||
        !GetClassInfoW(instance, kClassName, &existing) ||
        existing.lpfnWndProc != wndproc) {
      finish_viewer_thread(*state, nullptr, instance, false);
      return 1;
    }
  }

  RECT workArea{};

  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
    workArea.left = 0;
    workArea.top = 0;
    workArea.right = GetSystemMetrics(SM_CXSCREEN);
    workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
  }

  const int workWidth = workArea.right - workArea.left;
  const int workHeight = workArea.bottom - workArea.top;

  const int width = std::max(1, std::min(workWidth, 1200));
  const int height = std::max(1, std::min(workHeight, 900));

  const int x = workArea.left + (workWidth - width) / 2;
  const int y = workArea.top + (workHeight - height) / 2;

  HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"QOI",
                              WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr,
                              nullptr, instance, state.get());

  if (!hwnd) {
    finish_viewer_thread(*state, nullptr, instance, registeredClass);
    return 1;
  }

  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  start_image_load(*state);
  // Попытка передать фокус окну просмотрщика.
  HWND foreground = GetForegroundWindow();

  const DWORD farThreadId =
      foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;

  const DWORD ourThreadId = GetCurrentThreadId();

  if (farThreadId && farThreadId != ourThreadId)
    AttachThreadInput(farThreadId, ourThreadId, TRUE);

  BringWindowToTop(hwnd);
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);

  if (farThreadId && farThreadId != ourThreadId)
    AttachThreadInput(farThreadId, ourThreadId, FALSE);

  MSG msg{};
  BOOL messageResult = 0;

  for (;;) {
    messageResult = GetMessageW(&msg, nullptr, 0, 0);

    if (messageResult <= 0)
      break;

    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  finish_viewer_thread(*state, hwnd, instance, registeredClass);

  return messageResult == -1 ? 1 : 0;
}

/*
   Get the currently selected file from the active FAR panel.

   Current FAR API:
       FCTL_GETPANELINFO
       FCTL_GETCURRENTPANELITEM
       FCTL_GETPANELDIRECTORY

   FarPanelDirectory uses pointers to strings inside the
   caller-provided buffer. Param1 is the buffer size.
*/

static bool is_absolute_windows_path(const std::wstring &value) {
  if (value.size() >= 3 && std::iswalpha(value[0]) && value[1] == L':' &&
      (value[2] == L'\\' || value[2] == L'/')) {
    return true;
  }

  // UNC and extended-length paths (\\server\share, \\?\C:\...).
  return value.size() >= 2 &&
         ((value[0] == L'\\' && value[1] == L'\\') ||
          (value[0] == L'/' && value[1] == L'/'));
}

static bool get_current_filename(std::wstring &path) {
  if (!g_far.PanelControl)
    return false;

  PanelInfo pi{};

  pi.StructSize = sizeof(pi);

  if (!g_far.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &pi)) {
    return false;
  }

  if (pi.PanelType != PTYPE_FILEPANEL)
    return false;

  FarGetPluginPanelItem item{};

  item.StructSize = sizeof(item);

  const intptr_t required =
      g_far.PanelControl(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, &item);

  if (required <= 0)
    return false;

  std::vector<unsigned char> itemBuffer(static_cast<size_t>(required));

  item.StructSize = sizeof(item);
  item.Size = itemBuffer.size();
  item.Item = reinterpret_cast<PluginPanelItem *>(itemBuffer.data());

  if (!g_far.PanelControl(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, &item)) {
    return false;
  }

  if (!item.Item || !item.Item->FileName || !item.Item->FileName[0]) {
    return false;
  }

  const std::wstring filename = item.Item->FileName;

  // Plugin panels such as FAR's temporary panel may return a full path in
  // FileName and an empty CurDir. Use that path as-is instead of rejecting it
  // or incorrectly joining it to the process current directory.
  if (is_absolute_windows_path(filename)) {
    path = filename;
    return true;
  }

  /*
     First ask FAR for the required directory
     buffer size.
  */

  const intptr_t dirSize =
      g_far.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 0, nullptr);

  if (dirSize <= 0)
    return false;

  std::vector<unsigned char> dirBuffer(static_cast<size_t>(dirSize));

  auto *dir = reinterpret_cast<FarPanelDirectory *>(dirBuffer.data());

  dir->StructSize = sizeof(FarPanelDirectory);

  if (!g_far.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, dirSize, dir)) {
    return false;
  }

  if (!dir->Name || !dir->Name[0])
    return false;

  path = dir->Name;

  if (!path.empty() && path.back() != L'\\') {
    path.push_back(L'\\');
  }

  path += filename;

  return true;
}

static bool get_find_result_filename(HANDLE dialog,
                                      std::wstring &path) {
  if (!g_far.SendDlgMessage)
    return false;

  // FindFiles result dialog uses FD_LISTBOX == 1.
  constexpr intptr_t kFindResultsListId = 1;

  FarListPos position{};
  position.StructSize = sizeof(position);
  position.SelectPos = -1;
  position.TopPos = -1;
  g_far.SendDlgMessage(dialog, DM_LISTGETCURPOS, kFindResultsListId,
                       &position);

  if (position.SelectPos < 0)
    return false;

  const auto get_item_text = [&](intptr_t index,
                                 std::wstring &text) -> bool {
    FarListGetItem item{};
    item.StructSize = sizeof(item);
    item.ItemIndex = index;
    if (!g_far.SendDlgMessage(dialog, DM_LISTGETITEM, kFindResultsListId,
                              &item) ||
        !item.Item.Text) {
      return false;
    }

    text = item.Item.Text;
    return true;
  };

  std::wstring selectedText;
  if (!get_item_text(position.SelectPos, selectedText) ||
      selectedText.empty()) {
    return false;
  }

  // FindFiles inserts a separate directory-heading row before each group of
  // result files. It contains the full directory path and ends in a slash.
  std::wstring directory;
  for (intptr_t index = position.SelectPos - 1; index >= 0; --index) {
    std::wstring text;
    if (!get_item_text(index, text) || text.empty())
      continue;

    if (is_absolute_windows_path(text) &&
        (text.back() == L'\\' || text.back() == L'/')) {
      directory = std::move(text);
      break;
    }
  }

  if (directory.empty())
    return false;

  // Result rows contain formatted columns followed by the filename.
  // Prefer boundaries after known vertical separators, but fall back to
  // every suffix because FAR's box-drawing characters are configurable.
  std::vector<size_t> starts;
  starts.push_back(0);
  if (selectedText.front() == L' ')
    starts.push_back(1u);

  for (size_t i = 0; i < selectedText.size(); ++i) {
    const wchar_t ch = selectedText[i];
    if ((ch == L'\x2502' || (ch >= L'\x2500' && ch <= L'\x257F')) &&
        i + 1 < selectedText.size()) {
      starts.push_back(i + 1);
    }
  }

  std::sort(starts.begin(), starts.end());
  starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

  // First try the known display-column boundaries.
  const auto try_candidate = [&](size_t start) -> bool {
    if (start >= selectedText.size())
      return false;

    std::wstring candidate = directory;
    candidate.append(selectedText, start, std::wstring::npos);

    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      return false;
    }

    if (!probe_file(candidate))
      return false;

    path = std::move(candidate);
    return true;
  };

  for (const size_t start : starts) {
    if (try_candidate(start))
      return true;
  }

  // Fallback for custom separators / column layouts.
  for (size_t start = selectedText.size(); start-- > 0;) {
    if (try_candidate(start))
      return true;
  }
  return false;
}

static bool is_find_results_dialog(HANDLE dialog) {
  if (!g_far.SendDlgMessage)
    return false;

  // GUID FindFileResultId from Far Manager (Alt+F7 result dialog).
  static const GUID FindFileResultGuid = {
      0x536754EB,  0xC2D1, 0x4626,
      {0x93, 0x3F, 0xA2, 0x5D, 0x1E, 0x1D, 0x11, 0x0A}};

  DialogInfo dialogInfo{};
  dialogInfo.StructSize = sizeof(dialogInfo);
  return g_far.SendDlgMessage(dialog, DM_GETDIALOGINFO, 0, &dialogInfo) &&
         dialogInfo.Id == FindFileResultGuid;
}

static void show_error(const wchar_t *text) {
  if (!g_far.Message)
    return;

  const wchar_t *items[] = {L"Far Ride Viewer", L"", text, L"", L"OK"};

  g_far.Message(&PluginGuid, nullptr, FMSG_ERRORTYPE | FMSG_MB_OK, nullptr,
                items, 5, 1);
}

static bool show_image_file(const std::wstring &path) {
  if (focus_existing_viewer_if_busy())
    return true;

  ViewerState *state = nullptr;

  try {
    state = new ViewerState;
    state->filename = path;
    state->loadContext = std::make_shared<LoadContext>();
    state->loading = true;
    state->loadFailed = false;
  } catch (...) {
    delete state;

    show_error(L"Unable to allocate viewer state.");
    return true;
  }

  state->filename = path;

  HWND existingHwnd = nullptr;
  HANDLE thread = nullptr;
  bool busy = false;

  AcquireSRWLockExclusive(&g_stateLock);

  if (g_shuttingDown) {
    busy = true;
  } else {
    reap_finished_viewer_locked();

    if (g_viewerThread) {
      busy = true;

      if (g_state)
        existingHwnd = g_state->hwnd;
    } else {
      // Поток может стартовать сразу, но сначала ему
      // понадобится этот же lock. Поэтому дескриптор
      // будет сохранён до начала его работы.
      thread = CreateThread(nullptr, 0, viewer_thread, state, 0, nullptr);

      if (thread)
        g_viewerThread = thread;
    }
  }

  ReleaseSRWLockExclusive(&g_stateLock);

  if (busy) {
    release_gdi_resources(*state);
    delete state;

    if (existingHwnd)
      SetForegroundWindow(existingHwnd);

    return true;
  }

  if (!thread) {
    release_gdi_resources(*state);
    delete state;

    show_error(L"Unable to start viewer.");
    return true;
  }

  // Владение state передано viewer_thread().
  return true;
}

static bool show_image_for_current_file() {
  std::wstring path;
  if (!get_current_filename(path) || !probe_file(path))
    return false;
  return show_image_file(path);
}

/*
   FAR plugin initialization.
*/

extern "C" __declspec(dllexport) void WINAPI GetGlobalInfoW(GlobalInfo *info) {
  if (!info)
    return;

  info->StructSize = sizeof(*info);

  info->MinFarVersion =
      MAKEFARVERSION(FARMANAGERVERSION_MAJOR, FARMANAGERVERSION_MINOR,
                     FARMANAGERVERSION_REVISION, FARMANAGERVERSION_BUILD,
                     FARMANAGERVERSION_STAGE);

  info->Version = MAKEFARVERSION(1, 0, 0, 1, VS_RELEASE);

  info->Guid = PluginGuid;

  info->Title = L"Far Ride Viewer";

  info->Description = L"Modular image viewer for Far Manager";

  info->Author = L"Far Ride Viewer";

  info->Instance = nullptr;
}

extern "C" __declspec(dllexport) void WINAPI
SetStartupInfoW(const PluginStartupInfo *info) {
  if (!info)
    return;

  g_far = *info;

  if (info->FSF)
    g_fsf = *info->FSF;

  g_far.FSF = &g_fsf;
}

/*
   F11 -> Plugins -> Far Ride Viewer
*/

extern "C" __declspec(dllexport) void WINAPI GetPluginInfoW(PluginInfo *info) {
  if (!info)
    return;

  static const wchar_t *const menuStrings[] = {L"Far Ride Viewer"};

  static const UUID menuGuids[] = {MenuGuid};

  info->StructSize = sizeof(*info);

  info->Flags = PF_PRELOAD;

  info->DiskMenu.Guids = nullptr;
  info->DiskMenu.Strings = nullptr;
  info->DiskMenu.Count = 0;

  info->PluginMenu.Guids = menuGuids;
  info->PluginMenu.Strings = menuStrings;
  info->PluginMenu.Count = 1;

  info->PluginConfig.Guids = nullptr;
  info->PluginConfig.Strings = nullptr;
  info->PluginConfig.Count = 0;

  info->CommandPrefix = L"ride";

  info->Instance = nullptr;
}

extern "C" __declspec(dllexport) HANDLE WINAPI OpenW(const OpenInfo *info) {
  if (!info)
    return nullptr;

  if (info->OpenFrom == OPEN_PLUGINSMENU) {
    if (!info->Guid || *info->Guid != MenuGuid)
      return nullptr;

    show_image_for_current_file();
    return INVALID_HANDLE_VALUE;
  }

  if (info->OpenFrom == OPEN_COMMANDLINE) {
    const auto *c = reinterpret_cast<const OpenCommandLineInfo *>(info->Data);

    if (!c || !c->CommandLine || !c->CommandLine[0]) {
      show_error(L"Usage: ride: \"C:\\path\\image\"");
      return INVALID_HANDLE_VALUE;
    }

    std::wstring path = c->CommandLine;

    while (!path.empty() && iswspace(path.front())) {
      path.erase(path.begin());
    }

    while (!path.empty() && iswspace(path.back())) {
      path.pop_back();
    }

    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
      path = path.substr(1, path.size() - 2);
    }

    if (path.empty()) {
      show_error(L"Image file path is empty.");
      return INVALID_HANDLE_VALUE;
    }

    show_image_file(path);
    return INVALID_HANDLE_VALUE;
  }

  return nullptr;
}

extern "C" __declspec(dllexport) intptr_t WINAPI
ProcessConsoleInputW(ProcessConsoleInputInfo *info) {
  if (!info)
    return 0;

  if (info->StructSize < sizeof(ProcessConsoleInputInfo)) {
    return 0;
  }

  if (info->Rec.EventType != KEY_EVENT)
    return 0;

  const KEY_EVENT_RECORD &key = info->Rec.Event.KeyEvent;

  if (!key.bKeyDown)
    return 0;

  if (key.wVirtualKeyCode != VK_F3)
    return 0;

  if (key.dwControlKeyState &
      (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED |
       RIGHT_CTRL_PRESSED | SHIFT_PRESSED)) {
    return 0;
  }

  std::wstring path;

  if (!get_current_filename(path))
    return 0;

  if (!probe_file(path))
    return 0;

  show_image_file(path);

  return 1;
}

extern "C" __declspec(dllexport) intptr_t WINAPI
ProcessDialogEventW(const ProcessDialogEventInfo *info) {
  if (!info || info->StructSize < sizeof(ProcessDialogEventInfo) ||
      info->Event != DE_DLGPROCINIT || !info->Param) {
    return 0;
  }

  FarDialogEvent *event = info->Param;
  if (event->StructSize < sizeof(FarDialogEvent) ||
      event->Msg != DN_CONTROLINPUT || !event->Param2 ||
      !is_find_results_dialog(event->hDlg)) {
    return 0;
  }

  const INPUT_RECORD *record =
      static_cast<const INPUT_RECORD *>(event->Param2);
  if (record->EventType != KEY_EVENT)
    return 0;

  const KEY_EVENT_RECORD &key = record->Event.KeyEvent;
  if (!key.bKeyDown || key.wVirtualKeyCode != VK_F3)
    return 0;

  if (key.dwControlKeyState &
      (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED |
       RIGHT_CTRL_PRESSED | SHIFT_PRESSED)) {
    return 0;
  }

  std::wstring path;
  if (!get_find_result_filename(event->hDlg, path))
    return 0;

  // Let FAR handle F3 normally for files not recognized by an installed codec.
  // get_find_result_filename already probes the candidate before returning.
  show_image_file(path);
  event->Result = TRUE;
  return 1;
}

extern "C" __declspec(dllexport) void WINAPI
ExitFARW(const ExitInfo * /*info*/) {
  HWND hwnd = nullptr;

  AcquireSRWLockExclusive(&g_stateLock);

  g_shuttingDown = true;

  if (g_state)
    hwnd = g_state->hwnd;

  HANDLE thread = g_viewerThread;

  ReleaseSRWLockExclusive(&g_stateLock);

  if (hwnd && !PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
    // Резервный способ завершить цикл сообщений.
    const DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);

    if (threadId)
      PostThreadMessageW(threadId, WM_QUIT, 0, 0);
  }

  // Не удаляем блокировки и не выгружаем состояние,
  // пока рабочий поток использует код плагина.
  if (thread)
    WaitForSingleObject(thread, INFINITE);

  AcquireSRWLockExclusive(&g_stateLock);

  if (g_viewerThread) {
    CloseHandle(g_viewerThread);
    g_viewerThread = nullptr;
  }

  g_state = nullptr;

  ReleaseSRWLockExclusive(&g_stateLock);

  if (g_checkerBrush) {
    DeleteObject(g_checkerBrush);
    g_checkerBrush = nullptr;
  }
  for (auto &codec : g_codecs) {
    if (codec.module) {
      FreeLibrary(codec.module);
      codec.module = nullptr;
    }
  }
  g_codecs.clear();
  g_codecsLoaded = false;
}