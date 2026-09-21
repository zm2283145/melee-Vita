/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "update_runtime.h"

#include "update_core.hpp"
#include "update_vita.hpp"

#include <curl/curl.h>
#include <vita2d.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <pthread.h>
#include <string>
#include <string_view>
#include <utility>

#ifndef MELEE_VITA_VERSION
#define MELEE_VITA_VERSION "0.0.0"
#endif

namespace {

using melee::vita::update::Journal;
using melee::vita::update::JournalState;
using melee::vita::update::ReleaseAsset;
using melee::vita::update::ReleaseMetadata;
using melee::vita::update::SemVer;
using melee::vita::update::SignedManifest;
using melee::vita::update::StagingDecision;

constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/zm2283145/melee-Vita/releases/latest";
constexpr const char* kCaBundle = "vs0:data/external/cert/CA_LIST.cer";
constexpr unsigned kHealthFrames = 300;

enum class Mode {
    Dormant,
    Checking,
    Available,
    Downloading,
    ReadyToLaunch,
    HealthPending,
    Cleaning,
    Error,
    RecoveryNotice,
    RecoveryFailed
};

enum class DialogKind {
    None,
    Prompt,
    Progress,
    Error,
    Recovery
};

struct RuntimeState {
    Mode mode = Mode::Dormant;
    std::string release_version;
    std::string release_notes;
    std::string package_url;
    std::string package_name;
    std::array<std::uint8_t, 32> package_sha{};
    std::uint64_t package_size = 0;
    unsigned progress = 0;
    unsigned health_frames = 0;
    std::string error;
};

RuntimeState g_state;
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t g_worker{};
bool g_worker_joinable = false;
std::atomic_bool g_worker_running{false};
std::atomic_bool g_stop{false};
bool g_apputil_initialized = false;
DialogKind g_dialog_kind = DialogKind::None;
std::string g_dialog_text;
SceMsgDialogUserMessageParam g_user_message{};
SceMsgDialogProgressBarParam g_progress_bar{};
alignas(64) std::array<std::uint8_t, 1024 * 1024> g_network_memory{};

class StateLock {
public:
    StateLock() { pthread_mutex_lock(&g_mutex); }
    ~StateLock() { pthread_mutex_unlock(&g_mutex); }
};

void set_mode(Mode mode) {
    StateLock lock;
    g_state.mode = mode;
}

void set_error(std::string message) {
    StateLock lock;
    g_state.error = std::move(message);
    g_state.mode = Mode::Error;
}

std::string error_code_text(const char* action, int result) {
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08X).",
        action, static_cast<unsigned>(result));
    return buffer;
}

bool start_worker(void* (*entry)(void*)) {
    if (g_worker_running.load()) {
        return false;
    }
    if (g_worker_joinable) {
        pthread_join(g_worker, nullptr);
        g_worker_joinable = false;
    }
    g_worker_running = true;
    pthread_attr_t attributes;
    const int attribute_result = pthread_attr_init(&attributes);
    if (attribute_result != 0) {
        g_worker_running = false;
        set_error("Could not initialize the updater worker.");
        return false;
    }
    const int stack_result =
        pthread_attr_setstacksize(&attributes, 512 * 1024);
    if (stack_result != 0) {
        pthread_attr_destroy(&attributes);
        g_worker_running = false;
        set_error("Could not configure the updater worker stack.");
        return false;
    }
    const int result = pthread_create(&g_worker, &attributes, entry, nullptr);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
        g_worker_running = false;
        set_error("Could not start the updater background worker.");
        return false;
    }
    g_worker_joinable = true;
    return true;
}

class NetworkSession {
public:
    bool initialize(std::string& error) {
        const int module_result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        if (module_result < 0) {
            error = error_code_text("Loading network module", module_result);
            return false;
        }
        module_loaded_ = true;
        SceNetInitParam init{};
        init.memory = g_network_memory.data();
        init.size = g_network_memory.size();
        const int net_result = sceNetInit(&init);
        if (net_result < 0) {
            error = error_code_text("Initializing network", net_result);
            return false;
        }
        net_initialized_ = true;
        const int ctl_result = sceNetCtlInit();
        if (ctl_result < 0) {
            error = error_code_text("Initializing network control", ctl_result);
            return false;
        }
        ctl_initialized_ = true;
        int state = SCE_NETCTL_STATE_DISCONNECTED;
        if (sceNetCtlInetGetState(&state) < 0 ||
            state != SCE_NETCTL_STATE_CONNECTED)
        {
            offline_ = true;
            return false;
        }
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            error = "Initializing HTTPS support failed.";
            return false;
        }
        curl_initialized_ = true;
        return true;
    }

    bool offline() const { return offline_; }

    ~NetworkSession() {
        if (curl_initialized_) {
            curl_global_cleanup();
        }
        if (ctl_initialized_) {
            sceNetCtlTerm();
        }
        if (net_initialized_) {
            sceNetTerm();
        }
        if (module_loaded_) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        }
    }

