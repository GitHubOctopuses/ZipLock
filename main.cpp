// 7z 批量加密/解密工具
// 引擎: LZMA SDK (Igor Pavlov, public domain) 自带的 7zr.exe，与本程序放在同一目录。
// 加密: 递归把文件夹内每个文件(跳过 .7z)压成 原名+.7z，AES-256 + 文件名加密，成功后删除原文件。
// 解密: 递归把文件夹内每个 .7z 解回原文件，成功后删除 .7z。
// 编译: build.bat

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

namespace fs = std::filesystem;

// ---------------- 常量 ----------------
static const wchar_t* kAppTitle   = L"ZipLock";

// 视觉方案：7-Zip 蓝（取自图标）做唯一主色，贯穿主按钮/进度条；
// 次按钮白底描边；信息区两级灰。背景纯白。
static const COLORREF kBg           = RGB(255, 255, 255);
static const COLORREF kPrimary      = RGB(24, 100, 220);   // 主蓝
static const COLORREF kPrimaryHover = RGB(20, 86, 192);
static const COLORREF kPrimaryDown  = RGB(15, 72, 168);
static const COLORREF kSecFill      = RGB(247, 248, 250);  // 次按钮
static const COLORREF kSecHover     = RGB(232, 238, 248);
static const COLORREF kSecDown      = RGB(220, 230, 245);
static const COLORREF kSecText      = RGB(50, 58, 70);
static const COLORREF kBorder       = RGB(203, 209, 218);
static const COLORREF kDisabledFill = RGB(240, 241, 243);
static const COLORREF kDisabledText = RGB(168, 175, 186);
static const COLORREF kProgBack     = RGB(234, 237, 241);  // 进度条底
static const COLORREF kSep          = RGB(234, 236, 240);  // 分隔线
static const COLORREF kTextMain     = RGB(42, 47, 56);     // 状态行
static const COLORREF kTextSub      = RGB(153, 160, 168);  // 提示行
static const COLORREF kTextLabel    = RGB(64, 68, 76);     // "文件夹路径"

enum { MODE_ENCRYPT = 1, MODE_DECRYPT = 2 };

// 控件 ID
enum {
    IDC_APPICON = 100, IDC_LABEL_PATH, IDC_EDIT, IDC_BROWSE,
    IDC_ENCRYPT, IDC_DECRYPT, IDC_PROGRESS, IDC_STATUS, IDC_DETAIL, IDC_SEP,
    IDC_KEYEDIT, IDC_LABEL_KEY
};

// 工作线程 -> UI 消息
enum {
    WM_APP_RANGE  = WM_APP + 1,   // wParam = 文件总数（扫描完成）
    WM_APP_STATUS = WM_APP + 2,   // lParam = heap wchar_t*（状态行）
    WM_APP_ONE    = WM_APP + 3,   // wParam = 已完成个数
    WM_APP_DETAIL = WM_APP + 4,   // lParam = heap wchar_t*（统计行）
    WM_APP_DONE   = WM_APP + 5    // lParam = heap wchar_t*（结束消息）
};

// ---------------- 全局 ----------------
static HWND  g_hwnd, g_icon, g_labelPath, g_edit, g_browse, g_btnEnc, g_btnDec, g_prog, g_status, g_detail;
static HWND  g_keyEdit, g_labelKey, g_sep;
static HFONT g_font, g_fontSmall;
static HBRUSH g_bgBrush;
static HINSTANCE g_hInst;
static int   g_dpi = 96;
static bool  g_hover[3] = { false, false, false };   // encrypt / decrypt / browse
static std::thread g_worker;
static std::atomic<bool> g_cancel{ false };
static bool g_busy = false, g_quitPending = false;

static int Scale(int x) { return MulDiv(x, g_dpi, 96); }

// ---------------- 7zr.exe 调用 ----------------
static std::wstring SevenZrPath()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir = buf;
    size_t p = dir.find_last_of(L"\\/");
    return dir.substr(0, p + 1) + L"7zr.exe";
}

struct SevenZrResult {
    bool started = false;   // 进程是否成功启动
    int  exitCode = -1;
};

