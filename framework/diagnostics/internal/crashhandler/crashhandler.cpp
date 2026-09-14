/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Crashpad headers first: some transitively include mini_chromium's base/logging.h,
// which defines a LOG_STREAM macro that clashes with the muse logger's.
#include <client/crashpad_client.h>
#include <client/crashpad_info.h>
#include <client/crash_report_database.h>
#include <client/settings.h>
#include <client/simple_string_dictionary.h>
#undef LOG_STREAM

#include "crashhandler.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <typeinfo>

#ifdef _WIN32
#include <windows.h>
#endif

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#endif

#include <QDir>

#include "log.h"

using namespace muse::diagnostics;
using namespace crashpad;

namespace {
// Set at crash time. crashpad_handler reads them out of this process when it writes the dump
// and uploads them as form fields, like the annotations passed to StartHandler.
SimpleStringDictionary s_crashTimeAnnotations;

constexpr const char* TERMINATE_TYPE_KEY = "sentry[tags][terminate.type]";
constexpr const char* TERMINATE_WHAT_KEY = "sentry[tags][terminate.what]";
constexpr size_t SENTRY_TAG_VALUE_MAX = 200; // Sentry drops longer tag values

std::terminate_handler s_previousTerminate = nullptr;
#ifdef _WIN32
void (* s_previousAbortHandler)(int) = nullptr;
#endif

std::string demangle(const char* name)
{
#if __has_include(<cxxabi.h>)
    int status = 0;
    if (char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status)) {
        std::string result(demangled);
        std::free(demangled);
        return result;
    }
#endif
    return name;
}

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
// MSVC has no portable way to name an exception that is not a std::exception (an AudacityException,
// say). Its C++ exception record does lead to the thrown type though: ExceptionInformation is
// { 0x19930520, object, ThrowInfo*, image base }, and the ThrowInfo's catchable types carry the
// type_info of the thrown class and of each base. Offsets are image-relative on x64 and arm64.
constexpr DWORD MSVC_CPP_EXCEPTION = 0xE06D7363;
constexpr ULONG_PTR MSVC_CPP_EXCEPTION_MAGIC = 0x19930520;
struct MsvcThrowInfo { uint32_t attributes; int32_t pmfnUnwind; int32_t pForwardCompat; int32_t pCatchableTypeArray; };
struct MsvcCatchableTypeArray { int32_t nCatchableTypes; int32_t arrayOfCatchableTypes[1]; };
struct MsvcCatchableType { uint32_t properties; int32_t pType; int32_t mdisp; int32_t pdisp; int32_t vdisp; int32_t sizeOrOffset; int32_t copyFunction; };

// Last C++ exception thrown on this thread; a terminate() follows the throw on the same thread.
thread_local char t_lastThrownType[128] = "";
thread_local char t_lastThrownWhat[256] = "";

void copyTruncated(char* dst, size_t capacity, const char* src)
{
    std::strncpy(dst, src ? src : "", capacity - 1);
    dst[capacity - 1] = '\0';
}

