// sharesound - system audio bridge for browser screen sharing.
//
// Firefox does not implement audio capture in getDisplayMedia (bugzilla 1541425),
// so a screen share carries no sound. This helper captures system audio with
// WASAPI loopback and serves it as raw PCM over HTTP on localhost, where a
// userscript picks it up and injects it into the shared MediaStream.
//
// Capture modes:
//   all              - whole system mix
//   exclude <name>   - everything except that process tree (default: the browser,
//                      so remote participants do not hear themselves)
//   only <name>      - just that process tree (e.g. a game)
//
// Build: cl /O2 /EHsc /std:c++17 sharesound.cpp /link ole32.lib mmdevapi.lib ws2_32.lib

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <cmath>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>

#include "assets.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ws2_32.lib")

static const int kSampleRate = 48000;
static const int kChannels = 2;
static const int kBytesPerFrame = kChannels * 2;  // s16le stereo

// ---------------------------------------------------------------- config
enum class Mode { All, Exclude, Only };

struct Config {
    int port = 47823;
    Mode mode = Mode::All;
    std::wstring target = L"firefox.exe";
    std::wstring device;   // substring of an output device name; empty = default device
    bool verbose = false;
};
static Config g_cfg;
static std::mutex g_cfgMutex;

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring deviceFriendlyName(IMMDevice* dev);

static void logf(const char* fmt, ...) {
    if (!g_cfg.verbose) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    fflush(stdout);
}


// Play a test tone into one specific output device (used to verify that a
// device's audio really is isolated from the device we capture).
static int playTone(const std::wstring& want, double hz, int seconds) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)))) return 1;
    IMMDevice* dev = nullptr;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count && !dev; i++) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            if (deviceFriendlyName(d).find(want) != std::wstring::npos) dev = d;
            else d->Release();
        }
        col->Release();
    }
    en->Release();
    if (!dev) {
        printf("device not found: %s\n", narrow(want).c_str());
        return 1;
    }
    printf("playing %.0f Hz for %d s into [%s]\n", hz, seconds, narrow(deviceFriendlyName(dev)).c_str());
    fflush(stdout);

    IAudioClient* ac = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac)))) {
        dev->Release();
        return 1;
    }
    dev->Release();
    WAVEFORMATEX* mix = nullptr;
    ac->GetMixFormat(&mix);
    HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix, nullptr);
    if (FAILED(hr)) {
        printf("Initialize failed 0x%08X\n", hr);
        return 1;
    }
    IAudioRenderClient* rc = nullptr;
    ac->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rc));
    UINT32 bufFrames = 0;
    ac->GetBufferSize(&bufFrames);

    bool isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                   (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    const int ch = mix->nChannels;
    const double rate = mix->nSamplesPerSec;
    double phase = 0.0, step = 2.0 * 3.14159265358979 * hz / rate;

    ac->Start();
    DWORD until = GetTickCount() + seconds * 1000;
    while (GetTickCount() < until) {
        UINT32 padding = 0;
        ac->GetCurrentPadding(&padding);
        UINT32 avail = bufFrames - padding;
        if (avail == 0) { Sleep(5); continue; }
        BYTE* data = nullptr;
        if (FAILED(rc->GetBuffer(avail, &data))) break;
        for (UINT32 i = 0; i < avail; i++) {
            double v = sin(phase) * 0.3;
            phase += step;
            for (int c = 0; c < ch; c++) {
                if (isFloat) reinterpret_cast<float*>(data)[i * ch + c] = static_cast<float>(v);
                else reinterpret_cast<short*>(data)[i * ch + c] = static_cast<short>(v * 32767);
            }
        }
        rc->ReleaseBuffer(avail, 0);
        Sleep(5);
    }
    ac->Stop();
    rc->Release();
    ac->Release();
    CoTaskMemFree(mix);
    CoUninitialize();
    printf("done\n");
    return 0;
}

