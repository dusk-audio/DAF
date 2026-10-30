// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC

// The Windows drop target (dgl/src/FileDropWin.hpp, see dgl/src/drop-targets.md): its IDropTarget methods
// called the way OLE calls them, with a fake IDataObject holding CF_HDROP, over a real pugl window. And the
// program world's message loop, which a drag from another process needs. Needs a desktop, not a person.

#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "dgl/src/pugl.cpp"

#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace DGL_NAMESPACE;

static int checks = 0, failures = 0;

static void check(const bool ok, const char* const what)
{
    ++checks;
    if (! ok)
    {
        ++failures;
        std::fprintf(stderr, "FileDropWin: FAILED %s\n", what);
        std::fflush(stderr);
    }
}

// --------------------------------------------------------------------------------------------------------------------
// a source's data object

struct FakeData : IDataObject
{
    std::atomic<ULONG> references{1};
    std::vector<std::wstring> paths;
    bool hdrop = true;
    const wchar_t* descriptor = nullptr; // a virtual-file format offered as well
    int reads = 0;
    int requests = 0; // GetData calls for CF_HDROP, answered or not

    explicit FakeData(std::vector<std::wstring> files) : paths(std::move(files)) {}

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
        if (format->cfFormat == CF_HDROP && hdrop) return S_OK;
        if (descriptor != nullptr && format->cfFormat == RegisterClipboardFormatW(descriptor)) return S_OK;
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* const format, STGMEDIUM* const medium) override
    {
        if (format == nullptr || medium == nullptr) return E_POINTER;
        std::memset(medium, 0, sizeof(*medium));
        if (format->cfFormat == CF_HDROP) ++requests;
        if (format->cfFormat != CF_HDROP || ! hdrop) return DV_E_FORMATETC;
        if ((format->tymed & TYMED_HGLOBAL) == 0) return DV_E_TYMED;
        ++reads;

        size_t chars = 1;
        for (size_t i = 0; i < paths.size(); ++i)
            chars += paths[i].size() + 1;

        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
        if (memory == nullptr) return E_OUTOFMEMORY;
        DROPFILES* const files = static_cast<DROPFILES*>(GlobalLock(memory));
        files->pFiles = sizeof(DROPFILES);
        files->fWide = TRUE;
        wchar_t* text = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(files) + sizeof(DROPFILES));
        for (size_t i = 0; i < paths.size(); ++i)
        {
            std::memcpy(text, paths[i].c_str(), (paths[i].size() + 1) * sizeof(wchar_t));
            text += paths[i].size() + 1;
        }
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

// A target that does nothing, for asking OLE whether a window is registered
struct NullTarget : IDropTarget
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** const out) override
    {
        if (out == nullptr) return E_POINTER;
        *out = nullptr;
        if (! IsEqualIID(iid, IID_IUnknown) && ! IsEqualIID(iid, IID_IDropTarget)) return E_NOINTERFACE;
        *out = static_cast<IDropTarget*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD*) override { return S_OK; }
};

// whether OLE has a drop target on the window, without leaving anything behind
static bool registered(const HWND hwnd)
{
    static NullTarget probe;
    const HRESULT ole = OleInitialize(nullptr);
    const HRESULT result = RegisterDragDrop(hwnd, &probe);
    if (result == S_OK)
        RevokeDragDrop(hwnd);
    if (SUCCEEDED(ole))
        OleUninitialize();
    return result == DRAGDROP_E_ALREADYREGISTERED;
}

// whether COM is initialised on this thread, without leaving anything behind
static bool oleUp()
{
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(result))
        CoUninitialize();
    return result == S_FALSE;
}

// --------------------------------------------------------------------------------------------------------------------
// the view's side

struct Recorder
{
    std::string log;
    PuglStatus reply = PUGL_SUCCESS;
    std::function<void()> during;
    PuglDropEvent last = {};
    std::vector<std::string> files, filesAfter;

    std::string take()
    {
        const std::string result(log);
        log.clear();
        return result;
    }
};

static Recorder rec;

