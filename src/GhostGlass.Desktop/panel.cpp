#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <richedit.h>
#include <commdlg.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr UINT WM_PANEL_DATA=WM_APP+19,WM_PANEL_DONE=WM_APP+20;
constexpr int EDIT_HISTORY=1300,EDIT_COMPOSER=1301,BUTTON_SEND=1302,BUTTON_CANCEL=1303,BUTTON_PING=1304,BUTTON_SEARCH=1305,BUTTON_ATTACH=1306,BUTTON_REMOVE=1307;
static HWND panel=nullptr,output=nullptr,composer=nullptr,sendButton=nullptr,cancelButton=nullptr,pingButton=nullptr,searchButton=nullptr,attachButton=nullptr,removeButton=nullptr,statusLabel=nullptr;
static std::mutex resultMutex;
static std::wstring conversationText=L"";
static std::vector<std::pair<std::wstring,std::wstring>> chatTurns;
static std::wstring pendingUser;
static std::wstring pendingAssistant;
static std::wstring draft;
static std::wstring lastError;
static std::thread worker;
static std::atomic_bool active=false,cancelRequested=false;
static bool composing=false;
static WNDPROC previousComposerProc=nullptr;
static HMODULE richeditModule=nullptr;

static void ReportError(HWND destination,const std::wstring& reason){
 {std::lock_guard<std::mutex> lock(resultMutex);lastError=reason;}
 if(IsWindow(destination))PostMessageW(destination,WM_PANEL_DATA,0,0);
}
static void Publish(HWND destination,const std::wstring& update){
 {
  std::lock_guard<std::mutex> lock(resultMutex);
  conversationText+=update;
  if(conversationText.size()>60000)conversationText.erase(0,conversationText.size()-60000);
 }
 if(IsWindow(destination))PostMessageW(destination,WM_PANEL_DATA,0,0);
}
static bool WriteExact(HANDLE pipe,const char* bytes,DWORD size){
 DWORD total=0;
 while(total<size){
  DWORD written=0;
  if(!WriteFile(pipe,bytes+total,size-total,&written,nullptr)||written==0)return false;
  total+=written;
 }
 return true;
}
static bool ReadExact(HANDLE pipe,char* bytes,DWORD size){
 DWORD total=0;
 while(total<size){
  DWORD read=0;
  if(!ReadFile(pipe,bytes+total,size-total,&read,nullptr)||read==0)return false;
  total+=read;
 }
 return true;
}
static std::wstring GetText(HWND control){
 int chars=GetWindowTextLengthW(control);
 if(chars<1)return L"";
 std::wstring text(static_cast<size_t>(chars)+1,L'\0');
 int copied=GetWindowTextW(control,text.data(),chars+1);
 text.resize(static_cast<size_t>(std::max(copied,0)));
 return text;
}
static void CancelWorker(){
 cancelRequested.store(true);
 if(worker.joinable())CancelSynchronousIo(reinterpret_cast<HANDLE>(worker.native_handle()));
}
static std::wstring MakeHistory(const std::wstring& next){
 winrt::Windows::Data::Json::JsonArray entries;
 auto add=[&](const std::wstring& role,const std::wstring& text){winrt::Windows::Data::Json::JsonObject t;t.Insert(L"Role",winrt::Windows::Data::Json::JsonValue::CreateStringValue(role));t.Insert(L"Content",winrt::Windows::Data::Json::JsonValue::CreateStringValue(text));entries.Append(t);};
 add(L"system",L"You are a helpful assistant inside GhostGlass. Answer questions in context. Do not claim access to the user's screen or tools.");
 size_t begin=chatTurns.size()>18?chatTurns.size()-18:0;
 for(size_t i=begin;i<chatTurns.size();++i)add(chatTurns[i].first,chatTurns[i].second);
 add(L"user",next);
 return entries.Stringify().c_str();
}
static void RunRequest(HWND destination,std::wstring operation,std::wstring value){
 try{
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  if(!WaitNamedPipeW(L"\\\\.\\pipe\\GhostGlass.Host.v1",1000)){
   Publish(destination,L"\r\n[Host unavailable — start scripts\\run-host.ps1]\r\n");
  }else{
   HANDLE pipe=CreateFileW(L"\\\\.\\pipe\\GhostGlass.Host.v1",GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
   if(pipe==INVALID_HANDLE_VALUE){
    Publish(destination,L"\r\n[Local Host pipe busy or inaccessible]\r\n");
   }else{
    winrt::Windows::Data::Json::JsonObject request;
    request.Insert(L"Version",winrt::Windows::Data::Json::JsonValue::CreateNumberValue(1));
    request.Insert(L"Id",winrt::Windows::Data::Json::JsonValue::CreateStringValue(L"native-panel"));
    request.Insert(L"Op",winrt::Windows::Data::Json::JsonValue::CreateStringValue(operation));
    request.Insert(L"Value",winrt::Windows::Data::Json::JsonValue::CreateStringValue(value));
    std::string json=winrt::to_string(request.Stringify());
    DWORD length=static_cast<DWORD>(json.size());
    char header[4]={char(length),char(length>>8),char(length>>16),char(length>>24)};
    bool ok=WriteExact(pipe,header,4)&&WriteExact(pipe,json.data(),length);
    bool completed=false;std::wstring assistantResult;
    if(!ok)ReportError(destination,L"تعذر إرسال الطلب إلى المضيف");
    while(ok&&!cancelRequested.load()){
     if(!ReadExact(pipe,header,4)){ReportError(destination,L"انقطع الاتصال أثناء الرد");break;}
     DWORD size=static_cast<unsigned char>(header[0])|
       (DWORD(static_cast<unsigned char>(header[1]))<<8)|
       (DWORD(static_cast<unsigned char>(header[2]))<<16)|
       (DWORD(static_cast<unsigned char>(header[3]))<<24);
     if(size==0||size>1048576){ReportError(destination,L"استجابة غير صالحة من المضيف");break;}
     std::string frame(size,'\0');
     if(!ReadExact(pipe,frame.data(),size)){ReportError(destination,L"استجابة غير مكتملة");break;}
     auto response=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(frame));
     bool accepted=response.GetNamedBoolean(L"Ok",false);
     bool done=response.GetNamedBoolean(L"Done",true);
     if(!accepted){
      std::wstring reason=response.GetNamedString(L"Error",L"unknown_error").c_str();
      ReportError(destination,reason==L"model_unavailable"?L"النموذج غير متاح":reason==L"provider_request_failed"?L"تعذر الحصول على رد من النموذج (السبب غير محدد)":L"تعذر إكمال الطلب. التفاصيل: "+reason);
     }else{
      std::wstring part=response.GetNamedString(L"Result",L"").c_str();
      if(!part.empty()){Publish(destination,part);if(operation==L"chat")assistantResult+=part;}
     }
     if(done&&accepted)completed=true;
     if(done||!accepted)break;
    }
    CloseHandle(pipe);
    if(operation==L"chat" && completed && !cancelRequested.load() && !assistantResult.empty()){
      std::lock_guard<std::mutex> lock(resultMutex);
      pendingAssistant=assistantResult;
    }
    if(cancelRequested.load())ReportError(destination,L"تم إيقاف الطلب");
   }
  }
 }catch(const std::exception&){
  ReportError(destination,L"فشل الاتصال بالمضيف المحلي");
 }catch(const winrt::hresult_error&){
  ReportError(destination,L"استجابة غير صالحة من المضيف");
 }
 active.store(false);
 if(IsWindow(destination))PostMessageW(destination,WM_PANEL_DONE,0,0);
}
static void StartRequest(HWND destination,std::wstring operation,std::wstring text){
 if(active.load())return;
 if(worker.joinable())worker.join();
 active.store(true);cancelRequested.store(false);
 EnableWindow(sendButton,FALSE);EnableWindow(pingButton,FALSE);EnableWindow(searchButton,FALSE);EnableWindow(attachButton,FALSE);EnableWindow(removeButton,FALSE);EnableWindow(cancelButton,TRUE);
 worker=std::thread([destination,operation=std::move(operation),text=std::move(text)]()mutable{
  RunRequest(destination,std::move(operation),std::move(text));
 });
}
static LRESULT CALLBACK ComposerProc(HWND wnd,UINT message,WPARAM w,LPARAM l){
 if(message==WM_IME_STARTCOMPOSITION)composing=true;
 if(message==WM_IME_ENDCOMPOSITION)composing=false;
 if(message==WM_KEYDOWN&&w==VK_RETURN&&!composing&&!(GetKeyState(VK_SHIFT)&0x8000)){
  SendMessageW(GetParent(wnd),WM_COMMAND,MAKEWPARAM(BUTTON_SEND,BN_CLICKED),reinterpret_cast<LPARAM>(sendButton));
  return 0;
 }
 return CallWindowProcW(previousComposerProc,wnd,message,w,l);
}
static void UpdateControls(HWND win){
 RECT rc{};GetClientRect(win,&rc);
 int width=std::max(320L,rc.right-rc.left),height=std::max(270L,rc.bottom-rc.top);
 MoveWindow(output,14,45,width-28,height-205,TRUE);
 MoveWindow(composer,14,height-147,width-28,80,TRUE);
 MoveWindow(sendButton,14,height-54,78,32,TRUE);
 MoveWindow(cancelButton,100,height-54,76,32,TRUE);
 MoveWindow(pingButton,184,height-54,90,32,TRUE);
 MoveWindow(searchButton,282,height-54,90,32,TRUE);
 MoveWindow(attachButton,380,height-54,90,32,TRUE);
 MoveWindow(removeButton,478,height-54,90,32,TRUE);
}
static LRESULT CALLBACK PanelProc(HWND win,UINT msg,WPARAM w,LPARAM l){
 switch(msg){
 case WM_CREATE:{
  richeditModule=LoadLibraryW(L"Msftedit.dll");
  if(!richeditModule)return -1;
  HFONT font=static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  HWND header=CreateWindowExW(0,L"STATIC",L"GhostGlass",WS_CHILD|WS_VISIBLE,14,12,200,25,win,nullptr,nullptr,nullptr);
  statusLabel=CreateWindowExW(0,L"STATIC",L"deepseek-coder-v2:16b",WS_CHILD|WS_VISIBLE|SS_RIGHT,210,12,330,25,win,nullptr,nullptr,nullptr);
  output=CreateWindowExW(WS_EX_CLIENTEDGE,L"RICHEDIT50W",L"",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,14,45,520,205,win,reinterpret_cast<HMENU>(static_cast<INT_PTR>(EDIT_HISTORY)),nullptr,nullptr);
  composer=CreateWindowExW(WS_EX_CLIENTEDGE,L"RICHEDIT50W",L"",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,14,265,520,80,win,reinterpret_cast<HMENU>(static_cast<INT_PTR>(EDIT_COMPOSER)),nullptr,nullptr);
  sendButton=CreateWindowExW(0,L"BUTTON",L"Send",WS_CHILD|WS_VISIBLE|WS_TABSTOP,14,365,116,32,win,reinterpret_cast<HMENU>(static_cast<INT_PTR>(BUTTON_SEND)),nullptr,nullptr);
  cancelButton=CreateWindowExW(0,L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_DISABLED,140,365,94,32,win,reinterpret_cast<HMENU>(static_cast<INT_PTR>(BUTTON_CANCEL)),nullptr,nullptr);
  pingButton=CreateWindowExW(0,L"BUTTON",L"Diagnostics",WS_CHILD|WS_VISIBLE|WS_TABSTOP,184,365,90,32,win,reinterpret_cast<HMENU>(static_cast<INT_PTR>(BUTTON_PING)),nullptr,nullptr);
  searchButton=CreateWindowExW(0,L"BUTTON",L"Search",WS_CHILD|WS_VISIBLE|WS_TABSTOP,282,365,90,32,win,reinterpret_cast<HMENU>(static_cast<INT_PTR>(BUTTON_SEARCH)),nullptr,nullptr);
  attachButton=CreateWindowExW(0,L"BUTTON",L"Attach",WS_CHILD|WS_VISIBLE|WS_TABSTOP,380,365,90,32,win,reinterpret_cast<HMENU>(static_cast<INT_PTR>(BUTTON_ATTACH)),nullptr,nullptr);
  removeButton=CreateWindowExW(0,L"BUTTON",L"Files",WS_CHILD|WS_VISIBLE|WS_TABSTOP,478,365,90,32,win,reinterpret_cast<HMENU>(static_cast<INT_PTR>(BUTTON_REMOVE)),nullptr,nullptr);
  for(HWND child:{header,statusLabel,output,composer,sendButton,cancelButton,pingButton,searchButton,attachButton,removeButton})if(child)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
  if(!output||!composer||!sendButton||!cancelButton||!pingButton||!searchButton||!attachButton||!removeButton)return -1;
  previousComposerProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(composer,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ComposerProc)));
  if(conversationText.empty())SetWindowTextW(output,L"مرحباً بك في GhostGlass. اكتب سؤالك هنا، أو استخدم Tray لفتح المحادثة مرة أخرى.\r\n\r\nWelcome. Type your question below. You can reopen this panel from the tray.");
  else SetWindowTextW(output,conversationText.c_str());
  return 0;
 }
 case WM_SIZE:if(output)UpdateControls(win);return 0;
 case WM_COMMAND:{
  switch(LOWORD(w)){
  case BUTTON_SEND:{
   if(active.load())return 0;
   std::wstring prompt=GetText(composer);
   if(prompt.empty())return 0;
   if(prompt.size()>16000){MessageBoxW(win,L"Prompt exceeds 16000 characters.",L"GhostGlass",MB_OK|MB_ICONWARNING);return 0;}
   SetWindowTextW(composer,L"");draft.clear();
   std::wstring payload=MakeHistory(prompt);
   {std::lock_guard<std::mutex> lock(resultMutex);lastError.clear();}
   {std::lock_guard<std::mutex> lock(resultMutex);pendingUser=prompt;pendingAssistant.clear();}
   Publish(win,L"\r\nأنت: "+prompt+L"\r\n\r\nالمساعد: ");
   StartRequest(win,L"chat",payload);
   return 0;
  }
  case BUTTON_PING:
   MessageBoxW(win,L"Connection: local named pipe.\nOllama model: deepseek-coder-v2:16b.\nHost reachable is not proof of inference.",L"Diagnostics",MB_OK|MB_ICONINFORMATION);
   return 0;
  case BUTTON_SEARCH:{
   if(active.load())return 0;
   std::wstring query=GetText(composer);
   if(query.empty())return 0;
   Publish(win,L"\r\nLocal search: "+query+L"\r\n");
   StartRequest(win,L"search",query);return 0;
  }
  case BUTTON_ATTACH:{
   if(active.load())return 0;
   wchar_t file[32768]={};
   OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=win;dialog.lpstrFile=file;dialog.nMaxFile=32768;
   dialog.lpstrFilter=L"Text documents\0*.txt;*.md;*.json;*.csv\0\0";
   dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT;
   if(GetOpenFileNameW(&dialog)){
    Publish(win,L"\r\nIndexing explicitly selected file (local only): "+std::wstring(file)+L"\r\n");
    StartRequest(win,L"index-file",file);
   }
   return 0;
  }
  case BUTTON_REMOVE:{
   if(active.load())return 0;
   std::wstring id=GetText(composer);
   if(id.size()!=64||id.find_first_not_of(L"0123456789abcdefABCDEF")!=std::wstring::npos){
    MessageBoxW(win,L"Paste a 64-character SHA-256 source ID to remove an indexed copy.",L"GhostGlass",MB_OK|MB_ICONINFORMATION);return 0;
   }
   if(MessageBoxW(win,L"Remove this indexed copy and search entry? The original file will remain untouched.",L"Confirm local index deletion",MB_YESNO|MB_ICONQUESTION)==IDYES){
    Publish(win,L"\r\nRemoving indexed source: "+id+L"\r\n");StartRequest(win,L"remove-index",id);
   }
   return 0;
  }
  case BUTTON_CANCEL:CancelWorker();return 0;
  }
  break;
 }
 case WM_PANEL_DATA:{
  std::lock_guard<std::mutex> lock(resultMutex);
  if(statusLabel&&IsWindow(statusLabel))SetWindowTextW(statusLabel,lastError.empty()?L"deepseek-coder-v2:16b":lastError.c_str());
  if(output&&IsWindow(output)){
   SetWindowTextW(output,conversationText.c_str());
   SendMessageW(output,EM_SETSEL,conversationText.length(),conversationText.length());
   SendMessageW(output,EM_SCROLLCARET,0,0);
  }
  return 0;
 }
 case WM_PANEL_DONE:{
  if(worker.joinable())worker.join();
  {std::lock_guard<std::mutex> lock(resultMutex);
   if(!pendingAssistant.empty()){
    chatTurns.emplace_back(L"user",pendingUser);
    chatTurns.emplace_back(L"assistant",pendingAssistant);
   }
   pendingAssistant.clear();pendingUser.clear();
  }
  EnableWindow(sendButton,TRUE);EnableWindow(pingButton,TRUE);EnableWindow(searchButton,TRUE);EnableWindow(attachButton,TRUE);EnableWindow(removeButton,TRUE);EnableWindow(cancelButton,FALSE);
  SetFocus(composer);
  return 0;
 }
 case WM_CLOSE:draft=GetText(composer);CancelWorker();ShowWindow(win,SW_HIDE);return 0;
 case WM_DESTROY:{
  CancelWorker();if(worker.joinable())worker.join();
  panel=nullptr;statusLabel=nullptr;output=nullptr;composer=nullptr;sendButton=nullptr;cancelButton=nullptr;pingButton=nullptr;searchButton=nullptr;attachButton=nullptr;removeButton=nullptr;
  // Msftedit stays loaded until process exit: RichEdit children may still be handling destruction.
  return 0;
 }
 }
 return DefWindowProcW(win,msg,w,l);
}
void OpenGhostPanel(HWND owner){
 if(panel&&IsWindow(panel)){ShowWindow(panel,SW_RESTORE);SetWindowTextW(composer,draft.c_str());SetForegroundWindow(panel);return;}
 HINSTANCE inst=reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner,GWLP_HINSTANCE));
 WNDCLASSW cls{};cls.hInstance=inst;cls.lpfnWndProc=PanelProc;cls.lpszClassName=L"GhostGlass.LocalPanel.v1";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
 RegisterClassW(&cls);
 RECT work{};HMONITOR mon=MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST);MONITORINFO mi{sizeof(mi)};
 if(GetMonitorInfoW(mon,&mi))work=mi.rcWork;else SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
 int width=std::min(590L,work.right-work.left),height=std::min(500L,work.bottom-work.top);
 int x=work.left+std::max(0L,(work.right-work.left-width)/2),y=work.top+std::max(0L,(work.bottom-work.top-height)/2);
 panel=CreateWindowExW(0,cls.lpszClassName,L"GhostGlass - المحادثة",WS_OVERLAPPEDWINDOW|WS_VISIBLE,x,y,width,height,nullptr,nullptr,inst,nullptr);
}
bool GhostPanelSelfTest(HWND owner){
 OpenGhostPanel(owner);
 bool ok=panel&&IsWindow(panel)&&output&&IsWindow(output)&&composer&&IsWindow(composer)&&sendButton&&IsWindow(sendButton)&&cancelButton&&IsWindow(cancelButton)&&searchButton&&IsWindow(searchButton)&&attachButton&&IsWindow(attachButton)&&removeButton&&IsWindow(removeButton);
 if(panel)DestroyWindow(panel);
 return ok;
}