private:
    bool module_loaded_ = false;
    bool net_initialized_ = false;
    bool ctl_initialized_ = false;
    bool curl_initialized_ = false;
    bool offline_ = false;
};

struct MemoryResponse {
    std::string* body = nullptr;
    std::size_t limit = 0;
    bool exceeded = false;
};

std::size_t write_memory(
    void* data, std::size_t size, std::size_t count, void* user) {
    auto* response = static_cast<MemoryResponse*>(user);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        response->exceeded = true;
        return 0;
    }
    const std::size_t bytes = size * count;
    if (bytes > response->limit - response->body->size()) {
        response->exceeded = true;
        return 0;
    }
    response->body->append(static_cast<const char*>(data), bytes);
    return bytes;
}

struct FileResponse {
    SceUID file = -1;
    std::uint64_t received = 0;
    std::uint64_t limit = 0;
    bool failed = false;
};

std::size_t write_download(
    void* data, std::size_t size, std::size_t count, void* user) {
    auto* response = static_cast<FileResponse*>(user);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        response->failed = true;
        return 0;
    }
    const std::size_t bytes = size * count;
    if (response->received > response->limit ||
        bytes > response->limit - response->received)
    {
        response->failed = true;
        return 0;
    }
    const int written = sceIoWrite(response->file, data, bytes);
    if (written < 0 || static_cast<std::size_t>(written) != bytes) {
        response->failed = true;
        return 0;
    }
    response->received += bytes;
    {
        StateLock lock;
        if (g_state.package_size != 0) {
            g_state.progress = static_cast<unsigned>(std::min<std::uint64_t>(
                100, response->received * 100 / g_state.package_size));
        }
    }
    return bytes;
}

int transfer_progress(
    void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_stop.load() ? 1 : 0;
}

void configure_https(CURL* curl, const char* url, long timeout_seconds) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Smash-Melee-Vita-Updater/1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, kCaBundle);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
}

bool is_offline_error(CURLcode code) {
    return code == CURLE_COULDNT_RESOLVE_HOST ||
        code == CURLE_COULDNT_CONNECT ||
        code == CURLE_OPERATION_TIMEDOUT;
}

bool http_get_memory(const char* url, std::size_t limit, long timeout_seconds,
    std::string& body, bool& offline, std::string& error) {
    body.clear();
    offline = false;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        error = "Creating HTTPS request failed.";
        return false;
    }
    MemoryResponse response{&body, limit, false};
    configure_https(curl, url, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (response.exceeded) {
        error = "HTTPS response exceeded its size limit.";
        return false;
    }
    if (result != CURLE_OK) {
        offline = is_offline_error(result);
        error = std::string("HTTPS request failed: ") + curl_easy_strerror(result);
        return false;
    }
    if (status < 200 || status >= 300) {
        error = "GitHub returned HTTP " + std::to_string(status) + ".";
        return false;
    }
    return true;
}

bool http_download_file(const std::string& url, const char* path,
    std::uint64_t expected_size, std::string& error) {
    const std::string temporary = std::string(path) + ".part";
    sceIoRemove(temporary.c_str());
    const SceUID file = sceIoOpen(temporary.c_str(),
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (file < 0) {
        error = error_code_text("Opening staged download", file);
        return false;
    }
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        sceIoClose(file);
        sceIoRemove(temporary.c_str());
        error = "Creating package HTTPS request failed.";
        return false;
    }
    FileResponse response{file, 0, expected_size, false};
    configure_https(curl, url.c_str(), 15 * 60);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    const int sync_result = sceIoSyncByFd(file, 0);
    sceIoClose(file);
    if (result != CURLE_OK || status < 200 || status >= 300 ||
        response.failed || response.received != expected_size || sync_result < 0)
    {
        sceIoRemove(temporary.c_str());
        if (result != CURLE_OK) {
            error =
                std::string("Package download failed: ") + curl_easy_strerror(result);
        } else if (status < 200 || status >= 300) {
            error = "Package download returned HTTP " +
                std::to_string(status) + ".";
        } else {
            error = "Package download size or storage write did not match.";
        }
        return false;
    }
    sceIoRemove(path);
    const int rename_result = sceIoRename(temporary.c_str(), path);
    if (rename_result < 0) {
        sceIoRemove(temporary.c_str());
        error = error_code_text("Committing staged download", rename_result);
        return false;
    }
    sceIoSync("ux0:", 0);
    return true;
}

