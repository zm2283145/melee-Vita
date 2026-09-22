/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "update_vita.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <psp2/appmgr.h>
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef MELEE_UPDATE_PUBLIC_KEY_HEX
#define MELEE_UPDATE_PUBLIC_KEY_HEX ""
#endif

namespace melee::vita::update {
namespace {

constexpr std::size_t kCopyBufferSize = 128 * 1024;
constexpr unsigned kMaximumTreeDepth = 32;
constexpr std::uint64_t kMaximumEntries = 16384;
constexpr int kSceNoEntry = static_cast<int>(UINT32_C(0x80010002));

std::string system_error(const char* operation, int code) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%s failed: 0x%08X",
        operation, static_cast<unsigned>(code));
    return buffer;
}

bool is_update_path(std::string_view path) {
    return path == "ux0:/data/melee/update" ||
        path.starts_with("ux0:/data/melee/update/");
}

bool stat_path(const char* path, SceIoStat& stat) {
    std::memset(&stat, 0, sizeof(stat));
    return sceIoGetstat(path, &stat) >= 0;
}

bool write_all(SceUID fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size > 0) {
        const std::size_t chunk =
            std::min(size, static_cast<std::size_t>(INT_MAX));
        const int written = sceIoWrite(fd, bytes, chunk);
        if (written <= 0) {
            return false;
        }
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool read_all(SceUID fd, void* data, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    while (size > 0) {
        const std::size_t chunk =
            std::min(size, static_cast<std::size_t>(INT_MAX));
        const int received = sceIoRead(fd, bytes, chunk);
        if (received <= 0) {
            return false;
        }
        bytes += received;
        size -= static_cast<std::size_t>(received);
    }
    return true;
}

bool copy_file(const std::string& source, const std::string& destination,
    std::string& error) {
    const SceUID input = sceIoOpen(source.c_str(), SCE_O_RDONLY, 0);
    if (input < 0) {
        error = system_error("Opening source file", input);
        return false;
    }
    const SceUID output = sceIoOpen(destination.c_str(),
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (output < 0) {
        sceIoClose(input);
        error = system_error("Opening destination file", output);
        return false;
    }

    std::vector<std::uint8_t> buffer(kCopyBufferSize);
    bool succeeded = true;
    for (;;) {
        const int received = sceIoRead(input, buffer.data(), buffer.size());
        if (received < 0) {
            error = system_error("Reading source file", received);
            succeeded = false;
            break;
        }
        if (received == 0) {
            break;
        }
        if (!write_all(output, buffer.data(), static_cast<std::size_t>(received))) {
            error = "Writing destination file failed";
            succeeded = false;
            break;
        }
    }
    if (succeeded && sceIoSyncByFd(output, 0) < 0) {
        error = "Synchronizing destination file failed";
        succeeded = false;
    }
    sceIoClose(output);
    sceIoClose(input);
    if (!succeeded) {
        sceIoRemove(destination.c_str());
    }
    return succeeded;
}

bool remove_tree_impl(
    const std::string& path, unsigned depth, std::string& error) {
    if (depth > kMaximumTreeDepth) {
        error = "Update cleanup exceeded the directory depth limit";
        return false;
    }
    SceIoStat stat{};
    const int status = sceIoGetstat(path.c_str(), &stat);
    if (status < 0) {
        if (status == kSceNoEntry) {
            return true;
        }
        error = system_error("Reading update path", status);
        return false;
    }
    if (!SCE_S_ISDIR(stat.st_mode)) {
        const int result = sceIoRemove(path.c_str());
        if (result < 0) {
            error = system_error("Removing update file", result);
            return false;
        }
        return true;
    }

    const SceUID directory = sceIoDopen(path.c_str());
    if (directory < 0) {
        error = system_error("Opening update directory", directory);
        return false;
    }
    bool succeeded = true;
    for (;;) {
        SceIoDirent entry{};
        const int result = sceIoDread(directory, &entry);
        if (result < 0) {
            error = system_error("Reading update directory", result);
            succeeded = false;
            break;
        }
        if (result == 0) {
            break;
        }
        if (std::strcmp(entry.d_name, ".") == 0 ||
            std::strcmp(entry.d_name, "..") == 0)
        {
            continue;
        }
        if (!remove_tree_impl(
                path + "/" + entry.d_name, depth + 1, error))
        {
            succeeded = false;
            break;
        }
    }
    sceIoDclose(directory);
    if (!succeeded) {
        return false;
    }
    const int result = sceIoRmdir(path.c_str());
    if (result < 0) {
        error = system_error("Removing update directory", result);
        return false;
    }
    return true;
}

bool copy_tree_impl(const std::string& source, const std::string& destination,
    unsigned depth, std::uint64_t& entries, std::string& error) {
    if (depth > kMaximumTreeDepth || ++entries > kMaximumEntries) {
        error = "Application backup exceeded filesystem limits";
        return false;
    }
    SceIoStat stat{};
    if (!stat_path(source.c_str(), stat)) {
        error = "Backup source does not exist";
        return false;
    }
    if (SCE_S_ISREG(stat.st_mode)) {
        return copy_file(source, destination, error);
    }
    if (!SCE_S_ISDIR(stat.st_mode)) {
        error = "Backup source contains an unsupported file type";
        return false;
    }
    if (!ensure_directory(destination.c_str(), error)) {
        return false;
    }

    const SceUID directory = sceIoDopen(source.c_str());
    if (directory < 0) {
        error = system_error("Opening backup source", directory);
        return false;
    }
    bool succeeded = true;
    for (;;) {
        SceIoDirent entry{};
        const int result = sceIoDread(directory, &entry);
        if (result < 0) {
            error = system_error("Reading backup source", result);
            succeeded = false;
            break;
        }
        if (result == 0) {
            break;
        }
        if (std::strcmp(entry.d_name, ".") == 0 ||
            std::strcmp(entry.d_name, "..") == 0)
        {
            continue;
        }
        if (!copy_tree_impl(source + "/" + entry.d_name,
                destination + "/" + entry.d_name, depth + 1, entries, error))
        {
            succeeded = false;
            break;
        }
    }
    sceIoDclose(directory);
    return succeeded;
}

bool tree_size_impl(const std::string& path, unsigned depth,
    std::uint64_t& entries, std::uint64_t& size, std::string& error) {
    if (depth > kMaximumTreeDepth || ++entries > kMaximumEntries) {
        error = "Application tree exceeded filesystem limits";
        return false;
    }
    SceIoStat stat{};
    if (!stat_path(path.c_str(), stat)) {
        error = "Application path does not exist";
        return false;
    }
    if (SCE_S_ISREG(stat.st_mode)) {
        if (stat.st_size < 0 ||
            static_cast<std::uint64_t>(stat.st_size) >
                std::numeric_limits<std::uint64_t>::max() - size)
        {
            error = "Application size overflow";
            return false;
        }
        size += static_cast<std::uint64_t>(stat.st_size);
        return true;
    }
    if (!SCE_S_ISDIR(stat.st_mode)) {
        error = "Application tree contains an unsupported file type";
        return false;
    }

    const SceUID directory = sceIoDopen(path.c_str());
    if (directory < 0) {
        error = system_error("Opening application tree", directory);
        return false;
    }
    bool succeeded = true;
    for (;;) {
        SceIoDirent entry{};
        const int result = sceIoDread(directory, &entry);
        if (result < 0) {
            error = system_error("Reading application tree", result);
            succeeded = false;
            break;
        }
        if (result == 0) {
            break;
        }
        if (std::strcmp(entry.d_name, ".") == 0 ||
            std::strcmp(entry.d_name, "..") == 0)
        {
            continue;
        }
        if (!tree_size_impl(path + "/" + entry.d_name, depth + 1,
                entries, size, error))
        {
            succeeded = false;
            break;
        }
    }
    sceIoDclose(directory);
    return succeeded;
}

bool safe_archive_path(std::string_view path) {
    if (path.empty() || path.size() > 512 || path.front() == '/' ||
        path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos)
    {
        return false;
    }
    if (!std::all_of(path.begin(), path.end(), [](char value) {
            return (value >= '0' && value <= '9') ||
                (value >= 'A' && value <= 'Z') ||
                (value >= 'a' && value <= 'z') ||
                value == '/' || value == '_' || value == '-' || value == '.';
        }))
    {
        return false;
    }
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t end = path.find('/', start);
        const std::string_view part = path.substr(start,
            end == std::string_view::npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

bool ensure_parent_directory(const std::string& path, std::string& error) {
    const std::size_t separator = path.find_last_of('/');
    if (separator == std::string::npos) {
        error = "Destination path has no parent directory";
        return false;
    }
    return ensure_directory(path.substr(0, separator).c_str(), error);
}

const char* journal_state_name(JournalState state) {
    switch (state) {
    case JournalState::Staged:
        return "staged";
    case JournalState::HelperInstalled:
        return "helper-installed";
    case JournalState::Promoting:
        return "promoting";
    case JournalState::AwaitingHealth:
        return "awaiting-health";
    case JournalState::CleanupPending:
        return "cleanup-pending";
    case JournalState::RolledBack:
        return "rolled-back";
    case JournalState::RecoveryFailed:
        return "recovery-failed";
    default:
        return "invalid";
    }
}

JournalState parse_journal_state(std::string_view value) {
    if (value == "staged") {
        return JournalState::Staged;
    }
    if (value == "helper-installed") {
        return JournalState::HelperInstalled;
    }
    if (value == "promoting") {
        return JournalState::Promoting;
    }
    if (value == "awaiting-health") {
        return JournalState::AwaitingHealth;
    }
    if (value == "cleanup-pending") {
        return JournalState::CleanupPending;
    }
    if (value == "rolled-back") {
        return JournalState::RolledBack;
    }
    if (value == "recovery-failed") {
        return JournalState::RecoveryFailed;
    }
    return JournalState::Invalid;
}

int load_sce_paf() {
    // VitaShell uses this PAF initialization block before promoter operations.
    static std::uint32_t arguments[] = {
        0x180000, UINT32_MAX, UINT32_MAX, 1, UINT32_MAX, UINT32_MAX};
    int result = -1;
    SceSysmoduleOpt option{};
    option.flags = 0;
    option.result = &result;
    return sceSysmoduleLoadModuleInternalWithArg(
        SCE_SYSMODULE_INTERNAL_PAF, sizeof(arguments), arguments, &option);
}

void unload_sce_paf() {
    sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PAF);
}

template <typename Operation>
int with_promoter(Operation operation) {
    int result = load_sce_paf();
    if (result < 0) {
        return result;
    }
    result =
        sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (result < 0) {
        unload_sce_paf();
        return result;
    }
    result = scePromoterUtilityInit();
    if (result >= 0) {
        result = operation();
        const int exit_result = scePromoterUtilityExit();
        if (result >= 0 && exit_result < 0) {
            result = exit_result;
        }
    }
    sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    unload_sce_paf();
    return result;
}

#pragma pack(push, 1)
struct SfoHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t key_offset;
    std::uint32_t value_offset;
    std::uint32_t count;
};

struct SfoEntry {
    std::uint16_t name_offset;
    std::uint8_t alignment;
    std::uint8_t type;
    std::uint32_t value_size;
    std::uint32_t total_size;
    std::uint32_t data_offset;
};
#pragma pack(pop)

bool sfo_title_id(
    const std::string& contents, std::string& title_id, std::string& error) {
    if (contents.size() < sizeof(SfoHeader)) {
        error = "Package param.sfo is truncated";
        return false;
    }
    const auto* header =
        reinterpret_cast<const SfoHeader*>(contents.data());
    if (header->magic != 0x46535000 ||
        header->count > 128 ||
        sizeof(SfoHeader) + header->count * sizeof(SfoEntry) > contents.size())
    {
        error = "Package param.sfo has an invalid header";
        return false;
    }
    const auto* entries = reinterpret_cast<const SfoEntry*>(
        contents.data() + sizeof(SfoHeader));
    for (std::uint32_t index = 0; index < header->count; ++index) {
        const std::uint64_t key = static_cast<std::uint64_t>(header->key_offset) +
            entries[index].name_offset;
        const std::uint64_t value =
            static_cast<std::uint64_t>(header->value_offset) +
            entries[index].data_offset;
        if (key >= contents.size() || value >= contents.size()) {
            error = "Package param.sfo entry is out of range";
            return false;
        }
        const char* name = contents.data() + key;
        const std::size_t maximum_name = contents.size() - key;
        if (std::memchr(name, '\0', maximum_name) == nullptr) {
            error = "Package param.sfo key is unterminated";
            return false;
        }
        if (std::strcmp(name, "TITLE_ID") != 0) {
            continue;
        }
        if (entries[index].value_size == 0 ||
            value + entries[index].value_size > contents.size())
        {
            error = "Package TITLE_ID is out of range";
            return false;
        }
        const char* text = contents.data() + value;
        const void* terminator =
            std::memchr(text, '\0', entries[index].value_size);
        if (terminator == nullptr) {
            error = "Package TITLE_ID is unterminated";
            return false;
        }
        const std::size_t length =
            static_cast<const char*>(terminator) - text;
        title_id.assign(text, length);
        return true;
    }
    error = "Package param.sfo has no TITLE_ID";
    return false;
}

}  // namespace

bool parse_build_public_key(
    std::array<std::uint8_t, 32>& public_key, std::string& error) {
    constexpr std::string_view encoded = MELEE_UPDATE_PUBLIC_KEY_HEX;
    public_key.fill(0);
    if (encoded.size() != public_key.size() * 2) {
        error = "The updater public key was not compiled correctly";
        return false;
    }
    auto hex_value = [](char value) -> int {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0; index < public_key.size(); ++index) {
        const int high = hex_value(encoded[index * 2]);
        const int low = hex_value(encoded[index * 2 + 1]);
        if (high < 0 || low < 0) {
            error = "The updater public key contains non-hexadecimal data";
            return false;
        }
        public_key[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool read_file_limited(const char* path, std::size_t limit,
    std::string& contents, std::string& error) {
    contents.clear();
    SceIoStat stat{};
    const int status = sceIoGetstat(path, &stat);
    if (status < 0) {
        error = system_error("Reading file status", status);
        return false;
    }
    if (!SCE_S_ISREG(stat.st_mode) || stat.st_size < 0 ||
        static_cast<std::uint64_t>(stat.st_size) > limit)
    {
        error = "File does not satisfy the size limit";
        return false;
    }
    const SceUID file = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (file < 0) {
        error = system_error("Opening file", file);
        return false;
    }
    contents.resize(static_cast<std::size_t>(stat.st_size));
    const bool succeeded =
        contents.empty() || read_all(file, contents.data(), contents.size());
    sceIoClose(file);
    if (!succeeded) {
        contents.clear();
        error = "Reading file failed";
        return false;
    }
    return true;
}

bool write_file_atomic(
    const char* path, const void* data, std::size_t size, std::string& error) {
    const std::string target(path);
    if (!ensure_parent_directory(target, error)) {
        return false;
    }
    const std::string temporary = target + ".tmp";
    sceIoRemove(temporary.c_str());
    const SceUID file = sceIoOpen(temporary.c_str(),
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (file < 0) {
        error = system_error("Opening temporary file", file);
        return false;
    }
    bool succeeded = write_all(file, data, size);
    if (succeeded) {
        succeeded = sceIoSyncByFd(file, 0) >= 0;
    }
    sceIoClose(file);
    if (!succeeded) {
        sceIoRemove(temporary.c_str());
        error = "Writing temporary file failed";
        return false;
    }
    const std::string previous = target + ".previous";
    sceIoRemove(previous.c_str());
    if (path_exists(target.c_str())) {
        const int preserve_result =
            sceIoRename(target.c_str(), previous.c_str());
        if (preserve_result < 0) {
            sceIoRemove(temporary.c_str());
            error = system_error("Preserving previous file", preserve_result);
            return false;
        }
    }
    const int result = sceIoRename(temporary.c_str(), target.c_str());
    if (result < 0) {
        if (path_exists(previous.c_str())) {
            sceIoRename(previous.c_str(), target.c_str());
        }
        sceIoRemove(temporary.c_str());
        error = system_error("Committing temporary file", result);
        return false;
    }
    sceIoSync("ux0:", 0);
    sceIoRemove(previous.c_str());
    return true;
}

bool write_journal(const Journal& journal, std::string& error) {
    if (journal.state == JournalState::Invalid ||
        journal.version.empty() || journal.version.size() > 64)
    {
        error = "Refusing to write an invalid updater journal";
        return false;
    }
    char buffer[256];
    const int length = std::snprintf(buffer, sizeof(buffer),
        "format=1\nstate=%s\nversion=%s\nerror=%d\n",
        journal_state_name(journal.state), journal.version.c_str(),
        journal.error_code);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(buffer)) {
        error = "Updater journal exceeds its size limit";
        return false;
    }
    return write_file_atomic(
        kJournalPath, buffer, static_cast<std::size_t>(length), error);
}

bool read_journal(Journal& journal, std::string& error) {
    journal = {};
    std::string text;
    std::string selected_path = kJournalPath;
    if (!path_exists(selected_path.c_str())) {
        const std::string temporary = selected_path + ".tmp";
        const std::string previous = selected_path + ".previous";
        if (path_exists(previous.c_str())) {
            selected_path = previous;
        } else if (path_exists(temporary.c_str())) {
            selected_path = temporary;
        }
    }
    if (!read_file_limited(selected_path.c_str(), 512, text, error)) {
        return false;
    }
    if (text.empty() || text.back() != '\n') {
        error = "Updater journal is incomplete";
        return false;
    }
    std::array<std::string_view, 4> lines{};
    std::size_t start = 0;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            error = "Updater journal is truncated";
            return false;
        }
        lines[index] = std::string_view(text).substr(start, end - start);
        start = end + 1;
    }
    if (start != text.size() || lines[0] != "format=1" ||
        !lines[1].starts_with("state=") ||
        !lines[2].starts_with("version=") ||
        !lines[3].starts_with("error="))
    {
        error = "Updater journal has an invalid format";
        return false;
    }
    journal.state = parse_journal_state(lines[1].substr(6));
    journal.version.assign(lines[2].substr(8));
    const std::string error_text(lines[3].substr(6));
    char* end = nullptr;
    errno = 0;
    const long error_code = std::strtol(error_text.c_str(), &end, 10);
    if (journal.state == JournalState::Invalid || journal.version.empty() ||
        journal.version.size() > 64 || errno != 0 || end == nullptr ||
        *end != '\0' || error_code < INT_MIN || error_code > INT_MAX)
    {
        journal = {};
        error = "Updater journal contains invalid values";
        return false;
    }
    journal.error_code = static_cast<int>(error_code);
    return true;
}

bool journal_exists() {
    return path_exists(kJournalPath) ||
        path_exists((std::string(kJournalPath) + ".tmp").c_str()) ||
        path_exists((std::string(kJournalPath) + ".previous").c_str());
}

bool path_exists(const char* path) {
    SceIoStat stat{};
    return stat_path(path, stat);
}

bool ensure_directory(const char* path, std::string& error) {
    if (path == nullptr) {
        error = "Directory path is missing";
        return false;
    }
    const std::string value(path);
    if (!value.starts_with("ux0:/") || value.size() > 512) {
        error = "Directory path is outside ux0";
        return false;
    }
    for (std::size_t separator = value.find('/', 5);
         separator != std::string::npos;
         separator = value.find('/', separator + 1))
    {
        const std::string parent = value.substr(0, separator);
        const int result = sceIoMkdir(parent.c_str(), 0777);
        SceIoStat stat{};
        if (result < 0 &&
            (!stat_path(parent.c_str(), stat) || !SCE_S_ISDIR(stat.st_mode)))
        {
            error = system_error("Creating directory", result);
            return false;
        }
    }
    const int result = sceIoMkdir(value.c_str(), 0777);
    SceIoStat stat{};
    if (result < 0 &&
        (!stat_path(value.c_str(), stat) || !SCE_S_ISDIR(stat.st_mode)))
    {
        error = system_error("Creating directory", result);
        return false;
    }
    return true;
}

bool remove_update_tree(const char* path, std::string& error) {
    if (path == nullptr || !is_update_path(path)) {
        error = "Refusing to remove a path outside the updater staging root";
        return false;
    }
    return remove_tree_impl(path, 0, error);
}

bool copy_tree(const char* source, const char* destination, std::string& error) {
    if (source == nullptr || destination == nullptr ||
        !is_update_path(destination))
    {
        error = "Backup destination is outside the updater staging root";
        return false;
    }
    const std::string temporary = std::string(destination) + ".partial";
    if (!remove_update_tree(temporary.c_str(), error)) {
        return false;
    }
    std::uint64_t entries = 0;
    if (!copy_tree_impl(source, temporary, 0, entries, error)) {
        std::string cleanup_error;
        remove_update_tree(temporary.c_str(), cleanup_error);
        return false;
    }
    if (!remove_update_tree(destination, error)) {
        std::string cleanup_error;
        remove_update_tree(temporary.c_str(), cleanup_error);
        return false;
    }
    const int result = sceIoRename(temporary.c_str(), destination);
    if (result < 0) {
        std::string cleanup_error;
        remove_update_tree(temporary.c_str(), cleanup_error);
        error = system_error("Committing copied directory", result);
        return false;
    }
    sceIoSync("ux0:", 0);
    return true;
}

bool tree_size(const char* path, std::uint64_t& size, std::string& error) {
    size = 0;
    std::uint64_t entries = 0;
    return tree_size_impl(path, 0, entries, size, error);
}

bool available_storage(std::uint64_t& bytes, std::string& error) {
    SceIoDevInfo information{};
    const int result = sceIoDevctl(
        "ux0:", 0x3001, nullptr, 0, &information, sizeof(information));
    if (result < 0 || information.free_size < 0) {
        error = system_error("Reading free storage", result);
        return false;
    }
    bytes = static_cast<std::uint64_t>(information.free_size);
    return true;
}

bool sha256_file(const char* path, std::uint64_t maximum_size,
    std::array<std::uint8_t, 32>& digest, std::uint64_t& size,
    std::string& error) {
    digest.fill(0);
    size = 0;
    SceIoStat stat{};
    const int status = sceIoGetstat(path, &stat);
    if (status < 0 || !SCE_S_ISREG(stat.st_mode) || stat.st_size < 0 ||
        static_cast<std::uint64_t>(stat.st_size) > maximum_size)
    {
        error = "Downloaded package does not satisfy the size limit";
        return false;
    }
    size = static_cast<std::uint64_t>(stat.st_size);
    const SceUID file = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (file < 0) {
        error = system_error("Opening downloaded package", file);
        return false;
    }
    Sha256 sha256;
    std::vector<std::uint8_t> buffer(kCopyBufferSize);
    bool succeeded = true;
    for (;;) {
        const int received = sceIoRead(file, buffer.data(), buffer.size());
        if (received < 0) {
            error = system_error("Reading downloaded package", received);
            succeeded = false;
            break;
        }
        if (received == 0) {
            break;
        }
        sha256.update(buffer.data(), static_cast<std::size_t>(received));
    }
    sceIoClose(file);
    if (!succeeded) {
        return false;
    }
    digest = sha256.finish();
    return true;
}

bool extract_update_vpk(const char* archive_path, const char* destination,
    std::string& error) {
    if (destination == nullptr || !is_update_path(destination)) {
        error = "Archive destination is outside the updater staging root";
        return false;
    }
    if (!remove_update_tree(destination, error) ||
        !ensure_directory(destination, error))
    {
        return false;
    }

    archive* input = archive_read_new();
    if (input == nullptr) {
        error = "Creating archive reader failed";
        return false;
    }
    archive_read_support_format_zip(input);
    archive_read_support_filter_all(input);
    if (archive_read_open_filename(input, archive_path, kCopyBufferSize) !=
        ARCHIVE_OK)
    {
        error = archive_error_string(input) != nullptr ?
            archive_error_string(input) : "Opening update archive failed";
        archive_read_free(input);
        return false;
    }

    std::uint64_t extracted = 0;
    std::uint64_t entries = 0;
    std::vector<std::string> seen;
    std::vector<std::uint8_t> buffer(kCopyBufferSize);
    bool succeeded = true;
    archive_entry* entry = nullptr;
    for (;;) {
        const int header_result = archive_read_next_header(input, &entry);
        if (header_result == ARCHIVE_EOF) {
            break;
        }
        if (header_result != ARCHIVE_OK) {
            error = archive_error_string(input) != nullptr ?
                archive_error_string(input) : "Reading update archive header failed";
            succeeded = false;
            break;
        }
        if (++entries > kMaximumEntries) {
            error = "Update archive contains too many entries";
            succeeded = false;
            break;
        }
        const char* raw_path = archive_entry_pathname_utf8(entry);
        if (raw_path == nullptr) {
            raw_path = archive_entry_pathname(entry);
        }
        const std::string relative = raw_path != nullptr ? raw_path : "";
        std::string normalized = relative;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        if (!safe_archive_path(relative) ||
            std::find(seen.begin(), seen.end(), normalized) != seen.end() ||
            archive_entry_is_encrypted(entry) != 0)
        {
            error = "Update archive contains an unsafe or duplicate path";
            succeeded = false;
            break;
        }
        seen.push_back(std::move(normalized));
        const mode_t type = archive_entry_filetype(entry);
        const std::string target = std::string(destination) + "/" + relative;
        if (type == AE_IFDIR) {
            if (!ensure_directory(target.c_str(), error)) {
                succeeded = false;
                break;
            }
            continue;
        }
        const la_int64_t entry_size = archive_entry_size(entry);
        if (type != AE_IFREG || archive_entry_hardlink(entry) != nullptr ||
            archive_entry_symlink(entry) != nullptr || entry_size < 0 ||
            static_cast<std::uint64_t>(entry_size) >
                kMaxExtractedPackageBytes - extracted)
        {
            error = "Update archive contains an unsupported entry";
            succeeded = false;
            break;
        }
        if (!ensure_parent_directory(target, error)) {
            succeeded = false;
            break;
        }
        const SceUID output = sceIoOpen(target.c_str(),
            SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (output < 0) {
            error = system_error("Creating extracted file", output);
            succeeded = false;
            break;
        }
        std::uint64_t written_total = 0;
        for (;;) {
            const la_ssize_t received =
                archive_read_data(input, buffer.data(), buffer.size());
            if (received < 0) {
                error = archive_error_string(input) != nullptr ?
                    archive_error_string(input) : "Reading update archive failed";
                succeeded = false;
                break;
            }
            if (received == 0) {
                break;
            }
            if (static_cast<std::uint64_t>(received) >
                    static_cast<std::uint64_t>(entry_size) - written_total ||
                static_cast<std::uint64_t>(received) >
                    kMaxExtractedPackageBytes - extracted - written_total)
            {
                error = "Extracted data exceeds the advertised size limit";
                succeeded = false;
                break;
            }
            if (!write_all(output, buffer.data(),
                    static_cast<std::size_t>(received)))
            {
                error = "Writing extracted file failed";
                succeeded = false;
                break;
            }
            written_total += static_cast<std::uint64_t>(received);
        }
        if (succeeded && written_total != static_cast<std::uint64_t>(entry_size)) {
            error = "Extracted file size does not match the archive";
            succeeded = false;
        }
        if (succeeded && sceIoSyncByFd(output, 0) < 0) {
            error = "Synchronizing extracted file failed";
            succeeded = false;
        }
        sceIoClose(output);
        if (!succeeded) {
            sceIoRemove(target.c_str());
            break;
        }
        extracted += written_total;
    }
    const int close_result = archive_read_close(input);
    archive_read_free(input);
    if (succeeded && close_result != ARCHIVE_OK) {
        error = "Closing update archive failed";
        succeeded = false;
    }
    if (succeeded &&
        (!path_exists((std::string(destination) + "/eboot.bin").c_str()) ||
         !path_exists((std::string(destination) + "/sce_sys/param.sfo").c_str()) ||
         !path_exists(
             (std::string(destination) + "/sce_sys/package/head.bin").c_str()) ||
         !path_exists(
             (std::string(destination) + "/updater/eboot.bin").c_str()) ||
         !path_exists((std::string(destination) +
             "/updater/sce_sys/param.sfo").c_str()) ||
         !path_exists((std::string(destination) +
             "/updater/sce_sys/package/head.bin").c_str())))
    {
        error = "Update archive is missing required Vita package files";
        succeeded = false;
    }
    if (!succeeded) {
        std::string cleanup_error;
        remove_update_tree(destination, cleanup_error);
    }
    return succeeded;
}

bool package_title_matches(
    const char* package_path, const char* title_id, std::string& error) {
    const std::string sfo_path =
        std::string(package_path) + "/sce_sys/param.sfo";
    std::string contents;
    if (!read_file_limited(sfo_path.c_str(), 1024 * 1024, contents, error)) {
        return false;
    }
    std::string actual;
    if (!sfo_title_id(contents, actual, error)) {
        return false;
    }
    if (actual != title_id) {
        error = "Update package TITLE_ID does not match Melee";
        return false;
    }
    return true;
}

bool package_supports_updater(
    const char* package_path, std::string& error) {
    const std::string executable =
        std::string(package_path) + "/eboot.bin";
    const SceUID file = sceIoOpen(executable.c_str(), SCE_O_RDONLY, 0);
    if (file < 0) {
        error = system_error("Opening update executable", file);
        return false;
    }
    std::array<std::uint8_t, 0x88> header{};
    const bool read = read_all(file, header.data(), header.size());
    sceIoClose(file);
    if (!read) {
        error = "Update executable is too short";
        return false;
    }
    std::uint64_t auth_id = 0;
    std::memcpy(&auth_id, header.data() + 0x80, sizeof(auth_id));
    if (auth_id != UINT64_C(0x2808000000000000)) {
        error = "Update executable lacks the required cleanup permissions";
        return false;
    }
    const std::string helper = std::string(package_path) + "/updater";
    return package_title_matches(helper.c_str(), kHelperTitleId, error);
}

int promoter_install(const char* package_path) {
    return with_promoter(
        [package_path] { return scePromoterUtilityPromotePkgWithRif(
                            package_path, 1); });
}

int promoter_delete(const char* title_id) {
    sceAppMgrDestroyOtherApp();
    return with_promoter(
        [title_id] { return scePromoterUtilityDeletePkg(title_id); });
}

int promoter_check_exists(const char* title_id, bool& exists) {
    exists = false;
    int check_result = -1;
    const int result = with_promoter([title_id, &check_result] {
        int operation_result = 0;
        check_result =
            scePromoterUtilityCheckExist(title_id, &operation_result);
        return operation_result < 0 ? operation_result : 0;
    });
    if (result < 0) {
        return result;
    }
    exists = check_result >= 0;
    return 0;
}

int launch_title(const char* title_id) {
    char uri[64];
    const int length =
        std::snprintf(uri, sizeof(uri), "psgm:play?titleid=%s", title_id);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(uri)) {
        return -1;
    }
    return sceAppMgrLaunchAppByUri(0xFFFFF, uri);
}

}  // namespace melee::vita::update
