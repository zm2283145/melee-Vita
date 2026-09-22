/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "update_core.hpp"
#include "update_vita.hpp"

#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

extern "C" {
unsigned int _newlib_heap_size_user = 32u * 1024u * 1024u;
int sceUserMainThreadStackSize = 1024 * 1024;
}

namespace {

using melee::vita::update::Journal;
using melee::vita::update::JournalState;
using melee::vita::update::SignedManifest;

void set_status(const std::string&, const std::string& = {}) {}

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
        "Closing the helper before launching Melee. The backup remains until "
        "the game confirms a healthy startup.");
    return true;
}

}  // namespace

int main() {
    bool launch_main = true;
    sceAppMgrDestroyOtherApp();
    sceKernelPowerLock(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

    Journal journal;
    std::string error;
    if (!melee::vita::update::read_journal(journal, error)) {
        goto shutdown;
    }

    if (journal.state == JournalState::RolledBack) {
        goto shutdown;
    }
    if (journal.state == JournalState::CleanupPending) {
        goto shutdown;
    }
    if (journal.state == JournalState::Promoting ||
        journal.state == JournalState::AwaitingHealth ||
        journal.state == JournalState::RecoveryFailed)
    {
        rollback(journal, error);
        goto shutdown;
    }

    if (journal.state != JournalState::HelperInstalled &&
        journal.state != JournalState::Staged)
    {
        error = "The updater journal is not in a runnable state.";
        goto shutdown;
    }
    if (perform_update(journal, error)) {
        goto shutdown;
    }
    if (journal.state == JournalState::Promoting ||
        journal.state == JournalState::AwaitingHealth)
    {
        if (rollback(journal, error) >= 0) {
            goto shutdown;
        }
        goto shutdown;
    }

shutdown:
    sceKernelPowerUnlock(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    if (launch_main) {
        const int result = melee::vita::update::launch_title(
            melee::vita::update::kMainTitleId);
        if (result < 0) {
            sceKernelExitProcess(result);
        }
    }
    sceKernelExitProcess(0);
    return 0;
}
