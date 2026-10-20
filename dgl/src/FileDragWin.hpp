// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Included after the Win32 backend, inside the DGL namespace.
class FileDragFormats : public IEnumFORMATETC
{
    std::atomic<ULONG> references{1};
    ULONG index;
public:
    explicit FileDragFormats(ULONG at = 0) : index(at) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IEnumFORMATETC))
        { *out = static_cast<IEnumFORMATETC*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* out, ULONG* fetched) override
    {
        if (!out || (!fetched && count != 1)) return E_POINTER;
        ULONG written = 0;
        if (count && index == 0) { out[0] = FORMATETC{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; index = written = 1; }
        if (fetched) *fetched = written;
        return written == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override
    { const ULONG remaining = 1 - index; index += (std::min)(count, remaining); return count <= remaining ? S_OK : S_FALSE; }
    HRESULT STDMETHODCALLTYPE Reset() override { index = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** out) override
    { if (!out) return E_POINTER; *out = new (std::nothrow) FileDragFormats(index); return *out ? S_OK : E_OUTOFMEMORY; }
};

class FileDragData : public IDataObject
{
    std::atomic<ULONG> references{1};
    const std::wstring path;
public:
    explicit FileDragData(std::wstring filename) : path(std::move(filename)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDataObject))
        { *out = static_cast<IDataObject*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (!format) return E_POINTER;
        if (format->cfFormat != CF_HDROP) return DV_E_FORMATETC;
        if (!(format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        return format->lindex == -1 ? S_OK : DV_E_LINDEX;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        if (!medium) return E_POINTER;
        std::memset(medium, 0, sizeof(*medium));
        const HRESULT result = QueryGetData(format); if (FAILED(result)) return result;
        const SIZE_T size = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size); if (!memory) return E_OUTOFMEMORY;
        auto* files = static_cast<DROPFILES*>(GlobalLock(memory));
        if (!files) { GlobalFree(memory); return E_OUTOFMEMORY; }
        files->pFiles = sizeof(DROPFILES); files->fWide = TRUE;
        std::memcpy(reinterpret_cast<char*>(files) + sizeof(DROPFILES), path.c_str(), (path.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL; medium->hGlobal = memory; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override
    { if (!in || !out) return E_POINTER; out->ptd = nullptr; return DATA_S_SAMEFORMATETC; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
    { if (!out) return E_POINTER; *out = nullptr; if (direction != DATADIR_GET) return E_NOTIMPL; *out = new (std::nothrow) FileDragFormats; return *out ? S_OK : E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD* token) override
    { if (token) *token = 0; return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override
    { if (out) *out = nullptr; return OLE_E_ADVISENOTSUPPORTED; }
};

class FileDragSource : public IDropSource
{
    std::atomic<ULONG> references{1};
    const HWND window;
public:
    std::atomic<bool> cancelled{false};
    explicit FileDragSource(HWND owner) : window(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDropSource))
        { *out = static_cast<IDropSource*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override
    {
        if (escape || cancelled.load(std::memory_order_acquire) || !IsWindow(window)) return DRAGDROP_S_CANCEL;
        return keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

struct FileDragComRelease
{
    template<class T> void operator()(T* object) const { if (object) object->Release(); }
};

class WindowsFileDrag : public PuglFileDrag
{
    const HWND window;
    std::unique_ptr<FileDragData, FileDragComRelease> data;
    std::unique_ptr<FileDragSource, FileDragComRelease> source;
public:
    WindowsFileDrag(HWND owner, std::wstring path) : window(owner), data(new FileDragData(std::move(path))), source(new FileDragSource(owner)) {}
    ~WindowsFileDrag() override { source.reset(); data.reset(); OleUninitialize(); }
    void cancel() override { source->cancelled.store(true, std::memory_order_release); }
    bool idle() override
    {
        if (!source->cancelled.load(std::memory_order_acquire))
        {
            // WM_CAPTURECHANGED is synchronous and may close the editor.
            // Run it from native idle, never from the caller's drawing frame.
            if (GetCapture() == window) ReleaseCapture();
            if (source->cancelled.load(std::memory_order_acquire) || !IsWindow(window)) return false;
            DWORD effect = DROPEFFECT_NONE;
            DoDragDrop(data.get(), source.get(), DROPEFFECT_COPY, &effect);
        }
        return false;
    }
};

PuglFileDrag* puglStartFileDrag(PuglView* view, const char* filename)
{
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return nullptr;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, nullptr, 0);
    if (!count) return nullptr;
    std::vector<wchar_t> text(std::size_t(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, text.data(), count)) return nullptr;
    const bool unc = count > 2 && ((text[0] == L'\\' && text[1] == L'\\') || (text[0] == L'/' && text[1] == L'/'));
    const bool drive = count > 3 && ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z'))
                    && text[1] == L':' && (text[2] == L'\\' || text[2] == L'/');
    if (!unc && !drive) return nullptr;
    const DWORD needed = GetFullPathNameW(text.data(), 0, nullptr, nullptr); if (!needed) return nullptr;
    std::vector<wchar_t> path(std::size_t(needed), L'\0');
    const DWORD size = GetFullPathNameW(text.data(), needed, path.data(), nullptr);
    if (!size || size >= needed) return nullptr;
    const DWORD attributes = GetFileAttributesW(path.data());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return nullptr;
    const HRESULT result = OleInitialize(nullptr); if (FAILED(result)) return nullptr;
    try { return new WindowsFileDrag(view->impl->hwnd, std::wstring(path.data(), size)); }
    catch (...) { OleUninitialize(); return nullptr; }
}
