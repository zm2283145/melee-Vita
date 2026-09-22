/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../updater/update_core.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

std::string repeat(char value, std::size_t count) {
    return std::string(count, value);
}

const std::string kSignedPayload =
    "format=1\n"
    "version=0.6.1\n"
    "asset=SmashMeleevita.vpk\n"
    "size=1048576\n"
    "sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";

int accept_expected_signature(const std::uint8_t public_key[32],
    const std::uint8_t signature[64], const std::uint8_t* message,
    std::size_t message_size) {
    const std::string_view actual(reinterpret_cast<const char*>(message), message_size);
    return public_key[0] == 0x42 && signature[0] == 0x11 &&
            actual == kSignedPayload
        ? 1
        : 0;
}

int reject_signature(const std::uint8_t*, const std::uint8_t*,
    const std::uint8_t*, std::size_t) {
    return 0;
}

void test_semver() {
    using namespace melee::vita::update;
    std::string error;
    SemVer version;
    CHECK(parse_semver("v1.2.3-alpha.1+build.7", version, error));
    CHECK(version.major == 1 && version.minor == 2 && version.patch == 3);
    CHECK(version.prerelease == "alpha.1");
    CHECK(version.build == "build.7");

    SemVer alpha;
    SemVer alpha_one;
    SemVer beta;
    SemVer stable;
    CHECK(parse_semver("1.0.0-alpha", alpha, error));
    CHECK(parse_semver("1.0.0-alpha.1", alpha_one, error));
    CHECK(parse_semver("1.0.0-beta", beta, error));
    CHECK(parse_semver("1.0.0", stable, error));
    CHECK(compare_semver(alpha, alpha_one) < 0);
    CHECK(compare_semver(alpha_one, beta) < 0);
    CHECK(compare_semver(beta, stable) < 0);

    CHECK(!parse_semver("1.2", version, error));
    CHECK(!parse_semver("01.2.3", version, error));
    CHECK(!parse_semver("1.2.3-rc.01", version, error));
    CHECK(!parse_semver("1.2.3+bad!", version, error));
}

std::string valid_release_json() {
    return R"json({
        "tag_name":"v0.6.1",
        "name":"Melee Vita 0.6.1",
        "body":"Fixes and \u03b1 notes",
        "draft":false,
        "prerelease":false,
        "published_at":"2026-09-21T20:00:00Z",
        "assets":[
            {
                "name":"SmashMeleevita.vpk",
                "browser_download_url":"https://github.com/zm2283145/melee-Vita/releases/download/v0.6.1/SmashMeleevita.vpk",
                "size":1048576,
                "state":"uploaded"
            },
            {
                "name":"melee-update-manifest.txt",
                "browser_download_url":"https://github.com/zm2283145/melee-Vita/releases/download/v0.6.1/melee-update-manifest.txt",
                "size":512,
                "state":"uploaded"
            }
        ]
    })json";
}

void test_release_metadata() {
    using namespace melee::vita::update;
    ReleaseMetadata release;
    std::string error;
    CHECK(parse_github_release(valid_release_json(), release, error));
    CHECK(release.tag_name == "v0.6.1");
    CHECK(release.name == "Melee Vita 0.6.1");
    CHECK(release.notes == "Fixes and \xce\xb1 notes");
    CHECK(release.assets.size() == 2);
    CHECK(validate_release_asset_url(release.assets[0], release.tag_name));
    ReleaseAsset redirected = release.assets[0];
    redirected.download_url =
        "https://example.com/SmashMeleevita.vpk";
    CHECK(!validate_release_asset_url(redirected, release.tag_name));

    std::string draft = valid_release_json();
    draft.replace(draft.find("\"draft\":false"), 13, "\"draft\":true");
    CHECK(!parse_github_release(draft, release, error));

    std::string duplicate = valid_release_json();
    duplicate.replace(duplicate.find("\"name\":\"Melee Vita 0.6.1\""),
        std::string("\"name\":\"Melee Vita 0.6.1\"").size(),
        "\"tag_name\":\"v0.6.1\"");
    CHECK(!parse_github_release(duplicate, release, error));

    CHECK(!parse_github_release(
        std::string(kMaxReleaseMetadataBytes + 1, ' '), release, error));
}

std::string valid_manifest() {
    return kSignedPayload + "signature=" + repeat('1', 128) + "\n";
}