// ------------------------------------------------------------- subscribers
struct Client {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<BYTE>> q;
    bool alive = true;
    size_t dropped = 0;
};

static std::mutex g_clientsMutex;
static std::set<std::shared_ptr<Client>> g_clients;
static std::atomic<bool> g_captureRunning{false};
static std::atomic<long long> g_bytesCaptured{0};
static std::atomic<int> g_lastError{0};
static std::atomic<bool> g_restartRequested{false};
static std::string g_activeMode = "idle";

static void broadcast(const BYTE* data, size_t len) {
    std::lock_guard<std::mutex> lk(g_clientsMutex);
    for (auto& c : g_clients) {
        std::lock_guard<std::mutex> cl(c->m);
        // ~2 s of audio is plenty; a stalled reader must not grow this forever
        if (c->q.size() > 100) {
            c->q.pop_front();
            c->dropped++;
        }
        c->q.emplace_back(data, data + len);
        c->cv.notify_one();
    }
}

static bool haveClients() {
    std::lock_guard<std::mutex> lk(g_clientsMutex);
    return !g_clients.empty();
}

// ------------------------------------------------------------ process lookup
// Root of the process tree: the first process with that name whose parent is not
// also that name (Firefox spawns many children under one parent).
static DWORD findProcessTreeRoot(const std::wstring& name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    struct Ent { DWORD pid, parent; std::wstring exe; };
    std::vector<Ent> all;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            all.push_back({pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile});
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    auto lower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    };
    std::wstring want = lower(name);
    DWORD fallback = 0;
    for (const auto& e : all) {
        if (lower(e.exe) != want) continue;
        if (!fallback) fallback = e.pid;
        bool parentSame = false;
        for (const auto& p : all) {
            if (p.pid == e.parent && lower(p.exe) == want) { parentSame = true; break; }
        }
        if (!parentSame) return e.pid;
    }
    return fallback;
}

// ------------------------------------------- async activation completion handler
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hrActivate = E_FAIL;
    IAudioClient* client = nullptr;

    ~ActivateHandler() {
        if (done) CloseHandle(done);
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hrAct = E_FAIL;
        IUnknown* unk = nullptr;
        if (op) op->GetActivateResult(&hrAct, &unk);
        hrActivate = hrAct;
        if (SUCCEEDED(hrAct) && unk) {
            unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
            unk->Release();
        }
        SetEvent(done);
        return S_OK;
    }

private:
    LONG ref_ = 1;
};

static void fillWaveFormat(WAVEFORMATEX& wf) {
    wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = kChannels;
    wf.nSamplesPerSec = kSampleRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize = 0;
}

// Process-specific loopback (Windows 10 2004+): capture a process tree, or
// everything except it.
static HRESULT openProcessLoopback(DWORD pid, bool exclude, IAudioClient** out) {
    AUDIOCLIENT_ACTIVATION_PARAMS ap{};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = pid;
    ap.ProcessLoopbackParams.ProcessLoopbackMode =
        exclude ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
                : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(ap);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);

    ActivateHandler* handler = new ActivateHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                             __uuidof(IAudioClient), &pv, handler, &op);
    if (FAILED(hr)) {
        handler->Release();
        return hr;
    }
    WaitForSingleObject(handler->done, 5000);
    hr = handler->hrActivate;
    if (SUCCEEDED(hr) && handler->client) {
        *out = handler->client;
        handler->client = nullptr;
    } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
    }
    if (op) op->Release();
    handler->Release();
    return hr;
}

std::wstring deviceFriendlyName(IMMDevice* dev) {
    IPropertyStore* props = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return L"?";
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring name = L"?";
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
    PropVariantClear(&v);
    props->Release();
    return name;
}

