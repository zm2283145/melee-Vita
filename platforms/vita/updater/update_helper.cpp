/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "update_core.hpp"
#include "update_vita.hpp"

#include <vita2d.h>

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

extern "C" {
unsigned int _newlib_heap_size_user = 32u * 1024u * 1024u;
int sceUserMainThreadStackSize = 1024 * 1024;
}

namespace {

using melee::vita::update::Journal;
using melee::vita::update::JournalState;
using melee::vita::update::SignedManifest;

vita2d_pgf* g_font = nullptr;
std::string g_status;
std::string g_detail;

void draw_screen(const char* instruction = nullptr) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_pgf_draw_text(g_font, 42.0f, 72.0f,
        RGBA8(255, 255, 255, 255), 1.25f, "Melee Vita temporary updater");
    vita2d_pgf_draw_text(g_font, 42.0f, 126.0f,
        RGBA8(116, 205, 255, 255), 1.0f, g_status.c_str());

    std::string remaining = g_detail;
    float y = 174.0f;
    while (!remaining.empty() && y < 430.0f) {
        std::size_t split = std::min<std::size_t>(remaining.size(), 86);
        if (split < remaining.size()) {
            const std::size_t space = remaining.rfind(' ', split);
            if (space != std::string::npos && space > 20) {
                split = space;
            }
        }
        const std::string line = remaining.substr(0, split);
        vita2d_pgf_draw_text(g_font, 42.0f, y,
            RGBA8(230, 230, 230, 255), 0.85f, line.c_str());
        remaining.erase(0, split);
        while (!remaining.empty() && remaining.front() == ' ') {
            remaining.erase(remaining.begin());
        }
        y += 32.0f;
    }
    if (instruction != nullptr) {
        vita2d_pgf_draw_text(g_font, 42.0f, 500.0f,
            RGBA8(255, 226, 110, 255), 0.9f, instruction);
    }
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void set_status(std::string status, std::string detail = {}) {
    g_status = std::move(status);
    g_detail = std::move(detail);
    draw_screen();
}

unsigned wait_for_buttons(unsigned buttons) {
    SceCtrlData previous{};
    for (;;) {
        SceCtrlData current{};
        sceCtrlPeekBufferPositive(0, &current, 1);
        const unsigned pressed = current.buttons & ~previous.buttons;
        if ((pressed & buttons) != 0) {
            return pressed & buttons;
        }
        previous = current;
        draw_screen(g_status == "Recovery required" ?
            "X: roll back to the previous version" :
            "X: continue");
        sceKernelDelayThread(16 * 1000);
    }
}

bool launch_main(std::string& error) {
    const int result =
        melee::vita::update::launch_title(melee::vita::update::kMainTitleId);
    if (result < 0) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer),
            "Launching Melee failed (0x%08X).",
            static_cast<unsigned>(result));
        error = buffer;
        return false;
    }
    sceKernelExitProcess(0);
    return true;
}

bool verify_staged_update(
    SignedManifest& manifest, std::string& package_path, std::string& error) {
    std::string manifest_text;
    if (!melee::vita::update::read_file_limited(
            melee::vita::update::kManifestPath,
            melee::vita::update::kMaxManifestBytes, manifest_text, error) ||
        !melee::vita::update::parse_signed_manifest(
            manifest_text, manifest, error))
    {
        return false;
    }
    std::array<std::uint8_t, 32> public_key{};
    if (!melee::vita::update::parse_build_public_key(public_key, error) ||
        !melee::vita::update::verify_manifest_signature(
            manifest, public_key,
            melee::vita::update::verify_ed25519_sodium, error))
    {
        return false;
    }
    package_path =
        std::string(melee::vita::update::kStagingRoot) + manifest.asset_name;
    std::array<std::uint8_t, 32> digest{};
    std::uint64_t size = 0;
    if (!melee::vita::update::sha256_file(package_path.c_str(),
            melee::vita::update::kMaxPackageBytes, digest, size, error))
    {
        return false;
    }
    if (size != manifest.asset_size || digest != manifest.sha256) {
        error =
            "The staged package changed after download and was rejected.";
        return false;
    }
    return true;
}

int rollback(const Journal& source_journal, std::string& error) {
    set_status("Restoring the previous version",
        "The backup is being promoted through the Vita package manager.");
    if (!melee::vita::update::path_exists(
            melee::vita::update::kBackupPackagePath) ||
        !melee::vita::update::package_title_matches(
            melee::vita::update::kBackupPackagePath,
            melee::vita::update::kMainTitleId, error))
    {
        return -1;
    }
    const int result =
        melee::vita::update::promoter_install(
            melee::vita::update::kBackupPackagePath);
    Journal journal = source_journal;
    journal.error_code = result < 0 ? result : source_journal.error_code;
    journal.state = result < 0 ?
        JournalState::RecoveryFailed : JournalState::RolledBack;
    std::string journal_error;
    if (!melee::vita::update::write_journal(journal, journal_error)) {
        error = journal_error;
        return result < 0 ? result : -1;
    }
    if (result < 0) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer),
            "Rollback promotion failed (0x%08X).",
            static_cast<unsigned>(result));
        error = buffer;
    }
    return result;
}