static SevenZrResult Run7z(const std::wstring& args)
{
    SevenZrResult r;
    std::wstring exe = SevenZrPath();
    std::wstring cmd = L"\"" + exe + L"\" " + args;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr); CloseHandle(rd);
        return r;
    }
    CloseHandle(wr);
    r.started = true;

    // 持续吸收子进程输出，防止管道写满阻塞
    char buf[8192]; DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) { /* 丢弃 */ }
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = (int)code;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return r;
}

// ---------------- 任务核心（GUI 与 CLI 共用） ----------------
struct TaskCallbacks {
    std::function<void(int total)>              onRange;    // 扫描完成，设进度范围
    std::function<void(const std::wstring&)>    onStatus;   // 状态行
    std::function<void(int done)>               onOne;      // 完成 1 个
    std::function<void(const std::wstring&)>    onDetail;   // 统计行
    std::function<void(const std::wstring&)>    onDone;     // 任务结束
    std::function<bool()>                       cancelled;
};

struct Stats { int ok = 0, skip = 0, fail = 0, delFail = 0; };

static std::wstring StatsLine(const Stats& st)
{
    std::wstring s = L"已完成 " + std::to_wstring(st.ok) + L" ｜ 跳过 " +
                     std::to_wstring(st.skip) + L" ｜ 失败 " + std::to_wstring(st.fail);
    if (st.delFail) s += L" ｜ 原文件删除失败 " + std::to_wstring(st.delFail);
    return s;
}

// wantArchives=true 收集 .7z，否则收集除 .7z 外的所有普通文件
static std::vector<fs::path> Collect(const fs::path& root, bool wantArchives)
{
    std::vector<fs::path> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    for (fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;
        bool is7z = _wcsicmp(it->path().extension().c_str(), L".7z") == 0;
        if (is7z == wantArchives) out.push_back(it->path());
    }
    return out;
}

static std::wstring BuildDoneMsg(const wchar_t* what, const Stats& st, bool cancelled)
{
    if (cancelled) {
        std::wstring s = std::wstring(what) + L"已取消：完成 " + std::to_wstring(st.ok) +
                         L"，跳过 " + std::to_wstring(st.skip) + L"，失败 " + std::to_wstring(st.fail);
        return s;
    }
    std::wstring s = std::wstring(what) + L"完成：成功 " + std::to_wstring(st.ok) +
                     L"，跳过 " + std::to_wstring(st.skip) + L"，失败 " + std::to_wstring(st.fail);
    if (st.delFail) s += L"，删除 " + std::to_wstring(st.delFail) + L" 个已处理文件失败";
    if (st.fail || st.delFail) s += L"（相关文件均已保留）";
    return s;
}

static void EncryptTask(const std::wstring& rootW, const std::wstring& key, const TaskCallbacks& cb)
{
    const wchar_t* what = L"加密";
    Stats st;
    std::vector<fs::path> files;
    try {
        cb.onStatus(L"正在扫描文件…");
        files = Collect(fs::path(rootW), false);
    } catch (...) {
        cb.onDone(L"扫描失败：无法读取该文件夹");
        return;
    }
    int N = (int)files.size();
    cb.onRange(N);
    if (N == 0) { cb.onDone(L"该文件夹（含子文件夹）下没有可加密的文件"); return; }

    int k = 0;
    for (const auto& f : files) {
        if (cb.cancelled()) { cb.onDone(BuildDoneMsg(what, st, true)); return; }
        ++k;
        fs::path out = f; out += L".7z";
        std::wstring head = L"正在加密 (" + std::to_wstring(k) + L"/" + std::to_wstring(N) + L")：";
        std::error_code ec;
        if (fs::exists(out, ec)) {
            st.skip++;
            cb.onStatus(head + f.filename().wstring() + L"　[跳过：已存在同名 .7z]");
            cb.onOne(k); cb.onDetail(StatsLine(st));
            continue;
        }
        cb.onStatus(head + f.filename().wstring());
        std::wstring args = std::wstring(L"a -p\"") + key + L"\" -mhe=on -y -- \"" +
                            out.wstring() + L"\" \"" + f.wstring() + L"\"";
        SevenZrResult r = Run7z(args);
        bool ok = r.started && r.exitCode == 0;
        if (ok) ok = fs::exists(out, ec) && !ec && fs::file_size(out, ec) > 0 && !ec;
        if (ok) {
            if (!DeleteFileW(f.c_str())) {
                st.delFail++;
                cb.onStatus(head + f.filename().wstring() +
                            L"　[.7z 已生成，但删除原文件失败，错误码 " +
                            std::to_wstring(GetLastError()) + L"]");
            }
            st.ok++;
        } else {
            st.fail++;
            cb.onStatus(head + f.filename().wstring() +
                        L"　[失败：原文件已保留]（7zr 退出码 " +
                        std::to_wstring(r.started ? r.exitCode : -1) + L"）");
        }
        cb.onOne(k); cb.onDetail(StatsLine(st));
    }
    cb.onDone(BuildDoneMsg(what, st, false));
}

