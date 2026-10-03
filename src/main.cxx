// Intranet Safe Code Editor - Win7/Win10 x86, zero runtime deps.
// Byte-preserving editor: what you open is exactly what you save.
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

#include "Scintilla.h"
#include "ILexer.h"
#include "Lexilla.h"
#include "SciLexer.h"

extern "C" int Scintilla_RegisterClasses(void *hInstance);

// ---------------------------------------------------------------------------
// Globals

static HINSTANCE g_hInst = nullptr;
static HWND g_hwnd = nullptr, g_sci = nullptr, g_status = nullptr;
static HWND g_hwndFind = nullptr;
static std::wstring g_filePath;
static std::string g_findText;
static bool g_findMatchCase = false;
static UINT g_findReplMsg = 0;

// Format snapshot of the currently open file (for byte-exact save).
enum LoadMode { MODE_ANSI = 0, MODE_UTF8, MODE_UTF16LE, MODE_UTF16BE };
static LoadMode g_mode = MODE_ANSI;
static bool g_hasBom = false;
static char g_bom[3] = { 0, 0, 0 };
static int g_bomLen = 0;
static int g_eolMode = SC_EOL_CRLF;
// --- V5 globals (used by LoadFile and friends defined earlier in the file) ---
static FILETIME g_fileTime;
static bool g_fileTimeValid = false;
static bool g_viewEol = true;
static bool g_readOnly = false;
static int g_zoom = 0;
static std::wstring g_mru[8];
static int g_mruCount = 0;
static bool g_mruSuppress = false;   // selftest must not touch the real MRU
static HMENU g_mRecentMenu = nullptr;   // recent-files submenu (V5)
static void AddToMru(const std::wstring &path);
static void SaveMru();
static void LoadMru();
static void RefreshFileTime(const std::wstring &path);
static void ReportFormatStatus(const std::wstring &path);
static void SetStatusPart2(const wchar_t *s);
static bool ValidateFormat(std::wstring &err, int &line);
static void ApplyZoom(int delta);

// --- scripted message box (--uitest) ---
// Normal mode: straight to MessageBoxW. In uitest mode: log the call to
// C:\cedb\uitest-result.txt and answer from a scripted queue so modal dialogs
// never block automated runs.
static bool g_uiTest = false;
static int  g_uiAnswer = 0;          // 0 = default (OK/YES), else IDOK/IDCANCEL/...
static FILE *g_uiTestLog = nullptr;

static int AppMsgBox(HWND h, LPCWSTR text, LPCWSTR caption, UINT type) {
    if (!g_uiTest) return MessageBoxW(h, text, caption, type);
    if (g_uiTestLog) {
        fwprintf(g_uiTestLog, L"[mbox] type=0x%X text=%ls\n", type, text);
        fflush(g_uiTestLog);
    }
    if (g_uiAnswer) return g_uiAnswer;
    return (type & MB_YESNO) ? IDYES : IDOK;
}

static const wchar_t *kClassName = L"IntranetEditorWnd";
static const wchar_t *kAppTitle = L"\u4ee3\u7801\u7f16\u8f91\u5668";           // 代码编辑器
static const int kMaxFileBytes = 256 * 1024 * 1024;

// Menu / command ids
#define IDM_OPEN      1001
#define IDM_SAVE      1002
#define IDM_SAVEAS    1003
#define IDM_EXIT      1004
#define IDM_UNDO      1005
#define IDM_REDO      1006
#define IDM_CUT       1007
#define IDM_COPY      1008
#define IDM_PASTE     1009
#define IDM_SELECTALL 1010
#define IDM_FIND      1011
#define IDM_FINDNEXT  1012
#define IDM_ABOUT     1013
#define IDM_RELOAD_UTF8  1016
#define IDM_RELOAD_GBK   1017
#define IDM_RELOAD_UTF16 1018
#define IDM_ZOOM_IN    1019
#define IDM_ZOOM_OUT   1020
#define IDM_ZOOM_RESET 1021
#define IDM_VIEW_EOL   1022
#define IDM_MRU_BASE   1100

#define WM_APP_TITLE   (WM_APP + 1)
#define WM_APP_STATUS  (WM_APP + 2)

// ---------------------------------------------------------------------------
// Helpers

template <class T> static T Scim(int msg, uptr_t w = 0, sptr_t l = 0) {
    return (T)::SendMessageW(g_sci, msg, w, l);
}

static std::wstring ErrText(DWORD err) {
    wchar_t *buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&buf, 0, nullptr);
    std::wstring s = n ? buf : L"\u672a\u77e5\u9519\u8bef";
    if (buf) LocalFree(buf);
    return s;
}

static void ShowError(const wchar_t *what, const std::wstring &path) {
    DWORD e = GetLastError();          // capture once: paths can exceed 512 wchars
    std::wstring msg = std::wstring(what) + L"\n" + path +
        L"\n\n\u9519\u8bef\u4ee3\u7801 " + std::to_wstring(e) + L": " + ErrText(e);
    AppMsgBox(g_hwnd, msg.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
}

static std::wstring BaseName(const std::wstring &path) {
    std::wstring::size_type p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

static std::wstring ExtOf(const std::wstring &path) {
    std::wstring base = BaseName(path);
    std::wstring::size_type p = base.find_last_of(L'.');
    if (p == std::wstring::npos || p + 1 >= base.size()) return L"";
    std::wstring ext = base.substr(p + 1);
    for (size_t i = 0; i < ext.size(); ++i) ext[i] = (wchar_t)towlower(ext[i]);
    return ext;
}

// ---------------------------------------------------------------------------
// Extension -> lexer mapping (Lexilla 5.5.4 entries verified in Lexilla.cxx)

static const char *LexerForExt(const std::wstring &ext) {
    struct Entry { const wchar_t *ext; const char *lexer; };
    static const Entry table[] = {
        { L"ini", "props" }, { L"cfg", "props" }, { L"properties", "props" },
        { L"env", "props" },
        { L"conf", "conf" },
        { L"json", "json" },
        { L"xml", "xml" },
        { L"yaml", "yaml" }, { L"yml", "yaml" },
        { L"sql", "sql" },
        { L"bat", "batch" }, { L"cmd", "batch" },
        { L"html", "html" }, { L"htm", "html" },
        { L"js", "cpp" }, { L"mjs", "cpp" }, { L"c", "cpp" },
        { L"cpp", "cpp" }, { L"h", "cpp" }, { L"hpp", "cpp" },
        { L"cc", "cpp" }, { L"cxx", "cpp" }, { L"java", "cpp" }, { L"cs", "cpp" },
        { L"css", "css" },
        { L"py", "python" },
        { L"sh", "bash" },
        { nullptr, nullptr },
    };
    if (ext.empty()) return nullptr;
    for (const Entry *e = table; e->ext; ++e) {
        if (ext == e->ext) return e->lexer;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Line-ending counting / UTF-8 validation

struct EolCounts { int crlf, lf, cr; };
static EolCounts CountEols(const char *p, size_t n) {
    EolCounts c = { 0, 0, 0 };
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == '\r') {
            if (i + 1 < n && p[i + 1] == '\n') { ++c.crlf; ++i; }
            else ++c.cr;
        } else if (p[i] == '\n') ++c.lf;
    }
    return c;
}

static int MajorityEol(const EolCounts &c) {
    if (c.cr > c.lf && c.cr > c.crlf) return SC_EOL_CR;
    if (c.lf > c.crlf) return SC_EOL_LF;
    return SC_EOL_CRLF;   // zero counts (single-line file) -> Windows convention
}

static bool LooksUtf8(const char *p, size_t n, bool &hasHigh) {
    hasHigh = false;
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x80) { ++i; continue; }
        hasHigh = true;
        int extra;
        unsigned int cp;
        if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else return false;
        if (i + (size_t)extra >= n) return false;
        for (int k = 1; k <= extra; ++k) {
            unsigned char cc = (unsigned char)p[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += extra + 1;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Format detection & codec (shared by LoadFile, SaveFile, selftest)

static void DetectFormat(const std::vector<char> &buf, LoadMode &mode,
    bool &hasBom, char (&bom)[3]) {
    size_t n = buf.size();
    mode = MODE_ANSI; hasBom = false;
    bom[0] = bom[1] = bom[2] = 0;
    if (n >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        mode = MODE_UTF8; hasBom = true;
        bom[0] = (char)0xEF; bom[1] = (char)0xBB; bom[2] = (char)0xBF;
    } else if (n >= 2 && (unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE) {
        mode = MODE_UTF16LE; hasBom = true;
        bom[0] = (char)0xFF; bom[1] = (char)0xFE;
    } else if (n >= 2 && (unsigned char)buf[0] == 0xFE && (unsigned char)buf[1] == 0xFF) {
        mode = MODE_UTF16BE; hasBom = true;
        bom[0] = (char)0xFE; bom[1] = (char)0xFF;
    } else {
        bool hasHigh = false;
        mode = (LooksUtf8(buf.data(), n, hasHigh) && hasHigh) ? MODE_UTF8 : MODE_ANSI;
    }
}

static int BomLenOf(LoadMode mode, bool hasBom) {
    if (!hasBom) return 0;
    return mode == MODE_UTF8 ? 3 : 2;
}

// Decode file bytes into the editor's internal UTF-8/raw text.
static bool DecodeToText(const std::vector<char> &buf, LoadMode mode, int bomLen,
    std::string &text) {
    size_t n = buf.size();
    if (mode == MODE_UTF16LE || mode == MODE_UTF16BE) {
        const char *p = buf.data() + bomLen;
        size_t len = n - bomLen;
        if (len % 2 != 0) { text.assign(buf.data(), n); return false; }
        int wchars = (int)(len / 2);
        if (wchars == 0) { text.clear(); return true; }
        std::vector<wchar_t> tmp((size_t)wchars + 1, 0);
        if (mode == MODE_UTF16BE) {
            for (int i = 0; i < wchars; ++i) {
                unsigned char lo = (unsigned char)p[2 * i];
                unsigned char hi = (unsigned char)p[2 * i + 1];
                tmp[(size_t)i] = (wchar_t)((((wchar_t)lo) << 8) | hi);
            }
        } else {
            memcpy(&tmp[0], p, len);
        }
        tmp[(size_t)wchars] = 0;
        int outLen = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            &tmp[0], wchars, nullptr, 0, nullptr, nullptr);
        if (outLen <= 0) { text.assign(buf.data(), n); return false; }
        text.assign((size_t)outLen, 0);
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            &tmp[0], wchars, &text[0], outLen, nullptr, nullptr);
        return true;
    }
    if (mode == MODE_UTF8) text.assign(buf.data() + bomLen, n - bomLen);
    else text.assign(buf.data(), n);
    return true;
}

// Encode editor text back to the original file encoding.
static bool EncodeText(const std::string &text, LoadMode mode,
    const char bom[3], int bomLen, std::vector<char> &out) {
    if (mode == MODE_UTF16LE || mode == MODE_UTF16BE) {
        int clen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), (int)text.size(), nullptr, 0);
        if (clen == 0) return false;
        std::vector<wchar_t> tmp((size_t)clen + 1, 0);
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), (int)text.size(), &tmp[0], clen) == 0) return false;
        tmp[(size_t)clen] = 0;
        out.clear();
        out.insert(out.end(), bom, bom + bomLen);
        for (int i = 0; i < clen; ++i) {
            wchar_t w = tmp[(size_t)i];
            if (mode == MODE_UTF16BE) {
                out.push_back((char)((w >> 8) & 0xFF));
                out.push_back((char)(w & 0xFF));
            } else {
                out.push_back((char)(w & 0xFF));
                out.push_back((char)((w >> 8) & 0xFF));
            }
        }
        return true;
    }
    out.clear();
    if (mode == MODE_UTF8 && bomLen > 0) out.insert(out.end(), bom, bom + bomLen);
    out.insert(out.end(), text.data(), text.data() + text.size());
    return true;
}

// ---------------------------------------------------------------------------
// Read whole file to bytes (shared).

static bool ReadFileBytes(const std::wstring &path, std::vector<char> &buf) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return false; }
    if (size.QuadPart > kMaxFileBytes) {
        CloseHandle(h);
        SetLastError(ERROR_FILE_TOO_LARGE);
        return false;
    }
    size_t n = (size_t)size.QuadPart;
    buf.assign(n + 1, 0);
    DWORD got = 0;
    if (n && !ReadFile(h, &buf[0], (DWORD)n, &got, nullptr)) {
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    if ((size_t)got != n) return false;
    buf.resize(n);
    return true;
}

// ---------------------------------------------------------------------------
// Load file into the editor per the plan's algorithm.

