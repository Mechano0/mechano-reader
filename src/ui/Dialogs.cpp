#include "ui/Dialogs.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <shlobj.h>
#include "../../assets/resource.h"
#include "ui/Theme.h"

namespace mzr::ui {
namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
};

std::wstring ItemPath(IShellItem* item) {
    PWSTR s = nullptr;
    std::wstring out;
    if (item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &s))) { out = s; CoTaskMemFree(s); }
    return out;
}

void SetFolder(IFileDialog* dlg, const std::wstring& folder) {
    if (folder.empty()) return;
    Com<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item.p)))) dlg->SetFolder(item.p);
}

HINSTANCE Inst() { return GetModuleHandleW(nullptr); }

} // namespace

std::vector<std::wstring> PickArchivesToOpen(HWND owner) {
    std::vector<std::wstring> out;
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg.p)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    const COMDLG_FILTERSPEC types[] = {
        {L"Archives", L"*.zip;*.7z;*.rar;*.tar;*.gz;*.tgz;*.bz2;*.tbz2;*.xz;*.txz;*.zst;*.iso;*.cab;*.wim;*.arj;*.lzh;*.001;*.jar;*.apk"},
        {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"Open archive (select several to extract them all at once)");
    if (dlg->Show(owner) != S_OK) return out;
    Com<IShellItemArray> items;
    if (FAILED(dlg->GetResults(&items.p))) return out;
    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        Com<IShellItem> it;
        if (SUCCEEDED(items->GetItemAt(i, &it.p))) {
            auto p = ItemPath(it.p);
            if (!p.empty()) out.push_back(p);
        }
    }
    return out;
}

std::optional<std::wstring> PickFolder(HWND owner, const wchar_t* title, const std::wstring& initial) {
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg.p)))) return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);
    SetFolder(dlg.p, initial);
    if (dlg->Show(owner) != S_OK) return {};
    Com<IShellItem> item;
    if (FAILED(dlg->GetResult(&item.p))) return {};
    auto p = ItemPath(item.p);
    if (p.empty()) return {};
    return p;
}

std::vector<std::wstring> PickFilesToAdd(HWND owner) {
    std::vector<std::wstring> out;
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg.p)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Add files (tip: you can also drag files and folders onto the window)");
    if (dlg->Show(owner) != S_OK) return out;
    Com<IShellItemArray> items;
    if (FAILED(dlg->GetResults(&items.p))) return out;
    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        Com<IShellItem> it;
        if (SUCCEEDED(items->GetItemAt(i, &it.p))) {
            auto p = ItemPath(it.p);
            if (!p.empty()) out.push_back(p);
        }
    }
    return out;
}

std::optional<NewArchiveChoice> PickNewArchive(HWND owner, const std::wstring& suggestedName,
                                               const std::wstring& initialFolder) {
    Com<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg.p)))) return {};
    const COMDLG_FILTERSPEC types[] = {{L"ZIP archive (opens everywhere)", L"*.zip"},
                                       {L"7z archive (smaller files)", L"*.7z"}};
    dlg->SetFileTypes(2, types);
    dlg->SetFileTypeIndex(1);
    dlg->SetDefaultExtension(L"zip");
    dlg->SetTitle(L"Create new archive");
    dlg->SetFileName(suggestedName.c_str());
    SetFolder(dlg.p, initialFolder);

    // Extra controls inside the standard dialog.
    enum { kGroupLevel = 1, kLevel, kGroupPw, kPassword };
    Com<IFileDialogCustomize> cust;
    if (SUCCEEDED(dlg->QueryInterface(IID_PPV_ARGS(&cust.p)))) {
        cust->StartVisualGroup(kGroupLevel, L"Compression:");
        cust->AddComboBox(kLevel);
        cust->AddControlItem(kLevel, 0, L"Store (no compression)");
        cust->AddControlItem(kLevel, 1, L"Fastest");
        cust->AddControlItem(kLevel, 3, L"Fast");
        cust->AddControlItem(kLevel, 5, L"Normal");
        cust->AddControlItem(kLevel, 9, L"Maximum");
        cust->SetSelectedControlItem(kLevel, 5);
        cust->EndVisualGroup();
        cust->StartVisualGroup(kGroupPw, L"Password (optional):");
        cust->AddEditBox(kPassword, L"");
        cust->EndVisualGroup();
    }
    if (dlg->Show(owner) != S_OK) return {};
    Com<IShellItem> item;
    if (FAILED(dlg->GetResult(&item.p))) return {};

    NewArchiveChoice c;
    c.path = ItemPath(item.p);
    if (c.path.empty()) return {};
    UINT typeIndex = 1;
    dlg->GetFileTypeIndex(&typeIndex);
    const auto dot = c.path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : c.path.substr(dot + 1);
    CharLowerBuffW(ext.data(), static_cast<DWORD>(ext.size()));
    c.settings.format = (ext == L"7z" || (ext != L"zip" && typeIndex == 2)) ? L"7z" : L"zip";
    if (cust.p) {
        DWORD level = 5;
        if (SUCCEEDED(cust->GetSelectedControlItem(kLevel, &level))) c.settings.level = static_cast<archive::CompressionLevel>(level);
        WCHAR* pw = nullptr;
        if (SUCCEEDED(cust->GetEditBoxText(kPassword, &pw)) && pw) {
            c.settings.password = pw;
            SecureZeroMemory(pw, wcslen(pw) * sizeof(wchar_t));
            CoTaskMemFree(pw);
        }
        c.settings.encryptHeaders = c.settings.format == L"7z" && !c.settings.password.empty();
    }
    return c;
}

