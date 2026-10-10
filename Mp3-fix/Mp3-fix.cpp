#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <sstream>
#include <atomic>
#include <algorithm>

#include <windows.h>
#include <gdiplus.h>
#include <objbase.h>
#include <wincodec.h> // WIC for WebP decoding & JPEG conversion

// WebView2 & COM Headers
#include <wil/com.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

// Audio & Web Headers
#include <curl/curl.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

using namespace Microsoft::WRL;
namespace fs = std::filesystem;

// ========================================================================
// JSON String Unescaping Helper (Fixes Escaped Slashes in WebView2)
// ========================================================================
std::string UnescapeJsonString(const std::string& input) {
    if (input.empty() || input == "null" || input == "\"\"") return "";

    std::string s = input;
    // Strip leading and trailing quotes from JSON string representation
    while (s.length() >= 2 && s.front() == '\"' && s.back() == '\"') {
        s = s.substr(1, s.length() - 2);
    }

    if (s.empty() || s == "null") return "";

    std::string result;
    result.reserve(s.length());

    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '\\' && i + 1 < s.length()) {
            char next = s[i + 1];
            if (next == '/' || next == '\\' || next == '"') {
                result += next;
                i++;
            }
            else if (next == 'n') {
                result += '\n';
                i++;
            }
            else if (next == 'r') {
                result += '\r';
                i++;
            }
            else if (next == 't') {
                result += '\t';
                i++;
            }
            else {
                result += s[i];
            }
        }
        else {
            result += s[i];
        }
    }
    return result;
}

// ========================================================================
// Memory & Download Utilities
// ========================================================================

static size_t WriteMemoryCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    std::vector<char>* mem = static_cast<std::vector<char>*>(userp);
    mem->insert(mem->end(), static_cast<char*>(contents), static_cast<char*>(contents) + realsize);
    return realsize;
}

std::vector<char> DownloadImageToMemory(CURL* curl, const std::string& url) {
    std::vector<char> imageData;
    if (url.empty()) return imageData;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &imageData);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);

    if (res != CURLE_OK || httpCode != 200) {
        std::cerr << "   [!] Failed to download image (HTTP " << httpCode << "): " << curl_easy_strerror(res) << "\n";
        imageData.clear();
    }
    return imageData;
}

std::string UrlEncode(CURL* curl, const std::string& text) {
    if (text.empty()) return "";
    char* encoded = curl_easy_escape(curl, text.c_str(), static_cast<int>(text.length()));
    std::string result(encoded);
    curl_free(encoded);
    return result;
}

