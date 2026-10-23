/* pbnewdialoghook.c - PowerBuilder 2025 R2 IDE "File > New" dialog extension
 *
 * Copyright (c) 2026 Bruce Armstrong
 * Licensed under the MIT License. See the LICENSE file in this directory for the
 * full text (https://opensource.org/licenses/MIT).
 *
 * A thread-level hook maps this DLL into the PB250 IDE process. When the
 * "File > New" dialog opens, we find its object-type list (a standard
 * SysListView32, dialog control id 1031) and its category tab control
 * (SysTabControl32, id 400), subclass the dialog window, and append our own
 * entries - whose labels, icons and actions come from pbnewdialoghook.ini next to this
 * DLL - to the list on a chosen category tab (default "PB Object"). Each entry
 * gets its icon added to the list's image list. When the user activates one of
 * our entries (double-click or OK) we run its configured action and close the
 * dialog, so the IDE never tries to create a real object from it.
 *
 * Why a dialog subclass (not just a message hook): the IDE identifies the
 * selected object type by the list item's lParam, which it dereferences as an
 * internal pointer. Our items carry a tagged lParam (0x6E48xxxx) that encodes
 * the config index instead, so we MUST intercept activation of our items before
 * the IDE's own dialog proc reads the selection. Switching category tabs makes
 * the IDE repopulate the list (wiping our items), so we re-append after every
 * TCN_SELCHANGE.
 *
 * Config (pbnewdialoghook.ini, re-read on every injection so edits apply with no
 * rebuild):
 *   [pbnewdialoghook]
 *   Items=2
 *   Tab=PB Object                 ; default category tab for items without Tab=
 *   [Item1]
 *   Label=My template
 *   Icon=C:\Tools\mine.ico        ; icon shown in the list (optional)
 *   Command=C:\Tools\mytool.exe   ; an exe/bat/... or a built-in (:clipboard, :msgbox)
 *   Args="%t"                     ; placeholders expanded at launch
 *   Dir=C:\Tools                  ; optional working directory
 *   Tab=PB Object                 ; optional per-item category tab override
 * Placeholders in Label/Args: %t current target (as shown in the Target: combo),
 *   %T target name only (without the " (path)" suffix), %n newline, %% percent.
 *
 * This is the user's own IDE on their own machine; everything here uses
 * documented Win32 APIs.
 *
 * Build (MinGW-w64, 32-bit, from the i686 shell):
 *   i686-w64-mingw32-gcc -O2 -shared -o pbnewdialoghook.dll pbnewdialoghook.c -lcomctl32 -luser32 -lgdi32 -Wl,--kill-at
 */

#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <windowsx.h>       /* GET_X_LPARAM / GET_Y_LPARAM */
#include <commctrl.h>
#include <wchar.h>
#include <stdarg.h>

#define MAX_ITEMS        16
#define IDC_LIST         1031          /* object-type list (SysListView32)     */
#define IDC_TAB          400           /* category tabs (SysTabControl32)      */
#define LPTAG_BASE       0x6E480000    /* 'nH'<<16; marks + indexes our items  */
#define LPTAG_MASK       0xFFFF0000
#define WM_PBNH_INJECT   (WM_APP + 0x4E48)   /* self-posted: do initial inject */

/* ---- diagnostic log, switched on at runtime from pbnewdialoghook.ini ---------- */
static BOOL      g_logOn = FALSE;
static wchar_t   g_logPath[MAX_PATH] = L"";

static void Log(const char *fmt, ...)
{
    if (!g_logOn || !g_logPath[0]) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt); wvsprintfA(buf, fmt, ap); va_end(ap);
    HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[64];
    wsprintfA(ts, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    DWORD n;
    WriteFile(f, ts, lstrlenA(ts), &n, NULL);
    WriteFile(f, buf, lstrlenA(buf), &n, NULL);
    WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}

static HINSTANCE g_hinst   = NULL;
static HHOOK     g_hCwp    = NULL;   /* WH_CALLWNDPROC: detect + subclass dialog */
static HHOOK     g_hGetMsg = NULL;   /* WH_GETMESSAGE:  swallow dbl-click / Enter */