static void DecryptTask(const std::wstring& rootW, const std::wstring& key, const TaskCallbacks& cb)
{
    const wchar_t* what = L"解密";
    Stats st;
    std::vector<fs::path> archives;
    try {
        cb.onStatus(L"正在扫描 .7z 文件…");
        archives = Collect(fs::path(rootW), true);
    } catch (...) {
        cb.onDone(L"扫描失败：无法读取该文件夹");
        return;
    }
    int N = (int)archives.size();
    cb.onRange(N);
    if (N == 0) { cb.onDone(L"该文件夹（含子文件夹）下没有找到 .7z 文件"); return; }

    int k = 0;
    for (const auto& arc : archives) {
        if (cb.cancelled()) { cb.onDone(BuildDoneMsg(what, st, true)); return; }
        ++k;
        fs::path target = arc; target.replace_extension();   // a.docx.7z -> a.docx
        std::wstring head = L"正在解密 (" + std::to_wstring(k) + L"/" + std::to_wstring(N) + L")：";
        std::error_code ec;
        if (fs::exists(target, ec)) {
            st.skip++;
            cb.onStatus(head + arc.filename().wstring() + L"　[跳过：解出的同名文件已存在]");
            cb.onOne(k); cb.onDetail(StatsLine(st));
            continue;
        }
        cb.onStatus(head + arc.filename().wstring());
        std::wstring args = std::wstring(L"x -p\"") + key + L"\" -y -o\"" +
                            arc.parent_path().wstring() + L"\" -- \"" + arc.wstring() + L"\"";
        SevenZrResult r = Run7z(args);
        bool ok = r.started && r.exitCode == 0 && fs::exists(target, ec) && !ec;
        if (ok) {
            if (!DeleteFileW(arc.c_str())) {
                st.delFail++;
                cb.onStatus(head + arc.filename().wstring() +
                            L"　[文件已解出，但删除 .7z 失败，错误码 " +
                            std::to_wstring(GetLastError()) + L"]");
            }
            st.ok++;
        } else {
            st.fail++;
            cb.onStatus(head + arc.filename().wstring() +
                        L"　[失败：.7z 已保留]（7zr 退出码 " +
                        std::to_wstring(r.started ? r.exitCode : -1) + L"）");
        }
        cb.onOne(k); cb.onDetail(StatsLine(st));
    }
    cb.onDone(BuildDoneMsg(what, st, false));
}

// ---------------- CLI 模式（冒烟测试 / 命令行使用） ----------------
static void OutUtf8(const std::wstring& s)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u8(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u8.data(), len, nullptr, nullptr);
    u8 += "\r\n";
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n = 0;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, u8.data(), (DWORD)u8.size(), &n, nullptr);
}

static int RunCli(int argc, LPWSTR* argv)
{
    if (argc < 5) { OutUtf8(L"用法: ZipLock.exe --cli encrypt|decrypt <文件夹路径> <密钥>"); return 2; }
    std::wstring mode = argv[2], dir = argv[3], key = argv[4];
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { OutUtf8(L"错误：不是有效的文件夹：" + dir); return 2; }
    if (key.empty()) { OutUtf8(L"错误：密钥不能为空"); return 2; }
    if (key.find(L'"') != std::wstring::npos) { OutUtf8(L"错误：密钥不能包含英文双引号"); return 2; }
    TaskCallbacks cb;
    cb.onRange = [](int) {}; cb.onOne = [](int) {}; cb.onDetail = [](const std::wstring&) {};
    cb.onStatus = OutUtf8; cb.onDone = OutUtf8; cb.cancelled = [] { return false; };
    if (_wcsicmp(mode.c_str(), L"encrypt") == 0)      EncryptTask(dir, key, cb);
    else if (_wcsicmp(mode.c_str(), L"decrypt") == 0) DecryptTask(dir, key, cb);
    else { OutUtf8(L"未知模式：" + mode); return 2; }
    return 0;
}