// forceMode: -1 auto-detect; 0 force UTF-8; 1 force GBK/ANSI; 2 force UTF-16LE.
static bool LoadFile(const std::wstring &path, bool attachLexer = true,
    int forceMode = -1) {
    std::vector<char> buf;
    if (!ReadFileBytes(path, buf)) {
        if (GetLastError() == ERROR_FILE_TOO_LARGE)
            ShowError(L"\u6587\u4ef6\u8d85\u8fc7 256 MB\uff0c\u4e0d\u652f\u6301\u6253\u5f00\uff1a", path);
        else
            ShowError(L"\u65e0\u6cd5\u6253\u5f00\u6587\u4ef6\uff1a", path);
        return false;
    }

    LoadMode mode = MODE_ANSI;
    bool hasBom = false;
    char bom[3] = { 0, 0, 0 };
    DetectFormat(buf, mode, hasBom, bom);
    int bomLen = BomLenOf(mode, hasBom);

    if (forceMode >= 0) {
        // user-forced re-read (Edit > Reload as...): keep any detected BOM
        // skipped, but reinterpret the body with the chosen encoding.
        if (forceMode == 0) {
            hasBom = (mode == MODE_UTF8 && hasBom);
            mode = MODE_UTF8;
            bomLen = hasBom ? 3 : 0;
        } else if (forceMode == 1) {
            hasBom = false;
            mode = MODE_ANSI;
            bomLen = 0;   // GBK view: no BOM concept
        } else if (forceMode == 2) {
            hasBom = (mode == MODE_UTF16LE && hasBom);
            mode = MODE_UTF16LE;
            bomLen = hasBom ? 2 : 0;
        }
    }

    // The decode buffers below are the only file-sized allocations in this
    // function (a 256 MB file under a 32-bit 2 GB address space can OOM).
    std::string text;
    bool decoded;
    try {
        decoded = DecodeToText(buf, mode, bomLen, text);
        if (!decoded) {
            // UTF-16 decode failure (unpaired surrogate / odd length):
            // raw byte view still round-trips exactly.
            mode = MODE_ANSI; hasBom = false; bomLen = 0;
            text.assign(buf.begin(), buf.end());
        }
    } catch (const std::bad_alloc &) {
        AppMsgBox(g_hwnd,
            L"\u5185\u5b58\u4e0d\u8db3\uff0c\u65e0\u6cd5\u52a0\u8f7d\u6b64\u6587\u4ef6\u3002",
            kAppTitle, MB_OK | MB_ICONERROR);
        return false;
    }
    g_eolMode = MajorityEol(CountEols(text.data(), text.size()));

    UINT codepage = (mode == MODE_UTF8 || mode == MODE_UTF16LE
        || mode == MODE_UTF16BE) ? 65001 : 0;

    Scim<void>(SCI_SETCODEPAGE, (uptr_t)codepage);
    Scim<void>(SCI_SETPASTECONVERTENDINGS, 0, 0);   // never rewrite pasted EOLs
    // Layout cache PAGE: only visible pages cache layout (less memory on large
    // files than the document-wide default). Idle styling deliberately NOT
    // set: measured to only defer (not remove) styling work.
    Scim<void>(SCI_SETLAYOUTCACHE, (uptr_t)SC_CACHE_PAGE);            // page-only layout cache
    Scim<void>(SCI_SETEOLMODE, (uptr_t)g_eolMode);
    // The NUL-terminated style: Scintilla accepts C strings; embedded NULs are
    // handled by the SCI_SETTEXT variant below using explicit length.
    {
        Scim<void>(SCI_SETTEXT, 0, (sptr_t)text.c_str());
        // Ensure full length if text contains NULs: re-check document length.
        if ((size_t)Scim<int>(SCI_GETLENGTH, 0, 0) != text.size()) {
            // SCI_SETTEXT stopped at an embedded NUL. Use ALLOCATE free path:
            // document is not empty-safe; load with explicit length via AddText.
            Scim<void>(SCI_CLEARALL);
            std::vector<char> tmp;
            try {
                tmp.assign(text.size() + 1, 0);
                memcpy(&tmp[0], text.data(), text.size());
            } catch (const std::bad_alloc &) {
                AppMsgBox(g_hwnd,
                    L"\u5185\u5b58\u4e0d\u8db3\uff0c\u65e0\u6cd5\u52a0\u8f7d\u6b64\u6587\u4ef6\u3002",
                    kAppTitle, MB_OK | MB_ICONERROR);
                return false;
            }
            Scim<void>(SCI_ADDTEXT, (uptr_t)text.size(), (sptr_t)&tmp[0]);
        }
    }

    Scim<void>(SCI_STYLESETFONT, STYLE_DEFAULT,
        (sptr_t)(mode == MODE_ANSI ? "NSimSun" : "Consolas"));
    Scim<void>(SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
    Scim<void>(SCI_STYLESETCHARACTERSET, STYLE_DEFAULT,
        (sptr_t)(mode == MODE_ANSI ? SC_CHARSET_GB2312 : SC_CHARSET_ANSI));
    // dark theme defaults (STYLECLEARALL copies default into every style slot)
    Scim<void>(SCI_STYLESETBACK, STYLE_DEFAULT, 0x1E1E1E);  // #1E1E1E editor bg
    Scim<void>(SCI_STYLESETFORE, STYLE_DEFAULT, 0xD4D4D4);  // #D4D4D4 default fg
    Scim<void>(SCI_STYLECLEARALL);
    Scim<void>(SCI_SETTABWIDTH, 4);
    Scim<void>(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    Scim<int>(SCI_SETMARGINWIDTHN, 0,
        Scim<int>(SCI_TEXTWIDTH, STYLE_LINENUMBER, (sptr_t)"99999"));
    // line-number gutter: dark bg, dim gray numerals
    Scim<void>(SCI_SETMARGINBACKN, 0, 0x252526);
    Scim<void>(SCI_STYLESETBACK, STYLE_LINENUMBER, 0x252526);
    Scim<void>(SCI_STYLESETFORE, STYLE_LINENUMBER, 0x858585);

    // fold margin (margin 1) + markers are configured once in WM_CREATE
    // (static control settings, not file state).

    const char *lexer = nullptr;
    if (attachLexer) {
        lexer = LexerForExt(ExtOf(path));
        if (lexer) {
            ILexer5 *il = CreateLexer(lexer);
            if (il) {
                Scim<void>(SCI_SETILEXER, 0, (sptr_t)il);
                // dark-theme per-lexer token colours (VS Code Dark+ palette).
                // wParam = style number from SciLexer.h, lParam = COLORREF.
                #define TCOL(n, rgb) Scim<void>(SCI_STYLESETFORE, (uptr_t)(n), (sptr_t)(rgb))
                const int cComment = 0x6A9955, cKeyword = 0x569CD6,
                    cString = 0xCE9178, cNumber = 0xB5CEA8,
                    cAttr = 0x9CDCFE, cError = 0xF44747;
                if (!strcmp(lexer, "props")) {
                    TCOL(SCE_PROPS_COMMENT, cComment);
                    TCOL(SCE_PROPS_SECTION, cKeyword);
                    TCOL(SCE_PROPS_ASSIGNMENT, cAttr);
                    TCOL(SCE_PROPS_DEFVAL, cString);
                    TCOL(SCE_PROPS_KEY, cAttr);       Scim<void>(SCI_STYLESETBOLD, SCE_PROPS_KEY, 1);
                } else if (!strcmp(lexer, "conf")) {
                    TCOL(SCE_CONF_COMMENT, cComment);
                    TCOL(SCE_CONF_EXTENSION, cAttr);
                    TCOL(SCE_CONF_STRING, cString);
                    TCOL(SCE_CONF_PARAMETER, cAttr);   Scim<void>(SCI_STYLESETBOLD, SCE_CONF_PARAMETER, 1);
                    TCOL(SCE_CONF_DIRECTIVE, cKeyword);
                    TCOL(SCE_CONF_IP, cNumber);
                } else if (!strcmp(lexer, "json")) {
                    TCOL(SCE_JSON_NUMBER, cNumber);
                    TCOL(SCE_JSON_STRING, cString);
                    TCOL(SCE_JSON_PROPERTYNAME, cAttr); Scim<void>(SCI_STYLESETBOLD, SCE_JSON_PROPERTYNAME, 1);
                    TCOL(SCE_JSON_LINECOMMENT, cComment);
                    TCOL(SCE_JSON_BLOCKCOMMENT, cComment);
                    TCOL(SCE_JSON_OPERATOR, 0xD4D4D4);
                    TCOL(SCE_JSON_KEYWORD, cKeyword);   Scim<void>(SCI_STYLESETBOLD, SCE_JSON_KEYWORD, 1);
                    TCOL(SCE_JSON_ERROR, cError);
                } else if (!strcmp(lexer, "xml")) {
                    TCOL(SCE_H_TAG, cKeyword);
                    TCOL(SCE_H_ATTRIBUTE, cAttr);
                    TCOL(SCE_H_NUMBER, cNumber);
                    TCOL(SCE_H_DOUBLESTRING, cString);
                    TCOL(SCE_H_SINGLESTRING, cString);
                    TCOL(SCE_H_COMMENT, cComment);
                    TCOL(SCE_H_ENTITY, cNumber);
                    TCOL(SCE_H_VALUE, cString);
                    TCOL(SCE_H_XCCOMMENT, cComment);
                } else if (!strcmp(lexer, "html")) {
                    TCOL(SCE_H_TAG, cKeyword);
                    TCOL(SCE_H_ATTRIBUTE, cAttr);
                    TCOL(SCE_H_NUMBER, cNumber);
                    TCOL(SCE_H_DOUBLESTRING, cString);
                    TCOL(SCE_H_SINGLESTRING, cString);
                    TCOL(SCE_H_COMMENT, cComment);
                    TCOL(SCE_H_ENTITY, cNumber);
                    TCOL(SCE_H_VALUE, cString);
                } else if (!strcmp(lexer, "yaml")) {
                    TCOL(SCE_YAML_COMMENT, cComment);
                    TCOL(SCE_YAML_IDENTIFIER, cAttr);
                    TCOL(SCE_YAML_KEYWORD, cKeyword);   Scim<void>(SCI_STYLESETBOLD, SCE_YAML_KEYWORD, 1);
                    TCOL(SCE_YAML_NUMBER, cNumber);
                    TCOL(SCE_YAML_DOCUMENT, 0xC586C0);  Scim<void>(SCI_STYLESETBOLD, SCE_YAML_DOCUMENT, 1);
                    TCOL(SCE_YAML_REFERENCE, 0x4EC9B0);
                    TCOL(SCE_YAML_OPERATOR, 0xD4D4D4);
                } else if (!strcmp(lexer, "sql")) {
                    TCOL(SCE_SQL_COMMENT, cComment);
                    TCOL(SCE_SQL_COMMENTLINE, cComment);
                    TCOL(SCE_SQL_NUMBER, cNumber);
                    TCOL(SCE_SQL_WORD, cKeyword);       Scim<void>(SCI_STYLESETBOLD, SCE_SQL_WORD, 1);
                    TCOL(SCE_SQL_WORD2, 0x4EC9B0);
                    TCOL(SCE_SQL_STRING, cString);
                    TCOL(SCE_SQL_OPERATOR, 0xD4D4D4);
                    TCOL(SCE_SQL_QUOTEDIDENTIFIER, cAttr);
                } else if (!strcmp(lexer, "batch")) {
                    TCOL(SCE_BAT_COMMENT, cComment);
                    TCOL(SCE_BAT_WORD, cKeyword);
                    TCOL(SCE_BAT_LABEL, 0xDCDCAA);
                    TCOL(SCE_BAT_COMMAND, 0xDCDCAA);
                    TCOL(SCE_BAT_OPERATOR, 0xD4D4D4);
                } else if (!strcmp(lexer, "cpp")) {
                    TCOL(SCE_C_COMMENT, cComment);
                    TCOL(SCE_C_COMMENTLINE, cComment);
                    TCOL(SCE_C_NUMBER, cNumber);
                    TCOL(SCE_C_WORD, cKeyword);         Scim<void>(SCI_STYLESETBOLD, SCE_C_WORD, 1);
                    TCOL(SCE_C_WORD2, 0x4EC9B0);
                    TCOL(SCE_C_STRING, cString);
                    TCOL(SCE_C_CHARACTER, cString);
                    TCOL(SCE_C_PREPROCESSOR, 0xC586C0);
                    TCOL(SCE_C_OPERATOR, 0xD4D4D4);
                    TCOL(SCE_C_GLOBALCLASS, 0x4EC9B0);
                    TCOL(SCE_C_STRINGRAW, cString);
                    TCOL(SCE_C_ESCAPESEQUENCE, cNumber);
                } else if (!strcmp(lexer, "css")) {
                    TCOL(SCE_CSS_TAG, 0xD7BA7D);
                    TCOL(SCE_CSS_CLASS, cAttr);
                    TCOL(SCE_CSS_PSEUDOCLASS, 0x4EC9B0);
                    TCOL(SCE_CSS_IDENTIFIER, 0xD4D4D4);
                    TCOL(SCE_CSS_VALUE, cString);
                    TCOL(SCE_CSS_COMMENT, cComment);
                    TCOL(SCE_CSS_ID, 0x4EC9B0);
                    TCOL(SCE_CSS_IMPORTANT, 0xC586C0);
                    TCOL(SCE_CSS_DIRECTIVE, cKeyword);
                    TCOL(SCE_CSS_DOUBLESTRING, cString);
                    TCOL(SCE_CSS_SINGLESTRING, cString);
                    TCOL(SCE_CSS_VARIABLE, cAttr);
                } else if (!strcmp(lexer, "python")) {
                    TCOL(SCE_P_COMMENTLINE, cComment);
                    TCOL(SCE_P_NUMBER, cNumber);
                    TCOL(SCE_P_STRING, cString);
                    TCOL(SCE_P_CHARACTER, cString);
                    TCOL(SCE_P_WORD, 0xC586C0);
                    TCOL(SCE_P_TRIPLE, cString);
                    TCOL(SCE_P_TRIPLEDOUBLE, cString);
                    TCOL(SCE_P_CLASSNAME, 0x4EC9B0);
                    TCOL(SCE_P_DEFNAME, 0xDCDCAA);
                    TCOL(SCE_P_OPERATOR, 0xD4D4D4);
                    TCOL(SCE_P_COMMENTBLOCK, cComment);
                    TCOL(SCE_P_DECORATOR, 0xDCDCAA);
                    TCOL(SCE_P_FSTRING, cString);
                } else if (!strcmp(lexer, "bash")) {
                    TCOL(SCE_SH_COMMENTLINE, cComment);
                    TCOL(SCE_SH_NUMBER, cNumber);
                    TCOL(SCE_SH_WORD, cKeyword);
                    TCOL(SCE_SH_STRING, cString);
                    TCOL(SCE_SH_CHARACTER, cString);
                    TCOL(SCE_SH_OPERATOR, 0xD4D4D4);
                    TCOL(SCE_SH_IDENTIFIER, 0xD4D4D4);
                    TCOL(SCE_SH_SCALAR, cAttr);
                    TCOL(SCE_SH_PARAM, cAttr);
                    TCOL(SCE_SH_BACKTICKS, cString);
                }
                #undef TCOL
            }
        }
    }
    // framework elements (caret line, selection, caret)
    Scim<void>(SCI_SETCARETLINEVISIBLE, 1);
    Scim<void>(SCI_SETCARETLINEBACK, 0x264F78);
    Scim<void>(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET, 0xFFAEAFAD);
    Scim<void>(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, 0x40264F78);
    Scim<void>(SCI_SETSELBACK, 1, 0x264F78);

    Scim<void>(SCI_SETSAVEPOINT);
    Scim<void>(SCI_EMPTYUNDOBUFFER);
    // V5: EOL/whitespace visibility (toggle in View menu)
    Scim<void>(SCI_SETVIEWEOL, g_viewEol ? 1 : 0);
    Scim<void>(SCI_SETWHITESPACESIZE, 1);
    Scim<void>(SCI_SETWHITESPACEFORE, 0x3A3D41);
    Scim<void>(SCI_SETZOOM, (uptr_t)g_zoom);

    g_filePath = path;
    g_mode = mode; g_hasBom = hasBom; g_bomLen = bomLen;
    memcpy(g_bom, bom, sizeof(g_bom));
    RefreshFileTime(path);   // baseline for external-change detection
    // V5: read-only notice (inform, don't lock)
    {
        DWORD attr = GetFileAttributesW(path.c_str());
        g_readOnly = attr != INVALID_FILE_ATTRIBUTES &&
            (attr & FILE_ATTRIBUTE_READONLY) != 0;
    }
    AddToMru(path);          // V5: remember for the File menu

    if (g_status) {
        const wchar_t *modeTxt = L"ANSI";
        if (mode == MODE_UTF8) modeTxt = hasBom ? L"UTF-8 (BOM)" : L"UTF-8";
        else if (mode == MODE_UTF16LE) modeTxt = L"UTF-16LE (BOM)";
        else if (mode == MODE_UTF16BE) modeTxt = L"UTF-16BE (BOM)";
        const wchar_t *eolTxt = g_eolMode == SC_EOL_CR ? L"CR" :
            (g_eolMode == SC_EOL_LF ? L"LF" : L"CRLF");
        wchar_t s[64];
        wsprintfW(s, L"%s | %s%s", modeTxt, eolTxt,
            g_readOnly ? L" | \u53ea\u8bfb" : L"");
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)s);
    }
    if (g_hwnd) SendMessageW(g_hwnd, WM_APP_TITLE, 0, 0);
    return true;
}

