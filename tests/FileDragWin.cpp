// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "dgl/src/pugl.cpp"
#include <string>
#include <vector>

int main()
{
    using namespace DGL_NAMESPACE;
    int checks=0, failures=0;
    const auto check=[&](bool ok,const char* text){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",text);}};
    const std::wstring path=L"C:\\Users\\marc\\MIDI clips\\C# \u00e9 \U0001f3b5.mid";
    auto* object=new FileDragData(path);
    FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    check(object->QueryGetData(&format)==S_OK,"the standard file-drop format is offered");
    FORMATETC invalid=format;invalid.cfFormat=CF_TEXT;
    check(FAILED(object->QueryGetData(&invalid)),"unsupported text format is rejected");
    invalid=format;invalid.tymed=TYMED_ISTREAM;
    check(FAILED(object->QueryGetData(&invalid)),"unsupported storage is rejected");
    invalid=format;invalid.lindex=0;
    check(FAILED(object->QueryGetData(&invalid)),"indexed data is rejected");
    void* queried=nullptr;
    check(object->QueryInterface(IID_IDataObject,&queried)==S_OK && queried,"the native data interface is queryable");
    object->Release(); // The destination now owns the only remaining reference.
    auto* retained=static_cast<IDataObject*>(queried);
    STGMEDIUM first{},second{};
    check(retained->GetData(&format,&first)==S_OK && first.tymed==TYMED_HGLOBAL && first.pUnkForRelease==nullptr,
          "the destination obtains an independently owned native file list");
    check(retained->GetData(&format,&second)==S_OK && second.hGlobal!=first.hGlobal,"repeated reads allocate independent transfer buffers");
    const auto verify=[&](STGMEDIUM& medium){
        const UINT count=DragQueryFileW(static_cast<HDROP>(medium.hGlobal),0xffffffffu,nullptr,0);
        const UINT length=DragQueryFileW(static_cast<HDROP>(medium.hGlobal),0,nullptr,0);
        std::vector<wchar_t> actual(std::size_t(length)+1,L'\0');
        DragQueryFileW(static_cast<HDROP>(medium.hGlobal),0,actual.data(),UINT(actual.size()));
        check(count==1 && std::wstring(actual.data())==path,"the Windows shell decodes exactly one complete Unicode MIDI path");
        auto* files=static_cast<DROPFILES*>(GlobalLock(medium.hGlobal));
        const auto* text=reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(files)+files->pFiles);
        check(files->fWide && text[path.size()]==0 && text[path.size()+1]==0,"the UTF-16 file list has both required terminators");
        GlobalUnlock(medium.hGlobal);
    };
    if(first.hGlobal)verify(first);if(second.hGlobal)verify(second);
    IEnumFORMATETC* formats=nullptr;
    check(retained->EnumFormatEtc(DATADIR_GET,&formats)==S_OK && formats,"standard format enumeration is available");
    if(formats){
        FORMATETC values[2]{};ULONG fetched=0;
        check(formats->Next(2,values,&fetched)==S_FALSE && fetched==1 && values[0].cfFormat==CF_HDROP,
              "enumeration returns the complete supported format set");
        check(formats->Reset()==S_OK,"enumeration resets");
        IEnumFORMATETC* clone=nullptr;
        check(formats->Clone(&clone)==S_OK && clone,"enumerators clone their current cursor");
        if(clone){check(clone->Next(1,values,&fetched)==S_OK && fetched==1,"cloned enumeration remains independent");clone->Release();}
        check(formats->Skip(1)==S_OK && formats->Next(1,values,&fetched)==S_FALSE && fetched==0,
              "skip and exhausted enumeration obey COM rules");
        formats->Release();
    }
    check(retained->Release()==0,"the last destination reference releases the data object");
    if(first.hGlobal)verify(first);if(second.hGlobal)verify(second);
    if(first.hGlobal)ReleaseStgMedium(&first);if(second.hGlobal)ReleaseStgMedium(&second);
    HWND window=CreateWindowExW(0,L"STATIC",L"Dusk file drag control",0,0,0,1,1,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(window!=nullptr,"a private native source window is available");
    if(window){
        auto* source=new FileDragSource(window);
        check(source->QueryContinueDrag(FALSE,MK_LBUTTON)==S_OK,"held primary button continues a live drag");
        check(source->QueryContinueDrag(FALSE,0)==DRAGDROP_S_DROP,"primary release requests a drop");
        check(source->QueryContinueDrag(TRUE,MK_LBUTTON)==DRAGDROP_S_CANCEL,"Escape cancels the native drag");
        check(source->GiveFeedback(DROPEFFECT_COPY)==DRAGDROP_S_USEDEFAULTCURSORS,"the system supplies native copy feedback");
        source->cancelled.store(true,std::memory_order_release);
        check(source->QueryContinueDrag(FALSE,MK_LBUTTON)==DRAGDROP_S_CANCEL,"window-owner cancellation reaches OLE");
        source->cancelled.store(false,std::memory_order_release);DestroyWindow(window);
        check(source->QueryContinueDrag(FALSE,MK_LBUTTON)==DRAGDROP_S_CANCEL,"destroying the source native window cancels OLE");
        source->Release();
    }
    std::printf("Native OLE file payload: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