// ---------------- GUI ----------------
static void PostLine(HWND h, UINT msg, const std::wstring& s)
{
    wchar_t* p = new wchar_t[s.size() + 1];
    wcscpy_s(p, s.size() + 1, s.c_str());
    PostMessageW(h, msg, 0, (LPARAM)p);
}

static TaskCallbacks MakeGuiCallbacks(HWND h)
{
    TaskCallbacks cb;
    cb.onRange   = [h](int n) { PostMessageW(h, WM_APP_RANGE, (WPARAM)n, 0); };
    cb.onStatus  = [h](const std::wstring& s) { PostLine(h, WM_APP_STATUS, s); };
    cb.onOne     = [h](int k) { PostMessageW(h, WM_APP_ONE, (WPARAM)k, 0); };
    cb.onDetail  = [h](const std::wstring& s) { PostLine(h, WM_APP_DETAIL, s); };
    cb.onDone    = [h](const std::wstring& s) { PostLine(h, WM_APP_DONE, s); };
    cb.cancelled = [] { return g_cancel.load(); };
    return cb;
}

static std::wstring GetEditPath()
{
    int len = GetWindowTextLengthW(g_edit);
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(g_edit, s.data(), len + 1);
    s.resize(len);
    // 去掉粘贴路径可能带的引号和首尾空白
    size_t b = s.find_first_not_of(L" \t\"'");
    size_t e = s.find_last_not_of(L" \t\"'");
    if (b == std::wstring::npos) return L"";
    return s.substr(b, e - b + 1);
}

static std::wstring GetKey()
{
    int len = GetWindowTextLengthW(g_keyEdit);
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(g_keyEdit, s.data(), len + 1);
    s.resize(len);
    // 密钥只去首尾空白，原样保留其余字符
    size_t b = s.find_first_not_of(L" \t");
    size_t e = s.find_last_not_of(L" \t");
    if (b == std::wstring::npos) return L"";
    return s.substr(b, e - b + 1);
}

static bool IsDriveRoot(const std::wstring& p)
{
    std::wstring t = p;
    if (t.size() == 2 && t[1] == L':') return true;
    if (t.size() == 3 && t[1] == L':' && (t[2] == L'\\' || t[2] == L'/')) return true;
    return false;
}

static std::wstring PickFolder(HWND owner)
{
    std::wstring res;
    IFileDialog* fd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&fd)))) {
        DWORD opts = 0;
        fd->GetOptions(&opts);
        fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        fd->SetTitle(L"选择要处理的文件夹");
        if (SUCCEEDED(fd->Show(owner))) {
            IShellItem* si = nullptr;
            if (SUCCEEDED(fd->GetResult(&si))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    res = p;
                    CoTaskMemFree(p);
                }
                si->Release();
            }
        }
        fd->Release();
    }
    return res;
}