static PuglStatus onEvent(PuglView*, const PuglEvent* const event)
{
    switch (event->type)
    {
    case PUGL_DRAG_ENTER:  rec.log += "ENTER;";  break;
    case PUGL_DRAG_MOTION: rec.log += "MOTION;"; break;
    case PUGL_DRAG_LEAVE:  rec.log += "LEAVE;";  break;
    case PUGL_DROP:        rec.log += "DROP;";   break;
    default:
        return PUGL_SUCCESS;
    }

    rec.last = event->drop;
    rec.files.clear();
    for (uint32_t i = 0; i < event->drop.numFiles; ++i)
        rec.files.push_back(event->drop.files[i]);

    if (rec.during)
    {
        const std::function<void()> step(rec.during);
        rec.during = nullptr;
        step();
    }

    // the files stay readable until the callback returns, whatever it did
    rec.filesAfter.clear();
    for (uint32_t i = 0; i < event->drop.numFiles; ++i)
        rec.filesAfter.push_back(event->drop.files[i]);

    return rec.reply;
}

static PuglView* newView(PuglWorld* const world, const PuglNativeView parent = 0, const bool realize = true)
{
    PuglView* const view = puglNewView(world);
    puglSetBackend(view, puglStubBackend());
    puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 200, 150);
    puglSetEventFunc(view, onEvent);
    if (parent != 0)
        puglSetParent(view, parent);
    if (realize && puglRealize(view) != PUGL_SUCCESS)
        check(false, "a view realizes");
    return view;
}

static POINTL screenPoint(const HWND hwnd, const int x, const int y)
{
    POINT point = { x, y };
    ClientToScreen(hwnd, &point);
    const POINTL result = { point.x, point.y };
    return result;
}

static const DWORD kAll = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;

static DWORD enter(IDropTarget* const target, IDataObject* const data, const POINTL point,
                   const DWORD keys = MK_LBUTTON, const DWORD allowed = kAll)
{
    DWORD effect = allowed;
    target->DragEnter(data, keys, point, &effect);
    return effect;
}

static DWORD over(IDropTarget* const target, const POINTL point, const DWORD keys = MK_LBUTTON, const DWORD allowed = kAll)
{
    DWORD effect = allowed;
    target->DragOver(keys, point, &effect);
    return effect;
}

static DWORD drop(IDropTarget* const target, IDataObject* const data, const POINTL point,
                  const DWORD keys = 0, const DWORD allowed = kAll)
{
    DWORD effect = allowed;
    target->Drop(data, keys, point, &effect);
    return effect;
}

// UTF-8 of the paths below, spelt out so that no source encoding gets in the way
static const char kUnicodeUtf8[] = "C:\\Users\\marc\\C# \xc3\xa9 \xf0\x9f\x8e\xb5.mid";
static const wchar_t kUnicodeWide[] = L"C:\\Users\\marc\\C# \u00e9 \U0001f3b5.mid";

static std::vector<std::wstring> threeFiles()
{
    std::vector<std::wstring> files;
    files.push_back(L"C:\\Audio\\kick.wav");
    files.push_back(L"\\\\studio\\share\\b.mid");
    files.push_back(kUnicodeWide);
    return files;
}

// --------------------------------------------------------------------------------------------------------------------

static void testOptIn(PuglWorld* const world)
{
    check(! oleUp(), "the test starts with OLE down on its thread");

    PuglView* const view = newView(world);
    const HWND hwnd = view->impl->hwnd;
    check(! registered(hwnd), "a view that has not opted in has no drop target");

    PuglView* const child = newView(world, reinterpret_cast<PuglNativeView>(hwnd));
    check(puglSetAcceptsFileDrops(child, true) == PUGL_UNSUPPORTED && child->impl->drop == nullptr
          && ! registered(child->impl->hwnd), "an embedded view is refused");
    puglFreeView(child);

    PuglView* const unrealized = newView(world, 0, false);
    check(puglSetAcceptsFileDrops(unrealized, true) == PUGL_FAILURE, "a view without a window is refused");
    puglFreeView(unrealized);

    check(puglSetAcceptsFileDrops(view, true) == PUGL_SUCCESS, "a top-level view opts in");
    check(registered(hwnd), "and has its window registered with OLE");
    check(oleUp(), "which holds OLE on the thread");
    check(puglSetAcceptsFileDrops(view, true) == PUGL_SUCCESS && registered(hwnd), "opting in twice is the same");

    check(puglSetAcceptsFileDrops(view, false) == PUGL_SUCCESS, "opting out succeeds");
    check(! registered(hwnd), "and revokes the window");
    check(! oleUp(), "with one OleUninitialize for the one registration");
    check(puglSetAcceptsFileDrops(view, false) == PUGL_SUCCESS && ! oleUp(), "opting out twice is the same");

    // a view unrealized while OLE still holds the target, as DoDragDrop does during a drag
    check(puglSetAcceptsFileDrops(view, true) == PUGL_SUCCESS, "a view opts in again");
    PuglWinDropTarget* const target = view->impl->drop;
    target->AddRef();
    FakeData* const data = new FakeData(threeFiles());
    const POINTL point = screenPoint(hwnd, 10, 20);
    check(enter(target, data, point) == DROPEFFECT_COPY && rec.take() == "ENTER;", "a drag enters");

    puglUnrealize(view);
    check(view->impl->drop == nullptr && ! oleUp(), "unrealizing revokes the target and lets OLE go");
    check(rec.take().empty(), "the view hears nothing of its own window going");
    check(over(target, point) == DROPEFFECT_NONE && drop(target, data, point) == DROPEFFECT_NONE
          && enter(target, data, point) == DROPEFFECT_NONE, "the unrealized view's target answers none");
    target->DragLeave();
    check(rec.take().empty(), "and calls no view");
    target->Release();
    data->Release();
    puglFreeView(view);
}