const ReleaseAsset* find_asset(
    const ReleaseMetadata& release, std::string_view name) {
    const ReleaseAsset* match = nullptr;
    for (const ReleaseAsset& asset : release.assets) {
        if (asset.name == name) {
            if (match != nullptr) {
                return nullptr;
            }
            match = &asset;
        }
    }
    return match;
}

std::string clean_notes(std::string notes) {
    for (char& value : notes) {
        if (value == '\r' || value == '\n' || value == '\t') {
            value = ' ';
        } else if (static_cast<unsigned char>(value) < 0x20) {
            value = ' ';
        }
    }
    if (notes.size() > 300) {
        notes.resize(297);
        notes += "...";
    }
    return notes;
}

void run_check() {
    std::string error;
    NetworkSession network;
    if (!network.initialize(error)) {
        if (!network.offline()) {
            set_error(error);
        } else {
            set_mode(Mode::Dormant);
        }
        return;
    }

    std::string metadata_text;
    bool offline = false;
    if (!http_get_memory(kLatestReleaseUrl,
            melee::vita::update::kMaxReleaseMetadataBytes, 8,
            metadata_text, offline, error))
    {
        if (offline) {
            set_mode(Mode::Dormant);
        } else {
            set_error(error);
        }
        return;
    }

    ReleaseMetadata release;
    if (!melee::vita::update::parse_github_release(
            metadata_text, release, error))
    {
        set_error("GitHub release metadata was rejected: " + error);
        return;
    }
    if (release.prerelease) {
        set_mode(Mode::Dormant);
        return;
    }
    SemVer current_version;
    SemVer release_version;
    if (!melee::vita::update::parse_semver(
            MELEE_VITA_VERSION, current_version, error) ||
        !melee::vita::update::parse_semver(
            release.tag_name, release_version, error))
    {
        set_error("Release version comparison failed: " + error);
        return;
    }
    if (melee::vita::update::compare_semver(
            current_version, release_version) >= 0)
    {
        set_mode(Mode::Dormant);
        return;
    }

    const ReleaseAsset* manifest_asset = find_asset(
        release, melee::vita::update::kManifestAssetName);
    if (manifest_asset == nullptr ||
        manifest_asset->size > melee::vita::update::kMaxManifestBytes ||
        !melee::vita::update::validate_release_asset_url(
            *manifest_asset, release.tag_name))
    {
        set_mode(Mode::Dormant);
        return;
    }
    std::string manifest_text;
    if (!http_get_memory(manifest_asset->download_url.c_str(),
            melee::vita::update::kMaxManifestBytes, 8,
            manifest_text, offline, error))
    {
        if (offline) {
            set_mode(Mode::Dormant);
        } else {
            set_error(error);
        }
        return;
    }
    if (manifest_text.size() != manifest_asset->size) {
        set_error("The downloaded manifest size does not match GitHub metadata.");
        return;
    }

    SignedManifest manifest;
    if (!melee::vita::update::parse_signed_manifest(
            manifest_text, manifest, error))
    {
        set_error("The signed update manifest was rejected: " + error);
        return;
    }
    std::array<std::uint8_t, 32> public_key{};
    if (!melee::vita::update::parse_build_public_key(public_key, error)) {
        set_error(error);
        return;
    }
    std::uint64_t installed_size = 0;
    std::uint64_t free_size = 0;
    if (!melee::vita::update::tree_size(
            "ux0:/app/MLVITA002", installed_size, error) ||
        !melee::vita::update::available_storage(free_size, error))
    {
        set_error(error);
        return;
    }
    StagingDecision decision;
    if (!melee::vita::update::validate_release_for_staging(
            release, manifest, MELEE_VITA_VERSION, installed_size, free_size,
            public_key, melee::vita::update::verify_ed25519_sodium,
            decision, error))
    {
        set_error("The published update was rejected: " + error);
        return;
    }

    if (!melee::vita::update::remove_update_tree(
            "ux0:/data/melee/update", error) ||
        !melee::vita::update::ensure_directory(
            "ux0:/data/melee/update", error) ||
        !melee::vita::update::write_file_atomic(
            melee::vita::update::kManifestPath, manifest_text.data(),
            manifest_text.size(), error))
    {
        set_error(error);
        return;
    }

    {
        StateLock lock;
        g_state.release_version = manifest.version_text;
        g_state.release_notes = clean_notes(release.notes);
        g_state.package_url = decision.package_asset->download_url;
        g_state.package_name = decision.package_asset->name;
        g_state.package_size = manifest.asset_size;
        g_state.package_sha = manifest.sha256;
        g_state.progress = 0;
        g_state.mode = Mode::Available;
    }
}