/* One configured entry. */
typedef struct {
    wchar_t label[300];
    wchar_t icon[MAX_PATH];
    wchar_t command[MAX_PATH];
    wchar_t args[600];
    wchar_t dir[MAX_PATH];
    wchar_t tab[64];                 /* category tab this entry belongs on     */
    int     iImage;                  /* cached image-list index, -1 = none yet  */
} CFGITEM;

static CFGITEM   g_items[MAX_ITEMS];
static int       g_nitems = 0;
static wchar_t   g_defTab[64] = L"PB Object";

/* Subclass bookkeeping. We subclass ONLY the dialog window (for the OK button
 * and tab repopulation) - subclassing the IDE's own list view destabilises it
 * as the dialog populates. Double-click / Enter activation is intercepted via a
 * WH_GETMESSAGE hook instead (posted messages, seen before the list-view proc),
 * so we only remember the list's hwnd here, never replace its proc. */
static HWND      g_dlg          = NULL;
static WNDPROC   g_prevDlgProc  = NULL;
static HWND      g_list         = NULL;   /* the New dialog's object list       */
static HIMAGELIST g_imlCached   = NULL;   /* image list our icons were added to */

/* ---- config ------------------------------------------------------------- */

static void ConfigPath(wchar_t *out, int cch)
{
    GetModuleFileNameW(g_hinst, out, cch);
    wchar_t *s = wcsrchr(out, L'\\');
    if (s) wcscpy(s + 1, L"pbnewdialoghook.ini"); else wcscpy(out, L"pbnewdialoghook.ini");
}

static void LoadConfig(void)
{
    g_nitems = 0;
    wchar_t ini[MAX_PATH];
    ConfigPath(ini, MAX_PATH);

    g_logOn = GetPrivateProfileIntW(L"pbnewdialoghook", L"Log", 0, ini) != 0;
    GetPrivateProfileStringW(L"pbnewdialoghook", L"LogFile", L"", g_logPath, MAX_PATH, ini);
    if (g_logOn && !g_logPath[0]) {
        GetModuleFileNameW(g_hinst, g_logPath, MAX_PATH);
        wchar_t *s = wcsrchr(g_logPath, L'\\');
        if (s) wcscpy(s + 1, L"pbnewdialoghook.log"); else wcscpy(g_logPath, L"pbnewdialoghook.log");
    }

    GetPrivateProfileStringW(L"pbnewdialoghook", L"Tab", L"PB Object", g_defTab, 64, ini);

    int count = GetPrivateProfileIntW(L"pbnewdialoghook", L"Items", 0, ini);
    if (count > MAX_ITEMS) count = MAX_ITEMS;
    for (int i = 1; i <= count; i++) {
        wchar_t sec[32];
        wsprintfW(sec, L"Item%d", i);
        CFGITEM *it = &g_items[g_nitems];
        ZeroMemory(it, sizeof *it);
        it->iImage = -1;
        GetPrivateProfileStringW(sec, L"Label", L"", it->label, 300, ini);
        if (!it->label[0]) continue;
        GetPrivateProfileStringW(sec, L"Icon",    L"",       it->icon,    MAX_PATH, ini);
        GetPrivateProfileStringW(sec, L"Command", L"",       it->command, MAX_PATH, ini);
        GetPrivateProfileStringW(sec, L"Args",    L"",       it->args,    600,      ini);
        GetPrivateProfileStringW(sec, L"Dir",     L"",       it->dir,     MAX_PATH, ini);
        GetPrivateProfileStringW(sec, L"Tab",     g_defTab,  it->tab,     64,       ini);
        g_nitems++;
    }
    Log("LoadConfig: %d item(s), default tab \"%S\"", g_nitems, g_defTab);
}

/* ---- placeholder expansion (%t target, %n newline, %% percent) ---------- */

/* Cached target name. Filled while the New dialog is alive; actions run from a
 * timer AFTER the dialog closes, when the "Target:" combo can no longer be read. */
static wchar_t g_target[256] = L"";

static void RefreshTarget(void)
{
    if (g_dlg && IsWindow(g_dlg)) {
        wchar_t t[256] = L"";
        GetDlgItemTextW(g_dlg, 1045, t, 256);   /* "Target:" combo */
        if (t[0]) lstrcpynW(g_target, t, 256);
    }
}

