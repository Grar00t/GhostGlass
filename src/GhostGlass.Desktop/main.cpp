#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cmath>
#include <string>
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d2d1.lib")
#pragma comment(lib,"dcomp.lib")
#pragma comment(lib,"dxgi.lib")
using Microsoft::WRL::ComPtr;
constexpr UINT TRAY=WM_APP+1, TICK=10, HOTKEY=11, EXIT_DEMO=12;
void OpenGhostPanel(HWND owner);
bool GhostPanelSelfTest(HWND owner);
constexpr int MENU_TOGGLE=101,MENU_INTERACT=102,MENU_EXIT=103,MENU_MOTION=104,MENU_HOTKEY=105,MENU_PANEL=106,MENU_SETTINGS=107;
static HWND hwnd=nullptr;
static NOTIFYICONDATAW tray{};
static ComPtr<ID3D11Device> gpu;
static ComPtr<IDXGISwapChain1> swap;
static ComPtr<ID2D1Factory1> factory;
static ComPtr<ID2D1Device> device;
static ComPtr<ID2D1DeviceContext> dc;
static ComPtr<ID2D1Bitmap1> target;
static ComPtr<IDCompositionDevice> composition;
static ComPtr<IDCompositionTarget> compTarget;
static ComPtr<IDCompositionVisual> visual;
static bool interactive=false,visible=true,motion=true,ready=false,rendered=false,demomode=false,hotkeyRegistered=false;
static float phase=0;
static UINT dpi=96;
static int side=176;
static int hotModifiers=MOD_CONTROL|MOD_ALT,hotKey='G';
static HICON icon=nullptr;
static HANDLE instanceGuard=nullptr;
static bool testing=false;
static void OpenSettings(){MessageBoxW(hwnd,L"Settings: choose Ctrl+Alt+G or Ctrl+Alt+H in the Tray menu.\nToggle animation in Tray.\nDrag the ghost in interaction mode.\nPosition and motion are saved locally.",L"GhostGlass settings",MB_OK|MB_ICONINFORMATION);}
static constexpr wchar_t settingsKey[]=L"Software\\GhostGlass";
static void SaveValue(const wchar_t* name,DWORD value){RegSetKeyValueW(HKEY_CURRENT_USER,settingsKey,name,REG_DWORD,&value,sizeof(value));}
static DWORD ReadValue(const wchar_t* name,DWORD fallback){DWORD value=0,bytes=sizeof(value);return RegGetValueW(HKEY_CURRENT_USER,settingsKey,name,RRF_RT_REG_DWORD,nullptr,&value,&bytes)==ERROR_SUCCESS?value:fallback;}
static void SaveSettings(){if(!hwnd)return;RECT r{};if(GetWindowRect(hwnd,&r)){SaveValue(L"X",static_cast<DWORD>(r.left));SaveValue(L"Y",static_cast<DWORD>(r.top));}SaveValue(L"Motion",motion?1:0);SaveValue(L"Hotkey",static_cast<DWORD>(hotKey));}
static bool Fail(HRESULT hr){return FAILED(hr);}
static void StopTimer(){if(hwnd)KillTimer(hwnd,TICK);}
static void UpdateTimer(){StopTimer();if(visible&&motion&&IsWindowVisible(hwnd)) SetTimer(hwnd,TICK,50,nullptr);}
static void Log(const wchar_t* text){OutputDebugStringW(text);}
static void ReleaseGraphics(){if(dc)dc->SetTarget(nullptr);target.Reset();visual.Reset();compTarget.Reset();composition.Reset();swap.Reset();dc.Reset();device.Reset();factory.Reset();gpu.Reset();}
static bool InitGraphics(){
 ReleaseGraphics();
 UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT;
 D3D_FEATURE_LEVEL level{};
 HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,flags,nullptr,0,D3D11_SDK_VERSION,&gpu,&level,nullptr);
 if(Fail(hr))hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,flags,nullptr,0,D3D11_SDK_VERSION,&gpu,&level,nullptr);
 if(Fail(hr))return false;
 ComPtr<IDXGIDevice> dx; if(Fail(gpu.As(&dx)))return false;
 D2D1_FACTORY_OPTIONS options{};if(Fail(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),&options,reinterpret_cast<void**>(factory.GetAddressOf()))))return false;
 if(Fail(factory->CreateDevice(dx.Get(),&device)))return false;
 if(Fail(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&dc)))return false;
 ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> dxFactory;
 if(Fail(dx->GetAdapter(&adapter)))return false;
 if(Fail(adapter->GetParent(IID_PPV_ARGS(&dxFactory))))return false;
 DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=side;desc.Height=side;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
 if(Fail(dxFactory->CreateSwapChainForComposition(gpu.Get(),&desc,nullptr,&swap)))return false;
 ComPtr<IDXGISurface> surface;if(Fail(swap->GetBuffer(0,IID_PPV_ARGS(&surface))))return false;
 D2D1_BITMAP_PROPERTIES1 props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
 if(Fail(dc->CreateBitmapFromDxgiSurface(surface.Get(),&props,&target)))return false;
 dc->SetTarget(target.Get());dc->SetDpi(96,96);
 if(Fail(DCompositionCreateDevice(dx.Get(),IID_PPV_ARGS(&composition))))return false;
 if(Fail(composition->CreateTargetForHwnd(hwnd,TRUE,&compTarget)))return false;
 if(Fail(composition->CreateVisual(&visual)))return false;
 if(Fail(visual->SetContent(swap.Get())))return false;
 if(Fail(compTarget->SetRoot(visual.Get())))return false;
 return SUCCEEDED(composition->Commit());
}
static bool ResizeBuffers(){
 if(!swap||!dc)return false;
 dc->SetTarget(nullptr);target.Reset();
 HRESULT hr=swap->ResizeBuffers(0,side,side,DXGI_FORMAT_UNKNOWN,0);
 if(FAILED(hr))return false;
 ComPtr<IDXGISurface> surface;
 if(FAILED(swap->GetBuffer(0,IID_PPV_ARGS(&surface))))return false;
 D2D1_BITMAP_PROPERTIES1 props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
 if(FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(),&props,&target)))return false;
 dc->SetTarget(target.Get());return true;
}
static void RecoverGraphics();
static void Ellipse(ID2D1SolidColorBrush* b,float x,float y,float rx,float ry,D2D1_COLOR_F color){
 b->SetColor(color);dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),rx,ry),b);
}
static bool Draw(){
 if(!dc || !swap)return false;
 ComPtr<ID2D1SolidColorBrush> brush;
 if(Fail(dc->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1),&brush)))return false;
 dc->BeginDraw();dc->SetTransform(D2D1::Matrix3x2F::Identity());dc->Clear(D2D1::ColorF(0,0));
 float scale=side/176.f;
 dc->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale));
 float bob=motion?std::sin(phase)*4.f:0.f;
 // Genuine transparent composition: independent translucent vector ellipses and specular highlights.
 Ellipse(brush.Get(),88,93+bob,69,70,D2D1::ColorF(.12f,.82f,.84f,.11f));
 Ellipse(brush.Get(),88,87+bob,59,59,D2D1::ColorF(.14f,.84f,.78f,.39f));
 Ellipse(brush.Get(),88,82+bob,51,51,D2D1::ColorF(.54f,.99f,.95f,.45f));
 Ellipse(brush.Get(),67,60+bob,27,17,D2D1::ColorF(.99f,1.f,1.f,.44f));
 Ellipse(brush.Get(),62+std::sin(phase*.4f)*7,53+bob,13,6,D2D1::ColorF(1.f,1.f,1.f,.32f));
 Ellipse(brush.Get(),69,88+bob,5,7,D2D1::ColorF(.04f,.21f,.29f,1.f));
 Ellipse(brush.Get(),106,88+bob,5,7,D2D1::ColorF(.04f,.21f,.29f,1.f));
 Ellipse(brush.Get(),61,105+bob,10,5,D2D1::ColorF(1.f,.60f,.73f,.29f));
 Ellipse(brush.Get(),115,105+bob,10,5,D2D1::ColorF(1.f,.60f,.73f,.29f));
 Ellipse(brush.Get(),88,109+bob,9,3,D2D1::ColorF(.03f,.32f,.36f,.53f));
 if(interactive) Ellipse(brush.Get(),88,145+bob,5,5,D2D1::ColorF(.10f,.98f,.44f,.8f));
 HRESULT hr=dc->EndDraw();if(Fail(hr))return false;
 hr=swap->Present(1,0);rendered=SUCCEEDED(hr);return rendered;
}
static void RecoverGraphics(){StopTimer();if(InitGraphics()){Draw();UpdateTimer();}else{Log(L"GhostGlass GPU recreation failed\n");}}
static void ShowGhost(bool show){
 visible=show;ShowWindow(hwnd,show?SW_SHOWNOACTIVATE:SW_HIDE);UpdateTimer();if(show&&!Draw())RecoverGraphics();
}
static void SetInteraction(bool enable){
 interactive=enable;
 LONG_PTR ex=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
 if(enable)ex&=~WS_EX_TRANSPARENT;else ex|=WS_EX_TRANSPARENT;
 SetWindowLongPtrW(hwnd,GWL_EXSTYLE,ex);
 SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
 Draw();
}
static void ChangeHotkey(){
 const int next=(hotKey=='G')?'H':'G';
 if(!RegisterHotKey(hwnd,HOTKEY+1,hotModifiers|MOD_NOREPEAT,next)){
  MessageBoxW(hwnd,L"Requested shortcut is already in use. Current shortcut remains active.",L"Hotkey conflict",MB_OK|MB_ICONWARNING);return;
 }
 UnregisterHotKey(hwnd,HOTKEY);UnregisterHotKey(hwnd,HOTKEY+1);
 if(RegisterHotKey(hwnd,HOTKEY,hotModifiers|MOD_NOREPEAT,next)){hotkeyRegistered=true;hotKey=next;SaveSettings();}
 else {hotkeyRegistered=RegisterHotKey(hwnd,HOTKEY,hotModifiers|MOD_NOREPEAT,hotKey)!=0;MessageBoxW(hwnd,L"Hotkey change failed. Use the tray controls.",L"GhostGlass",MB_OK|MB_ICONERROR);}
}
static void Menu(){
 POINT p;GetCursorPos(&p);HMENU menu=CreatePopupMenu();
 AppendMenuW(menu,MF_STRING,MENU_TOGGLE,visible?L"Hide ghost":L"Show ghost");
 AppendMenuW(menu,MF_STRING,MENU_INTERACT,interactive?L"Enable click-through":L"Enable interaction / dragging");
 AppendMenuW(menu,MF_STRING|(motion?MF_CHECKED:0),MENU_MOTION,L"Animate");
 AppendMenuW(menu,MF_STRING,MENU_HOTKEY,hotKey=='G'?L"Shortcut: Ctrl+Alt+G (change to H)":L"Shortcut: Ctrl+Alt+H (change to G)");
 AppendMenuW(menu,MF_STRING,MENU_PANEL,L"Open Chat");
 AppendMenuW(menu,MF_STRING,MENU_SETTINGS,L"Settings");
 AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,MENU_EXIT,L"Exit GhostGlass");
 SetForegroundWindow(hwnd);TrackPopupMenu(menu,TPM_BOTTOMALIGN|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd,nullptr);DestroyMenu(menu);PostMessageW(hwnd,WM_NULL,0,0);
}
static HICON MakeIcon(){
 HDC screen=GetDC(nullptr);
 BITMAPV5HEADER hdr{};hdr.bV5Size=sizeof(hdr);hdr.bV5Width=32;hdr.bV5Height=-32;hdr.bV5Planes=1;hdr.bV5BitCount=32;hdr.bV5Compression=BI_BITFIELDS;hdr.bV5RedMask=0x00FF0000;hdr.bV5GreenMask=0x0000FF00;hdr.bV5BlueMask=0x000000FF;hdr.bV5AlphaMask=0xFF000000;
 void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(screen,reinterpret_cast<BITMAPINFO*>(&hdr),DIB_RGB_COLORS,&bits,nullptr,0);
 if(bitmap&&bits){auto* pixels=static_cast<unsigned*>(bits);for(int y=0;y<32;y++)for(int x=0;x<32;x++){float dx=(x-16)/14.f,dy=(y-16)/14.f;bool body=dx*dx+dy*dy<1;bool eye=(x>=11&&x<=13||x>=19&&x<=21)&&y>=14&&y<=17;pixels[y*32+x]=body?(eye?0xFF102B35:0xFF7AE5DF):0;}}
 HBITMAP mask=CreateBitmap(32,32,1,1,nullptr);ICONINFO ii{};ii.fIcon=TRUE;ii.hbmColor=bitmap;ii.hbmMask=mask;HICON result=CreateIconIndirect(&ii);
 DeleteObject(mask);DeleteObject(bitmap);ReleaseDC(nullptr,screen);return result;
}
static LRESULT CALLBACK Proc(HWND win,UINT msg,WPARAM w,LPARAM l){
 switch(msg){
 case WM_NCHITTEST:{
  if(!interactive)return HTTRANSPARENT;
  POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(win,&pt);
  float x=(pt.x*176.f/side-88)/63.f,y=(pt.y*176.f/side-87)/65.f;
  return (x*x+y*y<1.f)?HTCAPTION:HTTRANSPARENT;
 }
 case WM_HOTKEY: if(w==HOTKEY){if(!visible)ShowGhost(true);SetInteraction(!interactive);}return 0;
 case WM_TIMER:if(w==TICK){phase+=.047f;if(phase>6.28318f)phase-=6.28318f;if(!Draw())RecoverGraphics();}else if(w==EXIT_DEMO){KillTimer(win,EXIT_DEMO);DestroyWindow(win);}return 0;
 case WM_COMMAND:switch(LOWORD(w)){case MENU_TOGGLE:ShowGhost(!visible);break;case MENU_INTERACT:if(!visible)ShowGhost(true);SetInteraction(!interactive);break;case MENU_MOTION:motion=!motion;SaveSettings();UpdateTimer();Draw();break;case MENU_HOTKEY:ChangeHotkey();break;case MENU_PANEL:OpenGhostPanel(hwnd);break;case MENU_SETTINGS:OpenSettings();break;case MENU_EXIT:DestroyWindow(win);break;}return 0;
 case WM_DPICHANGED:{
  dpi=HIWORD(w);side=MulDiv(176,dpi,96);const RECT* proposed=reinterpret_cast<const RECT*>(l);
  SetWindowPos(win,nullptr,proposed->left,proposed->top,side,side,SWP_NOZORDER|SWP_NOACTIVATE);
  if(!ResizeBuffers())RecoverGraphics();else Draw();
  return 0;
 }
 case WM_SIZE: if(ready && w!=SIZE_MINIMIZED){int next=LOWORD(l);if(next>0 && next!=side){side=next;if(!ResizeBuffers())RecoverGraphics();else Draw();}}return 0;
 case WM_DISPLAYCHANGE: {HMONITOR mon=MonitorFromWindow(win,MONITOR_DEFAULTTONEAREST);MONITORINFO info{sizeof(info)};if(GetMonitorInfoW(mon,&info)){RECT r{};GetWindowRect(win,&r);int x=std::max(info.rcWork.left,std::min(r.left,info.rcWork.right-side));int y=std::max(info.rcWork.top,std::min(r.top,info.rcWork.bottom-side));SetWindowPos(win,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);}return 0;}
 case TRAY:if(l==WM_RBUTTONUP||l==WM_CONTEXTMENU)Menu();else if(l==WM_LBUTTONDBLCLK){if(!visible)ShowGhost(true);SetInteraction(!interactive);}return 0;
 case WM_EXITSIZEMOVE:SaveSettings();return 0;
 case WM_DESTROY:SaveSettings();StopTimer();UnregisterHotKey(win,HOTKEY);Shell_NotifyIconW(NIM_DELETE,&tray);ReleaseGraphics();PostQuitMessage(rendered?0:74);return 0;
 }
 return DefWindowProcW(win,msg,w,l);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR cmd,int){
 demomode=wcsstr(cmd,L"--self-test")!=nullptr;
 testing=demomode;
 if(!testing){
  instanceGuard=CreateMutexW(nullptr,TRUE,L"Local\\GhostGlass.Desktop.v1");
  if(!instanceGuard)return 74;
  if(GetLastError()==ERROR_ALREADY_EXISTS){
   HWND existing=FindWindowW(L"GhostGlass.Native.v1",nullptr);
   if(existing)PostMessageW(existing,WM_COMMAND,MAKEWPARAM(MENU_PANEL,0),0);
   CloseHandle(instanceGuard);return 0;
  }
 }
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.hInstance=instance;cls.lpfnWndProc=Proc;cls.lpszClassName=L"GhostGlass.Native.v1";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
 if(!RegisterClassExW(&cls))return 65;
 POINT pt{};GetCursorPos(&pt);HMONITOR monitor=MonitorFromPoint(pt,MONITOR_DEFAULTTOPRIMARY);
 dpi=GetDpiForSystem();side=MulDiv(176,dpi,96);
 BOOL systemMotion=TRUE;
 SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&systemMotion,0);
 motion=ReadValue(L"Motion",systemMotion?1:0)!=0 && systemMotion!=FALSE;
 DWORD savedKey=ReadValue(L"Hotkey",'G');hotKey=(savedKey=='G'||savedKey=='H')?static_cast<int>(savedKey):'G';
 MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(monitor,&mi);
 int x=static_cast<int>(ReadValue(L"X",static_cast<DWORD>(mi.rcWork.right-side-32)));
 int y=static_cast<int>(ReadValue(L"Y",static_cast<DWORD>(mi.rcWork.bottom-side-36)));
 x=std::max<int>(mi.rcWork.left,std::min<int>(x,mi.rcWork.right-side));y=std::max<int>(mi.rcWork.top,std::min<int>(y,mi.rcWork.bottom-side));
 hwnd=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TRANSPARENT,cls.lpszClassName,L"GhostGlass",WS_POPUP,x,y,side,side,nullptr,nullptr,instance,nullptr);
 if(!hwnd)return 65;
 if(!InitGraphics()){DestroyWindow(hwnd);return 74;}
 ready=true;
 icon=MakeIcon();tray.cbSize=sizeof(tray);tray.hWnd=hwnd;tray.uID=1;tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;tray.uCallbackMessage=TRAY;tray.hIcon=icon?icon:LoadIconW(nullptr,IDI_APPLICATION);wcscpy_s(tray.szTip,L"GhostGlass | Tray to interact");
 if(!Shell_NotifyIconW(NIM_ADD,&tray)){DestroyWindow(hwnd);if(icon)DestroyIcon(icon);return 74;}
 Shell_NotifyIconW(NIM_SETVERSION,&tray);
 const bool conflictTest=wcsstr(cmd,L"--self-test-hotkey")!=nullptr;
 if(conflictTest)RegisterHotKey(hwnd,HOTKEY+9,hotModifiers|MOD_NOREPEAT,hotKey);
 hotkeyRegistered=RegisterHotKey(hwnd,HOTKEY,hotModifiers|MOD_NOREPEAT,hotKey)!=0;
 if(!hotkeyRegistered&&!conflictTest)MessageBoxW(hwnd,L"The shortcut is reserved. GhostGlass is still available from the tray; choose another shortcut there.",L"GhostGlass hotkey conflict",MB_OK|MB_ICONWARNING);
 if(conflictTest){
  bool rejected=!hotkeyRegistered;
  UnregisterHotKey(hwnd,HOTKEY+9);
  bool alternate=RegisterHotKey(hwnd,HOTKEY+10,hotModifiers|MOD_NOREPEAT,(hotKey=='G'?'H':'G'))!=0;
  UnregisterHotKey(hwnd,HOTKEY+10);
  DestroyWindow(hwnd);if(icon)DestroyIcon(icon);return rejected&&alternate?0:65;
 }
 ShowWindow(hwnd,SW_SHOWNOACTIVATE);if(!Draw()){DestroyWindow(hwnd);if(icon)DestroyIcon(icon);return 74;}UpdateTimer();
 // First-use help lives in the chat panel; never show a modal welcome message.
 if(wcsstr(cmd,L"--self-test-panel")){bool ok=GhostPanelSelfTest(hwnd);DestroyWindow(hwnd);if(icon)DestroyIcon(icon);return ok?0:65;}
 if(wcsstr(cmd,L"--self-test-stress")){
  bool ok=true;
  for(int i=0;i<60;i++){
   ShowGhost(false);ok=ok&&!IsWindowVisible(hwnd);
   ShowGhost(true);ok=ok&&IsWindowVisible(hwnd);
   SetInteraction(true);ok=ok&&!(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TRANSPARENT);
   SetInteraction(false);ok=ok&&(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TRANSPARENT);
   if(!ok)break;
  }
  int original=side;
  side=original+12;ok=ok&&ResizeBuffers()&&Draw();
  side=original;ok=ok&&ResizeBuffers()&&Draw();
  DestroyWindow(hwnd);if(icon)DestroyIcon(icon);return ok?0:65;
 }
 if(wcsstr(cmd,L"--self-test-hidden")){ShowGhost(false);SetTimer(hwnd,EXIT_DEMO,6500,nullptr);}
 else if(wcsstr(cmd,L"--self-test-active"))SetTimer(hwnd,EXIT_DEMO,6500,nullptr);
 else if(demomode)SetTimer(hwnd,EXIT_DEMO,1300,nullptr);
 MSG message{};int exit=0;while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
 exit=static_cast<int>(message.wParam);if(icon)DestroyIcon(icon);
 if(instanceGuard){ReleaseMutex(instanceGuard);CloseHandle(instanceGuard);instanceGuard=nullptr;}
 return exit;
}