// Plain endpoint loopback: whole mix of one output device (default unless named).
static HRESULT openDeviceLoopback(const std::wstring& want, IAudioClient** out, std::wstring* usedName) {
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
    if (FAILED(hr)) return hr;

    IMMDevice* dev = nullptr;
    if (!want.empty()) {
        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
            UINT count = 0;
            col->GetCount(&count);
            for (UINT i = 0; i < count && !dev; i++) {
                IMMDevice* d = nullptr;
                if (FAILED(col->Item(i, &d))) continue;
                if (deviceFriendlyName(d).find(want) != std::wstring::npos) dev = d;
                else d->Release();
            }
            col->Release();
        }
    }
    if (!dev) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    en->Release();
    if (!dev) return FAILED(hr) ? hr : E_FAIL;
    if (usedName) *usedName = deviceFriendlyName(dev);
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(out));
    dev->Release();
    return hr;
}

// Diagnostics: output devices (default marked) and which processes are playing.
static void listDevicesAndSessions() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)))) return;

    IMMDevice* def = nullptr;
    std::wstring defName;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) defName = deviceFriendlyName(def);

    printf("Output devices:\n");
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            std::wstring n = deviceFriendlyName(d);
            printf("  %-9s %s\n", n == defName ? "[default]" : "", narrow(n).c_str());
            d->Release();
        }
        col->Release();
    }

    if (def) {
        printf("\nPlayback sessions on [%s]:\n", narrow(defName).c_str());
        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(def->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(&mgr)))) {
            IAudioSessionEnumerator* se = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
                int n = 0;
                se->GetCount(&n);
                for (int i = 0; i < n; i++) {
                    IAudioSessionControl* ctl = nullptr;
                    if (FAILED(se->GetSession(i, &ctl))) continue;
                    IAudioSessionControl2* ctl2 = nullptr;
                    if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                      reinterpret_cast<void**>(&ctl2)))) {
                        DWORD pid = 0;
                        ctl2->GetProcessId(&pid);
                        AudioSessionState st = AudioSessionStateInactive;
                        ctl->GetState(&st);
                        wchar_t exe[MAX_PATH] = L"?";
                        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                        if (h) {
                            DWORD sz = MAX_PATH;
                            QueryFullProcessImageNameW(h, 0, exe, &sz);
                            CloseHandle(h);
                        }
                        const wchar_t* base = wcsrchr(exe, L'\\');
                        printf("  pid %-6lu %-9s %s\n", pid,
                               st == AudioSessionStateActive ? "ACTIVE" : "idle",
                               narrow(base ? base + 1 : exe).c_str());
                        ctl2->Release();
                    }
                    ctl->Release();
                }
                se->Release();
            }
            mgr->Release();
        }
        def->Release();
    }
    en->Release();
    CoUninitialize();
}

static void ensureCapture();