static void Expand(wchar_t *out, int cch, const wchar_t *src)
{
    RefreshTarget();
    int o = 0;
    for (int i = 0; src[i] && o < cch - 1; i++) {
        if (src[i] != L'%') { out[o++] = src[i]; continue; }
        wchar_t c = src[i + 1];
        if (c == L't') { for (int k = 0; g_target[k] && o < cch - 1; k++) out[o++] = g_target[k]; i++; }
        else if (c == L'T') {   /* target NAME only: g_target up to " (" */
            for (int k = 0; g_target[k] && o < cch - 1; k++) {
                if (g_target[k] == L' ' && g_target[k + 1] == L'(') break;
                out[o++] = g_target[k];
            }
            i++;
        }
        else if (c == L'n') { out[o++] = L'\r'; if (o < cch - 1) out[o++] = L'\n'; i++; }
        else if (c == L'%') { out[o++] = L'%'; i++; }
        else out[o++] = src[i];
    }
    out[o] = 0;
}

/* ---- actions ------------------------------------------------------------ */

static void CopyToClipboard(const wchar_t *s)
{
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t bytes = (wcslen(s) + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        void *p = GlobalLock(g);
        if (p) { memcpy(p, s, bytes); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
        else GlobalFree(g);
    }
    CloseClipboard();
}

static void RunItem(const CFGITEM *it)
{
    wchar_t args[1200];
    Expand(args, 1200, it->args);
    Log("RunItem: command=\"%S\" args=\"%S\"", it->command, args);

    if (it->command[0] == L':') {
        if (!lstrcmpiW(it->command, L":clipboard"))    CopyToClipboard(args);
        else if (!lstrcmpiW(it->command, L":msgbox"))  MessageBoxW(NULL, args, L"pbnewdialoghook", MB_OK | MB_TOPMOST);
        else Log("RunItem: unknown built-in \"%S\"", it->command);
        return;
    }
    if (it->command[0]) {
        HINSTANCE r = ShellExecuteW(NULL, L"open", it->command, args[0] ? args : NULL,
                                    it->dir[0] ? it->dir : NULL, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) Log("RunItem: ShellExecute failed (code %d)", (int)(INT_PTR)r);
    }
}

/* ---- list-view injection ------------------------------------------------ */

static void TabLabel(HWND tab, int idx, wchar_t *out, int cch)
{
    out[0] = 0;
    TCITEMW ti; ZeroMemory(&ti, sizeof ti);
    ti.mask = TCIF_TEXT; ti.pszText = out; ti.cchTextMax = cch;
    SendMessageW(tab, TCM_GETITEMW, idx, (LPARAM)&ti);
}

static void CurrentTabLabel(HWND dlg, wchar_t *out, int cch)
{
    out[0] = 0;
    HWND tab = GetDlgItem(dlg, IDC_TAB);
    if (!tab) return;
    int cur = (int)SendMessageW(tab, TCM_GETCURSEL, 0, 0);
    if (cur >= 0) TabLabel(tab, cur, out, cch);
}

/* Add (once per image list) an entry's icon; return its image index or -1. */
static int IconIndex(HWND list, CFGITEM *it)
{
    HIMAGELIST himl = (HIMAGELIST)SendMessageW(list, LVM_GETIMAGELIST, LVSIL_NORMAL, 0);
    if (!himl) return -1;
    if (himl != g_imlCached) {                 /* list rebuilt: drop cached indices */
        g_imlCached = himl;
        for (int i = 0; i < g_nitems; i++) g_items[i].iImage = -1;
    }
    if (it->iImage >= 0) return it->iImage;     /* already added to this image list */
    if (!it->icon[0]) return 0;                 /* no icon configured: borrow slot 0 */

    int cx = 32, cy = 32;
    ImageList_GetIconSize(himl, &cx, &cy);
    HICON hIcon = (HICON)LoadImageW(NULL, it->icon, IMAGE_ICON, cx, cy, LR_LOADFROMFILE);
    if (!hIcon) { Log("IconIndex: LoadImage failed for \"%S\"", it->icon); return 0; }
    int idx = ImageList_ReplaceIcon(himl, -1, hIcon);
    DestroyIcon(hIcon);
    it->iImage = (idx >= 0) ? idx : 0;
    Log("IconIndex: \"%S\" -> image %d (himl=%08x)", it->icon, it->iImage, (unsigned)(UINT_PTR)himl);
    return it->iImage;
}

static void RemoveOurItems(HWND list)
{
    int n = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
    for (int i = n - 1; i >= 0; i--) {
        LVITEMW lv; ZeroMemory(&lv, sizeof lv);
        lv.mask = LVIF_PARAM; lv.iItem = i;
        if (SendMessageW(list, LVM_GETITEMW, 0, (LPARAM)&lv) &&
            ((DWORD)lv.lParam & LPTAG_MASK) == LPTAG_BASE)
            SendMessageW(list, LVM_DELETEITEM, i, 0);
    }
}

static void InjectForCurrentTab(HWND dlg)
{
    HWND list = GetDlgItem(dlg, IDC_LIST);
    if (!list) return;
    g_list = list;              /* remember it for the WH_GETMESSAGE interception */

    LoadConfig();
    RemoveOurItems(list);

    wchar_t cur[64];
    CurrentTabLabel(dlg, cur, 64);
    Log("Inject: current tab \"%S\"", cur);

    int base = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
    int added = 0;
    for (int i = 0; i < g_nitems; i++) {
        if (lstrcmpiW(g_items[i].tab, cur) != 0) continue;
        wchar_t label[400];
        Expand(label, 400, g_items[i].label);

        LVITEMW lv; ZeroMemory(&lv, sizeof lv);
        lv.mask     = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lv.iItem    = base + added;
        lv.pszText  = label;
        lv.iImage   = IconIndex(list, &g_items[i]);
        lv.lParam   = (LPARAM)(LPTAG_BASE | (i & 0xFFFF));
        SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&lv);
        added++;
    }
    Log("Inject: added %d item(s) on tab \"%S\"", added, cur);
}

