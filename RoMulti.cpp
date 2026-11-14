#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <random>
#include <iterator>
#include <thread>
#include <atomic>
#include <algorithm>
#include <functional>
#include <memory>
#include <cwctype>
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
#define ID_EDIT_PLACE     4
#define ID_LIST_ACCOUNTS  5
#define ID_BTN_REMOVE     6
#define ID_LIST_RUNNING   7
#define ID_BTN_CLOSE      8
#define ID_BTN_CLOSE_ALL  9
#define ID_BTN_RELOGIN    10
#define ID_BTN_FAVORITES  11
#define IDM_FAV_SAVE      5000
#define IDM_FAV_PICK      5100
#define IDM_FAV_REMOVE    5300
#define ID_TIMER_RUNNING  1

// Messages that worker threads post to the main window
#define WM_APP_LOG        (WM_APP + 1)
#define WM_APP_STATUS     (WM_APP + 2)
#define WM_APP_LAUNCHED   (WM_APP + 3)
#define WM_APP_START      (WM_APP + 4)
#define WM_APP_FAVORITE   (WM_APP + 5)

enum LoginStatus { LOGIN_CHECKING = 0, LOGIN_OK = 1, LOGIN_EXPIRED = 2, LOGIN_UNKNOWN = 3 };

HWND hMainWnd = NULL;
HWND hEditLog = NULL;
HWND hListAccounts = NULL;
HWND hListRunning = NULL;
HWND hEditPlace = NULL;
static DWORD g_mainThreadId = 0;
HBRUSH hBrushBackground = NULL;
HBRUSH hBrushEdit = NULL;

#define IDI_APPICON 101
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

struct RobloxAccount {
    std::wstring name;
    std::wstring cookie;
    int status = LOGIN_CHECKING;   // runtime only, never saved
};
std::vector<RobloxAccount> savedAccounts;

struct RunningClient { DWORD pid; std::wstring account; };
std::vector<RunningClient> runningClients;

struct StatusMsg { std::wstring name; int status; };
struct LaunchedMsg { DWORD pid; std::wstring name; };

struct Favorite { std::wstring id; std::wstring name; };
std::vector<Favorite> favorites;
struct FavoriteMsg { std::wstring id; std::wstring name; };
static std::atomic<bool> g_launching{ false };

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void LogMessage(const std::wstring& message) {
    if (!hEditLog) return;
    if (GetCurrentThreadId() != g_mainThreadId) {
        // Called from a worker thread: hand the text to the UI thread
        if (hMainWnd) PostMessageW(hMainWnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(message));
        return;
    }
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
            a.cookie = cookie;          // same account logged in again: refresh its cookie
            a.status = LOGIN_OK;
            SaveAccounts();
            if (hListAccounts) InvalidateRect(hListAccounts, NULL, TRUE);
            return;
        }
    }
    savedAccounts.push_back({ name, cookie, LOGIN_OK });
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

