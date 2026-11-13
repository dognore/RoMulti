#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <random>
#include <iterator>
#include <tlhelp32.h>
#include <wininet.h>
#include <filesystem>
#include <shlobj.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wrl.h>
#include "WebView2.h"

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace fs = std::filesystem;

// Set to true to launch through the roblox-player:// protocol instead of
// starting RobloxPlayerBeta.exe directly (try this if direct launch does nothing).
static const bool USE_PROTOCOL_LAUNCH = false;

// UI Control IDs
#define ID_BTN_LAUNCH     1
#define ID_BTN_OPEN_ADD   2
#define ID_EDIT_LOG       3
#define ID_LIST_ACCOUNTS  5
#define ID_BTN_REMOVE     6

HWND hMainWnd = NULL;
HWND hEditLog = NULL;
HWND hListAccounts = NULL;
HBRUSH hBrushBackground = NULL;
HBRUSH hBrushEdit = NULL;

#define IDI_APPICON 101
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

struct RobloxAccount {
    std::wstring name;
    std::wstring cookie;
};
std::vector<RobloxAccount> savedAccounts;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void LogMessage(const std::wstring& message) {
    if (!hEditLog) return;
    int length = GetWindowTextLengthW(hEditLog);
    SendMessageW(hEditLog, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    SendMessageW(hEditLog, EM_REPLACESEL, FALSE, (LPARAM)(message + L"\r\n").c_str());
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------------------------------------------------------------------
// Account persistence (encrypted with Windows DPAPI, tied to your Windows user)
// ---------------------------------------------------------------------------
static fs::path GetSavePath() {
    PWSTR p = NULL;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &p);
    fs::path dir = fs::path(p) / L"RoMulti";
    CoTaskMemFree(p);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / L"accounts.dat";
}

void SaveAccounts() {
    std::string plain;
    for (auto& a : savedAccounts)
        plain += ToUtf8(a.name) + "\t" + ToUtf8(a.cookie) + "\n";

    DATA_BLOB in = { (DWORD)plain.size(), (BYTE*)plain.data() };
    DATA_BLOB out = {};
    if (CryptProtectData(&in, L"RoMulti", NULL, NULL, NULL, 0, &out)) {
        std::ofstream f(GetSavePath(), std::ios::binary | std::ios::trunc);
        f.write((const char*)out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
    else {
        LogMessage(L"[-] Failed to encrypt/save accounts.");
    }
}

void LoadAccounts() {
    std::ifstream f(GetSavePath(), std::ios::binary);
    if (!f) return;
    std::string enc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (enc.empty()) return;

    DATA_BLOB in = { (DWORD)enc.size(), (BYTE*)enc.data() };
    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out)) {
        LogMessage(L"[-] Could not decrypt saved accounts (different Windows user?).");
        return;
    }
    std::string plain((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);

    size_t pos = 0;
    while (pos < plain.size()) {
        size_t eol = plain.find('\n', pos);
        if (eol == std::string::npos) eol = plain.size();
        std::string line = plain.substr(pos, eol - pos);
        pos = eol + 1;

        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;

        RobloxAccount acc{ FromUtf8(line.substr(0, tab)), FromUtf8(line.substr(tab + 1)) };
        savedAccounts.push_back(acc);
        SendMessageW(hListAccounts, LB_ADDSTRING, 0, (LPARAM)acc.name.c_str());
    }
}

void AddOrUpdateAccount(const std::wstring& name, const std::wstring& cookie) {
    for (auto& a : savedAccounts) {
        if (_wcsicmp(a.name.c_str(), name.c_str()) == 0) {
            a.cookie = cookie; // same account logged in again: refresh its cookie
            SaveAccounts();
            return;
        }
    }
    savedAccounts.push_back({ name, cookie });
    SendMessageW(hListAccounts, LB_ADDSTRING, 0, (LPARAM)name.c_str());
    SaveAccounts();
}

// ---------------------------------------------------------------------------
// Roblox helpers
// ---------------------------------------------------------------------------
std::wstring GetRobloxExecutablePath() {
    PWSTR pathLocalAppData = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &pathLocalAppData))) {
        std::wstring robloxDir = std::wstring(pathLocalAppData) + L"\\Roblox\\Versions";
        CoTaskMemFree(pathLocalAppData);

        std::error_code ec;
        if (fs::exists(robloxDir, ec)) {
            for (auto& entry : fs::directory_iterator(robloxDir, ec)) {
                if (entry.is_directory()) {
                    std::wstring exePath = entry.path().wstring() + L"\\RobloxPlayerBeta.exe";
                    if (fs::exists(exePath, ec)) {
                        return exePath;
                    }
                }
            }
        }
    }
    return L"";
}