static void UpdateTitle() {
    std::wstring t;
    if (Scim<int>(SCI_GETMODIFY, 0, 0)) t += L"*";
    if (g_readOnly) t += L"[\u53ea\u8bfb] ";
    if (!g_filePath.empty()) t += BaseName(g_filePath);
    else t += L"\u65b0\u5efa";
    t += L" - ";
    t += kAppTitle;
    SetWindowTextW(g_hwnd, t.c_str());
    // status part 2: modified marker
    if (g_status) {
        const wchar_t *m = Scim<int>(SCI_GETMODIFY, 0, 0)
            ? L"\u25cf \u5df2\u4fee\u6539" : L"\u5c31\u7eea";
        SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)m);
    }
}

static void UpdateStatusPos() {
    sptr_t pos = Scim<sptr_t>(SCI_GETCURRENTPOS, 0, 0);
    sptr_t line = Scim<sptr_t>(SCI_LINEFROMPOSITION, (uptr_t)pos, 0);
    sptr_t col = Scim<sptr_t>(SCI_GETCOLUMN, (uptr_t)pos, 0);
    wchar_t s[48];
    wsprintfW(s, L"\u884c %d, \u5217 %d", (int)line + 1, (int)col + 1);
    if (g_status) SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)s);
}

// ---------------------------------------------------------------------------
// Byte-exact save path.

static bool WriteAll(HANDLE h, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        DWORD chunk = (len - off > 0x10000000u) ? 0x10000000u : (DWORD)(len - off);
        DWORD wrote = 0;
        if (!WriteFile(h, p + off, chunk, &wrote, nullptr) || wrote != chunk)
            return false;
        off += chunk;
    }
    return true;
}

// Snapshot the whole document (length-exact; survives embedded NULs).
static std::string DocText() {
    size_t len = (size_t)Scim<int>(SCI_GETLENGTH, 0, 0);
    if (len == 0) return std::string();
    const char *p = (const char *)Scim<sptr_t>(SCI_GETCHARACTERPOINTER, 0, 0);
    if (p) return std::string(p, len);
    // Scintilla pre-4.2 API path
    std::vector<char> tmp(len + 1, 0);
    Scim<void>(SCI_GETTEXT, (uptr_t)(len + 1), (sptr_t)&tmp[0]);
    return std::string(&tmp[0], len);
}

// V5: zoom (Scintilla zoom points; clamp to sane range)
static void ApplyZoom(int delta) {
    g_zoom += delta;
    if (g_zoom < -10) g_zoom = -10;
    if (g_zoom > 20) g_zoom = 20;
    if (g_sci) Scim<void>(SCI_SETZOOM, (uptr_t)g_zoom);
}

// V5: heuristic structure check for json / xml-ish files before saving.
// Returns false with err (reason) and line (1-based) when suspicious.
static bool ValidateFormat(std::wstring &err, int &line) {
    line = 0;
    std::wstring ext = ExtOf(g_filePath);
    bool isJson = (ext == L"json");
    bool isXml = (ext == L"xml" || ext == L"html" || ext == L"htm");
    if (!isJson && !isXml) return true;

    std::string text;
    try {
        text = DocText();
    } catch (const std::bad_alloc &) {
        // Validation is an enhancement, not a gate: on OOM allow the save
        // instead of blocking the user with a dialog they cannot act on.
        return true;
    }
    size_t n = text.size();
    auto lineOf = [&](size_t pos) -> int {
        sptr_t p = (sptr_t)pos;
        if (p > (sptr_t)n) p = (sptr_t)n;
        return (int)Scim<sptr_t>(SCI_LINEFROMPOSITION, (uptr_t)p, 0) + 1;
    };

    if (isJson) {
        std::vector<char> stack;
        bool inStr = false, esc = false;
        for (size_t i = 0; i < n; ++i) {
            char c = text[i];
            if (inStr) {
                if (esc) { esc = false; continue; }
                if (c == '\\') { esc = true; continue; }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == '{' || c == '[') stack.push_back(c);
            else if (c == '}' || c == ']') {
                char want = (c == '}') ? '{' : '[';
                if (stack.empty() || stack.back() != want) {
                    err = (c == '}') ? L"\u62ec\u53f7 } \u4e0d\u914d\u5e73"
                                     : L"\u62ec\u53f7 ] \u4e0d\u914d\u5e73";
                    line = lineOf(i);
                    return false;
                }
                stack.pop_back();
            }
        }
        if (inStr) { err = L"\u5f15\u53f7\u672a\u95ed\u5408"; line = lineOf(n); return false; }
        if (!stack.empty()) {
            err = (stack.back() == '{') ? L"\u62ec\u53f7 { \u672a\u95ed\u5408"
                                        : L"\u62ec\u53f7 [ \u672a\u95ed\u5408";
            line = lineOf(n);
            return false;
        }
        return true;
    }

    // xml-ish: tag stack (skips comments, declarations, CDATA, self-closing)
    std::vector<std::string> stack;
    size_t i = 0;
    while (i < n) {
        if (text[i] != '<') { ++i; continue; }
        if (i + 3 < n && text.compare(i, 4, "<!--") == 0) {
            size_t e = text.find("-->", i + 4);
            if (e == std::string::npos) {
                err = L"\u6ce8\u91ca <!-- \u672a\u95ed\u5408";
                line = lineOf(i);
                return false;
            }
            i = e + 3;
            continue;
        }
        if (i + 8 < n && text.compare(i, 9, "<![CDATA[") == 0) {
            size_t e = text.find("]]>", i + 9);
            if (e == std::string::npos) {
                err = L"CDATA \u672a\u95ed\u5408";
                line = lineOf(i);
                return false;
            }
            i = e + 3;
            continue;
        }
        size_t e = text.find('>', i + 1);
        if (e == std::string::npos) { err = L"<\u6807\u7b7e\u672a\u95ed\u5408"; line = lineOf(i); return false; }
        std::string t = text.substr(i + 1, e - i - 1);
        i = e + 1;
        if (t.empty() || t[0] == '?' || t[0] == '!') continue;   // <?xml ?> / <!DOCTYPE >
        if (t[0] == '/') {
            std::string name = t.substr(1);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t' ||
                   name.back() == '\r' || name.back() == '\n')) name.pop_back();
            if (stack.empty() || stack.back() != name) {
                err = L"\u6807\u7b7e </" + std::wstring(name.begin(), name.end()) +
                    L"> \u4e0d\u5339\u914d";
                line = lineOf(i);
                return false;
            }
            stack.pop_back();
            continue;
        }
        if (!t.empty() && t.back() == '/') continue;   // self-closing
        // opening tag: strip attributes
        size_t sp = t.find_first_of(" \t\r\n");
        std::string name = (sp == std::string::npos) ? t : t.substr(0, sp);
        if (name.empty()) continue;
        stack.push_back(name);
    }
    if (!stack.empty()) {
        err = L"\u6807\u7b7e <" + std::wstring(stack.back().begin(), stack.back().end()) +
            L"> \u672a\u95ed\u5408";
        line = lineOf(n);
        return false;
    }
    return true;
}

// --- external change detection & format reporting (V5) ---
// (globals declared at the top of the file)

static void AddToMru(const std::wstring &path) {
    if (g_mruSuppress) return;
    // remove an existing occurrence first (shifting left), then insert at top
    // and trim — this keeps exactly 8 distinct entries (trim-before-dedup
    // would drop the oldest entry even though a slot was freed).
    int found = -1;
    for (int i = 0; i < g_mruCount; ++i) {
        if (_wcsicmp(g_mru[i].c_str(), path.c_str()) == 0) { found = i; break; }
    }
    if (found == 0) return;                       // already newest
    if (found > 0) {
        for (int j = found; j + 1 < g_mruCount; ++j) g_mru[j] = g_mru[j + 1];
        --g_mruCount;
    }
    int top = (g_mruCount < 8) ? g_mruCount : 7;
    for (int i = top; i > 0; --i) g_mru[i] = g_mru[i - 1];
    g_mru[0] = path;
    if (g_mruCount < 8) ++g_mruCount;
    SaveMru();
}

static void SaveMru() {
    if (g_mruCount <= 0) return;
    size_t bytes = 0;
    for (int i = 0; i < g_mruCount; ++i) bytes += (g_mru[i].size() + 1) * sizeof(wchar_t);
    bytes += sizeof(wchar_t);   // final terminator
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t), 0);
    size_t off = 0;
    for (int i = 0; i < g_mruCount; ++i) {
        memcpy(&buf[off], g_mru[i].c_str(), g_mru[i].size() * sizeof(wchar_t));
        off += g_mru[i].size() + 1;
    }
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\CodeEditor", 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &h, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(h, L"MRU", 0, REG_MULTI_SZ, (const BYTE *)&buf[0], (DWORD)bytes);
        RegCloseKey(h);
    }
}