static void testDrag(PuglWorld* const world)
{
    PuglView* const view = newView(world);
    const HWND hwnd = view->impl->hwnd;
    puglSetAcceptsFileDrops(view, true);
    IDropTarget* const target = view->impl->drop;

    // accepted, moved, dropped
    {
        FakeData* const data = new FakeData(threeFiles());
        rec.reply = PUGL_SUCCESS;
        check(enter(target, data, screenPoint(hwnd, 10, 20), MK_LBUTTON | MK_SHIFT) == DROPEFFECT_COPY,
              "an accepted drag is a copy, Shift held");
        check(rec.take() == "ENTER;", "the view hears the enter");
        check(rec.last.x == 10 && rec.last.y == 20, "at client pixels");
        check((rec.last.mods & PUGL_MOD_SHIFT) != 0 && (rec.last.mods & (PUGL_MOD_CTRL | PUGL_MOD_ALT)) == 0,
              "with the drag's modifiers");
        check(rec.files.size() == 3 && rec.files[0] == "C:\\Audio\\kick.wav" && rec.files[1] == "\\\\studio\\share\\b.mid"
              && rec.files[2] == kUnicodeUtf8, "and the files, in order and in UTF-8");

        check(over(target, screenPoint(hwnd, 30, 40), MK_LBUTTON | MK_CONTROL | MK_ALT) == DROPEFFECT_COPY
              && rec.take() == "MOTION;" && rec.last.x == 30 && rec.last.y == 40, "a move is a motion where it is");
        check((rec.last.mods & (PUGL_MOD_CTRL | PUGL_MOD_ALT)) == (PUGL_MOD_CTRL | PUGL_MOD_ALT)
              && (rec.last.mods & PUGL_MOD_SHIFT) == 0, "with the modifiers of that move");

        // the source always sees a copy: Shift asks Explorer for a move, Ctrl+Shift for a link
        const DWORD keyStates[] = { 0, MK_SHIFT, MK_CONTROL, MK_SHIFT | MK_CONTROL, MK_ALT, MK_LBUTTON | MK_RBUTTON };
        bool copies = true;
        for (size_t i = 0; i < sizeof(keyStates) / sizeof(keyStates[0]); ++i)
            copies = copies && over(target, screenPoint(hwnd, 30, 40), keyStates[i]) == DROPEFFECT_COPY;
        check(copies && rec.take().size() == 6 * std::strlen("MOTION;"), "an accepted position is a copy whatever the keys");
        check(over(target, screenPoint(hwnd, 30, 40), MK_SHIFT, DROPEFFECT_COPY) == DROPEFFECT_COPY
              && rec.take() == "MOTION;", "and with copy all the source allows");

        rec.reply = PUGL_FAILURE;
        check(over(target, screenPoint(hwnd, 31, 41)) == DROPEFFECT_NONE && rec.take() == "MOTION;",
              "a refused position is none");
        rec.reply = PUGL_SUCCESS;
        check(over(target, screenPoint(hwnd, 32, 42)) == DROPEFFECT_COPY && rec.take() == "MOTION;", "and accepted again");

        check(drop(target, data, screenPoint(hwnd, 32, 42), MK_SHIFT) == DROPEFFECT_COPY, "a taken drop is a copy");
        check(rec.take() == "DROP;" && rec.files.size() == 3 && rec.files[2] == kUnicodeUtf8 && rec.last.x == 32,
              "the view gets the drop and the files");
        target->DragLeave();
        check(rec.take().empty(), "no leave after a drop");
        check(data->reads == 1, "the files are read once, at the enter");
        data->Release();

        // the view's reference and OLE's registration, and the one taken here
        const ULONG references = target->AddRef();
        target->Release();
        check(references == 3, "the calls leave no reference to the target behind");
    }

    // refused at the enter, then left
    {
        FakeData* const data = new FakeData(threeFiles());
        rec.reply = PUGL_FAILURE;
        check(enter(target, data, screenPoint(hwnd, 5, 5)) == DROPEFFECT_NONE && rec.take() == "ENTER;",
              "a refused enter is none");
        target->DragLeave();
        check(rec.take() == "LEAVE;", "and its leave reaches the view that had the enter");
        rec.reply = PUGL_SUCCESS;
        data->Release();
    }

    // a drop the view does not take
    {
        FakeData* const data = new FakeData(threeFiles());
        enter(target, data, screenPoint(hwnd, 5, 5));
        rec.take();
        rec.reply = PUGL_FAILURE;
        check(drop(target, data, screenPoint(hwnd, 5, 5)) == DROPEFFECT_NONE && rec.take() == "DROP;",
              "a drop the view did not take is none");
        rec.reply = PUGL_SUCCESS;
        data->Release();
    }

    // sources that allow no copy
    {
        const DWORD noCopy[] = { DROPEFFECT_MOVE, DROPEFFECT_LINK, DROPEFFECT_MOVE | DROPEFFECT_LINK };
        for (size_t i = 0; i < sizeof(noCopy) / sizeof(noCopy[0]); ++i)
        {
            FakeData* const data = new FakeData(threeFiles());
            check(enter(target, data, screenPoint(hwnd, 5, 5), MK_LBUTTON, noCopy[i]) == DROPEFFECT_NONE
                  && over(target, screenPoint(hwnd, 6, 6), MK_LBUTTON, noCopy[i]) == DROPEFFECT_NONE,
                  "a source that allows no copy is refused");
            target->DragLeave();
            check(rec.take().empty(), "and the view never hears of it");
            data->Release();
        }
    }

    // file group descriptors, with files on disk and without
    {
        const wchar_t* const descriptors[] = { L"FileGroupDescriptorW", L"FileGroupDescriptor" };
        for (size_t i = 0; i < 2; ++i)
        {
            FakeData* const data = new FakeData(threeFiles());
            data->descriptor = descriptors[i];
            check(enter(target, data, screenPoint(hwnd, 5, 5)) == DROPEFFECT_COPY && rec.take() == "ENTER;"
                  && rec.files.size() == 3 && rec.files[0] == "C:\\Audio\\kick.wav",
                  "a descriptor next to CF_HDROP does not hide the files on disk");
            check(drop(target, data, screenPoint(hwnd, 5, 5)) == DROPEFFECT_COPY && rec.take() == "DROP;",
                  "and they can be dropped");
            data->Release();

            FakeData* const virtualOnly = new FakeData(threeFiles());
            virtualOnly->descriptor = descriptors[i];
            virtualOnly->hdrop = false;
            check(enter(target, virtualOnly, screenPoint(hwnd, 5, 5)) == DROPEFFECT_NONE
                  && over(target, screenPoint(hwnd, 6, 6)) == DROPEFFECT_NONE
                  && drop(target, virtualOnly, screenPoint(hwnd, 6, 6)) == DROPEFFECT_NONE,
                  "virtual files without CF_HDROP are refused");
            check(rec.take().empty(), "and the view never hears of them");
            check(virtualOnly->requests == 0, "nor is the source asked to render CF_HDROP it does not offer");
            virtualOnly->Release();
        }

        FakeData* const data = new FakeData(threeFiles());
        data->hdrop = false;
        check(enter(target, data, screenPoint(hwnd, 5, 5)) == DROPEFFECT_NONE, "data without CF_HDROP is refused");
        target->DragLeave();
        check(rec.take().empty(), "without the view hearing of it");
        check(data->requests == 0, "or the source being asked for it");
        data->Release();
    }

    // a path longer than MAX_PATH
    {
        std::wstring longWide(L"C:\\");
        std::string longUtf8("C:\\");
        for (int i = 0; i < 40; ++i)
        {
            longWide += L"directory\\";
            longUtf8 += "directory\\";
        }
        longWide += L"take.wav";
        longUtf8 += "take.wav";
        check(longWide.size() > MAX_PATH, "the long path passes MAX_PATH");

        FakeData* const data = new FakeData(std::vector<std::wstring>(1, longWide));
        check(enter(target, data, screenPoint(hwnd, 5, 5)) == DROPEFFECT_COPY && rec.take() == "ENTER;"
              && rec.files.size() == 1 && rec.files[0] == longUtf8, "a path past MAX_PATH arrives whole");
        target->DragLeave();
        rec.take();
        data->Release();
    }

    // paths without a UTF-8 form, and relative ones
    {
        std::vector<std::wstring> files;
        files.push_back(std::wstring(L"C:\\high ") + wchar_t(0xd800) + L" alone.wav");
        files.push_back(L"C:\\good.wav");
        files.push_back(std::wstring(L"C:\\low ") + wchar_t(0xdc00) + L" alone.wav");
        files.push_back(std::wstring(L"C:\\reversed ") + wchar_t(0xdc00) + wchar_t(0xd800) + L".wav");
        files.push_back(L"relative.wav");
        files.push_back(L"C:drive-relative.wav");
        FakeData* const data = new FakeData(files);
        check(enter(target, data, screenPoint(hwnd, 5, 5)) == DROPEFFECT_COPY && rec.take() == "ENTER;"
              && rec.files.size() == 1 && rec.files[0] == "C:\\good.wav",
              "a path with an unpaired surrogate, and a relative one, are skipped");
        target->DragLeave();
        rec.take();
        data->Release();

        files.erase(files.begin() + 1);
        FakeData* const none = new FakeData(files);
        check(enter(target, none, screenPoint(hwnd, 5, 5)) == DROPEFFECT_NONE, "a drag with no usable path is refused");
        target->DragLeave();
        check(rec.take().empty(), "and the view never hears of it");
        none->Release();
    }

    puglFreeView(view);
    check(! oleUp(), "freeing the view lets OLE go");
}