// ========================================================================
// Standalone Image Loader (Supports WebP/PNG/JPG & detaches stream memory)
// ========================================================================
Gdiplus::Bitmap* LoadBitmapFromMemory(std::vector<char>& data) {
    if (data.empty()) return nullptr;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!hMem) return nullptr;

    void* pMem = GlobalLock(hMem);
    memcpy(pMem, data.data(), data.size());
    GlobalUnlock(hMem);

    // 1. Try WIC decoding first (handles JPG, PNG, WEBP, BMP, GIF consistently)
    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pFactory));

    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(hMem, FALSE, &pStream) == S_OK && pFactory) {
        IWICBitmapDecoder* pDecoder = nullptr;
        hr = pFactory->CreateDecoderFromStream(pStream, NULL, WICDecodeMetadataCacheOnDemand, &pDecoder);

        if (SUCCEEDED(hr) && pDecoder) {
            IWICBitmapFrameDecode* pFrame = nullptr;
            if (SUCCEEDED(pDecoder->GetFrame(0, &pFrame))) {
                IWICFormatConverter* pConverter = nullptr;
                pFactory->CreateFormatConverter(&pConverter);

                if (pConverter) {
                    if (SUCCEEDED(pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppPBGRA,
                        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {

                        UINT width = 0, height = 0;
                        pConverter->GetSize(&width, &height);

                        if (width > 0 && height > 0) {
                            Gdiplus::Bitmap* wicBmp = new Gdiplus::Bitmap(width, height, PixelFormat32bppPARGB);
                            Gdiplus::BitmapData bmpData;
                            Gdiplus::Rect rect(0, 0, width, height);

                            if (wicBmp->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bmpData) == Gdiplus::Ok) {
                                UINT stride = bmpData.Stride;
                                UINT bufferSize = stride * height;
                                pConverter->CopyPixels(NULL, stride, bufferSize, static_cast<BYTE*>(bmpData.Scan0));
                                wicBmp->UnlockBits(&bmpData);

                                // Convert in-memory image to standard JPEG bytes for TagLib ID3 APIC compatibility
                                IWICBitmapEncoder* pEncoder = nullptr;
                                IStream* pOutStream = nullptr;
                                CreateStreamOnHGlobal(NULL, TRUE, &pOutStream);

                                if (pOutStream && SUCCEEDED(pFactory->CreateEncoder(GUID_ContainerFormatJpeg, NULL, &pEncoder))) {
                                    if (SUCCEEDED(pEncoder->Initialize(pOutStream, WICBitmapEncoderNoCache))) {
                                        IWICBitmapFrameEncode* pNewFrame = nullptr;
                                        IPropertyBag2* pPropertybag = nullptr;

                                        if (SUCCEEDED(pEncoder->CreateNewFrame(&pNewFrame, &pPropertybag))) {
                                            pNewFrame->Initialize(pPropertybag);
                                            pNewFrame->SetSize(width, height);
                                            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
                                            pNewFrame->SetPixelFormat(&format);

                                            IWICFormatConverter* pJpegConverter = nullptr;
                                            pFactory->CreateFormatConverter(&pJpegConverter);
                                            if (pJpegConverter) {
                                                pJpegConverter->Initialize(pFrame, GUID_WICPixelFormat24bppBGR,
                                                    WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
                                                pNewFrame->WriteSource(pJpegConverter, NULL);
                                                pJpegConverter->Release();
                                            }

                                            pNewFrame->Commit();
                                            pNewFrame->Release();
                                        }
                                        pEncoder->Commit();
                                        pEncoder->Release();

                                        STATSTG statstg;
                                        pOutStream->Stat(&statstg, STATFLAG_NONAME);
                                        ULONG jpegSize = statstg.cbSize.LowPart;

                                        if (jpegSize > 0) {
                                            HGLOBAL hOutMem = NULL;
                                            GetHGlobalFromStream(pOutStream, &hOutMem);
                                            if (hOutMem) {
                                                void* pOutBytes = GlobalLock(hOutMem);
                                                if (pOutBytes) {
                                                    data.assign(static_cast<char*>(pOutBytes), static_cast<char*>(pOutBytes) + jpegSize);
                                                    GlobalUnlock(hOutMem);
                                                }
                                            }
                                        }
                                    }
                                    pOutStream->Release();
                                }

                                pConverter->Release();
                                pFrame->Release();
                                pDecoder->Release();
                                pStream->Release();
                                pFactory->Release();
                                GlobalFree(hMem);

                                return wicBmp;
                            }
                            delete wicBmp;
                        }
                    }
                    pConverter->Release();
                }
                pFrame->Release();
            }
            pDecoder->Release();
        }
    }

    if (pFactory) pFactory->Release();
    if (pStream) pStream->Release();

    // 2. Fallback to standard GDI+ with Clone (standalone pixel copy)
    IStream* pGdiStream = nullptr;
    Gdiplus::Bitmap* resultBmp = nullptr;
    if (CreateStreamOnHGlobal(hMem, FALSE, &pGdiStream) == S_OK) {
        Gdiplus::Bitmap* tmpBmp = Gdiplus::Bitmap::FromStream(pGdiStream);
        if (tmpBmp && tmpBmp->GetLastStatus() == Gdiplus::Ok) {
            // Clone tmpBmp to create a standalone bitmap independent of pGdiStream and hMem!
            resultBmp = tmpBmp->Clone(0, 0, tmpBmp->GetWidth(), tmpBmp->GetHeight(), PixelFormat32bppPARGB);
        }
        if (tmpBmp) delete tmpBmp;
        pGdiStream->Release();
    }

    GlobalFree(hMem);
    return resultBmp;
}

// ========================================================================
// WebView2 Scraper Result Struct
// ========================================================================
struct WebScrapeResult {
    std::string appleArtUrl;
    std::string spotifyArtUrl;
};

// ========================================================================
// WebView2 Scraper Class
// ========================================================================
class WebViewScraper {
private:
    HWND hwnd = nullptr;
    wil::com_ptr<ICoreWebView2Environment> env;
    wil::com_ptr<ICoreWebView2Controller> controller;
    wil::com_ptr<ICoreWebView2> webview;
    EventRegistrationToken navToken = {};

    std::atomic<bool> navigationComplete{ false };

    HRESULT WaitForCompletion(std::atomic<bool>& flag, int timeoutMs = 15000) {
        int elapsed = 0;
        MSG msg;
        while (!flag.load() && elapsed < timeoutMs) {
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            Sleep(50);
            elapsed += 50;
        }
        return flag.load() ? S_OK : E_FAIL;
    }

public:
    bool Initialize() {
        WNDCLASS wc = {};
        wc.lpfnWndProc = DefWindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"WebViewVisibleHost";
        RegisterClass(&wc);

        hwnd = CreateWindowEx(0, L"WebViewVisibleHost", L"WebView2 Scraper Monitor",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
            nullptr, nullptr, wc.hInstance, nullptr);

        ShowWindow(hwnd, SW_SHOW);

        std::atomic<bool> envReady{ false };
        CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [&](HRESULT res, ICoreWebView2Environment* e) -> HRESULT {
                    if (FAILED(res)) return res;
                    env = e;
                    env->CreateCoreWebView2Controller(hwnd,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [&](HRESULT res, ICoreWebView2Controller* c) -> HRESULT {
                                if (FAILED(res)) return res;
                                controller = c;
                                controller->get_CoreWebView2(&webview);

                                RECT bounds;
                                GetClientRect(hwnd, &bounds);
                                controller->put_Bounds(bounds);

                                webview->add_NavigationCompleted(
                                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                            this->navigationComplete = true;
                                            return S_OK;
                                        }).Get(), &navToken);

                                envReady = true;
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());

        return WaitForCompletion(envReady, 10000) == S_OK;
    }

    std::string ExecuteScriptSync(const std::wstring& script) {
        std::atomic<bool> scriptDone{ false };
        std::string scriptResult = "";

        webview->ExecuteScript(script.c_str(),
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [&](HRESULT, PCWSTR result) -> HRESULT {
                    if (result) {
                        std::wstring ws(result);
                        std::string raw(ws.begin(), ws.end());
                        scriptResult = UnescapeJsonString(raw);
                    }
                    scriptDone = true;
                    return S_OK;
                }).Get());

        WaitForCompletion(scriptDone, 5000);
        return scriptResult;
    }

    WebScrapeResult ScrapeDirect(CURL* curl, const std::string& artist, const std::string& title, const std::string& album) {
        WebScrapeResult res;

        std::wstring findAppleAlbumLinkScript = L"(() => { "
            L"  let links = document.querySelectorAll('a[href*=\"/album/\"]'); "
            L"  if (links.length > 0 && links[0].href) return links[0].href; "
            L"  let allLinks = document.querySelectorAll('a'); "
            L"  for(let i=0; i<allLinks.length; i++) { "
            L"    if(allLinks[i].href && allLinks[i].href.includes('/album/')) { "
            L"      return allLinks[i].href; "
            L"    } "
            L"  } "
            L"  return ''; "
            L"})()";

        std::wstring findSpotifyTrackLinkScript = L"(() => { "
            L"  let links = document.querySelectorAll('a[href*=\"/track/\"]'); "
            L"  if (links.length > 0 && links[0].href) return links[0].href; "
            L"  let albumLinks = document.querySelectorAll('a[href*=\"/album/\"]'); "
            L"  if (albumLinks.length > 0 && albumLinks[0].href) return albumLinks[0].href; "
            L"  let allLinks = document.querySelectorAll('a'); "
            L"  for(let i=0; i<allLinks.length; i++) { "
            L"    if(allLinks[i].href && (allLinks[i].href.includes('/track/') || allLinks[i].href.includes('/album/'))) { "
            L"      return allLinks[i].href; "
            L"    } "
            L"  } "
            L"  return ''; "
            L"})()";

        // ==========================================================
        // 1. Apple Music (Searches by Album)
        // ==========================================================
        std::string searchAlbum = album.empty() ? title : album;
        std::string appleQuery = artist + " " + searchAlbum;
        std::string appleSearchUrl = "https://music.apple.com/ca/search?term=" + UrlEncode(curl, appleQuery);
        std::cout << "   [*] Apple Music: Loading search grid (Album)...\n";

        navigationComplete = false;
        std::wstring wAppleUrl(appleSearchUrl.begin(), appleSearchUrl.end());
        webview->Navigate(wAppleUrl.c_str());
        WaitForCompletion(navigationComplete, 15000);

        std::string appleAlbumUrl = "";
        for (int i = 0; i < 5; i++) {
            Sleep(1500);
            appleAlbumUrl = ExecuteScriptSync(findAppleAlbumLinkScript);
            if (!appleAlbumUrl.empty() && appleAlbumUrl != "null") break;
        }

        if (!appleAlbumUrl.empty() && appleAlbumUrl != "null") {
            std::cout << "   [*] Apple Music: Entering album -> " << appleAlbumUrl << "\n";

            navigationComplete = false;
            std::wstring wAppleAlbum(appleAlbumUrl.begin(), appleAlbumUrl.end());
            webview->Navigate(wAppleAlbum.c_str());
            WaitForCompletion(navigationComplete, 15000);

            std::wstring findAppleImageScript = L"(() => { "
                L"  let src = ''; "
                L"  let meta = document.querySelector('meta[property=\"og:image\"]'); "
                L"  if (meta && meta.content) src = meta.content; "
                L"  if (!src) { "
                L"    let img = document.querySelector('picture source[srcset*=\"mzstatic\"], img[src*=\"mzstatic\"]'); "
                L"    if (img) src = img.srcset ? img.srcset.split(' ')[0] : img.src; "
                L"  } "
                L"  if (src) { "
                L"    return src.replace(/\\/[^\\/]+\\.(jpg|jpeg|png|webp)$/i, '/1000x1000bb.jpg'); "
                L"  } "
                L"  return ''; "
                L"})()";

            for (int i = 0; i < 5; i++) {
                Sleep(1500);
                std::string artUrl = ExecuteScriptSync(findAppleImageScript);
                if (!artUrl.empty() && artUrl != "null") {
                    res.appleArtUrl = artUrl;
                    break;
                }
            }
        }

        // ==========================================================
        // 2. Spotify (Searches by Song Title)
        // ==========================================================
        std::string searchTitle = title.empty() ? album : title;
        std::string spotifyQuery = artist + " " + searchTitle;
        std::string spotifySearchUrl = "https://open.spotify.com/search/" + UrlEncode(curl, spotifyQuery) + "/tracks";
        std::cout << "   [*] Spotify: Loading search grid for song title (" << searchTitle << ")...\n";

        navigationComplete = false;
        std::wstring wSpotifyUrl(spotifySearchUrl.begin(), spotifySearchUrl.end());
        webview->Navigate(wSpotifyUrl.c_str());
        WaitForCompletion(navigationComplete, 15000);

        std::string spotifyTrackUrl = "";
        for (int i = 0; i < 5; i++) {
            Sleep(1500);
            spotifyTrackUrl = ExecuteScriptSync(findSpotifyTrackLinkScript);
            if (!spotifyTrackUrl.empty() && spotifyTrackUrl != "null") break;
        }

        if (!spotifyTrackUrl.empty() && spotifyTrackUrl != "null") {
            std::cout << "   [*] Spotify: Entering song track -> " << spotifyTrackUrl << "\n";

            navigationComplete = false;
            std::wstring wSpotifyTrack(spotifyTrackUrl.begin(), spotifyTrackUrl.end());
            webview->Navigate(wSpotifyTrack.c_str());
            WaitForCompletion(navigationComplete, 15000);

            std::wstring findSpotifyImageScript = L"(() => { "
                L"  let meta = document.querySelector('meta[property=\"og:image\"]'); "
                L"  if (meta && meta.content && meta.content.includes('i.scdn.co')) return meta.content; "
                L"  let imgTestId = document.querySelector('img[data-testid=\"cover-art-image\"]'); "
                L"  if (imgTestId) { "
                L"    if (imgTestId.src && imgTestId.src.includes('i.scdn.co')) return imgTestId.src; "
                L"    if (imgTestId.srcset) { "
                L"      let parts = imgTestId.srcset.split(','); "
                L"      let last = parts[parts.length - 1].trim().split(' ')[0]; "
                L"      if (last) return last; "
                L"    } "
                L"  } "
                L"  let imgs = document.querySelectorAll('img'); "
                L"  for (let i = 0; i < imgs.length; i++) { "
                L"    let src = imgs[i].src || ''; "
                L"    let srcset = imgs[i].srcset || ''; "
                L"    if (src.includes('i.scdn.co/image/')) return src; "
                L"    if (srcset.includes('i.scdn.co/image/')) { "
                L"      let parts = srcset.split(','); "
                L"      let last = parts[parts.length - 1].trim().split(' ')[0]; "
                L"      if (last) return last; "
                L"    } "
                L"  } "
                L"  if (meta && meta.content) return meta.content; "
                L"  return ''; "
                L"})()";

            for (int i = 0; i < 5; i++) {
                Sleep(1500);
                std::string artUrl = ExecuteScriptSync(findSpotifyImageScript);
                if (!artUrl.empty() && artUrl != "null") {
                    res.spotifyArtUrl = artUrl;
                    break;
                }
            }
        }

        return res;
    }

    ~WebViewScraper() {
        if (webview) webview->remove_NavigationCompleted(navToken);
        if (controller) controller->Close();
        if (hwnd) DestroyWindow(hwnd);
    }
};

