#pragma once
// Standard dialogs (Windows common item dialogs + TaskDialog for errors).
#include <windows.h>
#include <optional>
#include <string>
#include <vector>
#include "core/archive/ArchiveTypes.h"
#include "utils/ErrorInfo.h"

namespace mzr::ui {

// One or more archives (several = a batch: extract them all, or put them in one new archive).
std::vector<std::wstring>   PickArchivesToOpen(HWND owner);
std::optional<std::wstring> PickFolder(HWND owner, const wchar_t* title, const std::wstring& initial = {});
std::vector<std::wstring>   PickFilesToAdd(HWND owner);

// Save dialog for a new archive with a "Compression" combo and a "Password" box.
struct NewArchiveChoice {
    std::wstring                  path;
    archive::CompressionSettings  settings;
};
std::optional<NewArchiveChoice> PickNewArchive(HWND owner, const std::wstring& suggestedName,
                                               const std::wstring& initialFolder);

std::optional<std::wstring> AskPassword(HWND owner, const std::wstring& archiveName);

// Friendly message + expandable "Details" (brief section 26).
void ShowError(HWND owner, const std::wstring& title, const ErrorInfo& e);
// Summary of an extract / test / compress run.
void ShowReport(HWND owner, const std::wstring& title, const std::wstring& doneText,
                const archive::OperationReport& r);
bool Confirm(HWND owner, const std::wstring& title, const std::wstring& text, const std::wstring& detail = {});

} // namespace mzr::ui