static void StartTask(int mode)
{
    std::wstring dir = GetEditPath();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) {
        MessageBoxW(g_hwnd, L"请先在输入框填写一个有效的文件夹路径。", kAppTitle, MB_OK | MB_ICONWARNING);
        SetFocus(g_edit);
        return;
    }
    std::wstring key = GetKey();
    if (key.empty()) {
        MessageBoxW(g_hwnd, L"请先输入密钥，加密与解密必须使用同一个密钥。", kAppTitle, MB_OK | MB_ICONWARNING);
        SetFocus(g_keyEdit);
        return;
    }
    if (key.find(L'"') != std::wstring::npos) {
        MessageBoxW(g_hwnd, L"密钥不能包含英文双引号 \"，请更换一个密钥。", kAppTitle, MB_OK | MB_ICONWARNING);
        SetFocus(g_keyEdit);
        return;
    }
    if (IsDriveRoot(dir) &&
        MessageBoxW(g_hwnd,
            L"输入的是整个盘的根目录，将会处理全盘的文件！\n确定继续吗？",
            kAppTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    if (!fs::exists(fs::path(SevenZrPath()), ec)) {
        MessageBoxW(g_hwnd, L"未找到 7zr.exe，请将它与本程序放在同一目录。", kAppTitle, MB_OK | MB_ICONERROR);
        return;
    }

    g_busy = true; g_cancel = false; g_quitPending = false;
    EnableWindow(g_btnEnc, FALSE); EnableWindow(g_btnDec, FALSE);
    EnableWindow(g_edit, FALSE);   EnableWindow(g_browse, FALSE);
    EnableWindow(g_keyEdit, FALSE);
    SendMessageW(g_prog, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_status, mode == MODE_ENCRYPT ? L"状态: 正在扫描文件…" : L"状态: 正在扫描 .7z 文件…");
    SetWindowTextW(g_detail, mode == MODE_ENCRYPT
        ? L"提示: 请勿关闭程序，加密成功后原文件自动删除"
        : L"提示: 请勿关闭程序，解密成功后 .7z 自动删除");

    HWND h = g_hwnd;
    g_worker = std::thread([h, mode, dir, key]() {
        TaskCallbacks cb = MakeGuiCallbacks(h);
        if (mode == MODE_ENCRYPT) EncryptTask(dir, key, cb);
        else                      DecryptTask(dir, key, cb);
    });
}

static void OnTaskDone(HWND hwnd, wchar_t* msg)
{
    if (g_worker.joinable()) g_worker.join();
    g_busy = false;
    EnableWindow(g_btnEnc, TRUE); EnableWindow(g_btnDec, TRUE);
    EnableWindow(g_edit, TRUE);   EnableWindow(g_browse, TRUE);
    EnableWindow(g_keyEdit, TRUE);
    std::wstring done = std::wstring(L"状态: ") + msg;
    SetWindowTextW(g_status, done.c_str());
    delete[] msg;
    bool quit = g_quitPending;
    g_quitPending = false;
    if (quit) DestroyWindow(hwnd);
}

// ---------------- 自绘按钮（主=蓝实心，次=浅底描边） ----------------
static void DrawFancyButton(HDC hdc, const RECT& rc, bool hover, bool pressed,
                            bool disabled, bool primary, const wchar_t* text)
{
    FillRect(hdc, &rc, g_bgBrush);   // 圆角外露出父窗口背景
    COLORREF fill, textColor, border;
    if (primary) {
        fill    = disabled ? kDisabledFill : pressed ? kPrimaryDown : hover ? kPrimaryHover : kPrimary;
        textColor = disabled ? kDisabledText : RGB(255, 255, 255);
        border  = fill;
    } else {
        fill    = disabled ? kDisabledFill : pressed ? kSecDown : hover ? kSecHover : kSecFill;
        textColor = disabled ? kDisabledText : kSecText;
        border  = disabled ? RGB(227, 229, 233) : kBorder;
    }
    int r = Scale(6) + 1;
    HRGN rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right, rc.bottom, r, r);
    HBRUSH fb = CreateSolidBrush(fill);
    FillRgn(hdc, rgn, fb);
    DeleteObject(fb);
    HBRUSH bb = CreateSolidBrush(border);
    FrameRgn(hdc, rgn, bb, 1, 1);
    DeleteObject(bb);
    DeleteObject(rgn);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, textColor);
    DrawTextW(hdc, text, -1, (LPRECT)&rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// 悬停跟踪：owner-draw 按钮本身不提供 hover，需要子类化
static LRESULT CALLBACK BtnSubProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR idx, DWORD_PTR)
{
    switch (msg) {
    case WM_MOUSEMOVE: {
        if (!g_hover[idx]) {
            g_hover[idx] = true;
            InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        break;
    }
    case WM_MOUSELEAVE:
        g_hover[idx] = false;
        InvalidateRect(h, nullptr, FALSE);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void Layout()
{
    int m = Scale(24);
    RECT rc; GetClientRect(g_hwnd, &rc);
    int W = rc.right;
    int lw = Scale(76), gap = Scale(8), bw = Scale(76);
    int eh = Scale(30), bh = Scale(36), ph = Scale(14);
    int rx = m + Scale(48) + Scale(16);      // 图标右侧内容的统一左轴
    int ix = rx + lw + gap;                  // 输入框左轴

    MoveWindow(g_icon, m, Scale(34), Scale(48), Scale(48), TRUE);

    MoveWindow(g_labelPath, rx, Scale(25), lw, Scale(17), TRUE);
    int bx = W - m - bw;
    MoveWindow(g_edit, ix, Scale(18), bx - gap - ix, eh, TRUE);
    MoveWindow(g_browse, bx, Scale(18), bw, eh, TRUE);

    MoveWindow(g_labelKey, rx, Scale(59), lw, Scale(17), TRUE);
    MoveWindow(g_keyEdit, ix, Scale(52), W - m - ix, eh, TRUE);

    MoveWindow(g_btnEnc, ix, Scale(94), Scale(130), bh, TRUE);
    MoveWindow(g_btnDec, ix + Scale(138), Scale(94), Scale(130), bh, TRUE);

    MoveWindow(g_sep, m, Scale(146), W - m * 2, Scale(1), TRUE);
    MoveWindow(g_prog, m, Scale(160), W - m * 2, ph, TRUE);
    MoveWindow(g_status, m, Scale(180), W - m * 2, Scale(18), TRUE);
    MoveWindow(g_detail, m, Scale(200), W - m * 2, Scale(16), TRUE);
}

static HFONT MakeFontPt(int pt, int dpi)
{
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_bgBrush = CreateSolidBrush(kBg);
        g_icon = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ICON, 0, 0, 0, 0, hwnd, (HMENU)IDC_APPICON, nullptr, nullptr);
        SendMessageW(g_icon, STM_SETIMAGE, IMAGE_ICON,
            (LPARAM)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, Scale(48), Scale(48), LR_DEFAULTCOLOR));
        g_labelPath = CreateWindowExW(0, L"STATIC", L"文件夹路径:",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_LABEL_PATH, nullptr, nullptr);
        g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_EDIT, nullptr, nullptr);
        SendMessageW(g_edit, EM_SETCUEBANNER, FALSE,
            (LPARAM)L"例如 D:\\资料，或点击右侧浏览");
        g_labelKey = CreateWindowExW(0, L"STATIC", L"密钥:",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_LABEL_KEY, nullptr, nullptr);
        g_keyEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
            0, 0, 0, 0, hwnd, (HMENU)IDC_KEYEDIT, nullptr, nullptr);
        SendMessageW(g_keyEdit, EM_SETCUEBANNER, FALSE,
            (LPARAM)L"输入密钥（加密与解密必须一致）");
        g_browse = CreateWindowExW(0, L"BUTTON", L"浏览…",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_BROWSE, nullptr, nullptr);
        g_btnEnc = CreateWindowExW(0, L"BUTTON", L"一键加密",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_ENCRYPT, nullptr, nullptr);
        g_btnDec = CreateWindowExW(0, L"BUTTON", L"一键解密",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_DECRYPT, nullptr, nullptr);
        SetWindowSubclass(g_btnEnc,  BtnSubProc, 0, 0);
        SetWindowSubclass(g_btnDec,  BtnSubProc, 1, 0);
        SetWindowSubclass(g_browse,  BtnSubProc, 2, 0);
        g_prog = CreateWindowExW(0, PROGRESS_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, hwnd, (HMENU)IDC_PROGRESS, nullptr, nullptr);
        // 关掉进度条主题才能用自定义颜色；配色与主按钮一致
        SetWindowTheme(g_prog, L"", L"");
        SendMessageW(g_prog, PBM_SETBKCOLOR, 0, (LPARAM)kProgBack);
        SendMessageW(g_prog, PBM_SETBARCOLOR, 0, (LPARAM)kPrimary);
        g_sep = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_SEP, nullptr, nullptr);
        g_status = CreateWindowExW(0, L"STATIC", L"状态: 请输入密钥与文件夹路径",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, nullptr, nullptr);
        g_detail = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_DETAIL, nullptr, nullptr);
        return 0;

    case WM_SETFONT: return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        if (g_font) DeleteObject(g_font);
        if (g_fontSmall) DeleteObject(g_fontSmall);
        g_font = MakeFontPt(9, g_dpi);
        g_fontSmall = MakeFontPt(8, g_dpi);
        SendMessageW(g_icon, STM_SETIMAGE, IMAGE_ICON,
            (LPARAM)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, Scale(48), Scale(48), LR_DEFAULTCOLOR));
        EnumChildWindows(hwnd, [](HWND c, LPARAM lp) -> BOOL {
            SendMessageW(c, WM_SETFONT, (WPARAM)(HFONT)lp, TRUE); return TRUE;
        }, (LPARAM)g_font);
        SendMessageW(g_detail, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top,
                     r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_SIZE: Layout(); return 0;

    case WM_CTLCOLORDLG:
        return (LRESULT)g_bgBrush;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetBkColor(hdc, kBg);
        int id = GetDlgCtrlID((HWND)lp);
        if (id == IDC_LABEL_PATH || id == IDC_LABEL_KEY)
            SetTextColor(hdc, kTextLabel);
        else if (id == IDC_DETAIL)
            SetTextColor(hdc, kTextSub);
        else if (id == IDC_STATUS)
            SetTextColor(hdc, kTextMain);
        else
            SetTextColor(hdc, kTextMain);
        if (id == IDC_SEP)
            return (LRESULT)CreateSolidBrush(kSep);
        return (LRESULT)g_bgBrush;
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* d = (const DRAWITEMSTRUCT*)lp;
        if (d->CtlType != ODT_BUTTON) break;
        int id = d->CtlID;
        bool hover = false;
        if (id == IDC_ENCRYPT) hover = g_hover[0];
        else if (id == IDC_DECRYPT) hover = g_hover[1];
        else if (id == IDC_BROWSE) hover = g_hover[2];
        wchar_t buf[64];
        GetWindowTextW(d->hwndItem, buf, 64);
        DrawFancyButton(d->hDC, d->rcItem, hover,
                        (d->itemState & ODS_SELECTED) != 0,
                        (d->itemState & ODS_DISABLED) != 0,
                        id == IDC_ENCRYPT, buf);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE: {
            std::wstring d = PickFolder(hwnd);
            if (!d.empty()) SetWindowTextW(g_edit, d.c_str());
            SetFocus(g_edit);
            return 0;
        }
        case IDC_ENCRYPT: StartTask(MODE_ENCRYPT); return 0;
        case IDC_DECRYPT: StartTask(MODE_DECRYPT); return 0;
        }
        break;

    case WM_APP_RANGE:
        SendMessageW(g_prog, PBM_SETRANGE32, 0, (LPARAM)(int)wp);   // wParam=min=0, lParam=max=N
        SendMessageW(g_prog, PBM_SETPOS, 0, 0);
        return 0;
    case WM_APP_ONE:
        SendMessageW(g_prog, PBM_SETPOS, (WPARAM)wp, 0);
        return 0;
    case WM_APP_STATUS: {
        std::wstring s = std::wstring(L"状态: ") + (const wchar_t*)lp;
        SetWindowTextW(g_status, s.c_str());
        delete[] (wchar_t*)lp;
        return 0;
    }
    case WM_APP_DETAIL: {
        std::wstring s = std::wstring(L"提示: 请勿关闭程序 ｜ ") + (const wchar_t*)lp;
        SetWindowTextW(g_detail, s.c_str());
        delete[] (wchar_t*)lp;
        return 0;
    }
    case WM_APP_DONE:
        OnTaskDone(hwnd, (wchar_t*)lp);
        return 0;

    case WM_CLOSE:
        if (g_busy) {
            if (MessageBoxW(hwnd,
                    L"任务正在进行中，确定要退出吗？\n（当前文件处理完即停止；已完成的加密/解密不受影响）",
                    kAppTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                g_cancel = true;
                g_quitPending = true;
                SetWindowTextW(g_status, L"状态: 正在取消，等待当前文件处理完…");
            }
            return 0;   // 等 WM_APP_DONE 后再销毁
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow)
{
    g_hInst = hInst;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && wcscmp(argv[1], L"--cli") == 0) {
        int rc = RunCli(argc, argv);
        LocalFree(argv);
        return rc;
    }
    if (argv) LocalFree(argv);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    UINT dpi = GetDpiForSystem();
    g_dpi = (int)dpi;
    g_font = MakeFontPt(9, g_dpi);
    g_fontSmall = MakeFontPt(8, g_dpi);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SevenZipBatchToolWnd";
    RegisterClassExW(&wc);

    // 固定客户区约 600x188，再换算成窗口外框尺寸
    RECT rc{ 0, 0, Scale(600), Scale(240) };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             FALSE, 0, g_dpi);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, kAppTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);
    g_hwnd = hwnd;

    EnumChildWindows(hwnd, [](HWND c, LPARAM lp) -> BOOL {
        SendMessageW(c, WM_SETFONT, (WPARAM)(HFONT)lp, TRUE); return TRUE;
    }, (LPARAM)g_font);
    SendMessageW(g_detail, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_worker.joinable()) g_worker.join();
    CoUninitialize();
    return 0;
}