// The window deleted, or opting out, inside each of its callbacks
static void testInsideCallbacks(PuglWorld* const world)
{
    const char* const steps[] = { "enter", "motion", "leave", "drop" };

    for (int optOut = 0; optOut < 2; ++optOut)
    {
        for (int step = 0; step < 4; ++step)
        {
            PuglView* view = newView(world);
            const HWND hwnd = view->impl->hwnd;
            puglSetAcceptsFileDrops(view, true);
            PuglWinDropTarget* const target = view->impl->drop;
            target->AddRef(); // as OLE holds it during a drag
            FakeData* const data = new FakeData(threeFiles());
            const POINTL point = screenPoint(hwnd, 10, 20);
            rec.take();
            rec.reply = PUGL_SUCCESS;

            bool upInside = false;
            const std::function<void()> goAway = [&] {
                if (optOut)
                {
                    puglSetAcceptsFileDrops(view, false);
                }
                else
                {
                    puglFreeView(view);
                    view = nullptr;
                }
                upInside = oleUp();
            };

            std::string what(optOut ? "opting out inside " : "a window deleted inside ");
            what += steps[step];

            DWORD effect = DROPEFFECT_NONE;
            if (step == 0)
            {
                rec.during = goAway;
                effect = enter(target, data, point);
                check(rec.take() == "ENTER;" && effect == DROPEFFECT_NONE, (what + ": the drag is refused").c_str());
            }
            else
            {
                check(enter(target, data, point) == DROPEFFECT_COPY && rec.take() == "ENTER;", "the drag enters");
                rec.during = goAway;

                if (step == 1)
                {
                    effect = over(target, point);
                    check(rec.take() == "MOTION;" && effect == DROPEFFECT_NONE, (what + ": the drag is refused").c_str());
                }
                else if (step == 2)
                {
                    target->DragLeave();
                    check(rec.take() == "LEAVE;", (what + ": the leave went out").c_str());
                }
                else
                {
                    effect = drop(target, data, point);
                    check(rec.take() == "DROP;" && effect == DROPEFFECT_COPY,
                          (what + ": the drop still answers what the view said").c_str());
                }
            }

            check(rec.filesAfter == rec.files, (what + ": the files stay readable until the callback returns").c_str());
            check(upInside, (what + ": OLE stays up until the call returns").c_str());

            // what OLE may still send the target
            check(over(target, point) == DROPEFFECT_NONE && drop(target, data, point) == DROPEFFECT_NONE,
                  (what + ": later calls answer none").c_str());
            target->DragLeave();
            check(rec.take().empty(), (what + ": and reach no view").c_str());

            puglUpdate(world, 0.0);
            check(! oleUp(), (what + ": OLE goes at the next update").c_str());

            if (view != nullptr)
            {
                check(! registered(hwnd), (what + ": the window is revoked").c_str());

                // the window can opt in again and take the next drag
                puglSetAcceptsFileDrops(view, true);
                FakeData* const next = new FakeData(threeFiles());
                check(enter(view->impl->drop, next, point) == DROPEFFECT_COPY && rec.take() == "ENTER;",
                      (what + ": the next drag reaches the window once it opts in again").c_str());
                view->impl->drop->DragLeave();
                rec.take();
                next->Release();
                puglFreeView(view);
            }

            target->Release();
            data->Release();
        }
    }
}