void test_manifest_and_signature() {
    using namespace melee::vita::update;
    SignedManifest manifest;
    std::string error;
    CHECK(parse_signed_manifest(valid_manifest(), manifest, error));
    CHECK(manifest.version_text == "0.6.1");
    CHECK(manifest.asset_name == "SmashMeleevita.vpk");
    CHECK(manifest.asset_size == 1048576);
    CHECK(manifest.signed_payload == kSignedPayload);

    std::array<std::uint8_t, 32> key{};
    key[0] = 0x42;
    CHECK(verify_manifest_signature(manifest, key, accept_expected_signature, error));
    CHECK(!verify_manifest_signature(manifest, key, reject_signature, error));
    CHECK(!verify_manifest_signature(manifest, key, nullptr, error));

    std::string traversal = valid_manifest();
    traversal.replace(
        traversal.find("SmashMeleevita.vpk"), 19, "../SmashMeleevita");
    CHECK(!parse_signed_manifest(traversal, manifest, error));

    std::string reordered = valid_manifest();
    const std::size_t version = reordered.find("version=");
    const std::size_t asset = reordered.find("asset=");
    reordered.replace(version, asset - version, "asset=SmashMeleevita.vpk\n");
    CHECK(!parse_signed_manifest(reordered, manifest, error));
}

void test_sha256() {
    using namespace melee::vita::update;
    Sha256 empty;
    CHECK(sha256_hex(empty.finish()) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    Sha256 abc;
    abc.update("a", 1);
    abc.update("bc", 2);
    CHECK(sha256_hex(abc.finish()) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void test_staging_and_transaction() {
    using namespace melee::vita::update;
    ReleaseMetadata release;
    SignedManifest manifest;
    StagingDecision decision;
    TransactionPlan plan;
    std::array<std::uint8_t, 32> key{};
    std::string error;
    key[0] = 0x42;
    CHECK(parse_github_release(valid_release_json(), release, error));
    CHECK(parse_signed_manifest(valid_manifest(), manifest, error));
    CHECK(validate_release_for_staging(release, manifest, "0.6.0",
        8 * 1024 * 1024, 1024ULL * 1024ULL * 1024ULL, key,
        accept_expected_signature, decision, error));
    CHECK(decision.package_asset != nullptr);
    CHECK(decision.manifest_asset != nullptr);
    CHECK(decision.required_free_bytes ==
        8 * 1024 * 1024 + 1048576 + kMaxExtractedPackageBytes +
            kStorageReserveBytes);
    CHECK(!validate_release_for_staging(
        release, manifest, "0.6.1", 0, 1024ULL * 1024ULL * 1024ULL, key,
        accept_expected_signature, decision, error));
    CHECK(!validate_release_for_staging(
        release, manifest, "0.6.0", 8 * 1024 * 1024, 1, key,
        accept_expected_signature, decision, error));
    CHECK(!validate_release_for_staging(release, manifest, "0.6.0",
        8 * 1024 * 1024, 1024ULL * 1024ULL * 1024ULL, key, reject_signature,
        decision, error));

    CHECK(build_transaction_plan("MLVITA002", plan, error));
    CHECK(plan.requires_external_process);
    CHECK(plan.live_path == "ux0:/app/MLVITA002");
    CHECK(plan.staged_path == "ux0:/data/melee/update/new-package");
    CHECK(plan.protected_paths[0] == "ux0:/data/melee/save/");
    CHECK(plan.protected_paths[1] == "ux0:/data/melee/shadercache/");
    CHECK(plan.commit_steps.front().action ==
        TransactionAction::RequireMainProcessStopped);
    CHECK(plan.commit_steps[7].action ==
        TransactionAction::WaitForLaunchAcknowledgement);
    CHECK(plan.rollback_steps[1].action ==
        TransactionAction::RollbackPromoteBackup);
    CHECK(!build_transaction_plan("bad/title", plan, error));
}

}  // namespace

int main() {
    test_semver();
    test_release_metadata();
    test_manifest_and_signature();
    test_sha256();
    test_staging_and_transaction();
    if (failures != 0) {
        std::cerr << failures << " updater core test(s) failed\n";
        return 1;
    }
    std::cout << "PASS: Vita updater validation core\n";
    return 0;
}
