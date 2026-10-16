#include "jdmo.h"

#include "iosu.h"
#include "logger.h"
#include "youtube/patch.h"

#include <coreinit/dynload.h>
#include <coreinit/title.h>
#include <coreinit/memorymap.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint64_t HOST_TITLE = 0x0005000010443200ULL;
constexpr uint64_t TEMPLATE_TITLES[] = {
    0x00050000101B9000ULL, 0x00050000101B9800ULL,
    0x00050000101EB200ULL, 0x00050000101EAA00ULL,
    0x0005000010211300ULL, 0x0005000010210C00ULL,
    0x0005000010217000ULL, 0x0005000010217300ULL,
};

constexpr char OLD_TEMPLATE[] =
    "https://{env}public-ubiservices.ubi.com/{version}";
constexpr char NEW_TEMPLATE[] =
    "http://{env}harbour-proxy.c0llydoll.com/{version}";
static_assert(sizeof(OLD_TEMPLATE) == 50);
static_assert(sizeof(NEW_TEMPLATE) == sizeof(OLD_TEMPLATE));

struct HostPair { const char *old_host; const char *new_host; };
constexpr HostPair HOSTS[] = {
    {"msr-public-ubiservices.ubi.com", "harbour-proxy.c0llydoll.com"},
    {"public-ubiservices.ubi.com", "harbour-proxy.c0llydoll.com"},
    {"api-ubiservices.ubi.com", "harbour.c0llydoll.com"},
};

constexpr uint32_t ACCOUNT_ADDRESS = 0xE31930D4;
constexpr char HARBOUR_ACCOUNT[] = "http://%s%sharbour.c0llydoll.com/v%u/api/";
constexpr char PROTARIUM_ACCOUNT[] = "https://account.protarium.lol/v1/api/";
constexpr char PRETENDO_ACCOUNT[] = "https://account.pretendo.cc/v1/api/";
constexpr char PRETENDO_FORMAT[] = "https://%s%saccount.pretendo.cc/v%u/api/";

bool active = false;
bool account_saved = false;
char saved_account[sizeof(HARBOUR_ACCOUNT)];
uint32_t swaps = 0;

// One RPL listing, with the same cap used by JGMO's existing Miiverse scanner.
OSDynLoad_NotifyData rpls[64];
char host_replacement[512];

bool in_range(uint32_t start, uint32_t size) {
    return start != 0 && size != 0 && start + size > start;
}

bool write_long(uint32_t address, const char *bytes, size_t count) {
    // Reuse the page-aware writer already linked for JGMO's YouTube patcher.
    for (size_t i = 0; i < count; i += PATCH_MAX) {
        const uint32_t part = static_cast<uint32_t>(
            count - i < PATCH_MAX ? count - i : PATCH_MAX);
        if (!patch_write(address + static_cast<uint32_t>(i), bytes + i, part))
            return false;
    }
    return true;
}

uint32_t replace_template(uint32_t start, uint32_t size) {
    if (!in_range(start, size) || size < sizeof(OLD_TEMPLATE)) return 0;
    const uint32_t end = start + size;
    uint32_t count = 0;
    uint32_t from = start;
    while (from < end && end - from >= sizeof(OLD_TEMPLATE)) {
        const uint32_t at = patch_find(from, end, OLD_TEMPLATE, sizeof(OLD_TEMPLATE), 1);
        if (!at) break;
        if (patch_write(at, NEW_TEMPLATE, sizeof(NEW_TEMPLATE))) ++count;
        from = at + sizeof(OLD_TEMPLATE);
    }
    return count;
}

uint32_t replace_host(uint32_t start, uint32_t size, const HostPair &pair) {
    if (!in_range(start, size)) return 0;
    const uint32_t end = start + size;
    const size_t old_len = std::strlen(pair.old_host);
    const size_t new_len = std::strlen(pair.new_host);
    if (size < old_len) return 0;
    uint32_t count = 0;
    uint32_t from = start;
    while (from < end && end - from >= old_len) {
        const uint32_t host_at = patch_find(from, end, pair.old_host,
                                            static_cast<uint32_t>(old_len), 1);
        if (!host_at) break;
        from = host_at + static_cast<uint32_t>(old_len);

        // The original JDMOPatcher only rewrites hostnames within URL strings.
        uint32_t url_at = 0;
        if (host_at >= start + 8 &&
            std::memcmp(reinterpret_cast<const void *>(host_at - 8), "https://", 8) == 0)
            url_at = host_at - 8;
        else if (host_at >= start + 7 &&
                 std::memcmp(reinterpret_cast<const void *>(host_at - 7), "http://", 7) == 0)
            url_at = host_at - 7;
        if (!url_at) continue;

        const char *url = reinterpret_cast<const char *>(url_at);
        const size_t room = (end - url_at < 512) ? end - url_at : 512;
        // Never read past a partially mapped section, even if its recorded size is larger.
        bool mapped = true;
        for (uint32_t at = url_at; at < url_at + room;) {
            if (!OSIsAddressValid(at)) { mapped = false; break; }
            at = (at & ~(OS_PAGE_SIZE - 1)) + OS_PAGE_SIZE;
        }
        if (!mapped) continue;
        const char *nul = static_cast<const char *>(std::memchr(url, '\0', room));
        if (!nul) continue;
        const size_t original = static_cast<size_t>(nul - url);
        const size_t prefix = static_cast<size_t>(host_at - url_at);
        if (prefix + old_len > original) continue;
        const size_t suffix = original - prefix - old_len;
        const size_t result_len = 7 + new_len + suffix;
        const size_t write_len = (original > result_len ? original : result_len) + 1;
        if (write_len > room) continue;
        // Growth is possible for http://public-ubiservices... only with known padding.
        bool space = true;
        for (size_t i = original + 1; i < write_len; ++i) {
            if (url[i] != '\0') { space = false; break; }
        }
        if (!space) continue;

        std::memset(host_replacement, 0, write_len);
        std::memcpy(host_replacement, "http://", 7);
        std::memcpy(host_replacement + 7, pair.new_host, new_len);
        std::memcpy(host_replacement + 7 + new_len, url + prefix + old_len, suffix);
        if (write_long(url_at, host_replacement, write_len)) ++count;
    }
    return count;
}