static void LoadMru() {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\CodeEditor", 0,
            KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return;
    DWORD n = 0, type = 0;
    if (RegQueryValueExW(h, L"MRU", nullptr, &type, nullptr, &n) == ERROR_SUCCESS
        && type == REG_MULTI_SZ && n >= sizeof(wchar_t) * 2 && n <= 64 * 1024) {
        std::vector<wchar_t> buf(n / sizeof(wchar_t) + 2, 0);
        if (RegQueryValueExW(h, L"MRU", nullptr, &type, (LPBYTE)&buf[0], &n)
                == ERROR_SUCCESS) {
            size_t wchars = n / sizeof(wchar_t);
            buf[wchars] = L'\0';
            buf[wchars + 1] = L'\0';        // force double-NUL even if corrupt
            const wchar_t *p = &buf[0];
            const wchar_t *end = &buf[0] + wchars;
            int cnt = 0;
            g_mruCount = 0;
            while (p < end && *p && cnt < 8) {
                size_t len = 0;
                while (p + len < end && p[len]) ++len;    // bounded wcslen
                // buffer is NUL-forced at [wchars]/[wchars+1] and zero-filled,
                // so a truncated trailing entry reads in-bounds; accept it
                g_mru[cnt++] = std::wstring(p, len);
                p += len + 1;   // past end for a truncated tail: loop exits
            }
            g_mruCount = cnt;
        }
    }
    RegCloseKey(h);
}

static bool GetModifyTime(const std::wstring &path, FILETIME &out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ |
        FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME c, a;
    bool ok = GetFileTime(h, &c, &a, &out) != FALSE;
    CloseHandle(h);
    return ok;
}

static void RefreshFileTime(const std::wstring &path) {
    g_fileTimeValid = GetModifyTime(path, g_fileTime);
}

static bool FileWasModifiedExternally() {
    if (!g_fileTimeValid || g_filePath.empty()) return false;
    FILETIME now;
    if (!GetModifyTime(g_filePath, now)) return false;
    return memcmp(&now, &g_fileTime, sizeof(FILETIME)) != 0;
}

// Re-read the just-saved file and report whether encoding/BOM survived.
static void ReportFormatStatus(const std::wstring &path) {
    std::vector<char> buf;
    if (!ReadFileBytes(path, buf)) return;
    LoadMode m = MODE_ANSI; bool b = false; char bom[3] = { 0, 0, 0 };
    DetectFormat(buf, m, b, bom);
    bool same = (m == g_mode) && (b == g_hasBom);
    SetStatusPart2(same ? L"\u683c\u5f0f: \u5df2\u4fdd\u6301"
                        : L"\u683c\u5f0f: \u5df2\u53d8\u5316");
}

static bool SaveFile(const std::wstring &path) {
    bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    // automatic backup of the previous content before overwriting
    if (exists) {
        // CopyFileW carries the source's READ-ONLY attribute onto the backup;
        // a stale read-only .bak would then make every future refresh fail,
        // so clear the attribute first (no-op when the file doesn't exist).
        SetFileAttributesW((path + L".bak").c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE)) {
            AppMsgBox(g_hwnd,
                L"\u81ea\u52a8\u5907\u4efd\u5931\u8d25\uff08\u5c06\u7ee7\u7eed\u4fdd\u5b58\uff09\u3002",
                kAppTitle, MB_OK | MB_ICONWARNING);
        }
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        exists ? OPEN_EXISTING : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ShowError(L"\u65e0\u6cd5\u5199\u5165\u6587\u4ef6\uff08\u53ea\u8bfb\u6216\u65e0\u6743\u9650\uff09\uff1a", path);
        return false;
    }

    std::string text;
    std::vector<char> out;
    try {
        text = DocText();
        if (!EncodeText(text, g_mode, g_bom, g_bomLen, out)) {
            CloseHandle(h);
            AppMsgBox(g_hwnd, L"\u4fdd\u5b58\u5931\u8d25\uff1a\u7f16\u7801\u8f6c\u6362\u5f02\u5e38\u3002",
                kAppTitle, MB_OK | MB_ICONERROR);
            return false;
        }
    } catch (const std::bad_alloc &) {
        CloseHandle(h);
        AppMsgBox(g_hwnd,
            L"\u5185\u5b58\u4e0d\u8db3\uff0c\u65e0\u6cd5\u4fdd\u5b58\u6b64\u6587\u4ef6\u3002",
            kAppTitle, MB_OK | MB_ICONERROR);
        return false;
    }
    if (!WriteAll(h, out.empty() ? "" : &out[0], out.size())) goto fail;
    {
        LARGE_INTEGER total;
        total.QuadPart = (LONGLONG)out.size();
        SetFilePointerEx(h, total, nullptr, FILE_BEGIN);
        if (!SetEndOfFile(h)) goto fail;   // truncate old longer content
    }
    FlushFileBuffers(h);
    CloseHandle(h);

    Scim<void>(SCI_SETSAVEPOINT);
    g_filePath = path;
    RefreshFileTime(path);      // own save must not trigger external-change alert
    UpdateTitle();              // refresh title first...
    ReportFormatStatus(path);   // ...then the format verdict (overwrites part [2])
    return true;
fail:
    ShowError(L"\u5199\u5165\u6587\u4ef6\u5931\u8d25\uff1a", path);
    CloseHandle(h);
    return false;
}

// Returns false only when the user cancels.
static bool PromptSaveIfDirty() {
    if (!Scim<int>(SCI_GETMODIFY, 0, 0)) return true;
    int r = AppMsgBox(g_hwnd,
        L"\u6587\u4ef6\u5df2\u4fee\u6539\uff0c\u662f\u5426\u4fdd\u5b58\uff1f", kAppTitle,
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDNO) return true;
    if (!g_filePath.empty()) return SaveFile(g_filePath);
    SendMessageW(g_hwnd, WM_COMMAND, IDM_SAVEAS, 0);
    return !Scim<int>(SCI_GETMODIFY, 0, 0);
}

// ---------------------------------------------------------------------------
// Open / Save As dialogs

// Filter string generated from the extension map so it can never drift:
// "label\0patterns\0\0" style groups GetOpenFileNameW accepts.
static std::wstring BuildOpenFilter() {
    // groups ordered by frequency of use in intranet config work
    struct Group { const wchar_t *label; const wchar_t *exts; };
    static const Group groups[] = {
        { L"\u914d\u7f6e\u6587\u4ef6 (*.ini;*.cfg;*.conf;*.properties;*.env)",
          L"*.ini;*.cfg;*.conf;*.properties;*.env" },
        { L"\u6570\u636e\u6587\u4ef6 (*.json;*.xml;*.yaml;*.yml)",
          L"*.json;*.xml;*.yaml;*.yml" },
        { L"\u7f51\u9875 (*.html;*.htm;*.css;*.js)", L"*.html;*.htm;*.css;*.js" },
        { L"\u811a\u672c (*.py;*.sql;*.bat;*.cmd;*.sh)", L"*.py;*.sql;*.bat;*.cmd;*.sh" },
        { L"C/C++/Java/C# (*.c;*.cpp;*.h;*.hpp;*.cc;*.cxx;*.java;*.cs;*.mjs)",
          L"*.c;*.cpp;*.h;*.hpp;*.cc;*.cxx;*.java;*.cs;*.mjs" },
        { L"\u6587\u672c (*.txt;*.log)", L"*.txt;*.log" },
        { L"\u6240\u6709\u6587\u4ef6 (*.*)", L"*.*" },
    };
    std::wstring s;
    for (int i = 0; i < 7; ++i) {
        s += groups[i].label; s += L'\0';
        s += groups[i].exts;  s += L'\0';
    }
    s += L'\0';  // filter list terminator
    return s;
}

static std::wstring g_openFilter = BuildOpenFilter();

static void DoOpenDlg() {
    if (!PromptSaveIfDirty()) return;
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = g_openFilter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrTitle = L"\u6253\u5f00";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) LoadFile(file);
}

static bool DoSaveAsDlg() {
    wchar_t file[MAX_PATH * 4] = L"";
    if (!g_filePath.empty())
        wcsncpy(file, g_filePath.c_str(), MAX_PATH * 4 - 1);
    file[MAX_PATH * 4 - 1] = L'\0';   // wcsncpy omits NUL when src >= count
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    // default to the current file's own type
    ofn.lpstrFilter =
        L"\u914d\u7f6e\u6587\u4ef6 (*.ini)\0*.ini;*.cfg;*.conf;*.properties;*.env\0"
        L"\u6570\u636e\u6587\u4ef6\0*.json;*.xml;*.yaml;*.yml\0"
        L"\u7f51\u9875\u6587\u4ef6\0*.html;*.htm;*.css;*.js\0"
        L"\u811a\u672c\u6587\u4ef6\0*.py;*.sql;*.bat;*.cmd;*.sh\0"
        L"C/C++/Java/C#\0*.c;*.cpp;*.h;*.hpp;*.cc;*.cxx;*.java;*.cs;*.mjs\0"
        L"\u6587\u672c (*.txt;*.log)\0*.txt;*.log\0"
        L"\u6240\u6709\u6587\u4ef6 (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;  // user picks; no auto-magic
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrTitle = L"\u53e6\u5b58\u4e3a";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetSaveFileNameW(&ofn)) return SaveFile(file);
    return false;
}

// ---------------------------------------------------------------------------
// Find / Replace (comdlg32 common dialogs)

static FINDREPLACEW g_fr;
static wchar_t g_findBuf[256] = L"";
static wchar_t g_repBuf[256] = L"";

static std::string WideToUtf8(const wchar_t *w) {
    std::string s;
    if (!w) return s;
    for (const wchar_t *q = w; *q; ++q) {
        char utf8[8];
        int n = WideCharToMultiByte(CP_UTF8, 0, q, 1, utf8, sizeof(utf8), nullptr, nullptr);
        if (n > 0) s.append(utf8, (size_t)n);
    }
    return s;
}

static void FindNext(bool fromSelection);

// write status bar part [2] (shared by find counter / modified marker)
static void SetStatusPart2(const wchar_t *s) {
    if (g_status) SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)s);
}

// count all matches of the current find text (capped for pathological cases)
static int CountMatches() {
    if (g_findText.empty()) return 0;
    int count = 0;
    Scim<void>(SCI_SETSEARCHFLAGS, g_findMatchCase ? SCFIND_MATCHCASE : 0);
    sptr_t len = Scim<sptr_t>(SCI_GETLENGTH, 0, 0);
    sptr_t scan = 0;
    while (count < 9999 && scan <= len) {
        Scim<void>(SCI_SETTARGETSTART, (uptr_t)scan);
        Scim<void>(SCI_SETTARGETEND, (uptr_t)len);
        int pos = Scim<int>(SCI_SEARCHINTARGET, (uptr_t)g_findText.size(),
            (sptr_t)g_findText.c_str());
        if (pos < 0) break;
        ++count;
        scan = (sptr_t)pos + (sptr_t)g_findText.size();
    }
    return count;
}

