/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace melee::vita::update {

inline constexpr std::size_t kMaxReleaseMetadataBytes = 64 * 1024;
inline constexpr std::size_t kMaxReleaseNotesBytes = 8 * 1024;
inline constexpr std::size_t kMaxManifestBytes = 4 * 1024;
inline constexpr std::uint64_t kMaxPackageBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxExtractedPackageBytes =
    512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kStorageReserveBytes = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::string_view kStagingRoot = "ux0:/data/melee/update/";
inline constexpr std::string_view kManifestAssetName = "melee-update-manifest.txt";
inline constexpr std::string_view kReleaseDownloadPrefix =
    "https://github.com/zm2283145/melee-Vita/releases/download/";

struct SemVer {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    std::string prerelease;
    std::string build;
};

bool parse_semver(std::string_view text, SemVer& version, std::string& error);
int compare_semver(const SemVer& left, const SemVer& right);

struct ReleaseAsset {
    std::string name;
    std::string download_url;
    std::uint64_t size = 0;
};

struct ReleaseMetadata {
    std::string tag_name;
    std::string name;
    std::string notes;
    std::string published_at;
    bool prerelease = false;
    std::vector<ReleaseAsset> assets;
};

bool parse_github_release(
    std::string_view json, ReleaseMetadata& release, std::string& error);
bool validate_release_asset_url(
    const ReleaseAsset& asset, std::string_view release_tag);

struct SignedManifest {
    unsigned format = 0;
    SemVer version;
    std::string version_text;
    std::string asset_name;
    std::uint64_t asset_size = 0;
    std::array<std::uint8_t, 32> sha256{};
    std::array<std::uint8_t, 64> signature{};
    std::string signed_payload;
};

using SignatureVerifier = int (*)(const std::uint8_t public_key[32],
    const std::uint8_t signature[64], const std::uint8_t* message,
    std::size_t message_size);

bool parse_signed_manifest(
    std::string_view text, SignedManifest& manifest, std::string& error);
bool verify_manifest_signature(const SignedManifest& manifest,
    const std::array<std::uint8_t, 32>& public_key, SignatureVerifier verifier,
    std::string& error);

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    std::array<std::uint8_t, 32> finish();

private:
    void transform(const std::uint8_t block[64]);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t total_size_ = 0;
    std::size_t buffered_ = 0;
    bool finished_ = false;
};

std::string sha256_hex(const std::array<std::uint8_t, 32>& digest);

struct StagingDecision {
    const ReleaseAsset* package_asset = nullptr;
    const ReleaseAsset* manifest_asset = nullptr;
    std::uint64_t required_free_bytes = 0;
};

bool validate_release_for_staging(const ReleaseMetadata& release,
    const SignedManifest& manifest, std::string_view current_version,
    std::uint64_t installed_size, std::uint64_t available_bytes,
    const std::array<std::uint8_t, 32>& public_key, SignatureVerifier verifier,
    StagingDecision& decision, std::string& error);

enum class TransactionAction {
    RequireMainProcessStopped,
    WritePendingJournal,
    InstallTemporaryHelper,
    VerifyStagedPackage,
    CopyLiveToBackup,
    PromoteStagedPackage,
    RelaunchMain,
    WaitForLaunchAcknowledgement,
    DeleteTemporaryHelper,
    RemoveBackup,
    RemovePendingJournal,
    RollbackPromoteBackup,
    RemoveStaging
};

struct TransactionStep {
    TransactionAction action;
    std::string source;
    std::string destination;
};

struct TransactionPlan {
    bool requires_external_process = true;
    std::string live_path;
    std::string staged_path;
    std::string backup_path;
    std::string journal_path;
    std::array<std::string, 2> protected_paths;
    std::vector<TransactionStep> commit_steps;
    std::vector<TransactionStep> rollback_steps;
};

bool build_transaction_plan(
    std::string_view title_id, TransactionPlan& plan, std::string& error);

int verify_ed25519_sodium(const std::uint8_t public_key[32],
    const std::uint8_t signature[64], const std::uint8_t* message,
    std::size_t message_size);

}  // namespace melee::vita::update

#if defined(_WIN32)
#define MELEE_UPDATE_EXPORT __declspec(dllexport)
#else
#define MELEE_UPDATE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

using MeleeUpdateSignatureVerifierC = int (*)(const std::uint8_t public_key[32],
    const std::uint8_t signature[64], const std::uint8_t* message,
    std::size_t message_size);

MELEE_UPDATE_EXPORT int melee_update_verify_manifest_c(const char* manifest_text,
    std::size_t manifest_size, const std::uint8_t public_key[32],
    MeleeUpdateSignatureVerifierC verifier, char* error, std::size_t error_size);

}
