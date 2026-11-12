// The Win32 front end of folder_pick.h: the shell's open dialog (IFileOpenDialog; FOS_PICKFOLDERS for a folder), owned
// by the game window, on a thread of its own so the present thread that draws the menu keeps running, and the shell's
// Downloads folder.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <string>

extern "C" {
#include "folder_pick.h"
#include "host.h"
#include "input_win.h"
#include "platform/platform.h"
}

struct PickReq {
    std::string start;
    std::string ext;   // a file of this extension; empty: a folder
    FolderPickDone done;
    void *ctx;
};

static std::wstring wide(const std::string &utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    w.resize(wcslen(w.c_str()));
    return w;
}

static std::string utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    std::string s(n > 0 ? n : 1, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    s.resize(strlen(s.c_str()));
    return s;
}

// The dialog wants an absolute folder that exists; anything else leaves it at the shell's last folder.
static void set_start(IFileOpenDialog *d, const std::string &start) {
    wchar_t full[MAX_PATH * 2];
    if (start.empty() || !GetFullPathNameW(wide(start).c_str(), MAX_PATH * 2, full, NULL)) return;
    IShellItem *item = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(full, NULL, IID_PPV_ARGS(&item)))) {
        d->SetFolder(item);
        item->Release();
    }
}

static void pick_main(void *arg) {
    PickReq *r = (PickReq *)arg;
    std::string chosen;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog *d = NULL;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d));
    if (SUCCEEDED(hr)) {
        FILEOPENDIALOGOPTIONS opt = 0;
        d->GetOptions(&opt);
        if (r->ext.empty()) {
            d->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        } else {
            d->SetOptions(opt | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
            std::wstring pattern = L"*." + wide(r->ext);   // the pattern is its own name: no text to translate
            COMDLG_FILTERSPEC types[] = {{pattern.c_str(), pattern.c_str()}, {L"*.*", L"*.*"}};
            d->SetFileTypes(2, types);
        }
        set_start(d, r->start);
        hr = d->Show(gfx_window_handle());
        IShellItem *item = NULL;
        if (SUCCEEDED(hr) && SUCCEEDED(hr = d->GetResult(&item))) {
            PWSTR path = NULL;
            if (SUCCEEDED(hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                chosen = utf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
        d->Release();
    }
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        rt_log("folder_pick: the %s dialog failed (0x%08lx)", r->ext.empty() ? "folder" : "file", (unsigned long)hr);
    r->done(chosen.empty() ? NULL : chosen.c_str(), r->ctx);
    if (SUCCEEDED(co)) CoUninitialize();
    delete r;
}

static void pick(const char *start, const char *ext, FolderPickDone done, void *ctx) {
    PickReq *r = new PickReq{start ? start : "", ext ? ext : "", done, ctx};
    if (!plat_thread_spawn(pick_main, r, "folder dialog", 0)) {
        rt_log("folder_pick: cannot start the dialog thread");
        done(NULL, ctx);
        delete r;
    }
}

extern "C" void folder_pick(const char *start, FolderPickDone done, void *ctx) { pick(start, NULL, done, ctx); }

extern "C" void file_pick(const char *start, const char *ext, FolderPickDone done, void *ctx) {
    pick(start, ext && *ext ? ext : "*", done, ctx);
}

extern "C" int folder_downloads(char *out, size_t cap) {
    // FOLDERID_Downloads, spelled out so no import library has to carry it
    static const GUID downloads = {0x374de290, 0x123f, 0x4565, {0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b}};
    PWSTR path = NULL;
    int ok = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(downloads, 0, NULL, &path)) && path) {
        std::string s = utf8(path);
        ok = !s.empty() && s.size() < cap;
        if (ok) memcpy(out, s.c_str(), s.size() + 1);
    }
    CoTaskMemFree(path);
    return ok;
}