static void OnFindReplMessage(WPARAM wParam, LPARAM lParam) {
    if (wParam != 0) return;
    FINDREPLACEW *fr = (FINDREPLACEW *)lParam;
    g_findText = WideToUtf8(fr->lpstrFindWhat);
    g_findMatchCase = (fr->Flags & FR_MATCHCASE) != 0;
    if (fr->Flags & FR_DIALOGTERM) { g_hwndFind = nullptr; return; }

    bool doReplace = (fr->Flags & (FR_REPLACE | FR_REPLACEALL)) != 0;
    bool all = (fr->Flags & FR_REPLACEALL) != 0;
if (doReplace) {
        std::string rep = WideToUtf8(fr->lpstrReplaceWith);
        if (all) {
            // collect match positions first (forward scan), then replace from
            // the end so earlier positions stay valid; finite and immune to
            // self-matching replacements growing the document.
            std::vector<sptr_t> hits;
            {
                Scim<void>(SCI_SETSEARCHFLAGS, g_findMatchCase ? SCFIND_MATCHCASE : 0);
                sptr_t scan = 0;
                sptr_t len = Scim<sptr_t>(SCI_GETLENGTH, 0, 0);
                try {
                    while (!g_findText.empty() && hits.size() < 9999 && scan <= len) {
                        Scim<void>(SCI_SETTARGETSTART, (uptr_t)scan);
                        Scim<void>(SCI_SETTARGETEND, (uptr_t)len);
                        int pos = Scim<int>(SCI_SEARCHINTARGET, (uptr_t)g_findText.size(),
                            (sptr_t)g_findText.c_str());
                        if (pos < 0) break;
                        hits.push_back((sptr_t)pos);
                        scan = (sptr_t)pos + (sptr_t)g_findText.size();
                    }
                } catch (const std::bad_alloc &) {
                    SetStatusPart2(L"\u66ff\u6362\u5931\u8d25\uff1a\u5185\u5b58\u4e0d\u8db3");
                    return;
                }
            }
            if (hits.empty()) {
                SetStatusPart2(L"\u65e0\u5339\u914d");
                return;
            }
            wchar_t ask[128];
            wsprintfW(ask, L"\u786e\u5b9a\u8981\u66ff\u6362\u5168\u90e8 %d \u5904\u5417\uff1f",
                (int)hits.size());
            if (AppMsgBox(g_hwnd, ask, kAppTitle, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                return;
            for (size_t i = hits.size(); i-- > 0; ) {
                Scim<void>(SCI_SETTARGETSTART, (uptr_t)hits[i]);
                Scim<void>(SCI_SETTARGETEND,
                    (uptr_t)(hits[i] + (sptr_t)g_findText.size()));
                Scim<void>(SCI_REPLACETARGET, (uptr_t)rep.size(), (sptr_t)rep.c_str());
            }
            wchar_t done[64];
            wsprintfW(done, L"\u5df2\u66ff\u6362 %d \u5904", (int)hits.size());
            SetStatusPart2(done);
        } else if (fr->Flags & FR_REPLACE) {
            Scim<void>(SCI_TARGETFROMSELECTION);
            std::string rep = WideToUtf8(fr->lpstrReplaceWith);
            Scim<void>(SCI_REPLACETARGET, (uptr_t)rep.size(), (sptr_t)rep.c_str());
        }
        return;
    }
    FindNext(true);
}

static void DoFind() {
    if (g_hwndFind) { FindNext(true); return; }
    ZeroMemory(&g_fr, sizeof(g_fr));
    g_fr.lStructSize = sizeof(g_fr);
    g_fr.hwndOwner = g_hwnd;
    g_fr.lpstrFindWhat = g_findBuf;
    g_fr.lpstrReplaceWith = g_repBuf;
    g_fr.wFindWhatLen = 256;
    g_fr.wReplaceWithLen = 256;
    g_fr.Flags = FR_DOWN | (g_findMatchCase ? FR_MATCHCASE : 0);
    g_hwndFind = FindTextW(&g_fr);
    if (!g_hwndFind) ShowError(L"\u65e0\u6cd5\u6253\u5f00\u67e5\u627e\u5bf9\u8bdd\u6846\uff1a", L"");
}

static void FindNext(bool fromSelection) {
    if (g_findText.empty()) return;
    Scim<void>(SCI_SETSEARCHFLAGS, g_findMatchCase ? SCFIND_MATCHCASE : 0);
    sptr_t start = fromSelection
        ? Scim<sptr_t>(SCI_GETSELECTIONEND, 0, 0)
        : Scim<sptr_t>(SCI_GETTARGETEND, 0, 0);
    sptr_t len = Scim<sptr_t>(SCI_GETLENGTH, 0, 0);
    if (start > len) start = len;
    Scim<void>(SCI_SETTARGETSTART, (uptr_t)start);
    Scim<void>(SCI_SETTARGETEND, (uptr_t)len);
    int pos = Scim<int>(SCI_SEARCHINTARGET, (uptr_t)g_findText.size(),
        (sptr_t)g_findText.c_str());
    if (pos < 0) {
        int n = CountMatches();
        wchar_t s[64];
        if (n > 0) wsprintfW(s, L"\u5339\u914d %d \u5904", n);
        else wcscpy(s, L"\u65e0\u5339\u914d");
        SetStatusPart2(s);
        AppMsgBox(g_hwnd, L"\u5df2\u5230\u6587\u4ef6\u672b\u5c3e\u3002", kAppTitle,
            MB_OK | MB_ICONINFORMATION);
        return;
    }
    Scim<void>(SCI_SETSEL, (uptr_t)pos, (uptr_t)(pos + (int)g_findText.size()));
    int n = CountMatches();
    wchar_t s[64];
    wsprintfW(s, L"\u5339\u914d %d \u5904", n);
    SetStatusPart2(s);
}

// ---------------------------------------------------------------------------
// Auto-indent on newline (copies previous line's leading whitespace)

static void AutoIndent(char ch) {
    if (ch != '\n') return;
    sptr_t cur = Scim<sptr_t>(SCI_LINEFROMPOSITION, Scim<sptr_t>(SCI_GETCURRENTPOS, 0, 0), 0);
    if (cur <= 0) return;
    sptr_t prev = cur - 1;
    int len = Scim<int>(SCI_GETLINE, (uptr_t)prev, 0);
    if (len <= 0) return;
    std::vector<char> line((size_t)len + 1, 0);
    Scim<void>(SCI_GETLINE, (uptr_t)prev, (sptr_t)&line[0]);
    line[(size_t)len] = 0;
    sptr_t start = Scim<sptr_t>(SCI_POSITIONFROMLINE, (uptr_t)prev, 0);
    std::string ws;
    for (int i = 0; i < len; ++i) {
        char c = line[(size_t)i];
        if (c == ' ' || c == '\t') ws.push_back(c);
        else break;
    }
    if (!ws.empty())
        Scim<void>(SCI_INSERTTEXT, (uptr_t)Scim<sptr_t>(SCI_GETCURRENTPOS, 0, 0), (sptr_t)ws.c_str());
    (void)start;
}

// ---------------------------------------------------------------------------
// Window proc

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_status = CreateStatusWindowW(WS_CHILD | WS_VISIBLE | CCS_BOTTOM,
            L"", hwnd, 5000);
        g_sci = CreateWindowExW(0, L"Scintilla", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN,
            0, 0, 300, 200, hwnd, nullptr, g_hInst, nullptr);
        int parts[3] = { 220, 370, -1 };
        SendMessageW(g_status, SB_SETPARTS, 3, (LPARAM)parts);
        // one-time Scintilla control setup (independent of which file loads)
        {
            // per-type colours / fonts / codepages are per-file in LoadFile;
            // here only control-level static settings.
            Scim<void>(SCI_SETPROPERTY, (uptr_t)"fold", (sptr_t)"1");
            Scim<void>(SCI_SETMARGINTYPEN, 1, SC_MARGIN_SYMBOL);
            Scim<void>(SCI_SETMARGINWIDTHN, 1, 14);
            Scim<void>(SCI_SETMARGINMASKN, 1, (sptr_t)0xFE000000);
            Scim<void>(SCI_SETMARGINSENSITIVEN, 1, TRUE);
            Scim<void>(SCI_SETFOLDMARGINCOLOUR, 1, 0x252526);
            Scim<void>(SCI_SETFOLDMARGINHICOLOUR, 1, 0x252526);
            struct { int num; int shape; } tree[] = {
                { SC_MARKNUM_FOLDEROPEN,      SC_MARK_BOXMINUS },
                { SC_MARKNUM_FOLDER,          SC_MARK_BOXPLUS },
                { SC_MARKNUM_FOLDEROPENMID,   SC_MARK_BOXPLUSCONNECTED },
                { SC_MARKNUM_FOLDEREND,       SC_MARK_BOXMINUSCONNECTED },
                { SC_MARKNUM_FOLDERSUB,       SC_MARK_VLINE },
                { SC_MARKNUM_FOLDERTAIL,      SC_MARK_LCORNER },
                { SC_MARKNUM_FOLDERMIDTAIL,   SC_MARK_TCORNER },
            };
            for (int i = 0; i < 7; ++i) {
                Scim<void>(SCI_MARKERDEFINE, (uptr_t)tree[i].num, (sptr_t)tree[i].shape);
                Scim<void>(SCI_MARKERSETFORE, (uptr_t)tree[i].num, 0xE7E7E7);
                Scim<void>(SCI_MARKERSETBACK, (uptr_t)tree[i].num, 0x3A3D41);
            }
        }
        DragAcceptFiles(hwnd, TRUE);
        SendMessageW(hwnd, WM_APP_TITLE, 0, 0);
        return 0;
    }
    case WM_SIZE: {
        RECT rc, sr;
        GetClientRect(hwnd, &rc);
        GetClientRect(g_status, &sr);
        int sh = sr.bottom - sr.top;
        if (g_sci) MoveWindow(g_sci, 0, 0, rc.right, rc.bottom - sh, TRUE);
        return 0;
    }
    case WM_ACTIVATE: {
        // external-change check when the window regains focus (cheap: no polling)
        if (LOWORD(wParam) != WA_INACTIVE && g_fileTimeValid &&
            !g_filePath.empty() && FileWasModifiedExternally()) {
            if (Scim<int>(SCI_GETMODIFY, 0, 0)) {
                int r = AppMsgBox(hwnd,
                    L"\u6587\u4ef6\u5df2\u88ab\u5176\u4ed6\u7a0b\u5e8f\u4fee\u6539\uff0c"
                    L"\u662f\u5426\u91cd\u65b0\u52a0\u8f7d\uff1f",
                    kAppTitle, MB_YESNO | MB_ICONQUESTION);
                if (r == IDYES) LoadFile(g_filePath);
                else RefreshFileTime(g_filePath);   // keep local edits
            } else {
                LoadFile(g_filePath);   // no local edits: silent reload
            }
        }
        return 0;
    }
    case WM_INITMENUPOPUP: {
        // V5: rebuild recent-files submenu contents on open
        if ((HMENU)wParam == g_mRecentMenu && g_mRecentMenu) {
            while (GetMenuItemCount(g_mRecentMenu) > 0)
                DeleteMenu(g_mRecentMenu, 0, MF_BYPOSITION);
            if (g_mruCount == 0) {
                AppendMenuW(g_mRecentMenu, MF_STRING | MF_GRAYED, 0,
                    L"\uff08\u65e0\uff09");
            } else {
                for (int i = 0; i < g_mruCount; ++i) {
                    std::wstring label = std::to_wstring(i + 1) + L"  " +
                        BaseName(g_mru[i]);
                    AppendMenuW(g_mRecentMenu, MF_STRING,
                        (UINT_PTR)(IDM_MRU_BASE + i), label.c_str());
                }
            }
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wParam;
        wchar_t path[MAX_PATH * 4] = L"";
        if (DragQueryFileW(hd, 0, path, MAX_PATH * 4) > 0) {
            if (PromptSaveIfDirty()) LoadFile(path);
        }
        DragFinish(hd);
        return 0;
    }
    case WM_APP_TITLE:
        UpdateTitle();
        return 0;
    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lParam;
        if (nh->hwndFrom == g_sci) {
            SCNotification *sc = (SCNotification *)lParam;
            switch (sc->nmhdr.code) {
            case SCN_SAVEPOINTREACHED:
            case SCN_SAVEPOINTLEFT:
                UpdateTitle();
                if (sc->nmhdr.code == SCN_SAVEPOINTREACHED && !g_filePath.empty())
                    RefreshFileTime(g_filePath);   // own save: avoid self-trigger
                break;
            case SCN_MARGINCLICK:
                if (sc->margin == 1) {
                    int line = (int)Scim<sptr_t>(SCI_LINEFROMPOSITION,
                        (uptr_t)sc->position, 0);
                    Scim<void>(SCI_TOGGLEFOLD, (uptr_t)line, 0);
                }
                break;
            case SCN_UPDATEUI:
                UpdateStatusPos();
                break;
            case SCN_CHARADDED:
                AutoIndent((char)sc->ch);
                break;
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDM_OPEN: DoOpenDlg(); return 0;
        case IDM_SAVE: {
            // V5: heuristic structure check before writing json/xml
            if (!g_filePath.empty()) {
                std::wstring verr; int vline = 0;
                if (!ValidateFormat(verr, vline)) {
                    std::wstring verrShown = verr;
                    if (verrShown.size() > 160) verrShown.resize(160);   // document text: bound it
                    std::wstring ask = L"\u7b2c " + std::to_wstring(vline) +
                        L" \u884c\u53ef\u80fd\uff1a" + verrShown + L"\u3002\u4ecd\u8981\u4fdd\u5b58\u5417\uff1f";
                    if (AppMsgBox(hwnd, ask.c_str(), kAppTitle,
                            MB_OKCANCEL | MB_ICONWARNING) != IDOK)
                        return 0;
                }
            }
            if (g_filePath.empty()) DoSaveAsDlg();
            else SaveFile(g_filePath);
            return 0;
        }
        case IDM_SAVEAS: DoSaveAsDlg(); return 0;
        case IDM_EXIT: SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        case IDM_RELOAD_UTF8:
        case IDM_RELOAD_GBK:
        case IDM_RELOAD_UTF16: {
            if (g_filePath.empty()) return 0;
            if (!PromptSaveIfDirty()) return 0;
            int m = (LOWORD(wParam) == IDM_RELOAD_UTF8) ? 0
                  : (LOWORD(wParam) == IDM_RELOAD_GBK) ? 1 : 2;
            LoadFile(g_filePath, true, m);
            return 0;
        }
        case IDM_ZOOM_IN: ApplyZoom(1); return 0;
        case IDM_ZOOM_OUT: ApplyZoom(-1); return 0;
        case IDM_ZOOM_RESET: ApplyZoom(-g_zoom); return 0;
        case IDM_VIEW_EOL: {
            g_viewEol = !g_viewEol;
            Scim<void>(SCI_SETVIEWEOL, g_viewEol ? 1 : 0);
            HMENU hm = GetMenu(hwnd);
            CheckMenuItem(hm, IDM_VIEW_EOL,
                MF_BYCOMMAND | (g_viewEol ? MF_CHECKED : MF_UNCHECKED));
            return 0;
        }
        case IDM_UNDO: Scim<void>(SCI_UNDO); return 0;
        case IDM_REDO: Scim<void>(SCI_REDO); return 0;
        case IDM_CUT: Scim<void>(SCI_CUT); return 0;
        case IDM_COPY: Scim<void>(SCI_COPY); return 0;
        case IDM_PASTE: Scim<void>(SCI_PASTE); return 0;
        case IDM_SELECTALL: Scim<void>(SCI_SELECTALL); return 0;
        case IDM_FIND: DoFind(); return 0;
        case IDM_FINDNEXT: FindNext(true); return 0;
        case IDM_ABOUT:
            AppMsgBox(hwnd,
                L"\u672c\u7f16\u8f91\u5668\u4fdd\u8bc1\uff1a\u6587\u4ef6\u7684\u7f16\u7801\u3001BOM\u3001"
                L"\u6362\u884c\u7b26\u5728\u4fdd\u5b58\u540e\u4fdd\u6301\u539f\u6837\uff0c\u4e0d\u4f1a"
                L"\u50cf\u8bb0\u4e8b\u672c\u4e00\u6837\u81ea\u52a8\u8f6c\u6362\u3002",
                kAppTitle, MB_OK | MB_ICONINFORMATION);
            return 0;
        default:
            // V5: recent-files items (dynamic IDs)
            if (LOWORD(wParam) >= IDM_MRU_BASE &&
                LOWORD(wParam) < IDM_MRU_BASE + 8) {
                int idx = LOWORD(wParam) - IDM_MRU_BASE;
                if (idx < g_mruCount && !g_mru[idx].empty()) {
                    std::wstring p = g_mru[idx];
                    if (PromptSaveIfDirty()) LoadFile(p);
                }
                return 0;
            }
            break;
        }
        break;
    }
    case WM_CLOSE:
        if (PromptSaveIfDirty()) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Pseudo-UI check usable under job-object isolation (this harness cannot show
// a visible window): creates the real window via the real startup path and
// verifies the Scintilla child exists and a file round-trips. Window visibility
// must be eyeballed by the user on a real desktop.
static bool RunUiCheck(HWND hwnd, const std::wstring &openFile) {
    // Result path follows the machine, not the build farm: a delivery box has
    // no C:\cedb, and a failed fopen used to silently lose the whole report.
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring base = dir;
    base += L"CodeEditorUiCheck";
    CreateDirectoryW(base.c_str(), nullptr);
    FILE *f = _wfopen((base + L"\\uicheck-result.txt").c_str(), L"w, ccs=UTF-8");
    bool frameOk = hwnd && IsWindow(hwnd);
    bool sciOk = g_sci && IsWindow(g_sci);
    // pump messages so WM_CREATE completes and the Scintilla child attaches
    MSG m;
    for (int i = 0; i < 10; ++i) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(20);
    }
    // open the file through the real path now that the Scintilla child exists
    bool loaded = openFile.empty() ? true : LoadFile(openFile);
    wchar_t title[256] = {0};
    if (frameOk) GetWindowTextW(hwnd, title, 256);
    wchar_t status[128] = {0};
    if (g_status) SendMessageW(g_status, SB_GETTEXTW, 0, (LPARAM)status);
    if (f) {
        fwprintf(f, L"frame=%p frame_ok=%d sci=%p sci_ok=%d statusbar=%p loaded=%d\n",
            hwnd, frameOk ? 1 : 0, g_sci, sciOk ? 1 : 0, g_status, loaded ? 1 : 0);
        fwprintf(f, L"title_bytes=");
        for (int i = 0; i < lstrlenW(title); ++i)
            fwprintf(f, L"%02X%02X", (unsigned)(title[i] & 0xFF), (unsigned)((title[i] >> 8) & 0xFF));
        fwprintf(f, L"\nstatus_bytes=");
        for (int i = 0; i < lstrlenW(status); ++i)
            fwprintf(f, L"%02X%02X", (unsigned)(status[i] & 0xFF), (unsigned)((status[i] >> 8) & 0xFF));
        fwprintf(f, L"\n");

        // V4: prove syntax highlighting is live (JSON lexer attached, token
        // styled) and fold levels produced.
        int styleAt = -1, foldLine0 = 0, styleFore = 0;
        if (loaded && !openFile.empty()) {
            // lexer evidence: folded entry says json lexer ran
            foldLine0 = (int)Scim<sptr_t>(SCI_GETFOLDLEVEL, 0, 0);
            // force full-document styling (idle styling may not have run yet)
            Scim<void>(SCI_COLOURISE, 0, (sptr_t)-1);
            const char *probe = "value";   // string content in the doc
            Scim<void>(SCI_SETTARGETSTART, 0);
            Scim<void>(SCI_SETTARGETEND, Scim<int>(SCI_GETTEXTLENGTH));
            int pos = Scim<int>(SCI_SEARCHINTARGET, (uptr_t)strlen(probe),
                (sptr_t)probe);
            if (pos >= 0) {
                styleAt = (int)Scim<sptr_t>(SCI_GETSTYLEAT, (uptr_t)pos);
                styleFore = Scim<int>(SCI_STYLEGETFORE, (uptr_t)styleAt, 0);
                int styleBack = Scim<int>(SCI_STYLEGETBACK, (uptr_t)styleAt, 0);
                if (styleBack != 0x1E1E1E) frameOk = false;
            } else {
                frameOk = false;
            }
        }
        fwprintf(f, L"pos0_style=%d style_at_value=%d fore=0x%06X fold_level_0=0x%X\n",
            (int)Scim<sptr_t>(SCI_GETSTYLEAT, 0, 0), styleAt,
            (unsigned)styleFore, (unsigned)foldLine0);
        // "value" content bytes inside a JSON string are SCE_JSON_STRING(2)
        if (styleAt != 2) frameOk = false;
        if (!(foldLine0 & 0x400)) frameOk = false;
        fclose(f);
    }
    return frameOk && sciOk && loaded;
}

// ---------------------------------------------------------------------------
// Self test: hidden run, exercise the real Load/Save code paths, byte compare.

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void LogLine(FILE *f, const char *s) {
    if (f) fputs(s, f);
}

static bool WriteBytes(const std::wstring &path, const std::vector<char> &data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    if (!data.empty()) {
        DWORD wrote = 0;
        ok = WriteFile(h, &data[0], (DWORD)data.size(), &wrote, nullptr)
            && wrote == data.size();
    }
    CloseHandle(h);
    return ok;
}

struct TestCase {
    const char *name;
    std::vector<char> origin;      // bytes as loaded from disk
    std::vector<char> expectSave;  // bytes expected after untouched save
    bool modify;                   // halve the document before saving
};

static bool RunSelftest() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring base = dir;
    base += L"CodeEditorSelftest";
    CreateDirectoryW(base.c_str(), nullptr);

    std::vector<TestCase> cases;
    {
        // 1. UTF-8 BOM + CRLF + Chinese comments
        TestCase c; c.name = "utf8-bom-crlf";
        c.origin = { (char)0xEF,(char)0xBB,(char)0xBF,
            '{','\r','\n',' ','/','/',' ','\xd6','\xd0','\xce','\xc4','\r','\n','}','\r','\n' };
        c.expectSave = c.origin; c.modify = false;
        cases.push_back(c);
    }
    {
        // 2. UTF-8 no BOM + LF
        TestCase c; c.name = "utf8-lf";
        c.origin = { '{','\n','"','k','"',':','1','\n','}','\n' };
        c.expectSave = c.origin; c.modify = false;
        cases.push_back(c);
    }
    {
        // 3. GBK bytes D6D0 CEC4 (中文) + CRLF
        TestCase c; c.name = "gbk-crlf";
        c.origin = { '\xd6','\xd0','\xce','\xc4','\r','\n','x','\r','\n' };
        c.expectSave = c.origin; c.modify = false;
        cases.push_back(c);
    }
    {
        // 4. UTF-16LE BOM + CRLF
        TestCase c; c.name = "utf16le-bom";
        const wchar_t *w = L"ab\r\n";
        c.origin.push_back((char)0xFF); c.origin.push_back((char)0xFE);
        for (const wchar_t *q = w; *q; ++q) { c.origin.push_back((char)(*q & 0xFF)); c.origin.push_back((char)((*q >> 8) & 0xFF)); }
        c.expectSave = c.origin; c.modify = false;
        cases.push_back(c);
    }
    {
        // 5. UTF-16BE BOM + CR only
        TestCase c; c.name = "utf16be-cr";
        const wchar_t *w = L"ab\r";
        c.origin.push_back((char)0xFE); c.origin.push_back((char)0xFF);
        for (const wchar_t *q = w; *q; ++q) { c.origin.push_back((char)((*q >> 8) & 0xFF)); c.origin.push_back((char)(*q & 0xFF)); }
        c.expectSave = c.origin; c.modify = false;
        cases.push_back(c);
    }
    {
        // 6. no trailing newline + embedded NUL
        TestCase c; c.name = "no-eol-nul";
        c.origin = { 'a','b', 0, 'c','d' };
        c.expectSave = c.origin; c.modify = false;
        cases.push_back(c);
    }
    {
        // 7. shrink write: first half survives, old tail truncated
        TestCase c; c.name = "shrink-write";
        c.origin = { 'L','i','n','e','1','\r','\n','L','i','n','e','2','\r','\n' };
        c.expectSave = { 'L','i','n','e','1','\r','\n' };
        c.modify = true;
        cases.push_back(c);
    }
    {
        // 8. empty file
        TestCase c; c.name = "empty";
        c.origin.clear(); c.expectSave = c.origin; c.modify = false;
        cases.push_back(c);
    }

    std::wstring resultPath = base + L"\\selftest-result.txt";
    FILE *rf = _wfopen(resultPath.c_str(), L"wb, ccs=UTF-8");
    int pass = 0;
    // +4 extra cases appended below: format-validate, backup-overwrite,
    // force-reload, mru-corrupt.
    // Suppress MRU writes during selftest so temporary .dat files never leak
    // into the real recent-files list.
    bool mruWasEnabled = g_mruSuppress;
    g_mruSuppress = true;
    int total = (int)cases.size() + 4;
    for (size_t i = 0; i < cases.size(); ++i) {
        const TestCase &c = cases[i];
        std::wstring in = base + L"\\in_" + std::to_wstring(i) + L".dat";
        std::wstring out = base + L"\\out_" + std::to_wstring(i) + L".dat";
        DeleteFileW(out.c_str());
        bool ok = WriteBytes(in, c.origin);
        ok = ok && LoadFile(in, false);
        if (ok && c.modify) {
            // select second half of the document and delete it
            size_t len = (size_t)Scim<int>(SCI_GETLENGTH, 0, 0);
            Scim<void>(SCI_SETSEL, (uptr_t)(len / 2), (uptr_t)len);
            Scim<void>(SCI_CLEAR);
        }
        // SaveFile writes under g_filePath; force it:
        if (ok) { g_filePath = out; ok = SaveFile(out); }

        std::vector<char> actual;
        ReadFileBytes(out, actual);
        bool same = (actual == c.expectSave);
        if (same) ++pass;
        char line[400];
        // narrow names for the log (name is already narrow ASCII)
        char narrow[64];
        ZeroMemory(narrow, sizeof(narrow));
        strncpy_s(narrow, c.name, _TRUNCATE);
        sprintf_s(line, sizeof(line), "%s: %s (expected %u bytes, got %u)\r\n",
            narrow[0] ? narrow : "?", (same ? "PASS" : (ok ? "FAIL" : "LOAD-FAIL")),
            (unsigned)c.expectSave.size(), (unsigned)actual.size());
        LogLine(rf, line);
    }

    // 9. format-validate: pure ValidateFormat checks (no dialogs, no files)
    {
        std::wstring err;
        int line = -1;
        // valid JSON must pass
        Scim<void>(SCI_SETTEXT, 0, (sptr_t)"{\"a\":1,\"b\":[1,2]}");
        g_filePath = L"selftest.json";
        bool okValid = ValidateFormat(err, line);
        // broken JSON must fail with a line number
        Scim<void>(SCI_SETTEXT, 0, (sptr_t)"{\"a\":");
        bool okBroken = !ValidateFormat(err, line) && line > 0;
        // broken XML must fail too
        Scim<void>(SCI_SETTEXT, 0, (sptr_t)"<root><item></root>");
        g_filePath = L"selftest.xml";
        bool okXml = !ValidateFormat(err, line) && line > 0;
        // edge: escaped quote and braces inside JSON string are legal
        Scim<void>(SCI_SETTEXT, 0, (sptr_t)"{\"a\":\"b\\\"c\",\"s\":\"{[]}\"}");
        g_filePath = L"selftest.json";
        bool okEsc = ValidateFormat(err, line);
        // edge: valid XML with attribute, self-closing tag, comment with '>'
        Scim<void>(SCI_SETTEXT, 0,
            (sptr_t)"<root a=\"1\"><b/><!-- c > d --></root>");
        g_filePath = L"selftest.xml";
        bool okXmlOk = ValidateFormat(err, line);
        bool vpass = okValid && okBroken && okXml && okEsc && okXmlOk;
        if (vpass) ++pass;
        char line2[240];
        sprintf_s(line2, sizeof(line2),
            "format-validate: %s (valid=%d broken=%d xml=%d esc=%d xmlok=%d)\r\n",
            vpass ? "PASS" : "FAIL", okValid ? 1 : 0, okBroken ? 1 : 0,
            okXml ? 1 : 0, okEsc ? 1 : 0, okXmlOk ? 1 : 0);
        LogLine(rf, line2);
        g_filePath.clear();
    }

    // 10. backup-on-overwrite: SaveFile must leave the previous content in .bak
    {
        std::wstring in = base + L"\\bak_in.dat";
        std::wstring out = base + L"\\bak_out.dat";
        std::wstring bak = out + L".bak";
        DeleteFileW(bak.c_str());
        WriteBytes(in, std::vector<char>{ 'N', 'E', 'W' });
        WriteBytes(out, std::vector<char>{ 'O', 'L', 'D' });
        bool ok = LoadFile(in, false);
        if (ok) { g_filePath = out; ok = SaveFile(out); }
        std::vector<char> bakData, outData;
        ReadFileBytes(bak, bakData);
        ReadFileBytes(out, outData);
        std::vector<char> wantOld{ 'O', 'L', 'D' }, wantNew{ 'N', 'E', 'W' };
        bool bpass = ok && bakData == wantOld && outData == wantNew;
        if (bpass) ++pass;
        char line3[160];
        sprintf_s(line3, sizeof(line3),
            "backup-overwrite: %s (bak=%s out=%s)\r\n", bpass ? "PASS" : "FAIL",
            bakData == wantOld ? "OLD" : "?",
            outData == wantNew ? "NEW" : "?");
        LogLine(rf, line3);
    }

    // 11. force-reload: LoadFile(path, attach, forceMode) honours the override
    {
        std::wstring p = base + L"\\fr.dat";
        WriteBytes(p, std::vector<char>{ 'A', (char)0xE4, (char)0xB8, (char)0xAD });
        bool a = LoadFile(p, false, 0);        // force UTF-8
        bool okA = a && g_mode == MODE_UTF8 && !g_hasBom;
        bool b = LoadFile(p, false, 1);        // force GBK
        bool okB = b && g_mode == MODE_ANSI;
        std::wstring p2 = base + L"\\fr16.dat";
        WriteBytes(p2, std::vector<char>{ (char)0xFF, (char)0xFE, 'A', 0, 'B', 0 });
        bool c = LoadFile(p2, false, 2);       // force UTF-16LE
        bool okC = c && g_mode == MODE_UTF16LE && g_hasBom;
        bool fpass = okA && okB && okC;
        if (fpass) ++pass;
        char line4[200];
        sprintf_s(line4, sizeof(line4),
            "force-reload: %s (utf8=%d gbk=%d utf16=%d)\r\n",
            fpass ? "PASS" : "FAIL", okA ? 1 : 0, okB ? 1 : 0, okC ? 1 : 0);
        LogLine(rf, line4);
        g_filePath.clear();
        g_mruSuppress = mruWasEnabled;
        Scim<void>(SCI_SETTEXT, 0, (sptr_t)"");
    }
    // 12. mru-corrupt: LoadMru must survive a corrupted/oversized registry
    //    value. The real MRU is snapshotted first and unconditionally
    //    restored afterwards - this test must never destroy user data.
    {
        HKEY hk;
        bool had = false;
        std::vector<char> saved;
        DWORD savedType = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\CodeEditor", 0,
                KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
            DWORD n = 0;
            if (RegQueryValueExW(hk, L"MRU", nullptr, &savedType, nullptr, &n)
                    == ERROR_SUCCESS && n > 0) {
                saved.resize(n);
                if (RegQueryValueExW(hk, L"MRU", nullptr, &savedType,
                        (LPBYTE)&saved[0], &n) == ERROR_SUCCESS) {
                    had = true;
                    saved.resize(n);
                }
            }
            RegCloseKey(hk);
        }
        auto restore = [&]() {
            HKEY k2;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\CodeEditor", 0,
                    nullptr, 0, KEY_SET_VALUE, nullptr, &k2, nullptr)
                    == ERROR_SUCCESS) {
                if (had) RegSetValueExW(k2, L"MRU", 0, savedType,
                    (const BYTE *)&saved[0], (DWORD)saved.size());
                else RegDeleteValueW(k2, L"MRU");
                RegCloseKey(k2);
            }
        };
        bool cpass = true;
        bool aok = false, bok = false;
        // (a) unterminated entries "A\0B" (no trailing double NUL)
        {
            const wchar_t junk[] = { L'A', 0, L'B' };   // 3 wchars, no terminator
            HKEY k2;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\CodeEditor", 0,
                    nullptr, 0, KEY_SET_VALUE, nullptr, &k2, nullptr)
                    == ERROR_SUCCESS) {
                RegSetValueExW(k2, L"MRU", 0, REG_MULTI_SZ, (const BYTE *)junk,
                    sizeof(junk));
                RegCloseKey(k2);
            }
            g_mruCount = 0;
            LoadMru();
            aok = g_mruCount == 2 && g_mru[0] == L"A" && g_mru[1] == L"B";
            if (!aok) cpass = false;
        }
        // (b) oversized value must be rejected without touching g_mru
        {
            std::vector<wchar_t> big(33 * 1024, L'X');   // > 64 KB cap
            big[0] = L'A'; big[1] = 0; big[2] = 0;
            HKEY k2;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\CodeEditor", 0,
                    nullptr, 0, KEY_SET_VALUE, nullptr, &k2, nullptr)
                    == ERROR_SUCCESS) {
                RegSetValueExW(k2, L"MRU", 0, REG_MULTI_SZ, (const BYTE *)&big[0],
                    (DWORD)(big.size() * sizeof(wchar_t)));
                RegCloseKey(k2);
            }
            g_mruCount = 0;
            LoadMru();
            bok = g_mruCount == 0;
            if (!bok) cpass = false;
        }
        restore();
        // reload whatever the user really had
        g_mruCount = 0;
        LoadMru();
        if (cpass) ++pass;
        char line5[120];
        sprintf_s(line5, sizeof(line5), "mru-corrupt: %s (a=%d b=%d)\r\n",
            cpass ? "PASS" : "FAIL", aok ? 1 : 0, bok ? 1 : 0);
        LogLine(rf, line5);
    }
    if (rf) {
        char sum[128];
        sprintf_s(sum, sizeof(sum), "TOTAL %d/%d %s\r\n",
            (int)pass, total, (pass == total ? "PASS" : "FAIL"));
        LogLine(rf, sum);
        fclose(rf);
    }
    return pass == total;
}