// ========================================================================
// Custom Win32 Dialog System
// ========================================================================
struct DialogState {
    std::string title;
    std::string localText;
    std::string appleText;
    std::string spotifyText;

    std::vector<char> localImageData;
    std::vector<char> appleImageData;
    std::vector<char> spotifyImageData;

    Gdiplus::Bitmap* localBmp = nullptr;
    Gdiplus::Bitmap* appleBmp = nullptr;
    Gdiplus::Bitmap* spotifyBmp = nullptr;

    int result = 0; // -1 = Exit, 1 = Default (Local), 2 = Apple, 3 = Spotify, 4 = Auto All
};

LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogState* state = nullptr;

    if (msg == WM_CREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        state = reinterpret_cast<DialogState*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)state);

        SetWindowTextA(hwnd, state->title.c_str());

        state->localBmp = LoadBitmapFromMemory(state->localImageData);
        state->appleBmp = LoadBitmapFromMemory(state->appleImageData);
        state->spotifyBmp = LoadBitmapFromMemory(state->spotifyImageData);
        return 0;
    }

    state = reinterpret_cast<DialogState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!state) return DefWindowProc(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Gdiplus::Graphics graphics(hdc);

        int imgY = 25, imgSize = 250;
        int txtY = 285;

        int col1X = 30;
        int col2X = 350;
        int col3X = 670;

        Gdiplus::FontFamily fontFamily(L"Arial");
        Gdiplus::Font font(&fontFamily, 11, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::Font fontReg(&fontFamily, 9, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush blackBrush(Gdiplus::Color(255, 0, 0, 0));

        // Column Titles
        graphics.DrawString(L"1. DEFAULT (LOCAL)", -1, &font, Gdiplus::PointF(static_cast<Gdiplus::REAL>(col1X + 40), 5.0f), &blackBrush);
        graphics.DrawString(L"2. APPLE MUSIC", -1, &font, Gdiplus::PointF(static_cast<Gdiplus::REAL>(col2X + 60), 5.0f), &blackBrush);
        graphics.DrawString(L"3. SPOTIFY", -1, &font, Gdiplus::PointF(static_cast<Gdiplus::REAL>(col3X + 70), 5.0f), &blackBrush);

        // Render Previews
        if (state->localBmp) {
            graphics.DrawImage(state->localBmp, col1X, imgY, imgSize, imgSize);
        }
        else {
            graphics.DrawString(L"[No Local Image]", -1, &fontReg, Gdiplus::PointF(static_cast<Gdiplus::REAL>(col1X + 70), static_cast<Gdiplus::REAL>(imgY + 110)), &blackBrush);
        }

        if (state->appleBmp) {
            graphics.DrawImage(state->appleBmp, col2X, imgY, imgSize, imgSize);
        }
        else {
            graphics.DrawString(L"[No Apple Image]", -1, &fontReg, Gdiplus::PointF(static_cast<Gdiplus::REAL>(col2X + 70), static_cast<Gdiplus::REAL>(imgY + 110)), &blackBrush);
        }

        if (state->spotifyBmp) {
            graphics.DrawImage(state->spotifyBmp, col3X, imgY, imgSize, imgSize);
        }
        else {
            graphics.DrawString(L"[No Spotify Image]", -1, &fontReg, Gdiplus::PointF(static_cast<Gdiplus::REAL>(col3X + 70), static_cast<Gdiplus::REAL>(imgY + 110)), &blackBrush);
        }

        auto toWide = [](const std::string& s) {
            if (s.empty()) return std::wstring();
            int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
            std::wstring ws(len, 0);
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
            return ws;
            };

        graphics.DrawString(toWide(state->localText).c_str(), -1, &fontReg, Gdiplus::PointF(static_cast<Gdiplus::REAL>(col1X), static_cast<Gdiplus::REAL>(txtY)), &blackBrush);
        graphics.DrawString(toWide(state->appleText).c_str(), -1, &fontReg, Gdiplus::PointF(static_cast<Gdiplus::REAL>(col2X), static_cast<Gdiplus::REAL>(txtY)), &blackBrush);
        graphics.DrawString(toWide(state->spotifyText).c_str(), -1, &fontReg, Gdiplus::PointF(static_cast<Gdiplus::REAL>(col3X), static_cast<Gdiplus::REAL>(txtY)), &blackBrush);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == 1001) { // Choice 1: Default (Local)
            state->result = 1;
            DestroyWindow(hwnd);
        }
        else if (id == 1002) { // Choice 2: Apple
            state->result = 2;
            DestroyWindow(hwnd);
        }
        else if (id == 1003) { // Choice 3: Spotify
            state->result = 3;
            DestroyWindow(hwnd);
        }
        else if (id == 1004) { // Choice 4: Auto All
            state->result = 4;
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_CLOSE: {
        state->result = -1; // Exit
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY: {
        if (state->localBmp) { delete state->localBmp; state->localBmp = nullptr; }
        if (state->appleBmp) { delete state->appleBmp; state->appleBmp = nullptr; }
        if (state->spotifyBmp) { delete state->spotifyBmp; state->spotifyBmp = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int ShowComparisonUI(DialogState& state) {
    const char* className = "ComparisonDialog";
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = className;

    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, className, "Metadata Comparison",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 550,
        NULL, NULL, GetModuleHandle(NULL), &state);

    // Buttons for choices: 1 Default, 2 Apple, 3 Spotify, 4 Auto All
    CreateWindowA("BUTTON", "1. DEFAULT (LOCAL)", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        30, 440, 200, 45, hwnd, (HMENU)1001, GetModuleHandle(NULL), NULL);

    CreateWindowA("BUTTON", "2. APPLE MUSIC", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        260, 440, 200, 45, hwnd, (HMENU)1002, GetModuleHandle(NULL), NULL);

    CreateWindowA("BUTTON", "3. SPOTIFY", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        490, 440, 200, 45, hwnd, (HMENU)1003, GetModuleHandle(NULL), NULL);

    CreateWindowA("BUTTON", "4. AUTO ALL (WEB)", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        720, 440, 200, 45, hwnd, (HMENU)1004, GetModuleHandle(NULL), NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessage(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    UnregisterClassA(className, GetModuleHandle(NULL));
    return state.result;
}

// ========================================================================
// Main Application
// ========================================================================
int main(int argc, char* argv[]) {
    std::cout << "====================================================\n";
    std::cout << "     MP3 Tag Repair & WebView2 Art Updater System   \n";
    std::cout << "====================================================\n\n";

    if (argc < 3) {
        std::cerr << "[!] Error: Missing input and output directories.\n";
        std::cerr << "[-] Usage: " << argv[0] << " <input_folder> <output_folder>\n";
        return 1;
    }

    fs::path inputFolder = argv[1];
    fs::path outputFolder = argv[2];

    if (!fs::exists(inputFolder) || !fs::is_directory(inputFolder)) return 1;
    if (!fs::exists(outputFolder)) fs::create_directories(outputFolder);

    // Initialize COM for WebView2
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::cerr << "[!] Failed to initialize COM.\n";
        return 1;
    }

    // Initialize GDI+
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    // Initialize CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* curl = curl_easy_init();
    if (!curl) return 1;

    // Initialize WebView2 Scraper
    WebViewScraper webScraper;
    bool webViewReady = webScraper.Initialize();
    if (webViewReady) {
        std::cout << "[*] WebView2 Engine Initialized Successfully.\n";
    }
    else {
        std::cerr << "[!] WebView2 Initialization Failed.\n";
    }

    bool autoMode = false;

    for (const auto& entry : fs::directory_iterator(inputFolder)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mp3") continue;

        fs::path srcFilePath = entry.path();
        fs::path destFilePath = outputFolder / srcFilePath.filename();

        std::cout << "\n----------------------------------------------------\n";
        std::cout << "[*] Processing: " << srcFilePath.filename().string() << "\n";

        try {
            fs::copy_file(srcFilePath, destFilePath, fs::copy_options::overwrite_existing);
        }
        catch (const std::exception& e) {
            std::cerr << "   [!] Error copying file: " << e.what() << "\n";
            continue;
        }

        TagLib::MPEG::File mpegFile(destFilePath.c_str());
        if (!mpegFile.isValid()) continue;

        // --- Extract Local Metadata ---
        std::string localArtist, localTitle, localAlbum;
        unsigned int localYear = 0;
        std::vector<char> localArtData;

        if (mpegFile.tag()) {
            localArtist = mpegFile.tag()->artist().to8Bit(true);
            localTitle = mpegFile.tag()->title().to8Bit(true);
            localAlbum = mpegFile.tag()->album().to8Bit(true);
            localYear = mpegFile.tag()->year();
        }

        if (mpegFile.ID3v2Tag()) {
            const TagLib::ID3v2::FrameList& frameList = mpegFile.ID3v2Tag()->frameListMap()["APIC"];
            if (!frameList.isEmpty()) {
                auto* picFrame = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frameList.front());
                if (picFrame) {
                    TagLib::ByteVector bv = picFrame->picture();
                    localArtData.assign(bv.data(), bv.data() + bv.size());
                }
            }
        }

        // --- Query WebView2 (Apple Music & Spotify) ---
        WebScrapeResult scrapeRes;
        std::vector<char> appleArtData;
        std::vector<char> spotifyArtData;

        if (webViewReady) {
            std::cout << "   [*] Searching Apple Music (Album) and Spotify (Song Title) via WebView2...\n";
            scrapeRes = webScraper.ScrapeDirect(curl, localArtist, localTitle, localAlbum);
        }

        if (!scrapeRes.appleArtUrl.empty()) {
            std::cout << "   [+] Downloading Apple Artwork: " << scrapeRes.appleArtUrl << "\n";
            appleArtData = DownloadImageToMemory(curl, scrapeRes.appleArtUrl);
        }
        else {
            std::cout << "   [-] Apple Music artwork not found.\n";
        }

        if (!scrapeRes.spotifyArtUrl.empty()) {
            std::cout << "   [+] Downloading Spotify Artwork: " << scrapeRes.spotifyArtUrl << "\n";
            spotifyArtData = DownloadImageToMemory(curl, scrapeRes.spotifyArtUrl);
        }
        else {
            std::cout << "   [-] Spotify artwork not found.\n";
        }

        // --- Build UI Dialog State ---
        DialogState dlgState;
        dlgState.title = "Review: " + srcFilePath.filename().string();
        dlgState.localImageData = localArtData;
        dlgState.appleImageData = appleArtData;
        dlgState.spotifyImageData = spotifyArtData;

        std::ostringstream ltxt, atxt, stxt;
        ltxt << "Artist: " << localArtist << "\nTitle: " << localTitle << "\nAlbum: " << localAlbum << "\nYear: " << localYear;
        atxt << "Artist: " << localArtist << "\nTitle: " << localTitle << "\nAlbum: " << localAlbum << "\nSource: " << (scrapeRes.appleArtUrl.empty() ? "Not Found" : "Apple Music");
        stxt << "Artist: " << localArtist << "\nTitle: " << localTitle << "\nAlbum: " << localAlbum << "\nSource: " << (scrapeRes.spotifyArtUrl.empty() ? "Not Found" : "Spotify");

        dlgState.localText = ltxt.str();
        dlgState.appleText = atxt.str();
        dlgState.spotifyText = stxt.str();

        int choice = 0;

        // Check if Auto Mode was activated previously
        if (autoMode) {
            std::cout << "   [*] AUTO MODE: Automatically applying Web Metadata...\n";
            choice = 4;
        }
        else {
            std::cout << "   [*] Waiting for user input via GUI...\n";
            choice = ShowComparisonUI(dlgState);
        }

        if (choice == -1) {
            std::cout << "   [!] Exit requested. Shutting down application...\n";
            break;
        }

        if (choice == 0) {
            std::cout << "   [!] Skipped file.\n";
            continue;
        }

        // Engage auto mode for all remaining files if clicked "AUTO ALL"
        if (choice == 4) {
            autoMode = true;
        }

        // Select chosen artwork vector:
        // Choice 1: Default (Local)
        // Choice 2: Apple Music
        // Choice 3: Spotify
        // Choice 4: Auto All (Apple -> Spotify -> Local)
        std::vector<char>* chosenArt = &localArtData;

        if (choice == 2) { // Apple
            if (!appleArtData.empty()) {
                chosenArt = &appleArtData;
            }
            else {
                std::cout << "   [!] Selected Apple, but Apple artwork was empty. Falling back to Local.\n";
            }
        }
        else if (choice == 3) { // Spotify
            if (!spotifyArtData.empty()) {
                chosenArt = &spotifyArtData;
            }
            else {
                std::cout << "   [!] Selected Spotify, but Spotify artwork was empty. Falling back to Local.\n";
            }
        }
        else if (choice == 4) { // Auto All
            if (!appleArtData.empty()) {
                chosenArt = &appleArtData;
            }
            else if (!spotifyArtData.empty()) {
                chosenArt = &spotifyArtData;
            }
            else {
                chosenArt = &localArtData;
            }
        }

        // --- Rebuild Tags ---
        std::cout << "   [*] Stripping and Repairing ID3 headers...\n";
        mpegFile.strip(TagLib::MPEG::File::AllTags);
        TagLib::ID3v2::Tag* freshTag = mpegFile.ID3v2Tag(true);

        freshTag->setArtist(TagLib::String(localArtist, TagLib::String::UTF8));
        freshTag->setTitle(TagLib::String(localTitle, TagLib::String::UTF8));
        freshTag->setAlbum(TagLib::String(localAlbum, TagLib::String::UTF8));
        freshTag->setYear(localYear);

        if (!chosenArt->empty()) {
            TagLib::ID3v2::AttachedPictureFrame* picFrame = new TagLib::ID3v2::AttachedPictureFrame();
            picFrame->setMimeType("image/jpeg");
            picFrame->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
            picFrame->setPicture(TagLib::ByteVector(chosenArt->data(), static_cast<unsigned int>(chosenArt->size())));
            freshTag->addFrame(picFrame);
        }

        if (mpegFile.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripOthers, TagLib::ID3v2::v4)) {
            std::cout << "   [+] Repaired and saved successfully.\n";
        }
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();

    std::cout << "\n[+] Complete.\n";
    return 0;
}