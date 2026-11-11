#include "code_box.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <iterator>
#include <vector>
#endif

namespace gsr::code_box {

#ifdef _WIN32
namespace {

constexpr WORD kEdit = 100, kMessage = 101, kNoId = 0xFFFFu;
constexpr WORD kButtonClass = 0x0080u, kEditClass = 0x0081u, kStaticClass = 0x0082u;

std::wstring wide(const std::string& s) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), size);
    return w;
}

// An in-memory dialog template (DLGTEMPLATE and DLGITEMTEMPLATE, each
// DWORD-aligned), in dialog units.
struct Template {
    std::vector<WORD> words;
    void dword(DWORD d) {
        words.push_back(LOWORD(d));
        words.push_back(HIWORD(d));
    }
    void string(const std::wstring& s) {
        for (const wchar_t c : s) words.push_back(static_cast<WORD>(c));
        words.push_back(0);
    }
    void align() {
        if (words.size() % 2) words.push_back(0);
    }
    void item(DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const std::wstring& title) {
        align();
        dword(style | WS_CHILD | WS_VISIBLE);
        dword(0);
        for (const short v : {x, y, cx, cy}) words.push_back(static_cast<WORD>(v));
        words.push_back(id);
        words.push_back(0xFFFFu);
        words.push_back(cls);
        string(title);
        words.push_back(0);  // no creation data
    }
};

struct State {
    const Text* text;
    run_code::Run* out;
};

INT_PTR CALLBACK box_proc(HWND box, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(box, DWLP_USER, lp);
        SendDlgItemMessageW(box, kEdit, EM_LIMITTEXT, 32, 0);
        SetFocus(GetDlgItem(box, kEdit));
        return FALSE;  // the focus is set
    case WM_COMMAND: {
        const auto* state = reinterpret_cast<const State*>(GetWindowLongPtrW(box, DWLP_USER));
        const WORD id = LOWORD(wp);
        if (id == IDCANCEL) {
            EndDialog(box, 0);
            return TRUE;
        }
        if (id == IDOK) {
            wchar_t typed[64]{};
            GetDlgItemTextW(box, kEdit, typed, static_cast<int>(std::size(typed)));
            std::string code;
            for (const wchar_t* c = typed; *c; ++c) code += *c < 0x80 ? static_cast<char>(*c) : '?';
            if (run_code::decode_typed(code, state->out)) {
                EndDialog(box, 1);
                return TRUE;
            }
            SetDlgItemTextW(box, kMessage, wide(state->text->invalid).c_str());
            const HWND edit = GetDlgItem(box, kEdit);
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, -1);
            return TRUE;
        }
        if (id == kEdit && HIWORD(wp) == EN_CHANGE) SetDlgItemTextW(box, kMessage, L"");
        return FALSE;
    }
    default: return FALSE;
    }
}

// The game's window, so the box sits above it, fullscreen too.
BOOL CALLBACK find_game_window(HWND window, LPARAM found) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
    *reinterpret_cast<HWND*>(found) = window;
    return FALSE;
}

}  // namespace

bool available() { return true; }

bool ask(const Text& text, run_code::Run* out) {
    HWND owner = nullptr;
    EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&owner));

    Template t;
    t.dword(DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    t.dword(0);
    t.words.push_back(5);  // items
    for (const short v : {0, 0, 180, 72}) t.words.push_back(static_cast<WORD>(v));
    t.words.push_back(0);  // no menu
    t.words.push_back(0);  // the standard dialog class
    t.string(wide(text.title));
    t.words.push_back(9);  // points
    t.string(L"Segoe UI");
    t.item(SS_LEFT, 7, 7, 166, 9, kNoId, kStaticClass, wide(text.prompt));
    t.item(ES_AUTOHSCROLL | ES_UPPERCASE | WS_BORDER | WS_TABSTOP, 7, 19, 166, 14, kEdit, kEditClass, L"");
    t.item(SS_LEFT, 7, 37, 166, 9, kMessage, kStaticClass, L"");
    t.item(BS_DEFPUSHBUTTON | WS_TABSTOP, 69, 51, 50, 14, IDOK, kButtonClass, wide(text.ok));
    t.item(BS_PUSHBUTTON | WS_TABSTOP, 123, 51, 50, 14, IDCANCEL, kButtonClass, wide(text.cancel));

    State state{&text, out};
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(t.words.data()),
                                   owner, box_proc, reinterpret_cast<LPARAM>(&state)) == 1;
}
#else
bool available() { return false; }
bool ask(const Text&, run_code::Run*) { return false; }
#endif

}  // namespace gsr::code_box