// A target whose view held the last reference to it, OLE's aside, lasts until the call that lost it returns
static void testLastReference(PuglWorld* const world)
{
    PuglView* view = newView(world);
    const POINTL point = screenPoint(view->impl->hwnd, 10, 20);
    puglSetAcceptsFileDrops(view, true);
    IDropTarget* const target = view->impl->drop;
    FakeData* const data = new FakeData(threeFiles());
    rec.take();
    rec.during = [&] {
        puglFreeView(view);
        view = nullptr;
    };
    check(enter(target, data, point) == DROPEFFECT_NONE && rec.take() == "ENTER;",
          "a window deleted inside its enter, with the last reference to its target, answers none");
    puglUpdate(world, 0.0);
    data->Release();

    // opting out from outside any callback, the window deleted inside its leave: once revoked, OLE holds the
    // target no more, and the view's reference was the last
    view = newView(world);
    puglSetAcceptsFileDrops(view, true);
    FakeData* const next = new FakeData(threeFiles());
    check(enter(view->impl->drop, next, screenPoint(view->impl->hwnd, 10, 20)) == DROPEFFECT_COPY
          && rec.take() == "ENTER;", "a drag enters");
    rec.during = [&] {
        puglFreeView(view);
        view = nullptr;
    };
    puglSetAcceptsFileDrops(view, false);
    check(rec.take() == "LEAVE;" && view == nullptr, "opting out gives the leave, inside which the window goes");
    check(! oleUp(), "and lets OLE go");
    next->Release();
}

