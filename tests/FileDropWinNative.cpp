// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Interactive test: real OLE drags onto a DGL window that accepts file drops, with input injected only over
// the test's own windows. First a drag from a DGL window of the same process (Window::startFileDrag), then
// drags from a second process, which reach the window only while its program world dispatches OLE's messages.
// The second process is this program again, run with --source.
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "dgl/Application.hpp"
#include "dgl/Window.hpp"
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstring>

static int checks = 0, failures = 0;

static void check(const bool ok, const char* const message)
{
    ++checks;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    std::fflush(stdout);
    if (! ok)
        ++failures;
}

static std::string toUtf8(const std::wstring& text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> utf8(std::size_t(size), 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    return std::string(utf8.data());
}

// --------------------------------------------------------------------------------------------------------------------
// the source process: one window, which starts a drag of one file when pressed, and reports how it ended

struct SourceData : IDataObject
{
    std::atomic<ULONG> references{1};
    const std::wstring path;

    explicit SourceData(std::wstring file) : path(std::move(file)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** const out) override
    {
        if (out == nullptr) return E_POINTER;
        *out = nullptr;
        if (! IsEqualIID(iid, IID_IUnknown) && ! IsEqualIID(iid, IID_IDataObject)) return E_NOINTERFACE;
        *out = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --references; if (n == 0) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* const format) override
    {
        if (format == nullptr) return E_POINTER;
        return format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL) != 0 ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* const format, STGMEDIUM* const medium) override
    {
        if (medium == nullptr) return E_POINTER;
        std::memset(medium, 0, sizeof(*medium));
        const HRESULT result = QueryGetData(format);
        if (FAILED(result)) return result;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t));
        if (memory == nullptr) return E_OUTOFMEMORY;
        DROPFILES* const files = static_cast<DROPFILES*>(GlobalLock(memory));
        files->pFiles = sizeof(DROPFILES);
        files->fWide = TRUE;
        std::memcpy(reinterpret_cast<char*>(files) + sizeof(DROPFILES), path.c_str(), (path.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC** const out) override
    { if (out != nullptr) *out = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

struct SourceFeedback : IDropSource
{
    std::atomic<ULONG> references{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** const out) override
    {
        if (out == nullptr) return E_POINTER;
        *out = nullptr;
        if (! IsEqualIID(iid, IID_IUnknown) && ! IsEqualIID(iid, IID_IDropSource)) return E_NOINTERFACE;
        *out = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --references; if (n == 0) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(const BOOL escape, const DWORD keys) override
    {
        if (escape) return DRAGDROP_S_CANCEL;
        return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

static std::wstring sourcePath;
static DWORD sourceAllowed = DROPEFFECT_COPY;
static DWORD sourceResult = 0x200; // never pressed

static LRESULT CALLBACK sourceProcedure(const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam)
{
    if (message == WM_LBUTTONDOWN)
    {
        SourceData* const data = new SourceData(sourcePath);
        SourceFeedback* const feedback = new SourceFeedback;
        DWORD effect = DROPEFFECT_NONE;
        const HRESULT result = DoDragDrop(data, feedback, sourceAllowed, &effect);
        sourceResult = (result == DRAGDROP_S_DROP ? 0x100U : 0U) | effect;
        feedback->Release();
        data->Release();
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_TIMER)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// exit code: 0x100 when the drag ended in a drop, or'd with the effect the target reported
static int runSource(const std::wstring& path, const DWORD allowed)
{
    sourcePath = path;
    sourceAllowed = allowed;
    if (FAILED(OleInitialize(nullptr)))
        return 0x400;

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = sourceProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"DafDropTestSource";
    RegisterClassW(&windowClass);
    const std::wstring title = L"Dusk drop source " + std::to_wstring(GetCurrentProcessId());
    const HWND window = CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, title.c_str(),
                                        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 360, 220, 180,
                                        nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window == nullptr)
        return 0x400;

    SetTimer(window, 1, 30000, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(window);
    OleUninitialize();
    return static_cast<int>(sourceResult);
}

// --------------------------------------------------------------------------------------------------------------------
// the target: a DGL window that takes every drop

START_NAMESPACE_DGL

struct DropWindow : Window
{
    int enters = 0, moves = 0, leaves = 0, drops = 0;
    std::vector<std::string> dropped;
    uint dropMods = 0;

    explicit DropWindow(Application& app) : Window(app) {}

protected:
    bool onFileDragEnter(const FileDropEvent&) override { ++enters; return true; }
    bool onFileDragMove(const FileDropEvent&) override { ++moves; return true; }
    void onFileDragLeave() override { ++leaves; }
    bool onFileDrop(const FileDropEvent& ev) override
    {
        ++drops;
        dropped.assign(ev.files, ev.files + ev.numFiles);
        dropMods = ev.mod;
        return true;
    }
};

END_NAMESPACE_DGL

static void sendMouse(const DWORD flags)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    SendInput(1, &input, sizeof(input));
}

static void sendKey(const WORD key, const bool down)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}

// the pointer from one point to another in steps, as a hand moves it
static void glide(const POINT from, const POINT to)
{
    for (int i = 1; i <= 12; ++i)
    {
        SetCursorPos(from.x + (to.x - from.x) * i / 12, from.y + (to.y - from.y) * i / 12);
        Sleep(25);
    }
}

static HWND findWindowOf(const DWORD pid, const std::wstring& title)
{
    for (int i = 0; i < 200; ++i)
    {
        const HWND window = FindWindowW(L"DafDropTestSource", title.c_str());
        DWORD owner = 0;
        if (window != nullptr && GetWindowThreadProcessId(window, &owner) && owner == pid)
            return window;
        Sleep(25);
    }
    return nullptr;
}

// A drag from another process onto the target, the button released over it; Shift held throughout if asked
static void dragFromAnotherProcess(DGL_NAMESPACE::Application& app, DGL_NAMESPACE::DropWindow& target,
                                   const std::wstring& path, const DWORD allowed, const bool shift)
{
    const std::string label = shift ? " (Shift held, copy and move allowed)" : " (copy allowed)";
    target.enters = target.moves = target.leaves = target.drops = 0;
    target.dropped.clear();
    target.dropMods = 0;

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --source " + std::to_wstring(allowed) + L" \"" + path + L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(L'\0');
    if (! CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
    {
        check(false, ("the source process starts" + label).c_str());
        return;
    }

    const HWND source = findWindowOf(process.dwProcessId, L"Dusk drop source " + std::to_wstring(process.dwProcessId));
    check(source != nullptr, ("the source process shows its window" + label).c_str());

    bool ended = false;
    if (source != nullptr)
    {
        app.idle();
        RECT rect = {};
        GetClientRect(source, &rect);
        POINT start = { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
        ClientToScreen(source, &start);
        POINT finish = { 60, 60 };
        ClientToScreen(reinterpret_cast<HWND>(target.getNativeWindowHandle()), &finish);

        SetCursorPos(start.x, start.y);
        Sleep(100);
        check(GetAncestor(WindowFromPoint(start), GA_ROOT) == source, ("the gesture starts over the source" + label).c_str());

        std::atomic<bool> released(false);
        std::thread driver([&] {
            sendMouse(MOUSEEVENTF_LEFTDOWN);
            Sleep(250);
            // once the press has made the source the foreground, so that its drag sees the key
            if (shift)
            {
                sendKey(VK_SHIFT, true);
                Sleep(100);
            }
            glide(start, finish);
            Sleep(250);
            sendMouse(MOUSEEVENTF_LEFTUP);
            // the source reads the key state as it handles the release, so the key stays down until then
            if (shift)
            {
                Sleep(500);
                sendKey(VK_SHIFT, false);
            }
            released = true;
        });

        // the program's loop is all that dispatches OLE's calls from the source to the target
        const DWORD deadline = GetTickCount() + 15000;
        while (GetTickCount() < deadline)
        {
            app.idle();
            if (released && WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
            {
                ended = true;
                break;
            }
            Sleep(5);
        }
        driver.join();
    }

    if (! ended)
    {
        std::printf("STALL: target saw %d enters, %d moves; source still in its drag after 15 s%s\n",
                    target.enters, target.moves, label.c_str());
        TerminateProcess(process.hProcess, 0x800);
        WaitForSingleObject(process.hProcess, 5000);
    }

    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::printf("source exit 0x%lx, target %d enters, %d moves, %d leaves, %d drops, mods 0x%x\n",
                static_cast<unsigned long>(code), target.enters, target.moves, target.leaves, target.drops, target.dropMods);

    check(ended, ("the drag from another process ends" + label).c_str());
    check(target.enters == 1 && target.drops == 1 && target.leaves == 0,
          ("the target hears one enter and one drop, and no leave" + label).c_str());
    check(target.dropped.size() == 1 && target.dropped[0] == toUtf8(path),
          ("the target gets the exact Unicode path" + label).c_str());
    check(code == (0x100U | DROPEFFECT_COPY), ("the source hears the drop was taken as a copy" + label).c_str());
    if (shift)
        check((target.dropMods & DGL_NAMESPACE::kModifierShift) != 0, "the drop carries Shift");
}

// A drag from a DGL window of this process, as FileDragWinNative.cpp drags onto a plain OLE target
static void dragWithinProcess(DGL_NAMESPACE::Application& app, DGL_NAMESPACE::DropWindow& target, const std::wstring& path)
{
    using namespace DGL_NAMESPACE;
    target.enters = target.moves = target.leaves = target.drops = 0;
    target.dropped.clear();

    Window source(app);
    source.setTitle("Dusk drop test, DGL source");
    source.setSize(220, 180);
    source.show();
    app.idle();
    const HWND sourceWindow = reinterpret_cast<HWND>(source.getNativeWindowHandle());
    SetWindowPos(sourceWindow, HWND_TOPMOST, 40, 60, 260, 240, SWP_SHOWWINDOW);
    SetForegroundWindow(sourceWindow);
    app.idle();

    POINT start = { 80, 80 };
    ClientToScreen(sourceWindow, &start);
    SetCursorPos(start.x, start.y);
    Sleep(50);
    check(GetAncestor(WindowFromPoint(start), GA_ROOT) == sourceWindow, "the gesture starts over the DGL source");
    check(! (GetAsyncKeyState(VK_LBUTTON) & 0x8000), "no button is held before the test presses one");

    sendMouse(MOUSEEVENTF_LEFTDOWN);
    Sleep(30);
    app.idle();
    SetCapture(sourceWindow);
    const bool started = source.startFileDrag(toUtf8(path).c_str());
    check(started, "the DGL source starts its drag");

    if (started)
    {
        POINT finish = { 60, 60 };
        ClientToScreen(reinterpret_cast<HWND>(target.getNativeWindowHandle()), &finish);
        std::thread driver([&] {
            Sleep(180);
            glide(start, finish);
            Sleep(180);
            sendMouse(MOUSEEVENTF_LEFTUP);
        });
        app.idle(); // runs DoDragDrop until the drop
        driver.join();
    }
    else
    {
        sendMouse(MOUSEEVENTF_LEFTUP);
    }

    std::printf("same process: target %d enters, %d moves, %d leaves, %d drops\n",
                target.enters, target.moves, target.leaves, target.drops);
    check(target.enters == 1 && target.drops == 1 && target.leaves == 0,
          "within the process, the target hears one enter and one drop, and no leave");
    check(target.dropped.size() == 1 && target.dropped[0] == toUtf8(path), "and gets the exact Unicode path");
    source.close();
    app.idle();
}

int main()
{
    int argc = 0;
    wchar_t** const argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc == 4 && std::wstring(argv[1]) == L"--source")
        return runSource(argv[3], static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10)));

    using namespace DGL_NAMESPACE;

    // --target <seconds>: only the target, for a drag a person or a script starts elsewhere (Explorer)
    if (argc == 3 && std::wstring(argv[1]) == L"--target")
    {
        Application app(true);
        DropWindow target(app);
        target.setTitle("Dusk drop test, DGL target");
        target.setSize(240, 240);
        target.show();
        check(target.setAcceptingFileDrops(true), "the DGL window accepts file drops");
        SetWindowPos(reinterpret_cast<HWND>(target.getNativeWindowHandle()), HWND_TOPMOST, 700, 60, 260, 260, SWP_SHOWWINDOW);
        const DWORD deadline = GetTickCount() + 1000U * static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        while (target.drops == 0 && GetTickCount() < deadline)
        {
            app.idle();
            Sleep(5);
        }
        std::printf("target: %d enters, %d moves, %d leaves, %d drops, mods 0x%x\n",
                    target.enters, target.moves, target.leaves, target.drops, target.dropMods);
        for (size_t i = 0; i < target.dropped.size(); ++i)
            std::printf("dropped: %s\n", target.dropped[i].c_str());
        check(target.enters == 1 && target.drops == 1 && target.leaves == 0, "one enter and one drop, and no leave");
        target.close();
        LocalFree(argv);
        return failures != 0 ? 1 : 0;
    }

    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path = std::wstring(temp) + L"Dusk drop " + std::to_wstring(GetCurrentProcessId())
                            + L" C# \u00e9 \U0001f3b5.wav";
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    check(file != INVALID_HANDLE_VALUE, "the Unicode fixture file is created");
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);

    POINT oldCursor = {};
    GetCursorPos(&oldCursor);
    const HWND oldForeground = GetForegroundWindow();

    {
        // a standalone application: a program world, which pumps its whole thread
        Application app(true);
        DropWindow target(app);
        target.setTitle("Dusk drop test, DGL target");
        target.setSize(240, 240);
        target.show();
        app.idle();
        check(target.setAcceptingFileDrops(true), "the DGL window accepts file drops");
        SetWindowPos(reinterpret_cast<HWND>(target.getNativeWindowHandle()), HWND_TOPMOST, 440, 60, 260, 260, SWP_SHOWWINDOW);
        app.idle();

        dragWithinProcess(app, target, path);
        dragFromAnotherProcess(app, target, path, DROPEFFECT_COPY, false);
        dragFromAnotherProcess(app, target, path, DROPEFFECT_COPY | DROPEFFECT_MOVE, true);

        target.close();
    }

    DeleteFileW(path.c_str());
    SetCursorPos(oldCursor.x, oldCursor.y);
    if (IsWindow(oldForeground))
        SetForegroundWindow(oldForeground);
    LocalFree(argv);

    std::printf("Native OLE drops onto a DGL window: %d checks, %d failures\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
