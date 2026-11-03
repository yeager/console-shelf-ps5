// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/update_protocol.hpp"
#include "shelf/json.hpp"
#include <cstdio>
#include <cstring>

namespace shelf {
namespace {
void fail(char* output, std::size_t capacity, const char* message) {
    if (output && capacity)
        std::snprintf(output, capacity, "%s", message ? message : "Update information is invalid.");
}
const cJSON* field(const cJSON* object, const char* name) {
    return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, name) : nullptr;
}
bool exact_false(const cJSON* value) {
    return cJSON_IsFalse(value);
}
bool copy_text(char* output, std::size_t capacity, const char* value) {
    if (!output || !capacity || !value || std::strlen(value) >= capacity)
        return false;
    std::memcpy(output, value, std::strlen(value) + 1);
    return true;
}
} // namespace

UpdateDecision parse_update_release(std::string_view json, std::string_view current,
                                    UpdateRelease& release, char* error,
                                    std::size_t error_capacity) {
    release = {};
    unsigned current_parts[3]{};
    if (!update_semver(current, current_parts)) {
        fail(error, error_capacity, "This build has no valid production version.");
        return UpdateDecision::invalid;
    }
    if (json.empty() || json.size() > 1024 * 1024) {
        fail(error, error_capacity, "GitHub returned an invalid release response.");
        return UpdateDecision::invalid;
    }
    JsonDocument document;
    if (!document.parse(json.data(), json.size()) || !cJSON_IsObject(document.root)) {
        fail(error, error_capacity, "GitHub's release information could not be read.");
        return UpdateDecision::invalid;
    }
    const auto* tag_item = field(document.root, "tag_name");
    const auto* assets = field(document.root, "assets");
    if (!cJSON_IsString(tag_item) || !tag_item->valuestring ||
        !exact_false(field(document.root, "draft")) ||
        !exact_false(field(document.root, "prerelease")) || !cJSON_IsArray(assets)) {
        fail(error, error_capacity, "The latest GitHub entry is not a production release.");
        return UpdateDecision::invalid;
    }
    unsigned latest_parts[3]{};
    if (!update_semver(tag_item->valuestring, latest_parts) ||
        !copy_text(release.tag, sizeof(release.tag), tag_item->valuestring)) {
        fail(error, error_capacity, "The production release has an invalid version tag.");
        return UpdateDecision::invalid;
    }
    const char* title = json_string(document.root, "name");
    if (!*title) title = release.tag;
    if (!copy_text(release.title, sizeof(release.title), title))
        std::snprintf(release.title, sizeof(release.title), "%s", release.tag);

    const cJSON* selected = nullptr;
    for (const cJSON* item = assets->child; item; item = item->next) {
        if (!cJSON_IsObject(item)) continue;
        const auto* name = field(item, "name");
        if (!cJSON_IsString(name) || !name->valuestring ||
            std::strcmp(name->valuestring, update_asset_name) != 0)
            continue;
        if (selected) {
            fail(error, error_capacity, "The release contains more than one isShelf update package.");
            return UpdateDecision::invalid;
        }
        selected = item;
    }
    if (!selected) {
        fail(error, error_capacity, "The release has no PPSA99541.zip update package.");
        return UpdateDecision::invalid;
    }
    const auto* state = field(selected, "state");
    const auto* size = field(selected, "size");
    const auto* digest = field(selected, "digest");
    const auto* url = field(selected, "browser_download_url");
    if (!cJSON_IsString(state) || !state->valuestring || std::strcmp(state->valuestring, "uploaded") != 0 ||
        !cJSON_IsNumber(size) || size->valuedouble < static_cast<double>(update_min_archive_bytes) ||
        size->valuedouble > static_cast<double>(update_max_archive_bytes) ||
        size->valuedouble != static_cast<double>(static_cast<std::uint64_t>(size->valuedouble)) ||
        !cJSON_IsString(digest) || !digest->valuestring || std::strncmp(digest->valuestring, "sha256:", 7) != 0 ||
        !update_hex_sha256(digest->valuestring + 7) || !cJSON_IsString(url) || !url->valuestring) {
        fail(error, error_capacity, "The update package has no usable size or SHA-256 digest.");
        return UpdateDecision::invalid;
    }
    char expected[640];
    const int expected_size = std::snprintf(expected, sizeof(expected), "%s%s/%s",
                                            update_download_prefix, release.tag, update_asset_name);
    if (expected_size <= 0 || static_cast<std::size_t>(expected_size) >= sizeof(expected) ||
        std::strcmp(url->valuestring, expected) != 0 ||
        !copy_text(release.asset_url, sizeof(release.asset_url), url->valuestring) ||
        !copy_text(release.sha256, sizeof(release.sha256), digest->valuestring + 7)) {
        fail(error, error_capacity, "The update package URL is not this repository's release asset.");
        return UpdateDecision::invalid;
    }
    release.bytes = static_cast<std::uint64_t>(size->valuedouble);
    const int comparison = compare_update_versions(current, release.tag);
    if (comparison == 2) {
        fail(error, error_capacity, "The production release version cannot be compared with this build.");
        return UpdateDecision::invalid;
    }
    if (comparison == 0) return UpdateDecision::current;
    return comparison > 0 ? UpdateDecision::newer : UpdateDecision::available;
}
} // namespace shelf