// ------------------------------------------------------------- capture thread
static void captureThread() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInit = SUCCEEDED(hr);

    Config cfg;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        cfg = g_cfg;
    }

    IAudioClient* client = nullptr;
    bool processMode = false;
    if (cfg.mode != Mode::All) {
        DWORD pid = findProcessTreeRoot(cfg.target);
        if (pid) {
            hr = openProcessLoopback(pid, cfg.mode == Mode::Exclude, &client);
            if (SUCCEEDED(hr)) {
                processMode = true;
                g_activeMode = (cfg.mode == Mode::Exclude ? "exclude " : "only ") + narrow(cfg.target);
                logf("process loopback active (pid %lu, %s)", pid,
                     cfg.mode == Mode::Exclude ? "exclude tree" : "include tree");
            } else {
                logf("process loopback failed hr=0x%08X, falling back", hr);
            }
        } else {
            logf("target process '%s' not running, falling back to full mix", narrow(cfg.target).c_str());
        }
    }
    if (!client) {
        std::wstring used;
        hr = openDeviceLoopback(cfg.device, &client, &used);
        if (FAILED(hr)) {
            g_lastError = hr;
            logf("device loopback failed hr=0x%08X", hr);
            if (comInit) CoUninitialize();
            g_captureRunning = false;
            return;
        }
        g_activeMode = "all @ " + narrow(used);
        logf("device loopback on '%s'", narrow(used).c_str());
    }

    WAVEFORMATEX wf;
    fillWaveFormat(wf);
    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (!processMode) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &wf, nullptr);
    if (FAILED(hr)) {
        g_lastError = hr;
        logf("Initialize failed hr=0x%08X", hr);
        client->Release();
        if (comInit) CoUninitialize();
        g_captureRunning = false;
        return;
    }

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    client->SetEventHandle(evt);

    IAudioCaptureClient* cap = nullptr;
    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap));
    if (FAILED(hr)) {
        g_lastError = hr;
        client->Release();
        CloseHandle(evt);
        if (comInit) CoUninitialize();
        g_captureRunning = false;
        return;
    }
    client->Start();
    logf("capture started (%s)", g_activeMode.c_str());

    // A silent system produces no packets at all, but the browser needs a
    // continuous stream, so pad with real silence against a wall clock.
    LARGE_INTEGER freq, t0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    long long framesEmitted = 0;
    std::vector<BYTE> silence(kBytesPerFrame * 480, 0);  // 10 ms

    while (g_captureRunning && haveClients()) {
        WaitForSingleObject(evt, 20);
        UINT32 packet = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD bufFlags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &bufFlags, nullptr, nullptr))) break;
            size_t bytes = static_cast<size_t>(frames) * kBytesPerFrame;
            if (bufFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::vector<BYTE> z(bytes, 0);
                broadcast(z.data(), z.size());
            } else if (data && bytes) {
                broadcast(data, bytes);
            }
            g_bytesCaptured += static_cast<long long>(bytes);
            framesEmitted += frames;
            cap->ReleaseBuffer(frames);
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        long long expected = (now.QuadPart - t0.QuadPart) * kSampleRate / freq.QuadPart;
        long long behind = expected - framesEmitted;
        if (behind > 480) {  // more than 10 ms of gap - fill it
            long long fill = std::min<long long>(behind, kSampleRate / 2);
            while (fill > 0) {
                long long chunk = std::min<long long>(fill, 480);
                broadcast(silence.data(), static_cast<size_t>(chunk) * kBytesPerFrame);
                g_bytesCaptured += chunk * kBytesPerFrame;
                framesEmitted += chunk;
                fill -= chunk;
            }
        }
    }

    client->Stop();
    cap->Release();
    client->Release();
    CloseHandle(evt);
    if (comInit) CoUninitialize();
    g_activeMode = "idle";
    g_captureRunning = false;
    logf("capture stopped");
    if (g_restartRequested.exchange(false) && haveClients()) ensureCapture();
}

static void ensureCapture() {
    bool expected = false;
    if (g_captureRunning.compare_exchange_strong(expected, true)) {
        std::thread(captureThread).detach();
    }
}


// ------------------------------------------------------------------ doctor
// Whether remote participants hear themselves depends on the machine: either
// the OS can exclude the browser from capture, or the default output device is
// a virtual layer that the app's own audio can bypass.
static DWORD windowsBuild() {
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE h = GetModuleHandleW(L"ntdll.dll");
    if (!h) return 0;
    RtlGetVersionPtr f = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(h, "RtlGetVersion"));
    if (!f) return 0;
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (f(&vi) != 0) return 0;
    return vi.dwBuildNumber;
}