// ---------------------------------------------------------------------------
// Scripted UI-interaction test (--uitest). Drives the same WM_COMMAND /
// notification paths the menus produce, with scripted dialog answers, and
// asserts observable state after each step. No blocking dialogs, no desktop.

static bool RunUiTest(HWND hwnd) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring base = dir;
    base += L"CodeEditorUiTest";
    CreateDirectoryW(base.c_str(), nullptr);

    FILE *rf = _wfopen((base + L"\\uitest-result.txt").c_str(), L"w, ccs=UTF-8");
    g_uiTestLog = rf;
    int pass = 0, total = 0;
    auto check = [&](const char *name, bool ok) {
        ++total; if (ok) ++pass;
        if (rf) { fwprintf(rf, L"%hs: %ls\r\n", name, ok ? L"PASS" : L"FAIL"); fflush(rf); }
    };
    auto part2 = [&](std::wstring &out) {
        wchar_t b[128] = {0};
        SendMessageW(g_status, SB_GETTEXTW, 2, (LPARAM)b);
        out = b;
    };
    (void)part2;

    bool mruWas = g_mruSuppress;
    g_mruSuppress = true;   // LoadFile in tests must not touch the real MRU

    // --- 1. zoom commands (menu path) + clamping ---
    SendMessageW(hwnd, WM_COMMAND, IDM_ZOOM_RESET, 0);
    SendMessageW(hwnd, WM_COMMAND, IDM_ZOOM_IN, 0);
    SendMessageW(hwnd, WM_COMMAND, IDM_ZOOM_IN, 0);
    check("zoom-in-2", Scim<int>(SCI_GETZOOM, 0, 0) == 2);
    SendMessageW(hwnd, WM_COMMAND, IDM_ZOOM_OUT, 0);
    check("zoom-out-1", Scim<int>(SCI_GETZOOM, 0, 0) == 1);
    for (int i = 0; i < 40; ++i) SendMessageW(hwnd, WM_COMMAND, IDM_ZOOM_IN, 0);
    check("zoom-clamp-20", Scim<int>(SCI_GETZOOM, 0, 0) == 20);
    SendMessageW(hwnd, WM_COMMAND, IDM_ZOOM_RESET, 0);
    check("zoom-reset-0", Scim<int>(SCI_GETZOOM, 0, 0) == 0);

    // --- 2. EOL visibility toggle (menu path + check state) ---
    {
        UINT st0 = GetMenuState(GetMenu(hwnd), IDM_VIEW_EOL, MF_BYCOMMAND);
        SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_EOL, 0);
        bool off = Scim<int>(SCI_GETVIEWEOL, 0, 0) == 0;
        UINT st1 = GetMenuState(GetMenu(hwnd), IDM_VIEW_EOL, MF_BYCOMMAND);
        bool unchecked = (st1 & MF_CHECKED) == 0;
        SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_EOL, 0);
        bool on = Scim<int>(SCI_GETVIEWEOL, 0, 0) == 1;
        UINT st2 = GetMenuState(GetMenu(hwnd), IDM_VIEW_EOL, MF_BYCOMMAND);
        check("eol-toggle-off-on", off && unchecked && on && (st2 & MF_CHECKED));
        (void)st0;
    }

    // --- 3. reload-as-encoding (menu path), incl. cancel-on-dirty ---
    {
        std::wstring gbk = base + L"\\reload_gbk.txt";
        WriteBytes(gbk, std::vector<char>{ '\xd6', '\xd0', '\xce', '\xc4',
            '\r', '\n' });
        check("reload-initial", LoadFile(gbk) && g_mode == MODE_ANSI);
        SendMessageW(hwnd, WM_COMMAND, IDM_RELOAD_UTF8, 0);
        check("reload-as-utf8", g_mode == MODE_UTF8);
        SendMessageW(hwnd, WM_COMMAND, IDM_RELOAD_GBK, 0);
        check("reload-as-gbk", g_mode == MODE_ANSI);
        SendMessageW(hwnd, WM_COMMAND, IDM_RELOAD_UTF16, 0);
        check("reload-as-utf16", g_mode == MODE_UTF16LE);
        // dirty + cancel must abort the reload
        g_mode = MODE_ANSI;
        Scim<void>(SCI_INSERTTEXT, 0, (sptr_t)"X");
        g_uiAnswer = IDCANCEL;
        SendMessageW(hwnd, WM_COMMAND, IDM_RELOAD_UTF8, 0);
        check("reload-cancel-keeps-mode", g_mode == MODE_ANSI);
        g_uiAnswer = 0;
    }

    // --- 4. external-change detection (WM_ACTIVATE path) ---
    {
        std::wstring ext = base + L"\\ext.dat";
        WriteBytes(ext, std::vector<char>{ 'O', 'N', 'E' });
        check("ext-load", LoadFile(ext));
        Sleep(60);
        WriteBytes(ext, std::vector<char>{ 'T', 'W', 'O' });
        SendMessageW(hwnd, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
        check("ext-silent-reload", DocText() == "TWO");
        // local edit + external change + IDYES -> reload to disk content
        Scim<void>(SCI_INSERTTEXT, 0, (sptr_t)"L");
        Sleep(60);
        WriteBytes(ext, std::vector<char>{ 'T', 'H', 'R' });
        g_uiAnswer = IDYES;
        SendMessageW(hwnd, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
        check("ext-reload-yes", DocText() == "THR");
        // local edit + external change + IDNO -> keep local content
        Scim<void>(SCI_INSERTTEXT, 0, (sptr_t)"K");
        Sleep(60);
        WriteBytes(ext, std::vector<char>{ 'F', 'O', 'U' });
        g_uiAnswer = IDNO;
        SendMessageW(hwnd, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
        check("ext-reload-no-keeps", DocText() == "KTHR");
        g_uiAnswer = 0;
    }

    // --- 5. save validation dialog (cancel aborts, ok writes broken json) ---
    {
        std::wstring js = base + L"\\save_v.json";
        WriteBytes(js, std::vector<char>{ '{', '"', 'a', '"', ':', '1', '}' });
        check("savev-load", LoadFile(js));
        Scim<void>(SCI_SETTEXT, 0, (sptr_t)"{\"a\":");
        g_uiAnswer = IDCANCEL;
        SendMessageW(hwnd, WM_COMMAND, IDM_SAVE, 0);
        std::vector<char> afterCancel;
        ReadFileBytes(js, afterCancel);
        check("savev-cancel-no-write", afterCancel.size() == 7 &&
            Scim<int>(SCI_GETMODIFY, 0, 0) == 1);
        g_uiAnswer = IDOK;
        SendMessageW(hwnd, WM_COMMAND, IDM_SAVE, 0);
        std::vector<char> afterOk;
        ReadFileBytes(js, afterOk);
        check("savev-ok-writes", afterOk.size() == 5 &&
            memcmp(&afterOk[0], "{\"a\":", 5) == 0 &&
            Scim<int>(SCI_GETMODIFY, 0, 0) == 0);
        g_uiAnswer = 0;
    }

    // --- 5b. zoom persists across LoadFile ---
    {
        ApplyZoom(3);
        std::wstring z = base + L"\\zoom.dat";
        WriteBytes(z, std::vector<char>{ 'z' });
        LoadFile(z);
        check("zoom-persist-load", Scim<int>(SCI_GETZOOM, 0, 0) == 3);
        SendMessageW(hwnd, WM_COMMAND, IDM_ZOOM_RESET, 0);
    }

    // --- 5c. save success reports format preserved on status bar ---
    {
        std::wstring pj = base + L"\\fmt.json";
        WriteBytes(pj, std::vector<char>{ '{', '"', 'a', '"', ':', '1', '}' });
        LoadFile(pj);
        Scim<void>(SCI_INSERTTEXT, 0, (sptr_t)" ");
        SendMessageW(hwnd, WM_COMMAND, IDM_SAVE, 0);
        std::wstring p2;
        part2(p2);
        check("save-format-report", p2.find(L"\u683c\u5f0f") != std::wstring::npos);
    }

    // --- 5d. read-only file: save must fail and keep content/modified ---
    {
        std::wstring ro = base + L"\\ro.dat";
        WriteBytes(ro, std::vector<char>{ 'R', 'O' });
        LoadFile(ro);
        Scim<void>(SCI_INSERTTEXT, 0, (sptr_t)"X");
        SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_READONLY);
        SendMessageW(hwnd, WM_COMMAND, IDM_SAVE, 0);
        std::vector<char> cur;
        ReadFileBytes(ro, cur);
        bool unchanged = cur.size() == 2 && cur[0] == 'R' && cur[1] == 'O';
        bool stillDirty = Scim<int>(SCI_GETMODIFY, 0, 0) == 1;
        SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_NORMAL);
        check("readonly-save-blocked", unchanged && stillDirty);
    }

    // --- 6. find counter + replace-all warning (dialog message path) ---
    {
        Scim<void>(SCI_SETTEXT, 0, (sptr_t)"foo foo foo");
        FINDREPLACEW fr2;
        ZeroMemory(&fr2, sizeof(fr2));
        fr2.lStructSize = sizeof(fr2);
        wchar_t fwhat[16], rwith[16];
        wcscpy(fwhat, L"foo"); wcscpy(rwith, L"bar");
        fr2.lpstrFindWhat = fwhat;
        fr2.lpstrReplaceWith = rwith;
        fr2.Flags = FR_DOWN;
        OnFindReplMessage(0, (LPARAM)&fr2);
        std::wstring p2;
        part2(p2);
        check("find-counter-3", p2.find(L"3") != std::wstring::npos);
        fr2.Flags = FR_DOWN | FR_REPLACEALL;
        g_uiAnswer = IDCANCEL;
        OnFindReplMessage(0, (LPARAM)&fr2);
        check("replaceall-cancel", DocText() == "foo foo foo");
        g_uiAnswer = IDOK;
        OnFindReplMessage(0, (LPARAM)&fr2);
        check("replaceall-ok", DocText() == "bar bar bar");
        g_uiAnswer = 0;
    }

    // --- 7. MRU submenu rebuild + click (menu path) ---
    {
        std::wstring m1 = base + L"\\mru1.txt";
        std::wstring m2 = base + L"\\mru2.txt";
        WriteBytes(m1, std::vector<char>{ '1' });
        WriteBytes(m2, std::vector<char>{ '2' });
        g_mruCount = 0;
        g_mru[0] = m1;
        g_mru[1] = m2;
        g_mruCount = 2;
        SendMessageW(hwnd, WM_INITMENUPOPUP, (WPARAM)g_mRecentMenu, 0);
        check("mru-menu-2-items", GetMenuItemCount(g_mRecentMenu) == 2);
        if (PromptSaveIfDirty()) LoadFile(m2);
        SendMessageW(hwnd, WM_COMMAND, IDM_MRU_BASE + 1, 0);
        check("mru-click-loads", g_filePath == m2);
    }

    // --- 8b. pathological path through ShowError must not smash the stack ---
    {
        std::wstring longPath = base + L"\\" + std::wstring(600, L'x') + L".txt";
        bool loaded = LoadFile(longPath);   // nonexistent -> ShowError path
        check("long-path-error-safe", !loaded && IsWindow(hwnd) != 0);
    }

    // --- 8. close guard: dirty + cancel keeps window alive ---
    {
        Scim<void>(SCI_INSERTTEXT, 0, (sptr_t)"Z");
        g_uiAnswer = IDCANCEL;
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
        check("close-cancel-alive", IsWindow(hwnd) != 0);
        g_uiAnswer = 0;
        Scim<void>(SCI_SETSAVEPOINT, 0, 0);   // clear dirty for exit
    }

    g_mruSuppress = mruWas;
    if (rf) {
        fwprintf(rf, L"TOTAL %d/%d %ls\r\n", pass, total,
            (pass == total ? L"PASS" : L"FAIL"));
        fflush(rf);
        fclose(rf);
        g_uiTestLog = nullptr;
    }
    return pass == total;
}