static DWORD QueryStatus(HINTERNET hReq) {
    DWORD status = 0, size = sizeof(status);
    HttpQueryInfoW(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL);
    return status;
}

static std::wstring QueryHeader(HINTERNET hReq, const wchar_t* name) {
    wchar_t buf[4096];
    wcscpy_s(buf, name);          // input = header name, output = header value
    DWORD size = sizeof(buf);
    if (HttpQueryInfoW(hReq, HTTP_QUERY_CUSTOM, buf, &size, NULL))
        return std::wstring(buf, size / sizeof(wchar_t));
    return L"";
}

static HINTERNET OpenTicketRequest(HINTERNET hConnect, const std::wstring& cookie, const std::wstring& csrf) {
    HINTERNET hReq = HttpOpenRequestW(hConnect, L"POST", L"/v1/authentication-ticket", NULL, NULL, NULL,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hReq) return NULL;

    std::wstring headers =
        L"Cookie: .ROBLOSECURITY=" + cookie + L"\r\n"
        L"Content-Type: application/json\r\n"
        L"Referer: https://www.roblox.com/games/4924922222/Brookhaven-RP\r\n"
        L"Origin: https://www.roblox.com\r\n";
    if (!csrf.empty()) headers += L"X-CSRF-Token: " + csrf + L"\r\n";

    HttpAddRequestHeadersW(hReq, headers.c_str(), (DWORD)-1L, HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
    return hReq;
}

std::wstring GetAuthTicket(const std::wstring& cookie) {
    HINTERNET hNet = InternetOpenW(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet) return L"";

    HINTERNET hConnect = InternetConnectW(hNet, L"auth.roblox.com", INTERNET_DEFAULT_HTTPS_PORT,
        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) { InternetCloseHandle(hNet); return L""; }

    std::wstring csrf, ticket;

    // Request 1: expected to return 403 plus an X-CSRF-Token header
    if (HINTERNET r = OpenTicketRequest(hConnect, cookie, L"")) {
        if (HttpSendRequestW(r, NULL, 0, (LPVOID)"", 0)) {
            DWORD st = QueryStatus(r);
            LogMessage(L"[*] CSRF request status: " + std::to_wstring(st));
            csrf = QueryHeader(r, L"x-csrf-token");
        }
        else {
            LogMessage(L"[-] CSRF request error: " + std::to_wstring(GetLastError()));
        }
        InternetCloseHandle(r);
    }

    // Request 2: the real request with the token
    if (!csrf.empty()) {
        if (HINTERNET r = OpenTicketRequest(hConnect, cookie, csrf)) {
            if (HttpSendRequestW(r, NULL, 0, (LPVOID)"", 0)) {
                DWORD st = QueryStatus(r);
                LogMessage(L"[*] Ticket request status: " + std::to_wstring(st));
                if (st == 200) ticket = QueryHeader(r, L"rbx-authentication-ticket");
            }
            else {
                LogMessage(L"[-] Ticket request error: " + std::to_wstring(GetLastError()));
            }
            InternetCloseHandle(r);
        }
    }
    else {
        LogMessage(L"[-] No CSRF token returned. Cookie is probably invalid or expired.");
    }

    InternetCloseHandle(hConnect);
    InternetCloseHandle(hNet);
    return ticket;
}

// Returns the account's real username, or L"" on failure
std::wstring GetRobloxUsername(const std::wstring& cookie) {
    std::wstring result;
    HINTERNET hNet = InternetOpenW(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet) return L"";

    HINTERNET hConn = InternetConnectW(hNet, L"users.roblox.com", INTERNET_DEFAULT_HTTPS_PORT,
        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (hConn) {
        HINTERNET hReq = HttpOpenRequestW(hConn, L"GET", L"/v1/users/authenticated", NULL, NULL, NULL,
            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_COOKIES, 0);
        if (hReq) {
            std::wstring h = L"Cookie: .ROBLOSECURITY=" + cookie + L"\r\n";
            HttpAddRequestHeadersW(hReq, h.c_str(), (DWORD)-1L, HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);

            if (HttpSendRequestW(hReq, NULL, 0, NULL, 0)) {
                std::string body;
                char buf[1024];
                DWORD read = 0;
                while (InternetReadFile(hReq, buf, sizeof(buf), &read) && read > 0)
                    body.append(buf, read);

                const std::string key = "\"name\":\""; // leading quote avoids matching "displayName"
                size_t s = body.find(key);
                if (s != std::string::npos) {
                    s += key.size();
                    size_t e = body.find('"', s);
                    if (e != std::string::npos) result = FromUtf8(body.substr(s, e - s));
                }
            }
            InternetCloseHandle(hReq);
        }
        InternetCloseHandle(hConn);
    }
    InternetCloseHandle(hNet);
    return result;
}

// ---------------------------------------------------------------------------
// Launching
// ---------------------------------------------------------------------------
// Closes the "ROBLOX_singletonEvent" handle inside every running Roblox client so
// the next client is allowed to start. Returns how many handles were closed.
typedef LONG(NTAPI* pfnNtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);
typedef LONG(NTAPI* pfnNtQueryObject)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct MY_UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
struct HANDLE_ENTRY_EX {
    PVOID Object; ULONG_PTR UniqueProcessId; ULONG_PTR HandleValue; ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex; USHORT ObjectTypeIndex; ULONG HandleAttributes; ULONG Reserved;
};
struct HANDLE_INFO_EX { ULONG_PTR NumberOfHandles; ULONG_PTR Reserved; HANDLE_ENTRY_EX Handles[1]; };

int CloseRobloxSingletonEvent() {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return 0;
    auto NtQuerySystemInformationFn = (pfnNtQuerySystemInformation)GetProcAddress(hNtdll, "NtQuerySystemInformation");
    auto NtQueryObjectFn = (pfnNtQueryObject)GetProcAddress(hNtdll, "NtQueryObject");
    if (!NtQuerySystemInformationFn || !NtQueryObjectFn) return 0;

    // Open every running Roblox client
    std::vector<std::pair<DWORD, HANDLE>> procs;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"RobloxPlayerBeta.exe") == 0) {
                    HANDLE h = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pe.th32ProcessID);
                    if (h) procs.push_back({ pe.th32ProcessID, h });
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
    if (procs.empty()) return 0;

    // SystemExtendedHandleInformation (64) has a stable 64-bit layout
    ULONG size = 4 * 1024 * 1024;
    std::vector<BYTE> buffer;
    LONG status;
    do {
        buffer.resize(size);
        ULONG ret = 0;
        status = NtQuerySystemInformationFn(64, buffer.data(), size, &ret);
        if (status == (LONG)0xC0000004) size *= 2;
    } while (status == (LONG)0xC0000004 && size < 512u * 1024 * 1024);

    int closed = 0;
    if (status >= 0) {
        auto* info = (HANDLE_INFO_EX*)buffer.data();
        for (ULONG_PTR i = 0; i < info->NumberOfHandles; i++) {
            const HANDLE_ENTRY_EX& e = info->Handles[i];

            HANDLE hProc = NULL;
            for (auto& p : procs) if (p.first == (DWORD)e.UniqueProcessId) { hProc = p.second; break; }
            if (!hProc) continue;

            HANDLE dup = NULL;
            if (!DuplicateHandle(hProc, (HANDLE)e.HandleValue, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
                continue;

            // Only query names of Event objects (querying some other types can hang)
            alignas(8) BYTE typeBuf[1024] = {};
            ULONG ret = 0;
            bool isEvent = false;
            if (NtQueryObjectFn(dup, 2, typeBuf, sizeof(typeBuf), &ret) >= 0) {
                auto* t = (MY_UNICODE_STRING*)typeBuf;
                isEvent = t->Buffer && t->Length == 5 * sizeof(WCHAR) && wcsncmp(t->Buffer, L"Event", 5) == 0;
            }

            if (isEvent) {
                alignas(8) BYTE nameBuf[2048] = {};
                if (NtQueryObjectFn(dup, 1, nameBuf, sizeof(nameBuf), &ret) >= 0) {
                    auto* n = (MY_UNICODE_STRING*)nameBuf;
                    if (n->Buffer && n->Length > 0) {
                        std::wstring objName(n->Buffer, n->Length / sizeof(WCHAR));
                        if (objName.find(L"ROBLOX_singletonEvent") != std::wstring::npos) {
                            HANDLE dummy = NULL;
                            if (DuplicateHandle(hProc, (HANDLE)e.HandleValue, GetCurrentProcess(), &dummy,
                                0, FALSE, DUPLICATE_CLOSE_SOURCE)) {
                                if (dummy) CloseHandle(dummy);
                                closed++;
                            }
                        }
                    }
                }
            }
            CloseHandle(dup);
        }
    }

    for (auto& p : procs) CloseHandle(p.second);
    return closed;
}

void LaunchSelectedAccount() {
    int idx = (int)SendMessageW(hListAccounts, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR) {
        LogMessage(L"[-] Please select an account from the list first!");
        return;
    }

    RobloxAccount& acc = savedAccounts[idx];
    LogMessage(L"[+] Requesting auth ticket for: " + acc.name);

    std::wstring ticket = GetAuthTicket(acc.cookie);
    if (ticket.empty()) {
        LogMessage(L"[-] Failed to get authentication ticket. Account cookie may be expired; re-add the account.");
        return;
    }
    LogMessage(L"[+] Ticket acquired successfully.");

    int closedEvents = CloseRobloxSingletonEvent();
    if (closedEvents > 0)
        LogMessage(L"[*] Released singleton event in running Roblox (" + std::to_wstring(closedEvents) + L").");

    static std::mt19937_64 rng{ std::random_device{}() };
    std::uniform_int_distribution<unsigned long long> dist(10000000000ULL, 99999999999ULL);
    std::wstring tracker = std::to_wstring(dist(rng));

    if (USE_PROTOCOL_LAUNCH) {
        std::wstring uri = L"roblox-player:1+launchmode:app+gameinfo:" + ticket +
            L"+launchtime:" + std::to_wstring(GetTickCount64()) +
            L"+browsertrackerid:" + tracker +
            L"+robloxLocale:en_us+gameLocale:en_us+LaunchExp:InApp";
        HINSTANCE r = ShellExecuteW(NULL, L"open", uri.c_str(), NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)r > 32) LogMessage(L"[+] Launched Roblox (protocol) for " + acc.name);
        else LogMessage(L"[-] Protocol launch failed.");
        return;
    }

    std::wstring exePath = GetRobloxExecutablePath();
    if (exePath.empty()) {
        LogMessage(L"[-] RobloxPlayerBeta.exe could not be found locally.");
        return;
    }

    // No -j launcher URL, so the client opens to the Roblox menu instead of a game
    std::wstring cmdLine = L"\"" + exePath + L"\" --app -t " + ticket + L" -b " + tracker;

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exePath.c_str(), &cmdLine[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        LogMessage(L"[+] Launched Roblox for " + acc.name);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    else {
        LogMessage(L"[-] Process creation failed. Error: " + std::to_wstring(GetLastError()));
    }
}

// ---------------------------------------------------------------------------
// Embedded browser window used to log in and capture the account
// ---------------------------------------------------------------------------
LRESULT CALLBACK WebViewDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static Microsoft::WRL::ComPtr<ICoreWebView2Controller> webController;
    static Microsoft::WRL::ComPtr<ICoreWebView2> webView;
    static bool accountCaptured = false;

    switch (msg) {
    case WM_CREATE: {
        accountCaptured = false;

        PWSTR pathLocalAppData = NULL;
        std::wstring userDataFolder = L"";
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &pathLocalAppData))) {
            userDataFolder = std::wstring(pathLocalAppData) + L"\\RoMulti_BrowserData";
            CoTaskMemFree(pathLocalAppData);
        }

        CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder.c_str(), nullptr,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(result)) {
                        LogMessage(L"[-] WebView2 runtime not available.");
                        return result;
                    }

                    env->CreateCoreWebView2Controller(hwnd,
                        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [hwnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                                if (!controller) return S_OK;

                                webController = controller;
                                webController->get_CoreWebView2(&webView);

                                RECT bounds;
                                GetClientRect(hwnd, &bounds);
                                webController->put_Bounds(bounds);

                                webView->Navigate(L"https://www.roblox.com/login");

                                webView->add_NavigationCompleted(
                                    Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [hwnd](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                            LPWSTR uri = nullptr;
                                            sender->get_Source(&uri);

                                            bool onHome = uri && wcsstr(uri, L"roblox.com/home") != nullptr;
                                            if (uri) CoTaskMemFree(uri);
                                            if (!onHome || accountCaptured) return S_OK;

                                            Microsoft::WRL::ComPtr<ICoreWebView2_2> webView2;
                                            if (FAILED(sender->QueryInterface(IID_PPV_ARGS(&webView2)))) return S_OK;

                                            Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> cookieManager;
                                            if (FAILED(webView2->get_CookieManager(&cookieManager))) return S_OK;

                                            cookieManager->GetCookies(L"https://www.roblox.com",
                                                Microsoft::WRL::Callback<ICoreWebView2GetCookiesCompletedHandler>(
                                                    [hwnd](HRESULT result, ICoreWebView2CookieList* cookieList) -> HRESULT {
                                                        if (FAILED(result) || accountCaptured) return S_OK;

                                                        UINT count = 0;
                                                        cookieList->get_Count(&count);
                                                        for (UINT i = 0; i < count; i++) {
                                                            Microsoft::WRL::ComPtr<ICoreWebView2Cookie> cookie;
                                                            cookieList->GetValueAtIndex(i, &cookie);

                                                            LPWSTR name = nullptr;
                                                            cookie->get_Name(&name);
                                                            bool isAuth = name && _wcsicmp(name, L".ROBLOSECURITY") == 0;
                                                            if (name) CoTaskMemFree(name);
                                                            if (!isAuth) continue;

                                                            LPWSTR value = nullptr;
                                                            cookie->get_Value(&value);
                                                            std::wstring robloxSecurity(value ? value : L"");
                                                            if (value) CoTaskMemFree(value);

                                                            if (robloxSecurity.find(L"_|WARNING") == std::wstring::npos) continue;

                                                            accountCaptured = true;

                                                            std::wstring uname = GetRobloxUsername(robloxSecurity);
                                                            if (uname.empty())
                                                                uname = L"Account_" + std::to_wstring(savedAccounts.size() + 1);

                                                            AddOrUpdateAccount(uname, robloxSecurity);
                                                            LogMessage(L"[+] Added account: " + uname);

                                                            PostMessageW(hwnd, WM_CLOSE, 0, 0);
                                                            break;
                                                        }
                                                        return S_OK;
                                                    }).Get());
                                            return S_OK;
                                        }).Get(), nullptr);
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());
        break;
    }
    case WM_SIZE: {
        if (webController) {
            RECT bounds;
            GetClientRect(hwnd, &bounds);
            webController->put_Bounds(bounds);
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        if (webController) webController->Close();
        webController.Reset();
        webView.Reset();
        break;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

void OpenAddAccountWindow(HINSTANCE hInstance) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WebViewDlgProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"WebViewPopupClass";
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc); // harmless if already registered

    HWND hPopup = CreateWindowW(L"WebViewPopupClass", L"Log into Roblox (Embedded Browser)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, S(500), S(600), hMainWnd, NULL, hInstance, NULL);

    ShowWindow(hPopup, SW_SHOW);
    UpdateWindow(hPopup);
}

// ---------------------------------------------------------------------------
// Main window: dark theme matching the website preview
// ---------------------------------------------------------------------------
static HFONT hFontUI = NULL;
static HFONT hFontLog = NULL;

static void ApplyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF caption = RGB(17, 17, 17), text = RGB(170, 170, 170);
    DwmSetWindowAttribute(hwnd, 35 /*CAPTION_COLOR (Win11)*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /*TEXT_COLOR (Win11)*/, &text, sizeof(text));
}