static int doctor() {
    DWORD build = windowsBuild();
    printf("Windows build      : %lu\n", build);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    std::wstring defName = L"?";
    int activeOutputs = 0;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)))) {
        IMMDevice* def = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
            defName = deviceFriendlyName(def);
            def->Release();
        }
        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
            UINT c = 0;
            col->GetCount(&c);
            activeOutputs = (int)c;
            col->Release();
        }
        en->Release();
    }
    printf("Default output     : %s\n", narrow(defName).c_str());
    printf("Active outputs     : %d\n", activeOutputs);

    IAudioClient* probe = nullptr;
    DWORD selfPid = GetCurrentProcessId();
    HRESULT hr = openProcessLoopback(selfPid, true, &probe);
    bool perApp = SUCCEEDED(hr);
    if (probe) probe->Release();
    printf("Per-app capture    : %s", perApp ? "available\n" : "not available");
    if (!perApp) printf(" (needs Windows build 20348+, hr=0x%08X)\n", hr);
    CoUninitialize();

    printf("\nEcho verdict:\n");
    if (perApp) {
        printf("  Best case. Run with --exclude <browser.exe> and remote participants\n"
               "  will not hear themselves. Note this also drops audio playing in that\n"
               "  browser's own tabs; use --only <game.exe> if you want just one app.\n");
    } else if (activeOutputs > 1) {
        printf("  Capture stays on the default device [%s].\n", narrow(defName).c_str());
        printf("  In the voice app, set Output Device to a DIFFERENT device than the\n"
               "  default one - its sound then bypasses what we capture, and callers\n"
               "  stop hearing themselves. Verify with:  sharesound-cli --tone <device>\n");
    } else {
        printf("  Only one output device and no per-app capture, so the call audio\n"
               "  cannot be separated from everything else: participants will hear\n"
               "  themselves while they speak. Fixes: upgrade to Windows 11, or install\n"
               "  any virtual output device (VB-Cable, VoiceMeeter, FxSound), make it\n"
               "  the default, and point the voice app at your real headphones.\n");
    }
    return 0;
}

// ------------------------------------------------------------------ http
static void sendAll(SOCKET s, const char* data, int len) {
    while (len > 0) {
        int n = send(s, data, len, 0);
        if (n <= 0) return;
        data += n;
        len -= n;
    }
}

static void sendSimple(SOCKET s, const char* status, const char* ctype, const std::string& body) {
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                     "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, ctype, static_cast<int>(body.size()));
    sendAll(s, head, n);
    sendAll(s, body.data(), static_cast<int>(body.size()));
}

static void streamPcm(SOCKET s) {
    auto client = std::make_shared<Client>();
    {
        std::lock_guard<std::mutex> lk(g_clientsMutex);
        g_clients.insert(client);
    }
    ensureCapture();

    const char* head =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "X-Audio-Format: s16le;rate=48000;channels=2\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n";
    sendAll(s, head, static_cast<int>(strlen(head)));

    for (;;) {
        std::vector<BYTE> chunk;
        {
            std::unique_lock<std::mutex> lk(client->m);
            client->cv.wait_for(lk, std::chrono::milliseconds(500),
                               [&] { return !client->q.empty() || !client->alive; });
            if (!client->alive) break;
            if (client->q.empty()) continue;
            chunk = std::move(client->q.front());
            client->q.pop_front();
        }
        char sizeLine[32];
        int n = snprintf(sizeLine, sizeof(sizeLine), "%zx\r\n", chunk.size());
        sendAll(s, sizeLine, n);
        int sent = send(s, reinterpret_cast<const char*>(chunk.data()),
                        static_cast<int>(chunk.size()), 0);
        if (sent <= 0) break;
        sendAll(s, "\r\n", 2);
    }

    {
        std::lock_guard<std::mutex> lk(g_clientsMutex);
        g_clients.erase(client);
    }
    logf("pcm client gone (dropped %zu chunks)", client->dropped);
}