void* check_worker(void*) {
    run_check();
    g_worker_running = false;
    return nullptr;
}

void run_download() {
    std::string url;
    std::string name;
    std::string version;
    std::array<std::uint8_t, 32> expected_sha{};
    std::uint64_t expected_size = 0;
    {
        StateLock lock;
        url = g_state.package_url;
        name = g_state.package_name;
        version = g_state.release_version;
        expected_sha = g_state.package_sha;
        expected_size = g_state.package_size;
    }
    std::string error;
    NetworkSession network;
    if (!network.initialize(error)) {
        set_error(network.offline() ?
            "The network went offline before the update download began." : error);
        return;
    }
    const std::string package_path =
        std::string(melee::vita::update::kStagingRoot) + name;
    if (!http_download_file(url, package_path.c_str(), expected_size, error)) {
        set_error(error);
        return;
    }
    std::array<std::uint8_t, 32> actual_sha{};
    std::uint64_t actual_size = 0;
    if (!melee::vita::update::sha256_file(package_path.c_str(),
            melee::vita::update::kMaxPackageBytes, actual_sha, actual_size,
            error) ||
        actual_size != expected_size || actual_sha != expected_sha)
    {
        sceIoRemove(package_path.c_str());
        set_error(error.empty() ?
            "Downloaded package failed SHA-256 verification." : error);
        return;
    }

    if (!melee::vita::update::copy_tree(
            "app0:updater", melee::vita::update::kHelperPackagePath, error) ||
        !melee::vita::update::package_title_matches(
            melee::vita::update::kHelperPackagePath,
            melee::vita::update::kHelperTitleId, error))
    {
        set_error("Preparing the temporary updater failed: " + error);
        return;
    }
    Journal journal{JournalState::Staged, version, 0};
    if (!melee::vita::update::write_journal(journal, error)) {
        set_error(error);
        return;
    }
    const int promote_result =
        melee::vita::update::promoter_install(
            melee::vita::update::kHelperPackagePath);
    if (promote_result < 0) {
        set_error(error_code_text(
            "Installing the temporary updater", promote_result));
        return;
    }
    journal.state = JournalState::HelperInstalled;
    if (!melee::vita::update::write_journal(journal, error)) {
        set_error(error);
        return;
    }
    set_mode(Mode::ReadyToLaunch);
}

void* download_worker(void*) {
    run_download();
    g_worker_running = false;
    return nullptr;
}

void run_cleanup() {
    std::string error;
    bool helper_exists = false;
    const int check_result =
        melee::vita::update::promoter_check_exists(
            melee::vita::update::kHelperTitleId, helper_exists);
    if (check_result < 0) {
        set_error(error_code_text(
            "Checking the temporary updater", check_result));
        return;
    }
    if (helper_exists) {
        const int delete_result =
            melee::vita::update::promoter_delete(
                melee::vita::update::kHelperTitleId);
        if (delete_result < 0) {
            set_error(error_code_text(
                "Removing the temporary updater", delete_result));
            return;
        }
    }
    if (!melee::vita::update::remove_update_tree(
            "ux0:/data/melee/update", error))
    {
        set_error("Update cleanup failed: " + error);
        return;
    }
    set_mode(Mode::Dormant);
}

void* cleanup_worker(void*) {
    run_cleanup();
    g_worker_running = false;
    return nullptr;
}