bool perform_update(Journal& journal, std::string& error) {
    SignedManifest manifest;
    std::string package_path;
    set_status("Verifying the staged update",
        "The signed manifest and package SHA-256 are checked again.");
    if (!verify_staged_update(manifest, package_path, error)) {
        return false;
    }
    if (journal.version != manifest.version_text) {
        error = "The recovery journal version does not match the manifest.";
        return false;
    }

    set_status("Extracting the verified package",
        "Only regular files with safe relative paths are accepted.");
    if (!melee::vita::update::extract_update_vpk(
            package_path.c_str(), melee::vita::update::kNewPackagePath, error) ||
        !melee::vita::update::package_title_matches(
            melee::vita::update::kNewPackagePath,
            melee::vita::update::kMainTitleId, error) ||
        !melee::vita::update::package_supports_updater(
            melee::vita::update::kNewPackagePath, error))
    {
        return false;
    }

    set_status("Backing up the current installation",
        "Save data, the game image, and the shader cache are outside app0 "
        "and are never copied or replaced.");
    if (!melee::vita::update::copy_tree(
            "ux0:/app/MLVITA002",
            melee::vita::update::kBackupPackagePath, error) ||
        !melee::vita::update::package_title_matches(
            melee::vita::update::kBackupPackagePath,
            melee::vita::update::kMainTitleId, error))
    {
        return false;
    }

    journal.state = JournalState::Promoting;
    journal.error_code = 0;
    if (!melee::vita::update::write_journal(journal, error)) {
        return false;
    }
    set_status("Installing the signed update",
        "Melee is stopped. The Vita package promoter is replacing the "
        "registered application.");
    const int result =
        melee::vita::update::promoter_install(
            melee::vita::update::kNewPackagePath);
    if (result < 0) {
        journal.error_code = result;
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer),
            "Installing the new package failed (0x%08X).",
            static_cast<unsigned>(result));
        error = buffer;
        return false;
    }
    journal.state = JournalState::AwaitingHealth;
    if (!melee::vita::update::write_journal(journal, error)) {
        return false;
    }

    set_status("Update installed",
        "Launching Melee. The backup and this helper remain until the "
        "updated game confirms a healthy startup.");
    return launch_main(error);
}

}  // namespace

int main() {
    sceAppMgrDestroyOtherApp();
    sceKernelPowerLock(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    if (vita2d_init() < 0) {
        sceKernelPowerUnlock(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        return 1;
    }
    g_font = vita2d_load_default_pgf();
    if (g_font == nullptr) {
        vita2d_fini();
        sceKernelPowerUnlock(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        return 1;
    }

    Journal journal;
    std::string error;
    if (!melee::vita::update::read_journal(journal, error)) {
        g_status = "Recovery data is invalid";
        g_detail = error +
            " The helper will remain installed. Reinstall Melee manually.";
        wait_for_buttons(SCE_CTRL_CROSS);
        goto shutdown;
    }

    if (journal.state == JournalState::RolledBack) {
        set_status("Previous version restored",
            "Melee will report the failed update and remove this helper.");
        wait_for_buttons(SCE_CTRL_CROSS);
        launch_main(error);
        goto launch_failed;
    }
    if (journal.state == JournalState::Promoting ||
        journal.state == JournalState::AwaitingHealth ||
        journal.state == JournalState::RecoveryFailed)
    {
        g_status = "Recovery required";
        g_detail =
            "The previous update did not confirm a healthy launch. The "
            "preserved installation can be restored without touching saves.";
        wait_for_buttons(SCE_CTRL_CROSS);
        if (rollback(journal, error) >= 0) {
            set_status("Rollback completed",
                "The previous Melee installation was restored.");
            wait_for_buttons(SCE_CTRL_CROSS);
            launch_main(error);
        }
        goto recovery_failed;
    }

    if (journal.state != JournalState::HelperInstalled &&
        journal.state != JournalState::Staged)
    {
        error = "The updater journal is not in a runnable state.";
        goto launch_failed;
    }
    if (perform_update(journal, error)) {
        goto shutdown;
    }
    if (journal.state == JournalState::Promoting ||
        journal.state == JournalState::AwaitingHealth)
    {
        if (rollback(journal, error) >= 0) {
            set_status("Update failed; rollback completed",
                error + " The previous installation was restored.");
            wait_for_buttons(SCE_CTRL_CROSS);
            launch_main(error);
        }
        goto recovery_failed;
    }

launch_failed:
    g_status = "Update could not continue";
    g_detail = error +
        " The current Melee installation was not replaced. This helper will "
        "remain available for recovery.";
    wait_for_buttons(SCE_CTRL_CROSS);
    launch_main(error);
    goto shutdown;

recovery_failed:
    g_status = "Automatic recovery failed";
    g_detail = error +
        " Do not delete the backup under ux0:/data/melee/update/. Leave this "
        "helper installed and retry, or reinstall Melee manually.";
    wait_for_buttons(SCE_CTRL_CROSS);

shutdown:
    vita2d_free_pgf(g_font);
    vita2d_fini();
    sceKernelPowerUnlock(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    sceKernelExitProcess(0);
    return 0;
}