static BOOL CALLBACK SetFontProc(HWND child, LPARAM) {
    SendMessageW(child, WM_SETFONT, (WPARAM)hFontUI, TRUE);
    return TRUE;
}

static void DrawFrameAround(HDC hdc, HWND parent, HWND child) {
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(NULL, parent, (POINT*)&r, 2);
    InflateRect(&r, 1, 1);
    HBRUSH b = CreateSolidBrush(RGB(68, 68, 68));
    FrameRect(hdc, &r, b);
    DeleteObject(b);
}

static void DrawButton(const DRAWITEMSTRUCT* d) {
    bool primary = d->CtlID == ID_BTN_LAUNCH;
    bool pressed = (d->itemState & ODS_SELECTED) != 0;
    RECT r = d->rcItem;

    HBRUSH fill = CreateSolidBrush(pressed ? RGB(60, 60, 64) : RGB(45, 45, 48));
    FillRect(d->hDC, &r, fill);
    DeleteObject(fill);

    HBRUSH border = CreateSolidBrush(primary ? RGB(43, 75, 255) : RGB(85, 85, 85));
    FrameRect(d->hDC, &r, border);
    if (primary) {
        RECT inner = r;
        InflateRect(&inner, -1, -1);
        FrameRect(d->hDC, &inner, border);
    }
    DeleteObject(border);

    wchar_t text[64] = {};
    GetWindowTextW(d->hwndItem, text, 64);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, RGB(220, 220, 220));
    HFONT old = (HFONT)SelectObject(d->hDC, hFontUI);
    DrawTextW(d->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(d->hDC, old);

    if (d->itemState & ODS_FOCUS) {
        RECT f = d->rcItem;
        InflateRect(&f, -S(4), -S(4));
        DrawFocusRect(d->hDC, &f);
    }
}