/* Return config index if list item `item` is ours, else -1. */
static int ItemCfgIndex(HWND list, int item)
{
    if (item < 0) return -1;
    LVITEMW lv; ZeroMemory(&lv, sizeof lv);
    lv.mask = LVIF_PARAM; lv.iItem = item;
    if (!SendMessageW(list, LVM_GETITEMW, 0, (LPARAM)&lv)) return -1;
    if (((DWORD)lv.lParam & LPTAG_MASK) != LPTAG_BASE) return -1;
    int idx = (int)((DWORD)lv.lParam & 0xFFFF);
    return (idx >= 0 && idx < g_nitems) ? idx : -1;
}

/* Return config index of the selected list item if it is ours, else -1. */
static int SelectedOurCfgIndex(HWND dlg)
{
    HWND list = GetDlgItem(dlg, IDC_LIST);
    if (!list) return -1;
    int sel = (int)SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    return ItemCfgIndex(list, sel);
}

/* The action is run AFTER the dialog closes, from a thread timer at the top of
 * the message loop - never inline here, because activation is detected inside a
 * message hook / notification while the modal New dialog is still up, and a
 * modal action (e.g. :msgbox) plus closing the dialog from that nested context
 * crashes the IDE. */
static CFGITEM g_pending;
static BOOL    g_havePending = FALSE;

static void CALLBACK RunPendingTimer(HWND h, UINT msg, UINT_PTR id, DWORD t)
{
    (void)h; (void)msg; (void)t;
    KillTimer(NULL, id);
    if (g_havePending) { g_havePending = FALSE; RunItem(&g_pending); }
}

static void FireAndClose(HWND dlg, int cfgIndex)
{
    Log("Fire: item %d \"%S\" (deferred)", cfgIndex, g_items[cfgIndex].label);
    RefreshTarget();                    /* capture %t now, before the dialog closes */
    g_pending = g_items[cfgIndex];      /* copy; run after the dialog closes */
    g_havePending = TRUE;
    /* Strip our items BEFORE closing: the IDE walks the list on close to free
     * each item's descriptor, and would dereference our tagged lParam. */
    if (g_list && IsWindow(g_list)) RemoveOurItems(g_list);
    /* Close the New dialog the IDE's own way (no object created), then let the
     * timer run the action once we are back at the top-level message loop. */
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED),
                 (LPARAM)GetDlgItem(dlg, IDCANCEL));
    SetTimer(NULL, 0, 1, RunPendingTimer);
}