uint32_t patch_rpl(const OSDynLoad_NotifyData &rpl) {
    const struct { uint32_t address, size; } ranges[] = {
        {rpl.textAddr, rpl.textSize}, {rpl.dataAddr, rpl.dataSize},
        {rpl.readAddr, rpl.readSize},
    };
    uint32_t found = 0;
    for (const auto &range : ranges) {
        if (!in_range(range.address, range.size)) continue;
        if (OSGetTitleID() == HOST_TITLE) {
            for (const HostPair &pair : HOSTS)
                found += replace_host(range.address, range.size, pair);
        } else {
            found += replace_template(range.address, range.size);
        }
    }
    return found;
}

void late_rpl(OSDynLoad_Module, void *, OSDynLoad_NotifyReason reason,
              OSDynLoad_NotifyData *rpl) {
    if (active && reason == OS_DYNLOAD_NOTIFY_LOADED && rpl) {
        swaps += patch_rpl(*rpl);
    }
}

bool save_and_redirect_account() {
    IosuHandle iosu;
    if (!iosu.is_open() ||
        !iosu.read_bytes(ACCOUNT_ADDRESS, saved_account, sizeof(saved_account)))
        return false;
    if (std::memcmp(saved_account, PROTARIUM_ACCOUNT, sizeof(PROTARIUM_ACCOUNT)) != 0 &&
        std::memcmp(saved_account, PRETENDO_ACCOUNT, sizeof(PRETENDO_ACCOUNT)) != 0 &&
        std::memcmp(saved_account, PRETENDO_FORMAT, sizeof(PRETENDO_FORMAT)) != 0)
        return false;

    // This slot and width are the ones used by the original JDMOPatcher. A known
    // URL signature is required; a generic URL at this IOSU address isn't enough.
    account_saved = true;
    if (!iosu.write_bytes(ACCOUNT_ADDRESS, HARBOUR_ACCOUNT, sizeof(HARBOUR_ACCOUNT))) {
        if (iosu.write_bytes(ACCOUNT_ADDRESS, saved_account, sizeof(saved_account)))
            account_saved = false;
        return false;
    }
    return true;
}

} // namespace

bool jdmo_supported(uint64_t title) {
    if (title == HOST_TITLE) return true;
    for (uint64_t supported : TEMPLATE_TITLES)
        if (supported == title) return true;
    return false;
}

void jdmo_begin() {
    // main.cpp calls this only for supported titles with JDMO enabled.
    if (account_saved) return;
    swaps = 0;
    if (!save_and_redirect_account()) return;
    active = true;
    OSDynLoad_AddNotifyCallback(&late_rpl, nullptr);
}

void jdmo_apply() {
    if (!active) return;
    const int32_t count = OSDynLoad_GetNumberOfRPLs();
    if (count <= 0) return;
    const uint32_t wanted = count < 64 ? static_cast<uint32_t>(count) : 64;
    if (!OSDynLoad_GetRPLInfo(0, wanted, rpls)) return;
    for (uint32_t i = 0; i < wanted; ++i) swaps += patch_rpl(rpls[i]);
    LOG("JDMO: %u Ubisoft URLs patched", static_cast<unsigned>(swaps));
}

void jdmo_end() {
    if (!active && !account_saved) return;
    active = false;
    if (!account_saved) return;
    const IosuHandle iosu;
    if (!iosu.is_open()) return;
    if (iosu.matches(ACCOUNT_ADDRESS, saved_account, sizeof(saved_account))) {
        account_saved = false;
        return;
    }
    if (!iosu.matches(ACCOUNT_ADDRESS, HARBOUR_ACCOUNT, sizeof(HARBOUR_ACCOUNT))) return;
    if (!iosu.write_bytes(ACCOUNT_ADDRESS, saved_account, sizeof(saved_account))) return;
    account_saved = false;
}
