# MECHANO ZIP READER - root build file
# Build:  .\build.ps1            (7z.dll if needed + Release build + tests)
#         .\build.ps1 -Config Debug
#         .\build.ps1 -Arch Win32
cmake_minimum_required(VERSION 3.25)
project(MechanoZipReader VERSION 0.9.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "MECHANO ZIP READER targets Windows only (for now).")
endif()

set(SEVENZIP_DIR ${CMAKE_SOURCE_DIR}/third_party/7zip-26.05)
set(PUGIXML_DIR ${CMAKE_SOURCE_DIR}/third_party/pugixml-1.16)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(SEVENZIP_DLL ${SEVENZIP_DIR}/CPP/7zip/Bundles/Format7zF/x64/7z.dll)
else()
  set(SEVENZIP_DLL ${SEVENZIP_DIR}/CPP/7zip/Bundles/Format7zF/x86/7z.dll)
endif()
if(NOT EXISTS ${SEVENZIP_DLL})
  message(FATAL_ERROR "7z.dll not built yet: ${SEVENZIP_DLL}\nRun .\\build.ps1 (it builds 7z.dll first).")
endif()

# Unicode everywhere, Windows 10+ API surface, no min/max macros.
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)

if(MSVC)
  # /W4 warnings, /permissive- strict standard, /utf-8 so Arabic literals compile correctly,
  # /guard:cf + /sdl hardening, static CRT for a dependency-free exe.
  # Third-party headers (7-Zip SDK) are "external": no warnings from them.
  # Windows SDK headers (<...>, incl. C++/WinRT) are external too.
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /guard:cf /sdl /EHsc /external:W0 /external:anglebrackets)
  add_link_options(/guard:cf /DYNAMICBASE /NXCOMPAT)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  string(APPEND CMAKE_CXX_FLAGS_RELEASE " /O2 /Gy /GL")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /OPT:REF /OPT:ICF /LTCG")
  string(APPEND CMAKE_STATIC_LINKER_FLAGS_RELEASE " /LTCG")
endif()

# ---------------------------------------------------------------- core library
# Everything testable lives here (no UI).
add_library(mzr_core STATIC
  src/utils/Result.h
  src/utils/ErrorInfo.h
  src/utils/Format.h
  src/utils/Format.cpp
  src/utils/Log.h
  src/utils/Log.cpp
  src/utils/Settings.h
  src/utils/Settings.cpp
  src/core/security/PathSanitizer.h
  src/core/security/PathSanitizer.cpp
  src/core/security/ExtractGuard.h
  src/core/security/ExtractGuard.cpp
  src/core/filesystem/FileSystem.h
  src/core/filesystem/FileSystem.cpp
  src/core/archive/ArchiveTypes.h
  src/core/archive/ArchiveIndex.h
  src/core/archive/ArchiveIndex.cpp
  src/core/archive/SmartExtract.h
  src/core/archive/SmartExtract.cpp
  src/core/archive/ArchiveEngine.h
  src/core/archive/ArchiveEngine.cpp
  src/core/preview/PreviewDecode.h
  src/core/preview/PreviewDecode.cpp
  src/core/preview/PdfRender.h
  src/core/preview/PdfRender.cpp
  src/core/preview/Ooxml.h
  src/core/preview/Ooxml.cpp
  src/core/preview/Dwg.h
  src/core/preview/Dwg.cpp
  src/core/preview/Revit.h
  src/core/preview/Revit.cpp
  src/core/preview/Ifc.h
  src/core/preview/Ifc.cpp
  src/core/preview/Navisworks.h
  src/core/preview/Navisworks.cpp
  src/core/preview/AudioTags.h
  src/core/preview/AudioTags.cpp
  src/core/update/Updater.h
  src/core/update/Updater.cpp
  src/core/cad/Cad.h
  src/core/cad/CadDxf.cpp
  src/core/cad/CadRender.cpp
  src/core/cad/CadConvert.h
  src/core/cad/CadConvert.cpp
  ${PUGIXML_DIR}/src/pugixml.cpp
)
# pugixml is third-party: no warnings from it.
set_source_files_properties(${PUGIXML_DIR}/src/pugixml.cpp PROPERTIES COMPILE_OPTIONS "/W0")
target_include_directories(mzr_core PUBLIC src)
target_include_directories(mzr_core SYSTEM PRIVATE ${SEVENZIP_DIR}/CPP ${PUGIXML_DIR}/src)
target_link_libraries(mzr_core PUBLIC ole32 oleaut32 uuid shell32 windowscodecs windowsapp msimg32)