/* ---- dialog subclass (OK button + tab repopulation) --------------------- */

static LRESULT CALLBACK DlgProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_PBNH_INJECT:
        InjectForCurrentTab(h);
        return 0;

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)l;
        if (nh->idFrom == IDC_TAB && nh->code == TCN_SELCHANGE) {
            LRESULT r = CallWindowProcW(g_prevDlgProc, h, msg, w, l);  /* IDE repopulates */
            InjectForCurrentTab(h);
            return r;
        }
        if (nh->idFrom == IDC_LIST) {
            /* Activation of our item: run the action, keep the IDE out. */
            if (nh->code == NM_DBLCLK || (int)nh->code == LVN_ITEMACTIVATE || nh->code == NM_RETURN) {
                int idx = SelectedOurCfgIndex(h);
                if (idx >= 0) { FireAndClose(h, idx); return 0; }
            }
            /* THE KEY FIX: the IDE dereferences the selected item's lParam on a
             * selection change. Our items carry a tagged (non-pointer) lParam, so
             * swallow these notifications for our items before the IDE sees them.
             * The lParam is right in the notification struct - no re-entrant read.
             * The list view still updates its own visible selection. */
            if ((int)nh->code == LVN_ITEMCHANGED || (int)nh->code == LVN_ITEMCHANGING) {
                NMLISTVIEW *nlv = (NMLISTVIEW *)l;
                if (((DWORD)nlv->lParam & LPTAG_MASK) == LPTAG_BASE) {
                    /* Let OK still work for our entry even though the IDE never
                     * processed the selection. */
                    if ((int)nh->code == LVN_ITEMCHANGED && (nlv->uNewState & LVIS_SELECTED)) {
                        EnableWindow(GetDlgItem(h, IDOK), TRUE);
                        Log("Block select deref for our item (idx=%d)", (int)((DWORD)nlv->lParam & 0xFFFF));
                    }
                    return 0;
                }
            }
            if (nh->code == NM_CLICK || nh->code == NM_RCLICK) {
                NMITEMACTIVATE *nia = (NMITEMACTIVATE *)l;
                if (ItemCfgIndex((HWND)nh->hwndFrom, nia->iItem) >= 0) return 0;
            }
            if ((int)nh->code == LVN_DELETEITEM) {
                NMLISTVIEW *nlv = (NMLISTVIEW *)l;
                if (((DWORD)nlv->lParam & LPTAG_MASK) == LPTAG_BASE) return 0;
            }
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            int idx = SelectedOurCfgIndex(h);
            if (idx >= 0) { FireAndClose(h, idx); return 0; }   /* don't let IDE act */
        }
        /* Any close (OK on a normal item, Cancel): strip our items first so the
         * IDE's close cleanup never dereferences our tagged lParam. */
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)
            if (g_list && IsWindow(g_list)) RemoveOurItems(g_list);
        break;

    case WM_CLOSE:
    case WM_DESTROY:
        if (g_list && IsWindow(g_list)) RemoveOurItems(g_list);
        break;

    case WM_NCDESTROY: {
        WNDPROC prev = g_prevDlgProc;
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)prev);
        if (g_dlg == h) { g_dlg = NULL; g_prevDlgProc = NULL; g_imlCached = NULL; g_list = NULL; }
        return CallWindowProcW(prev, h, msg, w, l);
    }
    }
    return CallWindowProcW(g_prevDlgProc, h, msg, w, l);
}

/* A #32770 that owns both our list (1031) and tab (400) is the New dialog. */
static BOOL IsNewDialog(HWND h)
{
    wchar_t cls[32];
    if (!GetClassNameW(h, cls, 32) || lstrcmpW(cls, L"#32770") != 0) return FALSE;
    HWND list = GetDlgItem(h, IDC_LIST);
    HWND tab  = GetDlgItem(h, IDC_TAB);
    if (!list || !tab) return FALSE;
    GetClassNameW(list, cls, 32); if (lstrcmpW(cls, L"SysListView32")   != 0) return FALSE;
    GetClassNameW(tab,  cls, 32); if (lstrcmpW(cls, L"SysTabControl32") != 0) return FALSE;
    return TRUE;
}

