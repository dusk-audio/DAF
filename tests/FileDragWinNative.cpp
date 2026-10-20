// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Interactive test: injects a gesture only over its own native windows.
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include "dgl/Application.hpp"
#include "dgl/Window.hpp"
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <vector>
#include <cstring>

static int checks=0, failures=0;
static void check(bool ok,const char* message)
{ ++checks; std::printf("%s: %s\n",ok?"PASS":"FAIL",message);std::fflush(stdout);if(!ok)++failures; }

struct Target : IDropTarget
{
    std::atomic<ULONG> refs{1};
    std::function<void()> closeSource;
    const std::wstring expected;
    bool dropped=false;
    int enters=0, moves=0;
    explicit Target(std::wstring path):expected(std::move(path)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override
    {
        if(!out)return E_POINTER;*out=nullptr;
        if(IsEqualIID(iid,IID_IUnknown)||IsEqualIID(iid,IID_IDropTarget))
        {*out=static_cast<IDropTarget*>(this);AddRef();return S_OK;}
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{ULONG n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,DWORD,POINTL,DWORD* effect) override
    {
        ++enters;FORMATETC f{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
        check(data->QueryGetData(&f)==S_OK,"OLE target discovers standard file payload");
        check(*effect==DROPEFFECT_COPY,"OLE offers copy only");*effect=DROPEFFECT_COPY;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD,POINTL,DWORD* effect) override
    {++moves;*effect=DROPEFFECT_COPY;return S_OK;}
    HRESULT STDMETHODCALLTYPE DragLeave() override{return S_OK;}
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data,DWORD,POINTL,DWORD* effect) override
    {
        dropped=true;
        closeSource(); // Exercise real native modal reentrancy before retrieving the payload.
        FORMATETC f{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};STGMEDIUM value{};
        const HRESULT result=data->GetData(&f,&value);
        check(result==S_OK,"destination can read native payload after editor and Application destruction");
        if(SUCCEEDED(result))
        {
            const UINT count=DragQueryFileW(static_cast<HDROP>(value.hGlobal),0xffffffffu,nullptr,0);
            const UINT size=DragQueryFileW(static_cast<HDROP>(value.hGlobal),0,nullptr,0);
            std::vector<wchar_t> path(std::size_t(size)+1);
            DragQueryFileW(static_cast<HDROP>(value.hGlobal),0,path.data(),UINT(path.size()));
            check(count==1&&std::wstring(path.data())==expected,"native destination receives exact Unicode path");
            HANDLE file=CreateFileW(path.data(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
            char bytes[4]{};DWORD read=0;
            check(file!=INVALID_HANDLE_VALUE,"exported file survives editor teardown");
            if(file!=INVALID_HANDLE_VALUE){ReadFile(file,bytes,4,&read,nullptr);CloseHandle(file);}
            check(read==4&&std::memcmp(bytes,"MThd",4)==0,"destination reads original MIDI bytes");
            ReleaseStgMedium(&value);
        }
        *effect=DROPEFFECT_COPY;return S_OK;
    }
};

int main()
{
    using namespace DGL_NAMESPACE;
    check(SUCCEEDED(OleInitialize(nullptr)),"main thread initializes OLE");
    std::unique_ptr<Application> app(new Application(false));
    std::unique_ptr<Window> source(new Window(*app));
    source->setTitle("Dusk isolated drag source");source->setSize(220,180);source->show();app->idle();
    HWND sourceWindow=reinterpret_cast<HWND>(source->getNativeWindowHandle());
    WNDCLASSW targetClass{};targetClass.lpfnWndProc=DefWindowProcW;targetClass.hInstance=GetModuleHandleW(nullptr);targetClass.lpszClassName=L"DuskNativeDropFixture";RegisterClassW(&targetClass);
    HWND targetWindow=CreateWindowExW(WS_EX_TOPMOST,targetClass.lpszClassName,L"Dusk isolated drop target",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        440,60,240,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(targetWindow!=nullptr,"owned OLE destination creates");
    wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
    const std::wstring path=std::wstring(temp)+L"Dusk native " + std::to_wstring(GetCurrentProcessId()) + L" C# \u00e9 \U0001f3b5.mid";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);DWORD written=0;
    const bool created=file!=INVALID_HANDLE_VALUE;
    if(file!=INVALID_HANDLE_VALUE){WriteFile(file,"MThd",4,&written,nullptr);CloseHandle(file);}
    check(written==4,"owned Unicode MIDI fixture creates");
    auto* target=new Target(path);
    target->closeSource=[&]{source->close();source.reset();app.reset();};
    check(RegisterDragDrop(targetWindow,target)==S_OK,"owned native target registers with OLE");
    POINT oldCursor{};GetCursorPos(&oldCursor);
    HWND oldForeground=GetForegroundWindow();
    SetWindowPos(sourceWindow,HWND_TOPMOST,40,60,260,240,SWP_SHOWWINDOW);SetForegroundWindow(sourceWindow);
    POINT start{80,80};ClientToScreen(sourceWindow,&start);
    SetCursorPos(start.x,start.y);
    check(GetAncestor(WindowFromPoint(start),GA_ROOT)==sourceWindow,"gesture starts over the owned source window");
    INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
    const bool foreground=GetForegroundWindow()==sourceWindow
        && GetAncestor(WindowFromPoint(start),GA_ROOT)==sourceWindow;
    check(foreground,"test owns foreground before sending a gesture");
    const bool pressed=foreground&&!(GetAsyncKeyState(VK_LBUTTON)&0x8000)&&SendInput(1,&input,sizeof(input))==1;
    check(pressed,"primary input reaches the test desktop");
    bool accepted=false;
    if(pressed)
    {
        Sleep(30);app->idle();
        const int size=WideCharToMultiByte(CP_UTF8,0,path.c_str(),-1,nullptr,0,nullptr,nullptr);
        std::vector<char> utf8(std::size_t(size),0);WideCharToMultiByte(CP_UTF8,0,path.c_str(),-1,utf8.data(),size,nullptr,nullptr);
        accepted=source->startFileDrag(utf8.data());
        check(accepted,"public Window API accepts physical primary gesture");
        if(accepted)
        {
            POINT destination{60,60};ClientToScreen(targetWindow,&destination);
            std::thread driver([&]{Sleep(180);SetCursorPos(destination.x,destination.y);
                POINT actual{};GetCursorPos(&actual);
                if(GetAncestor(WindowFromPoint(actual),GA_ROOT)!=targetWindow)SetCursorPos(start.x,start.y);
                Sleep(180);
                INPUT release{};release.type=INPUT_MOUSE;release.mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,&release,sizeof(release));});
            app->idle();driver.join();
            check(target->enters>0&&target->dropped,"real DoDragDrop delivers to registered native destination");
            check(!source&&!app,"destination closes editor and Application during native dispatch");
        }
    }
    if(pressed){input.mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,&input,sizeof(input));}
    if(source){source->close();source.reset();}app.reset();
    RevokeDragDrop(targetWindow);DestroyWindow(targetWindow);UnregisterClassW(targetClass.lpszClassName,targetClass.hInstance);target->Release();
    if(created)DeleteFileW(path.c_str());SetCursorPos(oldCursor.x,oldCursor.y);if(IsWindow(oldForeground))SetForegroundWindow(oldForeground);
    OleUninitialize();
    std::printf("Native OLE public Window drop: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