# Copies 7z.dll next to a target after every build.
function(mzr_copy_7z target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SEVENZIP_DLL} $<TARGET_FILE_DIR:${target}>/7z.dll)
  # F7 trial: LibreDWG dwg2dxf (GPL-3, separate process) in a "cad" folder next to the exe.
  set(LIBREDWG_BIN ${CMAKE_SOURCE_DIR}/third_party/libredwg-0.14.8739/win64)
  if(EXISTS ${LIBREDWG_BIN}/dwg2dxf.exe)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target}>/cad
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIBREDWG_BIN}/dwg2dxf.exe ${LIBREDWG_BIN}/libredwg-0.dll ${LIBREDWG_BIN}/libiconv-2.dll $<TARGET_FILE_DIR:${target}>/cad/)
  endif()
endfunction()

# ---------------------------------------------------------------- application
add_executable(MechanoZip WIN32
  src/app/main.cpp
  src/ui/MainWindow.h
  src/ui/MainWindow.cpp
  src/ui/Dialogs.h
  src/ui/Dialogs.cpp
  src/ui/MediaPlayer.h
  src/ui/MediaPlayer.cpp
  src/ui/MediaBar.h
  src/ui/MediaBar.cpp
  src/ui/SheetView.h
  src/ui/SheetView.cpp
  src/ui/Theme.h
  src/ui/Theme.cpp
  src/workers/Worker.h
  src/workers/Worker.cpp
  assets/app.rc
)
target_link_libraries(MechanoZip PRIVATE mzr_core comctl32 uxtheme dwmapi shlwapi msimg32 mfplay mfplat)
# "assets/version.h" is shared by the code, app.rc and the installer.
target_include_directories(MechanoZip PRIVATE ${CMAKE_SOURCE_DIR})
# Our own manifest is embedded through app.rc, so stop the linker from adding a second one.
target_link_options(MechanoZip PRIVATE /MANIFEST:NO)
# Phase 6: video / audio DLLs load only on the first media preview (faster start).
# Safe with SetDefaultDllDirectories: delay loads also come only from System32.
target_link_options(MechanoZip PRIVATE /DELAYLOAD:mfplay.dll /DELAYLOAD:mfplat.dll /DELAYLOAD:msimg32.dll /DELAYLOAD:gdiplus.dll)
target_link_libraries(MechanoZip PRIVATE delayimp)
mzr_copy_7z(MechanoZip)

# ---------------------------------------------------------------- tests
enable_testing()
add_executable(mzr_tests
  tests/mzr_test.h
  tests/test_main.cpp
  tests/TestFiles.h
  tests/unit/test_path_sanitizer.cpp
  tests/unit/test_extract_guard.cpp
  tests/unit/test_archive_index.cpp
  tests/unit/test_smart_extract.cpp
  tests/unit/test_preview.cpp
  tests/unit/test_office.cpp
  tests/unit/test_dwg.cpp
  tests/unit/test_cad.cpp
  tests/unit/test_revit.cpp
  tests/unit/test_ifc.cpp
  tests/unit/test_navisworks.cpp
  tests/unit/test_update.cpp
  tests/stress/test_memory.cpp
  tests/security/test_zip_slip_names.cpp
  tests/security/test_malicious_archives.cpp
  tests/security/test_phase7.cpp
  tests/integration/test_roundtrip.cpp
)
target_link_libraries(mzr_tests PRIVATE mzr_core)
mzr_copy_7z(mzr_tests)
add_test(NAME all COMMAND mzr_tests)