LONG WINAPI onFirstChanceException(EXCEPTION_POINTERS* pointers)
{
    const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
    if (record->ExceptionCode != MSVC_CPP_EXCEPTION || record->NumberParameters < 4
        || record->ExceptionInformation[0] != MSVC_CPP_EXCEPTION_MAGIC) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto* object = reinterpret_cast<const char*>(record->ExceptionInformation[1]);
    const auto* throwInfo = reinterpret_cast<const MsvcThrowInfo*>(record->ExceptionInformation[2]);
    const auto imageBase = static_cast<uintptr_t>(record->ExceptionInformation[3]);
    const auto fromRva = [imageBase](int32_t rva) { return imageBase + static_cast<uint32_t>(rva); };
    if (!throwInfo || !throwInfo->pCatchableTypeArray) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    t_lastThrownType[0] = '\0';
    t_lastThrownWhat[0] = '\0';
    const auto* types = reinterpret_cast<const MsvcCatchableTypeArray*>(fromRva(throwInfo->pCatchableTypeArray));
    for (int32_t i = 0; i < types->nCatchableTypes; ++i) {
        const auto* type = reinterpret_cast<const MsvcCatchableType*>(fromRva(types->arrayOfCatchableTypes[i]));
        // an MSVC TypeDescriptor is laid out like std::type_info, so it can be used as one
        const auto* typeInfo = reinterpret_cast<const std::type_info*>(fromRva(type->pType));
        if (i == 0) {
            copyTruncated(t_lastThrownType, sizeof t_lastThrownType, typeInfo->name());
        }
        if (type->pdisp == -1 && std::strcmp(typeInfo->raw_name(), ".?AVexception@std@@") == 0) {
            copyTruncated(t_lastThrownWhat, sizeof t_lastThrownWhat,
                          reinterpret_cast<const std::exception*>(object + type->mdisp)->what());
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

std::string tagValue(std::string value)
{
    for (char& c : value) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    if (value.size() > SENTRY_TAG_VALUE_MAX) {
        value.resize(SENTRY_TAG_VALUE_MAX);
    }
    return value;
}

// A minidump records the abort() that std::terminate ends in, not the C++ exception behind it.
// Name that exception while it is still current, so the report carries it as tags.
void recordCurrentException()
{
    if (s_crashTimeAnnotations.GetValueForKey(TERMINATE_TYPE_KEY)) {
        return; // first one wins
    }

    std::string type = "none";
    std::string what;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    if (t_lastThrownType[0]) {
        type = t_lastThrownType;
        what = t_lastThrownWhat;
    } else
#endif
    if (std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            type = demangle(typeid(e).name());
            what = e.what();
        } catch (...) {
            type = "non-std exception";
#if __has_include(<cxxabi.h>)
            if (const std::type_info* info = abi::__cxa_current_exception_type()) {
                type = demangle(info->name());
            }
#endif
        }
    }

    s_crashTimeAnnotations.SetKeyValue(TERMINATE_TYPE_KEY, tagValue(type));
    if (!what.empty()) {
        s_crashTimeAnnotations.SetKeyValue(TERMINATE_WHAT_KEY, tagValue(what));
    }
}

void onTerminate()
{
    recordCurrentException();
    if (s_previousTerminate) {
        s_previousTerminate();
    }
    std::abort();
}

#ifdef _WIN32
// MSVC keeps the terminate handler per thread, so onTerminate only covers the thread that
// installed it. abort() raises SIGABRT, whose handler is process-wide; crashpad's is the previous one.
void onAbortSignal(int signum)
{
    recordCurrentException();
    if (s_previousAbortHandler) {
        s_previousAbortHandler(signum);
    }
}
#endif

void installTerminateReporting()
{
    CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(&s_crashTimeAnnotations);
    s_previousTerminate = std::set_terminate(onTerminate);
#ifdef _WIN32
    void (* previous)(int) = std::signal(SIGABRT, onAbortSignal);
    s_previousAbortHandler = (previous == SIG_ERR || previous == SIG_DFL || previous == SIG_IGN) ? nullptr : previous;
#endif
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    AddVectoredExceptionHandler(1 /* first */, onFirstChanceException);
#endif
}
} // namespace

CrashHandler::~CrashHandler()
{
    delete m_client;
}

bool CrashHandler::start(const muse::io::path_t& handlerFilePath, const muse::io::path_t& dumpsDir, const std::string& serverUrl)
{
    if (!fileSystem()->exists(handlerFilePath)) {
        LOGE() << "crash handler not exists, path: " << handlerFilePath;
        return false;
    }

    // Cache directory that will store crashpad information and minidumps
#ifdef _MSC_VER
    base::FilePath database(dumpsDir.toStdWString());
#else
    base::FilePath database(dumpsDir.toStdString());
#endif

    // Path to the out-of-process handler executable
#ifdef _MSC_VER
    base::FilePath handler(handlerFilePath.toStdWString());
#else
    base::FilePath handler(handlerFilePath.toStdString());
#endif

    // Optional annotations passed via --annotations to the handler
    std::map<std::string, std::string> annotations = {
        { "sentry[release]", application()->fullVersion().toStdString() + "." + application()->build().toStdString() }
    };
    for (const auto& [tag, value] : m_sessionTags) {
        annotations[muse::String{ "sentry[tags][%1]" }.arg(tag).toStdString()] = value.toStdString();
    }
    // Optional arguments to pass to the handler
    std::vector<std::string> arguments;
    arguments.push_back("--no-rate-limit");
    arguments.push_back("--no-upload-gzip");

    std::unique_ptr<CrashReportDatabase> db = crashpad::CrashReportDatabase::Initialize(database);
    if (db != nullptr && db->GetSettings() != nullptr) {
        db->GetSettings()->SetUploadsEnabled(true);
    }

    removePendingLockFiles(dumpsDir);

    m_client = new CrashpadClient();
    bool success = m_client->StartHandler(
        handler,
        database,
        database,
        serverUrl,
        annotations,
        arguments,
        true, // restartable
        false // asynchronous_start
        );

    if (success) {
        installTerminateReporting();
    }

    return success;
}

void CrashHandler::addSessionTag(const String& tag, const String& value)
{
    m_sessionTags.emplace(tag, value);
}

void CrashHandler::setSystemCrashReporterForwardingEnabled(bool enabled)
{
    CrashpadInfo::GetCrashpadInfo()->set_system_crash_reporter_forwarding(enabled ? TriState::kEnabled : TriState::kDisabled);
}

void CrashHandler::removePendingLockFiles(const muse::io::path_t& dumpsDir)
{
#ifdef _MSC_VER
    //! NOTE Different directory structure and no lock file on Windows
    (void)dumpsDir;
    return;
#else
    muse::io::path_t pendingDir = dumpsDir + "/pending";
    muse::RetVal<muse::io::paths_t> rv = fileSystem()->scanFiles(pendingDir, { "*.lock" }, muse::io::ScanMode::FilesInCurrentDir);
    if (!rv.ret) {
        LOGE() << "failed get pending lock files, err: " << rv.ret.toString();
        return;
    }

    for (const muse::io::path_t& p : rv.val) {
        if (!fileSystem()->remove(p)) {
            LOGE() << "failed remove file: " << p;
        }
    }
#endif
}