static void DrawListItem(const DRAWITEMSTRUCT* d) {
    if (d->itemID == (UINT)-1) return;
    bool selected = (d->itemState & ODS_SELECTED) != 0;

    HBRUSH bg = CreateSolidBrush(selected ? RGB(255, 226, 122) : RGB(37, 37, 38));
    FillRect(d->hDC, &d->rcItem, bg);
    DeleteObject(bg);

    wchar_t text[256] = {};
    SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)text);

    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, selected ? RGB(22, 26, 35) : RGB(220, 220, 220));
    RECT r = d->rcItem;
    r.left += S(10);
    HFONT old = (HFONT)SelectObject(d->hDC, hFontUI);
    DrawTextW(d->hDC, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(d->hDC, old);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

    switch (msg) {
    case WM_CREATE:
        hBrushBackground = CreateSolidBrush(RGB(30, 30, 30));
        hBrushEdit = CreateSolidBrush(RGB(37, 37, 38));

        hFontUI = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        hFontLog = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        ApplyDarkTitleBar(hwnd);

        CreateWindowW(L"STATIC", L"Saved Accounts:", WS_VISIBLE | WS_CHILD,
            S(20), S(15), S(160), S(20), hwnd, NULL, NULL, NULL);

        // Owner-drawn list so the selected account can be highlighted yellow
        hListAccounts = CreateWindowW(L"LISTBOX", L"",
            WS_VISIBLE | WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            S(21), S(36), S(338), S(118), hwnd, (HMENU)ID_LIST_ACCOUNTS, NULL, NULL);

        CreateWindowW(L"BUTTON", L"+ Add Account", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            S(20), S(168), S(110), S(32), hwnd, (HMENU)ID_BTN_OPEN_ADD, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Remove", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            S(140), S(168), S(80), S(32), hwnd, (HMENU)ID_BTN_REMOVE, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Launch", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            S(230), S(168), S(130), S(32), hwnd, (HMENU)ID_BTN_LAUNCH, NULL, NULL);

        hEditLog = CreateWindowW(L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY,
            S(21), S(216), S(338), S(154), hwnd, (HMENU)ID_EDIT_LOG, NULL, NULL);

        EnumChildWindows(hwnd, SetFontProc, 0);
        SendMessageW(hEditLog, WM_SETFONT, (WPARAM)hFontLog, TRUE);

        // Dark scrollbars
        SetWindowTheme(hListAccounts, L"DarkMode_Explorer", NULL);
        SetWindowTheme(hEditLog, L"DarkMode_Explorer", NULL);

        LoadAccounts();
        LogMessage(L"[*] RoMulti Account Manager Initialized. Accounts loaded: " + std::to_wstring(savedAccounts.size()));
        break;

    case WM_MEASUREITEM: {
        auto* m = (MEASUREITEMSTRUCT*)lp;
        if (m->CtlID == ID_LIST_ACCOUNTS) {
            m->itemHeight = S(26);
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        auto* d = (DRAWITEMSTRUCT*)lp;
        if (d->CtlID == ID_LIST_ACCOUNTS) { DrawListItem(d); return TRUE; }
        if (d->CtlType == ODT_BUTTON) { DrawButton(d); return TRUE; }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawFrameAround(hdc, hwnd, hListAccounts);
        DrawFrameAround(hdc, hwnd, hEditLog);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, RGB(220, 220, 220));

        // Read-only edit controls (the log) report as STATIC
        bool isInput = (msg != WM_CTLCOLORSTATIC) || GetDlgCtrlID((HWND)lp) == ID_EDIT_LOG;
        if (isInput) {
            SetBkColor(hdc, RGB(37, 37, 38));
            return (INT_PTR)hBrushEdit;
        }
        SetBkColor(hdc, RGB(30, 30, 30));
        return (INT_PTR)hBrushBackground;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_OPEN_ADD) {
            OpenAddAccountWindow(hInstance);
        }
        else if (LOWORD(wp) == ID_BTN_LAUNCH) {
            LaunchSelectedAccount();
        }
        else if (LOWORD(wp) == ID_BTN_REMOVE) {
            int idx = (int)SendMessageW(hListAccounts, LB_GETCURSEL, 0, 0);
            if (idx != LB_ERR) {
                savedAccounts.erase(savedAccounts.begin() + idx);
                SendMessageW(hListAccounts, LB_DELETESTRING, idx, 0);
                SaveAccounts();
            }
        }
        break;

    case WM_DESTROY:
        if (hBrushBackground) DeleteObject(hBrushBackground);
        if (hBrushEdit) DeleteObject(hBrushEdit);
        if (hFontUI) DeleteObject(hFontUI);
        if (hFontLog) DeleteObject(hFontLog);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

int APIENTRY WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    // Render at the real screen resolution instead of being bitmap-stretched (fixes blurry text)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    g_dpi = (int)GetDpiForSystem();

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); // required by WebView2

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RoMultiManagerClass";
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    RegisterClassW(&wc);

    hMainWnd = CreateWindowW(L"RoMultiManagerClass", L"RoMulti - Account Manager",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, S(400), S(430), NULL, NULL, hInstance, NULL);

    ShowWindow(hMainWnd, nCmdShow);
    UpdateWindow(hMainWnd);

    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
