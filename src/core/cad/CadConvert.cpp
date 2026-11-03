#include "core/cad/CadConvert.h"

#include <windows.h>
#include <vector>
#include "core/filesystem/FileSystem.h"

namespace mzr::cad {

std::wstring FindDwgConverter() {
    const std::wstring p = fs::ExeFolder() + L"\\cad\\dwg2dxf.exe";
    return fs::Exists(p) && !fs::IsDirectory(p) ? p : std::wstring();
}

Result<bool> ConvertDwgToDxf(const std::wstring& converter, const std::wstring& dwgPath, const std::wstring& dxfPath,
                             const std::function<bool()>& cancelled, const ConvertLimits& limits) {
    if (converter.empty())
        return ErrorInfo{ErrorCode::Internal, L"The DWG drawing viewer is not installed.", L"cad\\dwg2dxf.exe missing"};
    if (cancelled && cancelled()) return ErrorInfo{ErrorCode::Cancelled, L"Cancelled.", L""};
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(dwgPath.c_str(), GetFileExInfoStandard, &fa))
        return ErrorInfo{ErrorCode::Io, L"The drawing could not be prepared.", L"DWG temp file missing"};
    const uint64_t size = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    if (size > limits.maxDwgBytes)
        return ErrorInfo{ErrorCode::SizeLimitExceeded, L"This drawing is too large for the preview.", L"DWG bytes " + std::to_wstring(size)};

    // Job: memory + no children + killed with the job handle.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return ErrorInfo{ErrorCode::Internal, L"The drawing converter could not be started.", L"CreateJobObject"};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    li.BasicLimitInformation.ActiveProcessLimit = 1;
    li.ProcessMemoryLimit = static_cast<SIZE_T>(limits.maxProcessBytes);
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                             JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                             JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui));

    // Output to NUL (nobody reads it; a pipe could fill up and block the helper).
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = nul; si.StartupInfo.hStdOutput = nul; si.StartupInfo.hStdError = nul;
    // Only the NUL handle is inherited, never other handles of our process (open archives ...).
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    bool attrsOk = nul != INVALID_HANDLE_VALUE && InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                   UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &nul, sizeof(nul), nullptr, nullptr);
    if (attrsOk) si.lpAttributeList = attrs;

    // dwg2dxf -y -o out.dxf in.dwg   (working folder = the private temp folder)
    const std::wstring dir = dxfPath.substr(0, dxfPath.find_last_of(L'\\'));
    std::wstring cmd = L"\"" + converter + L"\" -y -o \"" + dxfPath + L"\" \"" + dwgPath + L"\"";
    PROCESS_INFORMATION pi{};
    const BOOL started = attrsOk && CreateProcessW(converter.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT,
                                        nullptr, dir.c_str(), &si.StartupInfo, &pi);
    const DWORD startErr = GetLastError();
    if (attrsOk) DeleteProcThreadAttributeList(attrs);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(job);
        return ErrorInfo{ErrorCode::Internal, L"The drawing converter could not be started.", L"CreateProcess " + std::to_wstring(startErr)};
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {             // never run it unlimited
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(job);
        return ErrorInfo{ErrorCode::Internal, L"The drawing converter could not be started safely.", L"AssignProcessToJobObject"};
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    const ULONGLONG start = GetTickCount64();
    bool timedOut = false, userCancel = false;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
        if (cancelled && cancelled()) { userCancel = true; break; }
        if (GetTickCount64() - start > limits.timeoutMs) { timedOut = true; break; }
    }
    DWORD code = 0;
    if (timedOut || userCancel) TerminateJobObject(job, 1);
    else GetExitCodeProcess(pi.hProcess, &code);
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hProcess);
    CloseHandle(job);

    if (userCancel) return ErrorInfo{ErrorCode::Cancelled, L"Cancelled.", L""};
    if (timedOut) return ErrorInfo{ErrorCode::Internal, L"The drawing took too long to convert and was skipped.",
                                   L"dwg2dxf timeout " + std::to_wstring(limits.timeoutMs) + L" ms"};
    // dwg2dxf returns non-zero for warnings too: the DXF file decides.
    WIN32_FILE_ATTRIBUTE_DATA out{};
    if (!GetFileAttributesExW(dxfPath.c_str(), GetFileExInfoStandard, &out) || (out.nFileSizeLow == 0 && out.nFileSizeHigh == 0))
        return ErrorInfo{ErrorCode::Corrupted, L"This DWG drawing could not be read (it may be damaged or use an unsupported version).",
                         L"dwg2dxf exit " + std::to_wstring(code)};
    return true;
}

} // namespace mzr::cad