// ---------------------------------------------------------------------------

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLineIn, int nShow) {
    g_hInst = hInst;

    // Parse args before any window/Scintilla setup so test switches run
    // without paying for window class registration.
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) argv = CommandLineToArgvW(cmdLineIn, &argc);
    bool selftest = false;
    bool uicheck = false;
    bool uitest = false;
    std::wstring openFile;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--selftest") == 0) selftest = true;
        else if (_wcsicmp(argv[i], L"--uicheck") == 0) uicheck = true;
        else if (_wcsicmp(argv[i], L"--uitest") == 0) uitest = true;
        else if (openFile.empty() && argv[i][0] != L'-' &&
            PathFileExistsW(argv[i])) openFile = argv[i];
    }
    LocalFree(argv);

    InitCommonControls();

    Scintilla_RegisterClasses(hInst);
    g_findReplMsg = RegisterWindowMessageW(FINDMSGSTRING);

    // register the main window class (required before CreateWindowExW)
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 2;

    HWND hwnd = CreateWindowExW(0, kClassName, kAppTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 650, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    g_hwnd = hwnd;

    // menu
    HMENU menu = CreateMenu();
    HMENU mFile = CreatePopupMenu();
    HMENU mRecent = CreatePopupMenu();     // populated on WM_INITMENUPOPUP
    g_mRecentMenu = mRecent;
    AppendMenuW(mFile, MF_POPUP, (UINT_PTR)mRecent, L"\u6700\u8fd1\u6253\u5f00(&R)");
    AppendMenuW(mFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mFile, MF_STRING, IDM_OPEN,  L"\u6253\u5f00(&O)\tCtrl+O");
    AppendMenuW(mFile, MF_STRING, IDM_SAVE,  L"\u4fdd\u5b58(&S)\tCtrl+S");
    AppendMenuW(mFile, MF_STRING, IDM_SAVEAS, L"\u53e6\u5b58\u4e3a(&A)...");
    AppendMenuW(mFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mFile, MF_STRING, IDM_EXIT, L"\u9000\u51fa(&X)");
    HMENU mEdit = CreatePopupMenu();
    AppendMenuW(mEdit, MF_STRING, IDM_UNDO, L"\u64a4\u9500(&U)\tCtrl+Z");
    AppendMenuW(mEdit, MF_STRING, IDM_REDO, L"\u91cd\u505a(&R)\tCtrl+Y");
    AppendMenuW(mEdit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mEdit, MF_STRING, IDM_CUT, L"\u526a\u5207(&T)\tCtrl+X");
    AppendMenuW(mEdit, MF_STRING, IDM_COPY, L"\u590d\u5236(&C)\tCtrl+C");
    AppendMenuW(mEdit, MF_STRING, IDM_PASTE, L"\u7c98\u8d34(&P)\tCtrl+V");
    AppendMenuW(mEdit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mEdit, MF_STRING, IDM_SELECTALL, L"\u5168\u9009(&A)\tCtrl+A");
    AppendMenuW(mEdit, MF_SEPARATOR, 0, nullptr);
    HMENU mReload = CreatePopupMenu();
    AppendMenuW(mReload, MF_STRING, IDM_RELOAD_UTF8, L"UTF-8(&8)");
    AppendMenuW(mReload, MF_STRING, IDM_RELOAD_GBK, L"GBK (ANSI)(&G)");
    AppendMenuW(mReload, MF_STRING, IDM_RELOAD_UTF16, L"UTF-16LE(&1)");
    AppendMenuW(mEdit, MF_POPUP, (UINT_PTR)mReload, L"\u6309\u5176\u4ed6\u7f16\u7801\u91cd\u65b0\u52a0\u8f7d(&R)");
    HMENU mSearch = CreatePopupMenu();
    AppendMenuW(mSearch, MF_STRING, IDM_FIND, L"\u67e5\u627e(&F)...\tCtrl+F");
    AppendMenuW(mSearch, MF_STRING, IDM_FINDNEXT, L"\u67e5\u627e\u4e0b\u4e00\u4e2a\tF3");
    HMENU mView = CreatePopupMenu();
    AppendMenuW(mView, MF_STRING, IDM_VIEW_EOL, L"\u663e\u793a\u884c\u5c3e\u7b26(&E)");
    AppendMenuW(mView, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mView, MF_STRING, IDM_ZOOM_IN, L"\u653e\u5927\u5b57\u53f7(&I)\tCtrl++");
    AppendMenuW(mView, MF_STRING, IDM_ZOOM_OUT, L"\u7f29\u5c0f\u5b57\u53f7(&O)\tCtrl+-");
    AppendMenuW(mView, MF_STRING, IDM_ZOOM_RESET, L"\u91cd\u7f6e\u5b57\u53f7(&0)\tCtrl+0");
    HMENU mHelp = CreatePopupMenu();
    AppendMenuW(mHelp, MF_STRING, IDM_ABOUT, L"\u5173\u4e8e(&A)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)mFile, L"\u6587\u4ef6(&F)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)mEdit, L"\u7f16\u8f91(&E)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)mSearch, L"\u641c\u7d22(&S)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)mView, L"\u67e5\u770b(&V)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)mHelp, L"\u5e2e\u52a9(&H)");
    SetMenu(hwnd, menu);
    DrawMenuBar(hwnd);

    // accelerators
    ACCEL acc[] = {
        { FCONTROL | FVIRTKEY, 'O', IDM_OPEN },
        { FCONTROL | FVIRTKEY, 'S', IDM_SAVE },
        { FCONTROL | FVIRTKEY, 'F', IDM_FIND },
        { FVIRTKEY, VK_F3, IDM_FINDNEXT },
        { FCONTROL | FVIRTKEY, 'Z', IDM_UNDO },
        { FCONTROL | FVIRTKEY, 'Y', IDM_REDO },
        { FCONTROL | FVIRTKEY, VK_OEM_PLUS, IDM_ZOOM_IN },
        { FCONTROL | FVIRTKEY, VK_OEM_MINUS, IDM_ZOOM_OUT },
        { FCONTROL | FVIRTKEY, '0', IDM_ZOOM_RESET },
    };
    HACCEL hAcc = CreateAcceleratorTableW(acc, 9);

    // V5: load recent-files cache for every path that may open files; writes
    // are suppressed in selftest (g_mruSuppress), uicheck accumulates normally.
    LoadMru();
    CheckMenuItem(menu, IDM_VIEW_EOL, MF_BYCOMMAND | MF_CHECKED);

    if (selftest) {
        ShowWindow(hwnd, SW_HIDE);
        bool ok = RunSelftest();
        DestroyWindow(hwnd);
        return ok ? 0 : 1;
    }
    if (uicheck) {
        bool ok = RunUiCheck(hwnd, openFile);
        return ok ? 0 : 1;
    }
    if (uitest) {
        g_uiTest = true;
        ShowWindow(hwnd, SW_HIDE);
        bool ok = RunUiTest(hwnd);
        g_uiTest = false;
        DestroyWindow(hwnd);
        return ok ? 0 : 1;
    }

    if (!openFile.empty()) LoadFile(openFile);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (g_hwndFind && IsDialogMessageW(g_hwndFind, &msg)) continue;
        if (hAcc && TranslateAcceleratorW(hwnd, hAcc, &msg)) continue;
        if (msg.message == g_findReplMsg) {
            OnFindReplMessage(msg.wParam, msg.lParam);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