// A drop that arrives from a nested event loop inside a callback is refused to its source at once, so the
// application never gets it: for the view the drag left
static void testNestedDrop(PuglWorld* const world)
{
    PuglView* const view = newView(world);
    const HWND hwnd = view->impl->hwnd;
    const POINTL point = screenPoint(hwnd, 10, 20);
    puglSetAcceptsFileDrops(view, true);
    PuglWinDropTarget* const target = view->impl->drop;
    target->AddRef();
    FakeData* const first = new FakeData(threeFiles());
    FakeData* const second = new FakeData(std::vector<std::wstring>(1, L"C:\\second.wav"));
    rec.take();

    // onFileDrop runs a modal loop, a message box say, in which a drag from another process drops
    check(enter(target, first, point) == DROPEFFECT_COPY && rec.take() == "ENTER;", "a drag enters");
    DWORD nestedDrop = 1;
    rec.during = [&] {
        enter(target, second, screenPoint(hwnd, 30, 40));
        over(target, screenPoint(hwnd, 31, 41));
        nestedDrop = drop(target, second, screenPoint(hwnd, 31, 41));
    };
    check(drop(target, first, point) == DROPEFFECT_COPY, "the first drop is taken");
    check(nestedDrop == DROPEFFECT_NONE, "the drop that arrived meanwhile is refused to its source");
    check(rec.take() == "DROP;ENTER;MOTION;LEAVE;", "and its drag reaches the view afterwards only as one that left");

    // the same, inside the drag's own enter
    nestedDrop = 1;
    rec.during = [&] { nestedDrop = drop(target, first, point); };
    check(enter(target, first, point) == DROPEFFECT_COPY && nestedDrop == DROPEFFECT_NONE,
          "a drop that arrives inside the enter is refused");
    check(rec.take() == "ENTER;LEAVE;", "and the view that had the enter gets a leave, not the drop");
    target->DragLeave();
    check(rec.take().empty(), "and nothing after it");

    puglFreeView(view);
    target->Release();
    first->Release();
    second->Release();
}