static void EnsureSubclass(HWND dlg)
{
    g_list = GetDlgItem(dlg, IDC_LIST);     /* for WH_GETMESSAGE interception */
    if ((WNDPROC)GetWindowLongPtrW(dlg, GWLP_WNDPROC) == DlgProc) return;  /* already ours */
    g_prevDlgProc = (WNDPROC)SetWindowLongPtrW(dlg, GWLP_WNDPROC, (LONG_PTR)DlgProc);
    g_dlg         = dlg;
    g_imlCached   = NULL;
    Log("Subclassed New dialog %08x (list %08x)", (unsigned)(UINT_PTR)dlg, (unsigned)(UINT_PTR)g_list);
}

/* ---- hook procedure (runs in the PB GUI thread) ------------------------- */

LRESULT CALLBACK CallWndProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        CWPSTRUCT *p = (CWPSTRUCT *)lParam;
        /* Catch the dialog as it initialises; also re-arm if it is re-activated
         * (covers the loader starting while a New dialog is already open). */
        if (p->message == WM_INITDIALOG ||
            (p->message == WM_ACTIVATE && LOWORD(p->wParam) != WA_INACTIVE)) {
            if (IsNewDialog(p->hwnd)) {
                EnsureSubclass(p->hwnd);
                PostMessageW(p->hwnd, WM_PBNH_INJECT, 0, 0);
            }
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

/* Swallow activation of OUR list items before the IDE's list-view proc runs.
 * Double-click and key presses are POSTED, so WH_GETMESSAGE sees them first; we
 * turn ours into WM_NULL and run the action instead. This avoids replacing the
 * IDE list view's own window proc (which crashes it as the dialog populates). */
LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == PM_REMOVE) {
        MSG *m = (MSG *)lParam;
        if (g_list && IsWindow(g_list) && m->hwnd == g_list) {
            if (m->message == WM_LBUTTONDBLCLK) {
                LVHITTESTINFO ht; ZeroMemory(&ht, sizeof ht);
                ht.pt.x = GET_X_LPARAM(m->lParam);
                ht.pt.y = GET_Y_LPARAM(m->lParam);
                int item = (int)SendMessageW(g_list, LVM_HITTEST, 0, (LPARAM)&ht);
                int idx = ItemCfgIndex(g_list, item);
                Log("GetMsg DBLCLK item=%d ours=%d", item, idx);
                if (idx >= 0) { m->message = WM_NULL; FireAndClose(g_dlg, idx); }
            }
            else if ((m->message == WM_KEYDOWN || m->message == WM_CHAR) && m->wParam == VK_RETURN) {
                int idx = SelectedOurCfgIndex(g_dlg);
                if (idx >= 0) { m->message = WM_NULL; Log("GetMsg Enter ours=%d", idx); FireAndClose(g_dlg, idx); }
            }
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

/* ---- exports called by the loader --------------------------------------- */

__declspec(dllexport) BOOL InstallHook(DWORD threadId)
{
    g_hCwp    = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, g_hinst, threadId);
    g_hGetMsg = SetWindowsHookExW(WH_GETMESSAGE,  GetMsgProc,  g_hinst, threadId);
    return g_hCwp && g_hGetMsg;
}

__declspec(dllexport) void RemoveHook(void)
{
    if (g_hCwp)    UnhookWindowsHookEx(g_hCwp);
    if (g_hGetMsg) UnhookWindowsHookEx(g_hGetMsg);
    g_hCwp = g_hGetMsg = NULL;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_dlg && IsWindow(g_dlg) &&
            (WNDPROC)GetWindowLongPtrW(g_dlg, GWLP_WNDPROC) == DlgProc)
            SetWindowLongPtrW(g_dlg, GWLP_WNDPROC, (LONG_PTR)g_prevDlgProc);
    }
    return TRUE;
}
