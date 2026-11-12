#pragma once
// The signed PawnIO module files (PawnIO.Modules, LGPL-2.1; core/pawnio.h loads them into the driver): where they
// are looked up and the download that fetches them. See docs/notes/pawnio.md.
//
// Lookup, per module: the per-user download folder (%LOCALAPPDATA%\ez-pc-bench\pawnio\<version>\), then a
// "pawnio" folder next to the exe (portable copies). Nothing is embedded in the exes.
//
// The download only happens on an explicit user action (GUI: Settings "Download & install" / "Download modules";
// CLI: --pawnio-modules): the release zip from github.com over HTTPS, its SHA-256 checked against the constant
// below, then only the module files the app loads plus the license text are unpacked (core/unzip.h).
#include "core/types.h"

// PawnIO.Modules release the app downloads. Bump PAWNIO_MODULES_VERSION and PAWNIO_MODULES_SHA256 together
// (CLAUDE.md, "PawnIO modules"): the URL is built from the version, the hash is that release zip's SHA-256.
constexpr const char* PAWNIO_MODULES_VERSION = "0.2.11";
constexpr const char* PAWNIO_MODULES_SHA256 = "43608cb89bc84247fef1368a139013f7d043e17db6d6c8dfc9b46bf0905a81f4";

// https://github.com/namazso/PawnIO.Modules/releases/download/<version>/release_<version, '.' -> '_'>.zip
void pawnio_modules_url(char* out, u32 size);
// The per-user download folder (no trailing backslash). False when %LOCALAPPDATA% is unknown.
bool pawnio_modules_dir(wchar_t* out, u32 cap);
// Full path of "<name>.bin": the download folder, else the "pawnio" folder next to the exe. False = in neither.
bool pawnio_module_path(const char* name, wchar_t* out, u32 cap);
// True when the download folder holds every module the app uses (core/pawnio.h) and the license text.
bool pawnio_modules_downloaded();
// Downloads the release zip, checks its SHA-256 and unpacks the modules + COPYING (and a README naming the
// release) into the download folder. Blocking (a second or two); call from a worker thread in the GUI.
// True on success; msg gets a one-line result or the reason it failed.
bool pawnio_modules_download(char* msg, u32 size);