static std::string urlDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size()) {
            out += static_cast<char>(strtol(in.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

static void handleClient(SOCKET s) {
    char buf[2048];
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        closesocket(s);
        return;
    }
    buf[n] = 0;
    std::string req(buf);
    std::string line = req.substr(0, req.find("\r\n"));
    size_t sp1 = line.find(' ');
    size_t sp2 = line.find(' ', sp1 + 1);
    std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
                           ? line.substr(sp1 + 1, sp2 - sp1 - 1)
                           : "/";
    std::string query;
    size_t q = path.find('?');
    if (q != std::string::npos) {
        query = path.substr(q + 1);
        path = path.substr(0, q);
    }

    if (path == "/pcm") {
        streamPcm(s);
    } else if (path == "/health") {
        size_t nClients;
        {
            std::lock_guard<std::mutex> lk(g_clientsMutex);
            nClients = g_clients.size();
        }
        char body[512];
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"mode\":\"%s\",\"clients\":%zu,\"captured\":%lld,\"error\":%d}",
                 g_activeMode.c_str(), nClients, g_bytesCaptured.load(), g_lastError.load());
        sendSimple(s, "200 OK", "application/json", body);
    } else if (path == "/mode") {
        // /mode?set=all | ?set=exclude&name=firefox.exe | ?set=only&name=game.exe
        std::string set, name;
        size_t p = 0;
        while (p < query.size()) {
            size_t amp = query.find('&', p);
            std::string kv = query.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
            size_t eq = kv.find('=');
            if (eq != std::string::npos) {
                std::string k = kv.substr(0, eq), v = urlDecode(kv.substr(eq + 1));
                if (k == "set") set = v;
                if (k == "name") name = v;
            }
            if (amp == std::string::npos) break;
            p = amp + 1;
        }
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            if (set == "all") g_cfg.mode = Mode::All;
            else if (set == "exclude") g_cfg.mode = Mode::Exclude;
            else if (set == "only") g_cfg.mode = Mode::Only;
            if (!name.empty()) g_cfg.target = widen(name);
        }
        g_restartRequested = true;
        g_captureRunning = false;  // restart capture with the new mode
        sendSimple(s, "200 OK", "application/json", "{\"ok\":true}");
    } else if (path == "/sharesound.user.js") {
        sendSimple(s, "200 OK", "text/javascript; charset=utf-8", kUserScript);
    } else if (path == "/" || path == "/index.html") {
        sendSimple(s, "200 OK", "text/html; charset=utf-8", kStatusPage);
    } else {
        sendSimple(s, "404 Not Found", "text/plain", "not found");
    }
    closesocket(s);
}

int wmain(int argc, wchar_t** argv) {
    // Built as a GUI binary so autostart shows no console window; when the user
    // runs it from a terminal with flags, borrow that terminal.
    if (argc > 1 && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(CP_UTF8);
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--port" && i + 1 < argc) g_cfg.port = _wtoi(argv[++i]);
        else if (a == L"--all") g_cfg.mode = Mode::All;
        else if (a == L"--exclude" && i + 1 < argc) { g_cfg.mode = Mode::Exclude; g_cfg.target = argv[++i]; }
        else if (a == L"--only" && i + 1 < argc) { g_cfg.mode = Mode::Only; g_cfg.target = argv[++i]; }
        else if (a == L"--device" && i + 1 < argc) g_cfg.device = argv[++i];
        else if (a == L"--list") { listDevicesAndSessions(); return 0; }
        else if (a == L"--doctor") { return doctor(); }
        else if (a == L"--tone" && i + 1 < argc) {
            std::wstring d = argv[++i];
            double hz = (i + 1 < argc && argv[i + 1][0] != L'-') ? _wtof(argv[++i]) : 997.0;
            return playTone(d, hz, 5);
        }
        else if (a == L"-v" || a == L"--verbose") g_cfg.verbose = true;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) return 1;
    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(g_cfg.port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        printf("bind failed on port %d (already running?)\n", g_cfg.port);
        return 1;
    }
    listen(srv, 8);
    printf("sharesound listening on http://127.0.0.1:%d  (capture: %s%s%s)\n", g_cfg.port,
           g_cfg.mode == Mode::All ? "system mix" : (g_cfg.mode == Mode::Exclude ? "exclude " : "only "),
           g_cfg.mode == Mode::All ? "" : narrow(g_cfg.target).c_str(),
           g_cfg.device.empty() ? "" : (" @ " + narrow(g_cfg.device)).c_str());
    fflush(stdout);

    for (;;) {
        SOCKET c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::thread(handleClient, c).detach();
    }
}

