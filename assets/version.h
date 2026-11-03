#pragma once
// ONE place for the version: used by app.rc (file properties), the app (F1 / About) and the
// installer script (read back from the built exe). Change it here only.
#define MZR_VER_MAJOR 0
#define MZR_VER_MINOR 9
#define MZR_VER_PATCH 0
#define MZR_VER_BUILD 0
#define MZR_VER_LABEL "Beta"            // "" for a final release

#define MZR_STR2(x) #x
#define MZR_STR(x) MZR_STR2(x)
#define MZR_VERSION_STR MZR_STR(MZR_VER_MAJOR) "." MZR_STR(MZR_VER_MINOR) "." MZR_STR(MZR_VER_PATCH)
#define MZR_VERSION_WSTR L"" MZR_VERSION_STR
#define MZR_VERSION_FULL_WSTR L"" MZR_VERSION_STR L" " MZR_VER_LABEL

#define MZR_PRODUCT_NAME "Mechano Reader"
#define MZR_AUTHOR "Eng. Eslam Abdelqader"
#define MZR_COPYRIGHT "Copyright (C) 2026 Eng. Eslam Abdelqader. MIT License."