bool initialize_user_dialog(
    std::string message, bool prompt, DialogKind kind) {
    g_dialog_text = std::move(message);
    g_user_message = {};
    g_user_message.buttonType = prompt ?
        SCE_MSG_DIALOG_BUTTON_TYPE_YESNO : SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    g_user_message.msg =
        reinterpret_cast<const SceChar8*>(g_dialog_text.c_str());
    g_user_message.buttonParam = nullptr;
    SceMsgDialogParam parameters;
    sceMsgDialogParamInit(&parameters);
    parameters.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    parameters.userMsgParam = &g_user_message;
    const int result = sceMsgDialogInit(&parameters);
    if (result < 0) {
        set_error(error_code_text("Opening update dialog", result));
        return false;
    }
    g_dialog_kind = kind;
    return true;
}

bool initialize_progress_dialog() {
    g_dialog_text = "Downloading and verifying the update...";
    g_progress_bar = {};
    g_progress_bar.barType = SCE_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
    g_progress_bar.sysMsgParam.sysMsgType = SCE_MSG_DIALOG_SYSMSG_TYPE_WAIT;
    g_progress_bar.msg =
        reinterpret_cast<const SceChar8*>(g_dialog_text.c_str());
    SceMsgDialogParam parameters;
    sceMsgDialogParamInit(&parameters);
    parameters.mode = SCE_MSG_DIALOG_MODE_PROGRESS_BAR;
    parameters.progBarParam = &g_progress_bar;
    const int result = sceMsgDialogInit(&parameters);
    if (result < 0) {
        set_error(error_code_text("Opening update progress", result));
        return false;
    }
    g_dialog_kind = DialogKind::Progress;
    return true;
}

std::string update_prompt() {
    StateLock lock;
    char prefix[192];
    const double megabytes =
        static_cast<double>(g_state.package_size) / (1024.0 * 1024.0);
    std::snprintf(prefix, sizeof(prefix),
        "Melee Vita %s is available (%.1f MiB).\n\n",
        g_state.release_version.c_str(), megabytes);
    std::string message(prefix);
    message += g_state.release_notes.empty() ?
        "No release notes were provided." : g_state.release_notes;
    message += "\n\nInstall this signed update?\nYes = Update, No = Later";
    if (message.size() >= SCE_MSG_DIALOG_USER_MSG_SIZE) {
        message.resize(SCE_MSG_DIALOG_USER_MSG_SIZE - 1);
    }
    return message;
}

void handle_finished_dialog() {
    SceMsgDialogResult result{};
    if (g_dialog_kind == DialogKind::Prompt) {
        sceMsgDialogGetResult(&result);
    }
    sceMsgDialogTerm();
    const DialogKind finished = g_dialog_kind;
    g_dialog_kind = DialogKind::None;

    if (finished == DialogKind::Prompt) {
        if (result.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES) {
            set_mode(Mode::Downloading);
            start_worker(download_worker);
        } else {
            set_mode(Mode::Dormant);
        }
        return;
    }
    if (finished == DialogKind::Recovery) {
        Mode mode;
        {
            StateLock lock;
            mode = g_state.mode;
        }
        if (mode == Mode::RecoveryNotice) {
            set_mode(Mode::Cleaning);
            start_worker(cleanup_worker);
        } else {
            set_mode(Mode::Dormant);
        }
        return;
    }
    if (finished == DialogKind::Error) {
        set_mode(Mode::Dormant);
    }
}

}  // namespace