// ------------------------------------------------------------- password dialog

namespace {
struct PwState {
    std::wstring archiveName;
    std::wstring result;
};

INT_PTR CALLBACK PwProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        auto* st = reinterpret_cast<PwState*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        std::wstring t = L"Enter the password for \"" + st->archiveName + L"\":";
        SetDlgItemTextW(dlg, IDC_PW_TEXT, t.c_str());
        // theme: cream dialog, coral OK, flat Cancel, Windows 11 caption colours
        theme::ApplyTitleBar(dlg);
        theme::MakeFlatButton(GetDlgItem(dlg, IDOK), theme::ButtonStyle::Primary);
        theme::MakeFlatButton(GetDlgItem(dlg, IDCANCEL), theme::ButtonStyle::Secondary);
        SetFocus(GetDlgItem(dlg, IDC_PW_EDIT));
        return FALSE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wp), theme::kText);
        SetBkColor(reinterpret_cast<HDC>(wp), theme::kBack);
        return reinterpret_cast<INT_PTR>(theme::Brush(theme::kBack));
    case WM_DRAWITEM:
        if (theme::DrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp))) { SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE); return TRUE; }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PW_SHOW: {
            const bool show = IsDlgButtonChecked(dlg, IDC_PW_SHOW) == BST_CHECKED;
            HWND e = GetDlgItem(dlg, IDC_PW_EDIT);
            SendMessageW(e, EM_SETPASSWORDCHAR, show ? 0 : 0x25CF, 0);
            InvalidateRect(e, nullptr, TRUE);
            return TRUE;
        }
        case IDOK: {
            auto* st = reinterpret_cast<PwState*>(GetWindowLongPtrW(dlg, DWLP_USER));
            HWND e = GetDlgItem(dlg, IDC_PW_EDIT);
            const int n = GetWindowTextLengthW(e);
            st->result.assign(static_cast<size_t>(n) + 1, L'\0');
            GetWindowTextW(e, st->result.data(), n + 1);
            st->result.resize(static_cast<size_t>(n));
            SetWindowTextW(e, L"");             // do not leave the password in the control
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}
} // namespace

std::optional<std::wstring> AskPassword(HWND owner, const std::wstring& archiveName) {
    PwState st{archiveName, {}};
    if (DialogBoxParamW(Inst(), MAKEINTRESOURCEW(IDD_PASSWORD), owner, PwProc, reinterpret_cast<LPARAM>(&st)) != IDOK)
        return std::nullopt;
    std::optional<std::wstring> out = st.result;
    SecureZeroMemory(st.result.data(), st.result.size() * sizeof(wchar_t));
    return out;
}

// ------------------------------------------------------------- messages

void ShowError(HWND owner, const std::wstring& title, const ErrorInfo& e) {
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.hwndParent = owner;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    tc.pszWindowTitle = L"Mechano Reader";
    tc.pszMainIcon = TD_ERROR_ICON;
    tc.pszMainInstruction = title.c_str();
    tc.pszContent = e.friendly.c_str();
    tc.pszExpandedInformation = e.details.empty() ? nullptr : e.details.c_str();
    tc.pszCollapsedControlText = L"Details";
    tc.pszExpandedControlText = L"Hide details";
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    TaskDialogIndirect(&tc, nullptr, nullptr, nullptr);
}

void ShowReport(HWND owner, const std::wstring& title, const std::wstring& doneText, const archive::OperationReport& r) {
    std::wstring details;
    size_t shown = 0;
    for (const auto& i : r.issues) {
        if (++shown > 200) { details += L"\n... and " + std::to_wstring(r.issues.size() - 200) + L" more."; break; }
        details += (i.path.empty() ? L"" : i.path + L"\n    ") + i.friendly;
        if (!i.details.empty()) details += L"  [" + i.details + L"]";
        details += L"\n";
    }
    std::wstring content = doneText;
    if (r.wrongPassword) content += L"\n\nThe password seems to be wrong.";
    if (!r.issues.empty()) content += L"\n\n" + std::to_wstring(r.issues.size()) + L" problem(s) - see Details.";

    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.hwndParent = owner;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    tc.pszWindowTitle = L"Mechano Reader";
    tc.pszMainIcon = r.cancelled ? TD_INFORMATION_ICON : r.issues.empty() ? TD_INFORMATION_ICON : TD_WARNING_ICON;
    tc.pszMainInstruction = title.c_str();
    tc.pszContent = content.c_str();
    tc.pszExpandedInformation = details.empty() ? nullptr : details.c_str();
    tc.pszCollapsedControlText = L"Details";
    tc.pszExpandedControlText = L"Hide details";
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    TaskDialogIndirect(&tc, nullptr, nullptr, nullptr);
}

bool Confirm(HWND owner, const std::wstring& title, const std::wstring& text, const std::wstring& detail) {
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.hwndParent = owner;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = L"Mechano Reader";
    tc.pszMainIcon = TD_WARNING_ICON;
    tc.pszMainInstruction = title.c_str();
    tc.pszContent = text.c_str();
    tc.pszExpandedInformation = detail.empty() ? nullptr : detail.c_str();
    tc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    tc.nDefaultButton = IDNO;
    int button = 0;
    TaskDialogIndirect(&tc, &button, nullptr, nullptr);
    return button == IDYES;
}

} // namespace mzr::ui