// Checks whether a saved login still works: LOGIN_OK, LOGIN_EXPIRED, or LOGIN_UNKNOWN (network problem)
int CheckCookieStatus(const std::wstring& cookie) {
    int result = LOGIN_UNKNOWN;
    HINTERNET hNet = InternetOpenW(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet) return result;

    HINTERNET hConn = InternetConnectW(hNet, L"users.roblox.com", INTERNET_DEFAULT_HTTPS_PORT,
        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (hConn) {
        HINTERNET hReq = HttpOpenRequestW(hConn, L"GET", L"/v1/users/authenticated", NULL, NULL, NULL,
            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_COOKIES, 0);
        if (hReq) {
            std::wstring h = L"Cookie: .ROBLOSECURITY=" + cookie + L"\r\n";
            HttpAddRequestHeadersW(hReq, h.c_str(), (DWORD)-1L, HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
            if (HttpSendRequestW(hReq, NULL, 0, NULL, 0)) {
                DWORD st = QueryStatus(hReq);
                if (st == 200) result = LOGIN_OK;
                else if (st == 401 || st == 403) result = LOGIN_EXPIRED;
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

// --- helpers shared by the launch / running-clients features ---
static std::vector<int> GetSelectedIndices(HWND list) {
    std::vector<int> v;
    int n = (int)SendMessageW(list, LB_GETSELCOUNT, 0, 0);
    if (n <= 0) return v;
    v.resize(n);
    SendMessageW(list, LB_GETSELITEMS, n, (LPARAM)v.data());
    return v;
}

static fs::path GetSettingsPath() {
    return GetSavePath().parent_path() / L"settings.ini";
}

// Accepts "123456", or a full link such as https://www.roblox.com/games/123456/Name
static std::wstring ExtractPlaceId(const std::wstring& input) {
    std::wstring digits;
    size_t pos = input.find(L"/games/");
    if (pos != std::wstring::npos) {
        for (size_t i = pos + 7; i < input.size() && iswdigit(input[i]); i++) digits += input[i];
        return digits;
    }
    for (wchar_t c : input) {
        if (iswdigit(c)) digits += c;
        else if (c != L' ') return L"";   // not a plain number
    }
    return digits;
}

static bool IsRobloxProcess(HANDLE h) {
    wchar_t path[MAX_PATH * 2];
    DWORD len = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (!QueryFullProcessImageNameW(h, 0, path, &len)) return false;
    const wchar_t* exe = L"RobloxPlayerBeta.exe";
    size_t exeLen = wcslen(exe);
    return len >= exeLen && _wcsicmp(path + len - exeLen, exe) == 0;
}

static void RefreshRunningList() {
    SendMessageW(hListRunning, LB_RESETCONTENT, 0, 0);
    for (auto& r : runningClients) {
        std::wstring t = r.account + L"   (PID " + std::to_wstring(r.pid) + L")";
        SendMessageW(hListRunning, LB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
    InvalidateRect(hListAccounts, NULL, TRUE); // accounts list shows a "running" tag
}

// Drop clients whose window/process has gone away
static void PruneRunning() {
    bool changed = false;
    for (size_t i = 0; i < runningClients.size();) {
        bool alive = false;
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, runningClients[i].pid);
        if (h) {
            alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT && IsRobloxProcess(h);
            CloseHandle(h);
        }
        if (!alive) { runningClients.erase(runningClients.begin() + i); changed = true; }
        else i++;
    }
    if (changed) RefreshRunningList();
}

static void CloseClients(std::vector<int> indices) {
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    for (int i : indices) {
        if (i < 0 || i >= (int)runningClients.size()) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, runningClients[i].pid);
        if (h) {
            if (IsRobloxProcess(h)) {
                TerminateProcess(h, 0);
                LogMessage(L"[*] Closed Roblox for " + runningClients[i].account);
            }
            CloseHandle(h);
        }
        runningClients.erase(runningClients.begin() + i);
    }
    RefreshRunningList();
}

// Runs on a background thread so the window stays responsive while launching
static void LaunchWorker(std::vector<RobloxAccount> accounts, std::wstring placeId) {
    static std::mt19937_64 rng{ std::random_device{}() };
    std::uniform_int_distribution<unsigned long long> dist(10000000000ULL, 99999999999ULL);

    for (size_t i = 0; i < accounts.size(); i++) {
        if (i > 0) Sleep(3500); // give the previous client time to start before the next one
        const RobloxAccount& acc = accounts[i];

        LogMessage(L"[+] Requesting auth ticket for: " + acc.name);
        std::wstring ticket = GetAuthTicket(acc.cookie);
        if (ticket.empty()) {
            LogMessage(L"[-] Failed to get authentication ticket for " + acc.name + L".");
            if (hMainWnd && CheckCookieStatus(acc.cookie) == LOGIN_EXPIRED)
                PostMessageW(hMainWnd, WM_APP_STATUS, 0, (LPARAM)new StatusMsg{ acc.name, LOGIN_EXPIRED });
            continue;
        }
        LogMessage(L"[+] Ticket acquired successfully.");

        int closedEvents = CloseRobloxSingletonEvent();
        if (closedEvents > 0)
            LogMessage(L"[*] Released singleton event in running Roblox (" + std::to_wstring(closedEvents) + L").");

        std::wstring tracker = std::to_wstring(dist(rng));

        if (USE_PROTOCOL_LAUNCH) {
            if (!placeId.empty()) LogMessage(L"[*] Game ID is ignored in protocol launch mode.");
            std::wstring uri = L"roblox-player:1+launchmode:app+gameinfo:" + ticket +
                L"+launchtime:" + std::to_wstring(GetTickCount64()) +
                L"+browsertrackerid:" + tracker +
                L"+robloxLocale:en_us+gameLocale:en_us+LaunchExp:InApp";
            HINSTANCE r = ShellExecuteW(NULL, L"open", uri.c_str(), NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)r > 32) LogMessage(L"[+] Launched Roblox (protocol) for " + acc.name + L" (not tracked in Running).");
            else LogMessage(L"[-] Protocol launch failed.");
            continue;
        }

        std::wstring exePath = GetRobloxExecutablePath();
        if (exePath.empty()) {
            LogMessage(L"[-] RobloxPlayerBeta.exe could not be found locally.");
            break;
        }

        // No -j launcher URL = Roblox menu; with a game ID = join that game
        std::wstring cmdLine = L"\"" + exePath + L"\" --app -t " + ticket + L" -b " + tracker;
        if (!placeId.empty()) {
            std::wstring url = L"https://assetgame.roblox.com/game/PlaceLauncher.ashx?request=RequestGame"
                L"&browserTrackerId=" + tracker + L"&placeId=" + placeId + L"&isPlayTogetherGame=false";
            cmdLine += L" -j \"" + url + L"\"";
        }

        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(exePath.c_str(), &cmdLine[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            LogMessage(L"[+] Launched Roblox for " + acc.name);
            if (hMainWnd)
                PostMessageW(hMainWnd, WM_APP_LAUNCHED, 0, (LPARAM)new LaunchedMsg{ pi.dwProcessId, acc.name });
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        else {
            LogMessage(L"[-] Process creation failed. Error: " + std::to_wstring(GetLastError()));
        }
    }
    g_launching = false;
}

// ---------------------------------------------------------------------------
// Favorite games (saved in favorites.txt next to your accounts)
// ---------------------------------------------------------------------------
static fs::path GetFavoritesPath() {
    return GetSavePath().parent_path() / L"favorites.txt";
}

static void SaveFavorites() {
    std::string out;
    for (auto& f : favorites) {
        std::wstring name = f.name;
        for (auto& c : name) if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        out += ToUtf8(f.id) + "\t" + ToUtf8(name) + "\n";
    }
    std::ofstream file(GetFavoritesPath(), std::ios::binary | std::ios::trunc);
    file.write(out.data(), (std::streamsize)out.size());
}

static void LoadFavorites() {
    std::ifstream file(GetFavoritesPath(), std::ios::binary);
    if (!file) return;
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (pos < data.size()) {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        std::string line = data.substr(pos, eol - pos);
        pos = eol + 1;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        favorites.push_back({ FromUtf8(line.substr(0, tab)), FromUtf8(line.substr(tab + 1)) });
    }
}

static std::string HttpsGet(const wchar_t* host, const std::wstring& path) {
    std::string body;
    HINTERNET hNet = InternetOpenW(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet) return body;
    HINTERNET hConn = InternetConnectW(hNet, host, INTERNET_DEFAULT_HTTPS_PORT,
        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (hConn) {
        HINTERNET hReq = HttpOpenRequestW(hConn, L"GET", path.c_str(), NULL, NULL, NULL,
            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_COOKIES, 0);
        if (hReq) {
            if (HttpSendRequestW(hReq, NULL, 0, NULL, 0) && QueryStatus(hReq) == 200) {
                char buf[2048];
                DWORD read = 0;
                while (InternetReadFile(hReq, buf, sizeof(buf), &read) && read > 0)
                    body.append(buf, read);
            }
            InternetCloseHandle(hReq);
        }
        InternetCloseHandle(hConn);
    }
    InternetCloseHandle(hNet);
    return body;
}

// Reads the JSON string value that follows `key` (for example "\"name\":\"")
static std::wstring JsonStringAfter(const std::string& body, const std::string& key) {
    size_t s = body.find(key);
    if (s == std::string::npos) return L"";
    s += key.size();

    std::string raw;
    for (size_t i = s; i < body.size(); i++) {
        if (body[i] == '\\' && i + 1 < body.size()) { raw += body[i]; raw += body[++i]; continue; }
        if (body[i] == '"') break;
        raw += body[i];
    }

    std::wstring w = FromUtf8(raw), out;
    for (size_t i = 0; i < w.size(); i++) {
        if (w[i] == L'\\' && i + 1 < w.size()) {
            wchar_t c = w[++i];
            if (c == L'u' && i + 4 < w.size()) {
                out += (wchar_t)wcstoul(w.substr(i + 1, 4).c_str(), NULL, 16);
                i += 4;
            }
            else if (c == L'n' || c == L't' || c == L'r') out += L' ';
            else out += c;   // \" \\ \/
        }
        else out += w[i];
    }
    return out;
}

// Looks up a game's display name from its place ID (empty string if it fails)
static std::wstring ResolveGameName(const std::wstring& placeId) {
    std::string r1 = HttpsGet(L"apis.roblox.com", L"/universes/v1/places/" + placeId + L"/universe");
    const std::string key = "\"universeId\":";
    size_t p = r1.find(key);
    if (p == std::string::npos) return L"";
    p += key.size();
    std::string uid;
    while (p < r1.size() && isdigit((unsigned char)r1[p])) uid += r1[p++];
    if (uid.empty()) return L"";

    std::string r2 = HttpsGet(L"games.roblox.com", L"/v1/games?universeIds=" + FromUtf8(uid));
    return JsonStringAfter(r2, "\"name\":\"");
}

static void SaveCurrentAsFavorite() {
    wchar_t buf[512] = {};
    GetWindowTextW(hEditPlace, buf, 512);
    std::wstring id = ExtractPlaceId(buf);
    if (id.empty()) {
        LogMessage(L"[-] Type a game ID or link first, then save it.");
        return;
    }
    for (auto& f : favorites) {
        if (f.id == id) { LogMessage(L"[*] That game is already in your favorites."); return; }
    }

    LogMessage(L"[*] Looking up the game name...");
    HWND hwnd = hMainWnd;
    std::thread([hwnd, id]() {
        std::wstring name = ResolveGameName(id);
        if (name.empty()) name = L"Game " + id;
        if (name.size() > 40) name = name.substr(0, 40) + L"...";
        PostMessageW(hwnd, WM_APP_FAVORITE, 0, (LPARAM)new FavoriteMsg{ id, name });
        }).detach();
}

// Data attached to each owner-drawn (dark) menu item
struct MenuItemData { std::wstring text; bool separator = false; bool submenu = false; };

static void ShowFavoritesMenu(HWND hwnd) {
    std::vector<std::unique_ptr<MenuItemData>> keep;   // must outlive the menu
    auto add = [&](HMENU menu, UINT flags, UINT_PTR id, const std::wstring& text, bool sep = false, bool sub = false) {
        auto data = std::make_unique<MenuItemData>();
        data->text = text;
        data->separator = sep;
        data->submenu = sub;
        AppendMenuW(menu, flags | MF_OWNERDRAW, id, (LPCWSTR)data.get());
        keep.push_back(std::move(data));
        };

    HMENU menu = CreatePopupMenu();
    HMENU removeMenu = CreatePopupMenu();

    if (favorites.empty()) add(menu, MF_STRING | MF_GRAYED, 0, L"(no favorites yet)");
    for (size_t i = 0; i < favorites.size() && i < 99; i++) {
        add(menu, MF_STRING, IDM_FAV_PICK + i, favorites[i].name + L"  (" + favorites[i].id + L")");
        add(removeMenu, MF_STRING, IDM_FAV_REMOVE + i, favorites[i].name);
    }
    add(menu, MF_STRING | MF_GRAYED, 0, L"", true);   // separator line
    add(menu, MF_STRING, IDM_FAV_SAVE, L"Save current game as favorite");
    if (!favorites.empty()) add(menu, MF_POPUP, (UINT_PTR)removeMenu, L"Remove favorite", false, true);

    RECT r;
    GetWindowRect(GetDlgItem(hwnd, ID_BTN_FAVORITES), &r);
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, hwnd, NULL);
    DestroyMenu(menu);   // also frees the submenu

    if (cmd == IDM_FAV_SAVE) {
        SaveCurrentAsFavorite();
    }
    else if (cmd >= IDM_FAV_PICK && cmd < IDM_FAV_PICK + 99) {
        size_t i = (size_t)(cmd - IDM_FAV_PICK);
        if (i < favorites.size()) SetWindowTextW(hEditPlace, favorites[i].id.c_str());
    }
    else if (cmd >= IDM_FAV_REMOVE && cmd < IDM_FAV_REMOVE + 99) {
        size_t i = (size_t)(cmd - IDM_FAV_REMOVE);
        if (i < favorites.size()) {
            LogMessage(L"[*] Removed favorite: " + favorites[i].name);
            favorites.erase(favorites.begin() + i);
            SaveFavorites();
        }
    }
}

// Launch every selected account (one after another)
void StartLaunch() {
    if (g_launching) { LogMessage(L"[*] Already launching, please wait."); return; }

    std::vector<int> sel = GetSelectedIndices(hListAccounts);
    if (sel.empty()) {
        LogMessage(L"[-] Please select an account from the list first!");
        return;
    }

    wchar_t buf[512] = {};
    GetWindowTextW(hEditPlace, buf, 512);
    std::wstring input(buf), placeId;
    if (input.find_first_not_of(L' ') != std::wstring::npos) {
        placeId = ExtractPlaceId(input);
        if (placeId.empty()) {
            LogMessage(L"[-] Game ID must be a number or a roblox.com/games/... link. Leave it empty for the menu.");
            return;
        }
    }
    WritePrivateProfileStringW(L"General", L"GameId", input.c_str(), GetSettingsPath().c_str());

    std::vector<RobloxAccount> toLaunch;
    for (int i : sel) {
        if (i < 0 || i >= (int)savedAccounts.size()) continue;
        if (savedAccounts[i].status == LOGIN_EXPIRED) {
            LogMessage(L"[-] " + savedAccounts[i].name + L": login expired. Select it and click Re-login.");
            continue;
        }
        toLaunch.push_back(savedAccounts[i]);
    }
    if (toLaunch.empty()) return;

    if (placeId.empty()) LogMessage(L"[*] Opening the Roblox menu.");
    else LogMessage(L"[*] Joining game " + placeId);
    if (toLaunch.size() > 1)
        LogMessage(L"[*] Launching " + std::to_wstring(toLaunch.size()) + L" accounts, one every few seconds.");

    g_launching = true;
    std::thread(LaunchWorker, toLaunch, placeId).detach();
}

// Checks every saved login in the background and marks expired ones
void StartLoginCheck(HWND hwnd) {
    std::vector<std::pair<std::wstring, std::wstring>> items;
    for (auto& a : savedAccounts) items.push_back({ a.name, a.cookie });
    if (items.empty()) return;

    LogMessage(L"[*] Checking saved logins...");
    std::thread([hwnd, items]() {
        for (auto& it : items) {
            int st = CheckCookieStatus(it.second);
            PostMessageW(hwnd, WM_APP_STATUS, 0, (LPARAM)new StatusMsg{ it.first, st });
        }
        }).detach();
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

                                // Start logged out so every login window is a clean session
                                Microsoft::WRL::ComPtr<ICoreWebView2_2> wv2;
                                if (SUCCEEDED(webView.As(&wv2))) {
                                    Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> cm;
                                    if (SUCCEEDED(wv2->get_CookieManager(&cm))) cm->DeleteAllCookies();
                                }
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

// ---- Dark popup menu (owner-drawn) ----
static void MeasureMenuItem(MEASUREITEMSTRUCT* m) {
    auto* data = (MenuItemData*)m->itemData;
    if (!data) return;
    if (data->separator) {
        m->itemWidth = S(100);
        m->itemHeight = S(9);
        return;
    }
    HDC hdc = GetDC(hMainWnd);
    HFONT old = (HFONT)SelectObject(hdc, hFontUI);
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, data->text.c_str(), (int)data->text.size(), &sz);
    SelectObject(hdc, old);
    ReleaseDC(hMainWnd, hdc);
    m->itemWidth = sz.cx + S(data->submenu ? 56 : 36);
    m->itemHeight = S(28);
}

static void DrawMenuItem(const DRAWITEMSTRUCT* d) {
    auto* data = (MenuItemData*)d->itemData;
    if (!data) return;
    bool disabled = (d->itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
    bool hot = (d->itemState & ODS_SELECTED) != 0 && !disabled;
    RECT r = d->rcItem;

    HBRUSH bg = CreateSolidBrush(hot ? RGB(255, 226, 122) : RGB(37, 37, 38));
    FillRect(d->hDC, &r, bg);
    DeleteObject(bg);

    if (data->separator) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(68, 68, 68));
        HPEN oldPen = (HPEN)SelectObject(d->hDC, pen);
        int y = (r.top + r.bottom) / 2;
        MoveToEx(d->hDC, r.left + S(8), y, NULL);
        LineTo(d->hDC, r.right - S(8), y);
        SelectObject(d->hDC, oldPen);
        DeleteObject(pen);
        return;
    }

    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, disabled ? RGB(120, 120, 125) : (hot ? RGB(22, 26, 35) : RGB(230, 230, 230)));
    HFONT old = (HFONT)SelectObject(d->hDC, hFontUI);
    RECT tr = r;
    tr.left += S(14);
    tr.right -= S(14);
    DrawTextW(d->hDC, data->text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (data->submenu)
        DrawTextW(d->hDC, L"\x203A", -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(d->hDC, old);
}

// Best effort: ask Windows to draw the menu frame dark too (undocumented, Windows 10 1903+ / 11)
static void EnableDarkMenus() {
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) return;
    typedef int (WINAPI* SetPreferredAppModeFn)(int);
    typedef void (WINAPI* FlushMenuThemesFn)();
    auto setMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) {
        setMode(2 /* ForceDark */);
        if (flush) flush();
    }
}

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

// Draws both lists. The accounts list also shows a status tag on the right
// ("running", "login expired" or "checking...").
static void DrawListItem(const DRAWITEMSTRUCT* d) {
    if (d->itemID == (UINT)-1) return;
    bool selected = (d->itemState & ODS_SELECTED) != 0;

    HBRUSH bg = CreateSolidBrush(selected ? RGB(255, 226, 122) : RGB(37, 37, 38));
    FillRect(d->hDC, &d->rcItem, bg);
    DeleteObject(bg);

    wchar_t text[256] = {};
    SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)text);

    const wchar_t* tag = L"";
    COLORREF tagColor = RGB(220, 220, 220);
    if (d->CtlID == ID_LIST_ACCOUNTS && d->itemID < savedAccounts.size()) {
        const RobloxAccount& a = savedAccounts[d->itemID];
        bool isRunning = false;
        for (auto& r : runningClients)
            if (_wcsicmp(r.account.c_str(), a.name.c_str()) == 0) { isRunning = true; break; }

        if (isRunning) { tag = L"running"; tagColor = selected ? RGB(15, 105, 45) : RGB(90, 200, 120); }
        else if (a.status == LOGIN_EXPIRED) { tag = L"login expired"; tagColor = selected ? RGB(170, 30, 30) : RGB(255, 120, 120); }
        else if (a.status == LOGIN_CHECKING) { tag = L"checking..."; tagColor = selected ? RGB(90, 90, 90) : RGB(140, 140, 150); }
    }

    SetBkMode(d->hDC, TRANSPARENT);
    HFONT old = (HFONT)SelectObject(d->hDC, hFontUI);
    RECT r = d->rcItem;
    r.left += S(10);
    r.right -= S(10);

    if (tag[0]) {
        SIZE sz = {};
        GetTextExtentPoint32W(d->hDC, tag, (int)wcslen(tag), &sz);
        SetTextColor(d->hDC, tagColor);
        DrawTextW(d->hDC, tag, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        r.right -= sz.cx + S(10);
    }

    SetTextColor(d->hDC, selected ? RGB(22, 26, 35) : RGB(220, 220, 220));
    DrawTextW(d->hDC, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(d->hDC, old);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

    switch (msg) {
    case WM_CREATE: {
        hMainWnd = hwnd;
        hBrushBackground = CreateSolidBrush(RGB(30, 30, 30));
        hBrushEdit = CreateSolidBrush(RGB(37, 37, 38));

        hFontUI = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        hFontLog = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        ApplyDarkTitleBar(hwnd);

        // --- Saved accounts (several can be selected with Ctrl/Shift) ---
        CreateWindowW(L"STATIC", L"Saved Accounts (Ctrl+click to pick several):", WS_VISIBLE | WS_CHILD,
            S(20), S(12), S(340), S(20), hwnd, NULL, NULL, NULL);
        hListAccounts = CreateWindowW(L"LISTBOX", L"",
            WS_VISIBLE | WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            S(21), S(34), S(338), S(116), hwnd, (HMENU)ID_LIST_ACCOUNTS, NULL, NULL);

        CreateWindowW(L"BUTTON", L"+ Add Account", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            S(20), S(160), S(110), S(30), hwnd, (HMENU)ID_BTN_OPEN_ADD, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Re-login", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            S(145), S(160), S(100), S(30), hwnd, (HMENU)ID_BTN_RELOGIN, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Remove", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            S(260), S(160), S(100), S(30), hwnd, (HMENU)ID_BTN_REMOVE, NULL, NULL);

        // --- Optional game + Launch ---
        CreateWindowW(L"STATIC", L"Game ID (optional, empty = Roblox menu):", WS_VISIBLE | WS_CHILD,
            S(20), S(200), S(340), S(20), hwnd, NULL, NULL, NULL);
        hEditPlace = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
            S(21), S(225), S(150), S(26), hwnd, (HMENU)ID_EDIT_PLACE, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Favs", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            S(180), S(222), S(50), S(32), hwnd, (HMENU)ID_BTN_FAVORITES, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Launch", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            S(240), S(222), S(120), S(32), hwnd, (HMENU)ID_BTN_LAUNCH, NULL, NULL);

        // --- Running clients ---
        CreateWindowW(L"STATIC", L"Running clients:", WS_VISIBLE | WS_CHILD,
            S(20), S(266), S(200), S(20), hwnd, NULL, NULL, NULL);
        hListRunning = CreateWindowW(L"LISTBOX", L"",
            WS_VISIBLE | WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            S(21), S(289), S(218), S(70), hwnd, (HMENU)ID_LIST_RUNNING, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Close", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            S(250), S(288), S(110), S(34), hwnd, (HMENU)ID_BTN_CLOSE, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Close All", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            S(250), S(326), S(110), S(34), hwnd, (HMENU)ID_BTN_CLOSE_ALL, NULL, NULL);

        // --- Log ---
        hEditLog = CreateWindowW(L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY,
            S(21), S(373), S(338), S(130), hwnd, (HMENU)ID_EDIT_LOG, NULL, NULL);

        EnumChildWindows(hwnd, SetFontProc, 0);
        SendMessageW(hEditLog, WM_SETFONT, (WPARAM)hFontLog, TRUE);
        SendMessageW(hEditLog, EM_SETLIMITTEXT, 0, 0);
        SendMessageW(hEditPlace, EM_SETLIMITTEXT, 200, 0);
        SendMessageW(hEditPlace, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(6), S(6)));

        // Dark scrollbars
        SetWindowTheme(hListAccounts, L"DarkMode_Explorer", NULL);
        SetWindowTheme(hListRunning, L"DarkMode_Explorer", NULL);
        SetWindowTheme(hEditLog, L"DarkMode_Explorer", NULL);

        // Remember the last game ID
        wchar_t gid[256] = {};
        GetPrivateProfileStringW(L"General", L"GameId", L"", gid, 256, GetSettingsPath().c_str());
        SetWindowTextW(hEditPlace, gid);
        LoadFavorites();

        LoadAccounts();
        LogMessage(L"[*] RoMulti Account Manager Initialized. Accounts loaded: " + std::to_wstring(savedAccounts.size()));

        SetTimer(hwnd, ID_TIMER_RUNNING, 2000, NULL);
        PostMessageW(hwnd, WM_APP_START, 0, 0); // start the login check once the window is up
        break;
    }

    case WM_APP_START:
        StartLoginCheck(hwnd);
        return 0;

    case WM_APP_LOG: {
        auto* text = (std::wstring*)lp;
        LogMessage(*text);
        delete text;
        return 0;
    }

    case WM_APP_STATUS: {
        auto* m = (StatusMsg*)lp;
        for (auto& a : savedAccounts) {
            if (_wcsicmp(a.name.c_str(), m->name.c_str()) == 0) {
                a.status = m->status;
                if (m->status == LOGIN_EXPIRED)
                    LogMessage(L"[-] " + a.name + L": login expired. Select it and click Re-login.");
                break;
            }
        }
        InvalidateRect(hListAccounts, NULL, TRUE);
        delete m;
        return 0;
    }

    case WM_APP_LAUNCHED: {
        auto* m = (LaunchedMsg*)lp;
        runningClients.push_back({ m->pid, m->name });
        RefreshRunningList();
        delete m;
        return 0;
    }

    case WM_APP_FAVORITE: {
        auto* m = (FavoriteMsg*)lp;
        bool exists = false;
        for (auto& f : favorites) if (f.id == m->id) { exists = true; break; }
        if (!exists) {
            favorites.push_back({ m->id, m->name });
            SaveFavorites();
            LogMessage(L"[+] Saved favorite: " + m->name);
        }
        delete m;
        return 0;
    }

    case WM_TIMER:
        if (wp == ID_TIMER_RUNNING) PruneRunning();
        return 0;

    case WM_MEASUREITEM: {
        auto* m = (MEASUREITEMSTRUCT*)lp;
        if (m->CtlType == ODT_MENU) { MeasureMenuItem(m); return TRUE; }
        if (m->CtlID == ID_LIST_ACCOUNTS || m->CtlID == ID_LIST_RUNNING) {
            m->itemHeight = S(26);
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        auto* d = (DRAWITEMSTRUCT*)lp;
        if (d->CtlType == ODT_MENU) { DrawMenuItem(d); return TRUE; }
        if (d->CtlID == ID_LIST_ACCOUNTS || d->CtlID == ID_LIST_RUNNING) { DrawListItem(d); return TRUE; }
        if (d->CtlType == ODT_BUTTON) { DrawButton(d); return TRUE; }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawFrameAround(hdc, hwnd, hListAccounts);
        DrawFrameAround(hdc, hwnd, hEditPlace);
        DrawFrameAround(hdc, hwnd, hListRunning);
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
        switch (LOWORD(wp)) {
        case ID_BTN_OPEN_ADD:
            OpenAddAccountWindow(hInstance);
            break;

        case ID_BTN_RELOGIN: {
            std::vector<int> sel = GetSelectedIndices(hListAccounts);
            if (sel.empty() || sel[0] >= (int)savedAccounts.size()) {
                LogMessage(L"[-] Select the account you want to re-login first.");
            }
            else {
                LogMessage(L"[*] Log in as " + savedAccounts[sel[0]].name + L" to refresh its login.");
                OpenAddAccountWindow(hInstance);
            }
            break;
        }

        case ID_BTN_FAVORITES:
            ShowFavoritesMenu(hwnd);
            break;

        case ID_BTN_LAUNCH:
            StartLaunch();
            break;

        case ID_BTN_REMOVE: {
            std::vector<int> sel = GetSelectedIndices(hListAccounts);
            std::sort(sel.begin(), sel.end(), std::greater<int>());
            for (int idx : sel) {
                if (idx < 0 || idx >= (int)savedAccounts.size()) continue;
                savedAccounts.erase(savedAccounts.begin() + idx);
                SendMessageW(hListAccounts, LB_DELETESTRING, idx, 0);
            }
            if (!sel.empty()) SaveAccounts();
            break;
        }

        case ID_BTN_CLOSE: {
            std::vector<int> sel = GetSelectedIndices(hListRunning);
            if (sel.empty()) LogMessage(L"[-] Select a running client to close first.");
            else CloseClients(sel);
            break;
        }

        case ID_BTN_CLOSE_ALL: {
            if (runningClients.empty()) {
                LogMessage(L"[*] No running clients to close.");
                break;
            }
            std::vector<int> all;
            for (int i = 0; i < (int)runningClients.size(); i++) all.push_back(i);
            CloseClients(all);
            break;
        }
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_RUNNING);
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
    g_mainThreadId = GetCurrentThreadId();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    g_dpi = (int)GetDpiForSystem();

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); // required by WebView2
    EnableDarkMenus();

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
        CW_USEDEFAULT, CW_USEDEFAULT, S(400), S(570), NULL, NULL, hInstance, NULL);

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