extern "C" int melee_vita_updater_init(void) {
    SceAppUtilInitParam init{};
    SceAppUtilBootParam boot{};
    const int apputil_result = sceAppUtilInit(&init, &boot);
    if (apputil_result < 0) {
        set_error(error_code_text(
            "Initializing update dialogs", apputil_result));
        return apputil_result;
    }
    g_apputil_initialized = true;
    SceCommonDialogConfigParam dialog_config;
    sceCommonDialogConfigParamInit(&dialog_config);
    int system_language = 0;
    int enter_button = 0;
    int system_param_result = sceAppUtilSystemParamGetInt(
        SCE_SYSTEM_PARAM_ID_LANG, &system_language);
    if (system_param_result < 0) {
        set_error(error_code_text(
            "Reading the system language", system_param_result));
        return system_param_result;
    }
    system_param_result = sceAppUtilSystemParamGetInt(
        SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter_button);
    if (system_param_result < 0) {
        set_error(error_code_text(
            "Reading the system confirm button", system_param_result));
        return system_param_result;
    }
    dialog_config.language = static_cast<SceSystemParamLang>(system_language);
    dialog_config.enterButtonAssign =
        static_cast<SceSystemParamEnterButtonAssign>(enter_button);
    const int dialog_result = sceCommonDialogSetConfigParam(&dialog_config);
    if (dialog_result < 0) {
        set_error(error_code_text(
            "Configuring update dialogs", dialog_result));
        return dialog_result;
    }

    g_stop = false;
    Journal journal;
    std::string error;
    if (melee::vita::update::journal_exists())
    {
        if (!melee::vita::update::read_journal(journal, error)) {
            set_error("Updater recovery journal is invalid: " + error);
            return 0;
        }
        if (journal.state == JournalState::AwaitingHealth &&
            journal.version == MELEE_VITA_VERSION)
        {
            set_mode(Mode::HealthPending);
            return 0;
        }
        if (journal.state == JournalState::RolledBack) {
            StateLock lock;
            g_state.error =
                "The update failed and Melee was rolled back safely. "
                "Error code: " + std::to_string(journal.error_code) + ".";
            g_state.mode = Mode::RecoveryNotice;
            return 0;
        }
        if (journal.state == JournalState::RecoveryFailed) {
            StateLock lock;
            g_state.error =
                "Automatic rollback failed. Leave the temporary updater "
                "installed and launch its bubble to retry recovery. Error code: " +
                std::to_string(journal.error_code) + ".";
            g_state.mode = Mode::RecoveryFailed;
            return 0;
        }
        if (journal.state == JournalState::Staged) {
            StateLock lock;
            g_state.error =
                "The previous update stopped before the helper was installed. "
                "Staged files will be removed.";
            g_state.mode = Mode::RecoveryNotice;
            return 0;
        }
        StateLock lock;
        g_state.error =
            "An update transaction was interrupted. Launch the temporary "
            "Melee updater bubble to recover or roll back.";
        g_state.mode = Mode::RecoveryFailed;
        return 0;
    }

    set_mode(Mode::Checking);
    start_worker(check_worker);
    return 0;
}

extern "C" void melee_vita_updater_poll(void) {
    bool begin_cleanup = false;
    {
        StateLock lock;
        if (g_state.mode == Mode::HealthPending &&
            ++g_state.health_frames >= kHealthFrames)
        {
            g_state.mode = Mode::Cleaning;
            begin_cleanup = true;
        }
    }
    if (begin_cleanup) {
        start_worker(cleanup_worker);
    }
}

extern "C" void melee_vita_updater_render_dialog(void) {
    const SceCommonDialogStatus status = sceMsgDialogGetStatus();
    if (status == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        vita2d_common_dialog_update();
        unsigned progress = 0;
        Mode mode;
        {
            StateLock lock;
            progress = g_state.progress;
            mode = g_state.mode;
        }
        if (g_dialog_kind == DialogKind::Progress) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
            sceMsgDialogProgressBarSetValue(
                SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, progress);
            if (mode == Mode::ReadyToLaunch || mode == Mode::Error) {
                sceMsgDialogClose();
            }
        }
        return;
    }
    if (status == SCE_COMMON_DIALOG_STATUS_FINISHED &&
        g_dialog_kind != DialogKind::None)
    {
        handle_finished_dialog();
        return;
    }
    if (status != SCE_COMMON_DIALOG_STATUS_NONE) {
        return;
    }

    Mode mode;
    std::string error;
    {
        StateLock lock;
        mode = g_state.mode;
        error = g_state.error;
    }
    if (mode == Mode::Available) {
        initialize_user_dialog(update_prompt(), true, DialogKind::Prompt);
    } else if (mode == Mode::Downloading) {
        initialize_progress_dialog();
    } else if (mode == Mode::Error) {
        initialize_user_dialog(
            "Update error:\n\n" + error, false, DialogKind::Error);
    } else if (mode == Mode::RecoveryNotice ||
               mode == Mode::RecoveryFailed)
    {
        initialize_user_dialog(error, false, DialogKind::Recovery);
    } else if (mode == Mode::ReadyToLaunch) {
        const int result =
            melee::vita::update::launch_title(
                melee::vita::update::kHelperTitleId);
        if (result < 0) {
            set_error(error_code_text(
                "Launching the temporary updater", result));
        } else {
            sceKernelExitProcess(0);
        }
    }
}

extern "C" void melee_vita_updater_shutdown(void) {
    g_stop = true;
    if (g_worker_joinable) {
        pthread_join(g_worker, nullptr);
        g_worker_joinable = false;
    }
    if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_NONE) {
        sceMsgDialogAbort();
        sceMsgDialogTerm();
    }
    if (g_apputil_initialized) {
        sceAppUtilShutdown();
        g_apputil_initialized = false;
    }
}