// Opting out from outside any callback, during a drag
static void testOptOutDuringDrag(PuglWorld* const world)
{
    PuglView* const view = newView(world);
    const HWND hwnd = view->impl->hwnd;
    const POINTL point = screenPoint(hwnd, 10, 20);

    // plainly
    {
        puglSetAcceptsFileDrops(view, true);
        IDropTarget* const target = view->impl->drop;
        target->AddRef();
        FakeData* const data = new FakeData(threeFiles());
        check(enter(target, data, point) == DROPEFFECT_COPY && rec.take() == "ENTER;", "a drag enters");
        puglSetAcceptsFileDrops(view, false);
        check(rec.take() == "LEAVE;", "opting out mid-drag gives the view its leave");
        check(over(target, point) == DROPEFFECT_NONE && drop(target, data, point) == DROPEFFECT_NONE
              && rec.take().empty(), "and refuses the rest of the drag without the view");
        check(! registered(hwnd) && ! oleUp(), "the window is revoked and OLE let go");
        target->Release();
        data->Release();
    }

    // with a leave that opts in again while OLE calls the target from a nested event loop
    {
        puglSetAcceptsFileDrops(view, true);
        PuglWinDropTarget* const target = view->impl->drop;
        target->AddRef();
        FakeData* const first = new FakeData(threeFiles());
        FakeData* const second = new FakeData(std::vector<std::wstring>(1, L"C:\\second.wav"));
        check(enter(target, first, point) == DROPEFFECT_COPY && rec.take() == "ENTER;", "a drag enters");

        std::string nested;
        DWORD nestedOver = 1, nestedEnter = 1, nestedMove = 1;
        rec.during = [&] {
            check(puglSetAcceptsFileDrops(view, true) == PUGL_SUCCESS, "the leave opts in again");
            nestedOver = over(target, point);
            target->DragLeave();
            nestedEnter = enter(target, second, screenPoint(hwnd, 50, 60));
            nestedMove = over(target, screenPoint(hwnd, 51, 61));
            nested = rec.log;
        };
        puglSetAcceptsFileDrops(view, false);

        check(nested == "LEAVE;", "nothing reaches the view inside its leave");
        check(nestedOver == DROPEFFECT_NONE && nestedEnter == DROPEFFECT_NONE && nestedMove == DROPEFFECT_NONE,
              "the calls that arrive meanwhile are answered none");
        check(rec.take() == "LEAVE;ENTER;MOTION;", "the second drag reaches the view once the opt-out is done");
        check(rec.files.size() == 1 && rec.files[0] == "C:\\second.wav" && rec.last.x == 51 && rec.last.y == 61,
              "with its own files and position");
        check(over(target, screenPoint(hwnd, 52, 62)) == DROPEFFECT_COPY && rec.take() == "MOTION;",
              "and goes on from there");
        check(registered(hwnd) && oleUp(), "the window stays registered for it");

        puglSetAcceptsFileDrops(view, false);
        check(rec.take() == "LEAVE;" && ! oleUp(), "turning drops off again ends it, and lets OLE go");
        target->Release();
        first->Release();
        second->Release();
    }

    puglFreeView(view);
}

// A thread in COM's multi-threaded apartment cannot host an OLE drop target
static void testMultithreadedApartment()
{
    bool refused = false, quiet = false, kept = false;
    std::thread thread([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        PuglWorld* const world = puglNewWorld(PUGL_PROGRAM, 0);
        puglSetWorldString(world, PUGL_CLASS_NAME, "DafFileDropWinMTA");
        PuglView* const view = newView(world);
        refused = puglSetAcceptsFileDrops(view, true) == PUGL_FAILURE;
        quiet = view->impl->drop == nullptr || ! view->impl->drop->accepting;
        puglFreeView(view);
        puglFreeWorld(world);

        // still in the apartment the thread chose: nothing uninitialised it
        const HRESULT again = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        kept = again == S_FALSE;
        if (SUCCEEDED(again))
            CoUninitialize();
        CoUninitialize();
    });
    thread.join();
    check(refused, "opting in on a thread in the multi-threaded apartment fails");
    check(quiet, "and registers nothing");
    check(kept, "and leaves the thread's apartment as it was");
}

// --------------------------------------------------------------------------------------------------------------------
// the message loop

static int probeMessages = 0;
static bool probeEatsMark = false, probeMarkEaten = false, probeReposts = false;

static LRESULT CALLBACK probeProcedure(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam)
{
    if (message == WM_USER + 7)
    {
        ++probeMessages;
        return 0;
    }
    if (message == WM_USER + 8)
    {
        // as a modal loop does, taking every message of the thread, the pump's mark among them
        MSG msg;
        if (probeEatsMark && PeekMessageW(&msg, reinterpret_cast<HWND>(-1), PUGL_LOCAL_MARK_MSG, PUGL_LOCAL_MARK_MSG, PM_REMOVE))
            probeMarkEaten = true;
        probeEatsMark = false;

        // and a window that always has more to do
        if (probeReposts)
            PostMessageW(hwnd, WM_USER + 8, 0, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static void testMessageLoop()
{
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = probeProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"DafFileDropWinProbe";
    RegisterClassW(&windowClass);
    // the window stands for OLE's hidden one: on the thread, and no view of any world
    const HWND probe = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr,
                                       windowClass.hInstance, nullptr);
    check(probe != nullptr, "a window of the thread's own");

    PuglWorld* const module = puglNewWorld(PUGL_MODULE, 0);
    puglSetWorldString(module, PUGL_CLASS_NAME, "DafFileDropWinModule");
    PuglView* const moduleView = newView(module);
    probeMessages = 0;
    PostMessageW(probe, WM_USER + 7, 0, 0);
    puglUpdate(module, 0.0);
    check(probeMessages == 0, "a module world leaves the rest of its thread's messages to its host");

    PuglWorld* const program = puglNewWorld(PUGL_PROGRAM, 0);
    puglSetWorldString(program, PUGL_CLASS_NAME, "DafFileDropWinProgram");
    PuglView* const programView = newView(program);
    puglUpdate(program, 0.0);
    check(probeMessages == 1, "a program world dispatches the rest of its thread's messages");

    PostMessageW(probe, WM_USER + 7, 0, 0);
    PostMessageW(probe, WM_USER + 7, 0, 0);
    puglUpdate(program, 0.01);
    check(probeMessages == 3, "also while it waits");

    PostQuitMessage(3);
    puglUpdate(program, 0.0);
    MSG msg;
    check(PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && msg.wParam == 3,
          "and leaves a quit message for the loop it is meant for");

    // a modal loop inside a dispatched message takes the pump's mark, and a window keeps posting to itself
    {
        std::atomic<bool> returned(false);
        std::thread watchdog([&] {
            for (int i = 0; i < 500 && ! returned; ++i)
                Sleep(10);
            if (! returned)
            {
                std::fprintf(stderr, "FileDropWin: FAILED a program world's pass ends without its mark (still running after 5 s)\n");
                std::fflush(stderr);
                ExitProcess(1);
            }
        });
        probeEatsMark = probeReposts = true;
        probeMarkEaten = false;
        PostMessageW(probe, WM_USER + 8, 0, 0);
        puglUpdate(program, 0.0);
        returned = true;
        watchdog.join();
        check(probeMarkEaten, "a program world's pass ends without its mark, the queue never empty");

        probeReposts = false;
        while (PeekMessageW(&msg, probe, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
    }

    puglFreeView(programView);
    puglFreeWorld(program);
    puglFreeView(moduleView);
    puglFreeWorld(module);
    DestroyWindow(probe);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
}

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    PuglWorld* const world = puglNewWorld(PUGL_PROGRAM, 0);
    puglSetWorldString(world, PUGL_CLASS_NAME, "DafFileDropWin");

    testOptIn(world);
    testDrag(world);
    testInsideCallbacks(world);
    testLastReference(world);
    testNestedDrop(world);
    testOptOutDuringDrag(world);
    testMultithreadedApartment();

    puglFreeWorld(world);
    check(! oleUp(), "nothing is left initialised");

    testMessageLoop();

    std::printf("FileDropWin: %d checks, %d failures\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
