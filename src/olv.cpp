/*  Copyright 2026 HandyAndy87

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "olv.h"
#include "logger.h"
#include "scan.h"
#include "jdmo.h"
#include "token.h"

#include <wups.h>

#include <coreinit/debug.h>
#include <coreinit/dynload.h>
#include <coreinit/filesystem.h>
#include <coreinit/memory.h>
#include <coreinit/thread.h>

#include <cstring>

namespace {

// What Protarium's module leaves behind in Nintendo's slot.
//
// Nintendo's string is 38 characters, so the slot is 39 bytes. Protarium only
// writes 31 of them, and their terminator strands the tail of the original right
// where it was: "ndpoint" and the final NUL. So all 39 bytes are known, and matching
// all 39 is what proves this is Nintendo's slot after Protarium got to it, rather
// than some other string that just happens to start the same way.
constexpr char PROTAVERSE_SLOT[39] = "disc.protarium.lol/v1/endpoint\0" "ndpoint";
static_assert(sizeof(PROTAVERSE_SLOT) == 39);

// Juxt's is 37 characters. It goes in with its own terminator and one more NUL to
// fill the slot, so the write is 39 bytes over the 39 that matched.
constexpr char JUXT_SLOT[39] = "discovery.olv.pretendo.cc/v1/endpoint\0";
static_assert(sizeof(JUXT_SLOT) == 39);

// Roseverse's is 38 characters, same as Nintendo's, so it fills the slot exactly:
// its own terminator is the 39th byte and there is no filler NUL. Don't add one for
// symmetry with JUXT_SLOT above. A 38-character literal plus an explicit "\0" is a
// 40-byte initializer and won't compile as char[39], which is the compiler making
// the point for you rather than being a nuisance.
constexpr char ROSEVERSE_SLOT[39] = "disco.olv.projectrose.cafe/v1/endpoint";
static_assert(sizeof(ROSEVERSE_SLOT) == 39);

// The equal-width rule used to be obvious at the call site, back when the compiler
// could see both objects were char[39] there. A replacement picked at runtime is
// just a const char * by the time it reaches swap_all, and the compiler stops
// proving anything about it, so the proof moves up here. Every replacement is the
// width of the key, and the key is the only thing that width is ever taken from.
static_assert(sizeof(JUXT_SLOT) == sizeof(PROTAVERSE_SLOT));
static_assert(sizeof(ROSEVERSE_SLOT) == sizeof(PROTAVERSE_SLOT));

// The applet browser's allowlist: one record naming the only domain it will open.
// It has to follow the discovery URL, or the applet refuses the page it was just
// sent to. Every field but the domain holds the same value in the record their
// module wrote, so I can rebuild that record here exactly and swap it for the one I
// want. Confirmed byte for byte, down to the 114 trailing NULs and the 277-byte stride.
struct AllowlistEntry {
    char scheme[16];
    char domain[128];
    char path[128];
    unsigned char flags[5];
};
static_assert(sizeof(AllowlistEntry) == 277);

constexpr AllowlistEntry allowlist_for(const char *domain) {
    AllowlistEntry entry = {};
    // No strcpy in a constant expression, so it goes in a byte at a time.
    const char scheme[] = "https";
    for (size_t i = 0; scheme[i] != '\0'; ++i) entry.scheme[i] = scheme[i];
    for (size_t i = 0; domain[i] != '\0'; ++i) entry.domain[i] = domain[i];
    entry.flags[0] = 1;
    entry.flags[1] = 1;
    entry.flags[2] = 1;
    entry.flags[3] = 1;
    entry.flags[4] = 1;
    return entry;
}

constexpr AllowlistEntry PROTAVERSE_ALLOWLIST = allowlist_for(".protarium.lol");
constexpr AllowlistEntry JUXT_ALLOWLIST = allowlist_for(".pretendo.cc");
constexpr AllowlistEntry ROSEVERSE_ALLOWLIST = allowlist_for(".projectrose.cafe");

// The window I search for the allowlist record in. It is not the memory bounds the
// URL swap uses: the two searches cover different ranges.
constexpr uint32_t APPLET_WINDOW_START = 0x10000000;
constexpr uint32_t APPLET_WINDOW_SIZE = 0x10000000;

// The applet's copy of the public suffix list, and the four bytes this rewrites in
// it.
//
// The console's list predates ".cafe", so the applet can't tell projectrose.cafe
// from a bare public suffix and won't treat it as a registrable domain. Writing
// "cafe" over "aero" as the file is read fixes that. Four bytes for four is the
// point rather than a coincidence: the file is read in fixed-size chunks, so
// anything that changed its length would put every offset after the edit in the
// wrong place. It is also scan.h's single-width rule holding in the one place here
// that does not go through swap_all, and there is no room argument to get wrong
// because nothing is being fitted into anything.
//
// Why this is needed at all was never established: .lol has the same gap in the same
// list and Protaverse works anyway, so this one is counted rather than trusted.
constexpr const char *TLD_FILE = "vol/content/browser/effective_tld_names.dat";
constexpr char TLD_KEY[4] = {'a', 'e', 'r', 'o'};
constexpr char TLD_REPLACEMENT[4] = {'c', 'a', 'f', 'e'};
static_assert(sizeof(TLD_REPLACEMENT) == sizeof(TLD_KEY));

// The file names, for matching what the loader reports it has loaded.
constexpr const char *OLV_RPL = "nn_olv.rpl";
constexpr const char *OLV2_RPL = "nn_olv2.rpl";

// The same two libraries, spelled the way the loader wants them when you ask whether
// they are already present: no extension.
constexpr const char *OLV_MODULE = "nn_olv";
constexpr const char *OLV2_MODULE = "nn_olv2";

bool olv_is_loaded() {
    OSDynLoad_Module handle = nullptr;
    if (OSDynLoad_IsModuleLoaded(OLV_MODULE, &handle) == OS_DYNLOAD_OK && handle != nullptr) {
        return true;
    }
    handle = nullptr;
    return OSDynLoad_IsModuleLoaded(OLV2_MODULE, &handle) == OS_DYNLOAD_OK && handle != nullptr;
}

// The setting, snapshotted where the applet process can read it. Nothing else
// reaches that process: no hook runs there, storage isn't open, and the log goes
// nowhere. A plain global written before the applet is ever opened is the whole
// mechanism, and it's the one the deleted predecessor of this file used on hardware
// successfully.
Miiverse target = Miiverse::Protaverse;

// Per process rather than per boot. A plugin global outlives the process it was
// set in, so anything meant to happen once per title has to be cleared when one
// starts.
bool foreground_done = false;
bool bounded = false;

size_t token_trigger_swaps = 0;

size_t applet_visits = 0;
size_t applet_url_swaps = 0;
size_t applet_allowlist_swaps = 0;
size_t foreground_url_swaps = 0;
size_t late_loads_seen = 0;
size_t late_load_swaps = 0;

// The applet's handle for the suffix list, tracked from its open to its close.
//
// Tracked rather than re-matched, because a read gets a handle and never a path, so
// the open is the only place the file can be recognized. The close hook isn't
// optional either: handle numbers get reused freely once a file is closed, so
// without it this would eventually rewrite four bytes in the middle of somebody
// else's file.
bool tld_open = false;
uint32_t tld_handle = 0;

size_t tld_opens = 0;
size_t tld_reads = 0;
size_t tld_rewrites = 0;

// What goes into the slot, or nothing at all.
//
// Protaverse isn't a replacement, it's the absence of one. Picking it is a choice to
// write nothing, and returning null here is where that gets enforced once instead of
// in each of the four callers.
//
// Both switches below name every enumerator and have no default, so a fourth
// Miiverse added later warns right here under the -Wall the Makefile already passes.
// That's a warning and not an error, so treat it as a nudge rather than a guarantee:
// the fallthrough is the do-nothing case, which is the safe way to be wrong but is
// also the way a new target could quietly do nothing. Read the warnings.
const char *url_replacement() {
    switch (target) {
        case Miiverse::Juxt: return JUXT_SLOT;
        case Miiverse::Roseverse: return ROSEVERSE_SLOT;
        case Miiverse::Protaverse: break;
    }
    return nullptr;
}

const AllowlistEntry *allowlist_replacement() {
    switch (target) {
        case Miiverse::Juxt: return &JUXT_ALLOWLIST;
        case Miiverse::Roseverse: return &ROSEVERSE_ALLOWLIST;
        case Miiverse::Protaverse: break;
    }
    return nullptr;
}

// Swap the discovery URL wherever it appears in a range. It can land in more than
// one place, so this counts every hit instead of returning after the first.
size_t swap_urls(uint32_t start, uint32_t size) {
    const char *replacement = url_replacement();
    if (replacement == nullptr) {
        return 0;
    }
    // One width, still taken from the key and never from the replacement. The static
    // assertions up by the declarations are what keep that safe now that the compiler
    // can't see the replacement's size here.
    return swap_all(start, size, PROTAVERSE_SLOT, replacement, sizeof(PROTAVERSE_SLOT));
}

bool ends_with(const char *path, const char *suffix) {
    const size_t path_length = std::strlen(path);
    const size_t suffix_length = std::strlen(suffix);
    if (path_length < suffix_length) {
        return false;
    }
    return std::strcmp(path + path_length - suffix_length, suffix) == 0;
}

void rpl_loaded(OSDynLoad_Module, void *, OSDynLoad_NotifyReason reason,
                OSDynLoad_NotifyData *rpl) {
    if (reason != OS_DYNLOAD_NOTIFY_LOADED || rpl == nullptr || rpl->name == nullptr) {
        return;
    }
    // The Miiverse library goes by one name in some titles and the other in the
    // rest, and watching only the first is how half your games silently stay where
    // they were.
    if (!ends_with(rpl->name, OLV_RPL) && !ends_with(rpl->name, OLV2_RPL)) {
        return;
    }

    ++late_loads_seen;

    // Counted before the setting is consulted, so "seen above swapped" still means
    // the library loaded and nothing was written, whichever reason that was.
    late_load_swaps += swap_urls(rpl->dataAddr, rpl->dataSize);
}

// Ordinary stores, unlike everything else in this file.
//
// swap_all goes through the kernel because the memory it targets belongs to a
// library and a plugin can't write that. This buffer is the caller's own, the read
// has just filled it, and it goes back before the caller ever sees it, so there is
// nothing here worth escalating for.
//
// Two things worth knowing about before anybody improves them.
//
// It matches the four bytes anywhere rather than as a whole line, so ".aero"
// second-level entries like "aeroclub.aero" turn into nonsense suffixes. Harmless,
// nothing will ever match them.
//
// A match split across two reads is invisible to it. The file arrives in chunks and
// nothing here carries bytes over the boundary. That's why rewrites are counted
// separately from reads rather than assumed: a console that reads the file and
// rewrites nothing is what that failure looks like, and it's the first thing to
// suspect if Miiverse still refuses.
size_t rewrite_tld(uint8_t *buffer, size_t length) {
    if (buffer == nullptr || length < sizeof(TLD_KEY)) {
        return 0;
    }

    size_t replaced = 0;
    for (size_t i = 0; i + sizeof(TLD_KEY) <= length; ++i) {
        if (std::memcmp(buffer + i, TLD_KEY, sizeof(TLD_KEY)) != 0) {
            continue;
        }
        std::memcpy(buffer + i, TLD_REPLACEMENT, sizeof(TLD_KEY));
        ++replaced;
        // Skip past what was just written, so the replacement can't be rescanned. It
        // couldn't match anyway, but only because "cafe" and "aero" share no prefix,
        // and that is a property of these two strings rather than of the loop.
        i += sizeof(TLD_KEY) - 1;
    }
    return replaced;
}

// Everything the Miiverse applet needs, run from inside a file-open it performs
// while starting up.
void repoint_applet() {
    ++applet_visits;

    const AllowlistEntry *allowlist = allowlist_replacement();
    if (allowlist == nullptr) {
        // Protaverse. The visit is still counted, because "the applet was opened
        // and nothing was written" is the reading that says the default is working.
        return;
    }

    uint32_t base = 0;
    uint32_t size = 0;
    if (OSGetMemBound(OS_MEM2, &base, &size) == 0) {
        applet_url_swaps += swap_urls(base, size);
    }

    applet_allowlist_swaps += swap_all(APPLET_WINDOW_START, APPLET_WINDOW_SIZE,
                                       &PROTAVERSE_ALLOWLIST, allowlist,
                                       sizeof(AllowlistEntry));
}

DECL_FUNCTION(int, FSOpenFile, FSClient *client, FSCmdBlock *block, char *path, const char *mode,
              uint32_t *handle, int error) {
    const bool is_trigger =
        path != nullptr && std::strcmp("vol/content/initial.oma", path) == 0;

    // Matched on the tail instead of the whole string, unlike the trigger above. That
    // one is a path this plugin has watched work on hardware; this one has not been
    // seen on a console at all, and a leading slash or a device prefix both still end
    // the same way. Roseverse only: the rewrite is what drops Rose's TLD in, and
    // there is no reason to take "aero" out of anybody else's list.
    const bool is_tld =
        path != nullptr && target == Miiverse::Roseverse && ends_with(path, TLD_FILE);

    // Let the real open finish first, and do it unconditionally.
    //
    // This is what keeps the body correct whichever way round the two patches end up
    // chained. Protarium's is normally outermost, because theirs is added when their
    // plugin initializes and this one is installed before any plugin initializes, so
    // their body runs first and this sees the memory image they left. But the loader
    // re-runs when the plugin set changes, and after that the nesting is the other
    // way round. Delegating first means their work is done by the time anything here
    // looks at memory, either way round. Don't move this below the swap.
    const int result = real_FSOpenFile(client, block, path, mode, handle, error);

    if (is_trigger) {
        // No guard against running twice. The applet can be opened again without
        // rebooting, their module re-applies its own swaps every time it is, and a
        // once-per-session flag here would silently hand the second visit back to
        // Protaverse. Counting the visits is the diagnostic; preventing them is a
        // bug.
        repoint_applet();

        // The applet fetches its own sign-in token in this process, and the static
        // token replacement never reaches that. This installs the answer by address,
        // right here, at the one point the applet is known to be running and nn_act is
        // loadable. It gates on the setting itself and takes back a stale patch on the
        // others, so it gets called for every setting, like repoint_applet above.
        token_apply_applet_hook();
    }

    // After the real open, because the handle doesn't exist until it returns.
    // FS_STATUS_OK is zero and every failure is negative, so a refused open leaves
    // nothing tracked and the read hook below stays inert.
    if (is_tld && result >= 0 && handle != nullptr) {
        ++tld_opens;
        tld_open = true;
        tld_handle = *handle;
    }

    return result;
}

DECL_FUNCTION(int, FSReadFile, FSClient *client, FSCmdBlock *block, uint8_t *buffer, uint32_t size,
              uint32_t count, uint32_t handle, uint32_t unk1, int error) {
    // Theirs first and unconditionally, for the same chaining reason FSOpenFile
    // above delegates first, and because there is nothing in the buffer to rewrite
    // until the read has filled it.
    const int result = real_FSReadFile(client, block, buffer, size, count, handle, unk1, error);

    if (!tld_open || handle != tld_handle || result <= 0) {
        return result;
    }

    ++tld_reads;

    // What was actually read, not what was asked for. FSReadFile answers in items,
    // which is bytes exactly when the item width is one, so multiplying by size is
    // right either way. Scanning size times count regardless would, on a short read,
    // walk whatever the buffer held before the call. Clamped as well, because the
    // multiply is only trustworthy if the return value is.
    const size_t asked = static_cast<size_t>(size) * count;
    const size_t got = static_cast<size_t>(result) * size;
    tld_rewrites += rewrite_tld(buffer, got < asked ? got : asked);

    return result;
}

DECL_FUNCTION(int, FSCloseFile, FSClient *client, FSCmdBlock *block, uint32_t handle, int error) {
    // Forgotten before the close and whether or not the close succeeds. The two
    // ways to be wrong are not equal: forgetting a handle that somehow stays open
    // costs a missed rewrite, while remembering one that has been reused costs four
    // bytes written into the middle of a file this has no business touching.
    if (tld_open && handle == tld_handle) {
        tld_open = false;
    }
    return real_FSCloseFile(client, block, handle, error);
}

} // namespace

void olv_set_target(Miiverse chosen) {
    target = chosen;
}


// The sweep itself, with the once-per-title gate, shared by both triggers below.
//
// Two triggers rather than one because the foreground alone is late. The gate is
// shared, so whichever gets there first does the work and the other turns into a
// no-op, and the count each one keeps says which was which.
//
// Neither is early enough on its own. A console pointed at Roseverse still resolved
// Protarium's discovery host in the same process that had just taken a Roseverse
// token from this plugin, so the library reads the value before either of these
// fires. A third trigger that claimed the string at application start did fix that,
// and had to go: see the note by olv_apply_foreground in olv.h.
// The library's own loaded ranges, so a sweep doesn't have to read all of MEM2.
//
// This exists because of a regression I caused. Sweeping every byte of MEM2 with a
// 39-byte compare at each offset costs real time, this plugin did it once per title,
// and adding the early claim made it twice, with one of the two on the
// application-start path where a console is booting or changing title. Hardware
// noticed immediately.
//
// Bounding the search fixes that and the precision problem in one move. Nintendo's
// string is not distinctive enough to sweep memory for: their module carries a copy
// of it as its own search key, so an unbounded sweep overwrites theirs along with
// the live one. Inside the Miiverse library's own ranges there is only the live one.
//
// A .rodata section is reported as `read`, so all three get collected.
constexpr size_t MAX_RANGES = 6;

struct Range {
    uint32_t start;
    uint32_t size;
};

// Fills `out` and returns how many ranges were found. Zero means the question could
// not be answered, which callers treat as "do not sweep" rather than as "sweep
// everything": the enumeration needs a raised security level, and guessing wrong
// costs a boot rather than a lookup.
// File scope rather than on the stack. The pre-main trigger below runs on a stack the
// loader has just wiped for the title's benefit, and 2.5 KB of notify records dirtied
// there is the exact thing that wipe exists to prevent.
OSDynLoad_NotifyData rpl_infos[64];

static size_t olv_ranges(Range *out) {
    const int32_t count = OSDynLoad_GetNumberOfRPLs();
    if (count <= 0) {
        return 0;
    }

    auto &infos = rpl_infos;
    const uint32_t want = (static_cast<uint32_t>(count) < 64) ? static_cast<uint32_t>(count) : 64;
    if (!OSDynLoad_GetRPLInfo(0, want, infos)) {
        return 0;
    }

    size_t found = 0;
    for (uint32_t i = 0; i < want && found < MAX_RANGES; ++i) {
        const char *name = infos[i].name;
        if (name == nullptr) continue;
        if (!ends_with(name, OLV_RPL) && !ends_with(name, OLV2_RPL)) continue;

        if (infos[i].textAddr && infos[i].textSize && found < MAX_RANGES) {
            out[found++] = {infos[i].textAddr, infos[i].textSize};
        }
        if (infos[i].dataAddr && infos[i].dataSize && found < MAX_RANGES) {
            out[found++] = {infos[i].dataAddr, infos[i].dataSize};
        }
        if (infos[i].readAddr && infos[i].readSize && found < MAX_RANGES) {
            out[found++] = {infos[i].readAddr, infos[i].readSize};
        }
    }
    return found;
}

static size_t apply_sweep() {
    if (url_replacement() == nullptr || foreground_done) {
        return 0;
    }

    // The foreground comes back every time the console returns from the HOME menu or
    // an applet, and most titles never load the Miiverse library at all. Without this
    // gate, each of those returns costs a byte-at-a-time sweep of every megabyte of
    // MEM2, on the thread the system is waiting on, hunting for something that can't
    // be there. Anything that loads after this point is the callback's job.
    if (!olv_is_loaded()) {
        return 0;
    }

    // Bounded to the library if the loader will say where it is, falling back to all
    // of MEM2 if it won't. The fallback is what this did before the ranges existed, so
    // a console that can't answer the question is no slower than it used to be rather
    // than being left unpatched.
    Range ranges[MAX_RANGES];
    const size_t range_count = olv_ranges(ranges);

    uint32_t base = 0;
    uint32_t size = 0;
    if (range_count == 0 && OSGetMemBound(OS_MEM2, &base, &size) != 0) {
        LOG("could not read the memory bounds, leaving Miiverse alone");
        return 0;
    }

    // Set once the sweep is actually about to happen, so a failed bounds read
    // gets another turn on the next foreground rather than costing the title its
    // only attempt.
    foreground_done = true;
    bounded = range_count > 0;

    size_t swaps = 0;
    if (range_count > 0) {
        for (size_t i = 0; i < range_count; ++i) {
            swaps += swap_urls(ranges[i].start, ranges[i].size);
        }
    } else {
        swaps = swap_urls(base, size);
    }
    if (swaps == 0) {
        // The library is loaded and their string isn't in it. Either their module
        // hasn't patched this process or it no longer writes what this looks for, and
        // the count in the menu is where that shows up.
        return 0;
    }
    LOG("Miiverse discovery re-pointed at %s in %u place(s)", miiverse_name(target),
        static_cast<unsigned>(swaps));
    return swaps;
}


void olv_apply_foreground() {
    foreground_url_swaps += apply_sweep();
}

// Called from the token hook, because a request for the Miiverse sign-in token is
// this plugin's earliest warning that the process is about to use Miiverse. Taking
// the foreground only says the process came to the front, which the Wii U Menu does
// long before it decides to fetch anything.
void olv_apply_for_token() {
    token_trigger_swaps += apply_sweep();
}

// The gap between their module's write and the title's first instruction.
//
// The loader calls every module's all-starts-done hook, wipes the stack, then calls
// OSCheckActiveThreads() on its way to the title's real entry point. That one call is
// the whole mechanism: it lands after their Miiverse write whatever order the modules
// loaded in, and before anything the title does with the result.
//
// This knows nothing at all about their build. It runs the same leftover-keyed sweep
// the two triggers above run, sharing their once-per-title gate, so whichever fires
// first does the work and the others report the zero that says so.
static bool premain_armed = false;
static size_t premain_runs = 0;
static size_t premain_swaps = 0;
static uint32_t premain_pid = 0xffffffffu;

DECL_FUNCTION(int32_t, OSCheckActiveThreads) {
    // Theirs first and unconditionally, so the loader gets its answer whatever
    // happens here and nothing this does can change what the count means.
    const int32_t count = real_OSCheckActiveThreads();

    if (premain_armed) {
        // Once per title. Any other caller reaching this between the arming and the
        // loader's own call spends the shot, which is why runs and swaps are
        // counted separately: that case reads as a run that swapped nothing.
        premain_armed = false;
        ++premain_runs;
        premain_pid = static_cast<uint32_t>(OSGetUPID());
        premain_swaps += apply_sweep();
        jdmo_apply();
    }

    return count;
}

void olv_arm_premain() {
    premain_armed = true;
}

size_t olv_premain_runs() { return premain_runs; }

size_t olv_premain_swaps() { return premain_swaps; }

uint32_t olv_premain_pid() { return premain_pid; }

// Game and menu, which is where the loader runs the hooks this sits after. The applet
// has no module hooks and so no gap to sit in; it's covered by the FSOpenFile
// replacement at the bottom of this file instead.
WUPS_MUST_REPLACE(OSCheckActiveThreads, WUPS_LOADER_LIBRARY_COREINIT, OSCheckActiveThreads);

size_t olv_token_trigger_swaps() { return token_trigger_swaps; }

// Whether the last sweep searched the library's own ranges or the whole of MEM2.
// Worth showing, because the difference is the console feeling slow at every title
// change and is also the difference between a precise key and a hopeful one.
bool olv_sweep_bounded() { return bounded; }

void reset_olv_sweep() {
    foreground_done = false;
}

void olv_watch_late_loads() {
    // Once per process, which is what an application-start hook already gives you. The
    // callback list belongs to the starting process's own loader and is empty until
    // this runs, so nothing accumulates. A flag guarding this would be the bug rather
    // than the fix: plugin globals outlive the process they were set in, so one would
    // register in the first title after a boot and in none of the rest, and the late
    // loads in every other title would go unnoticed.
    OSDynLoad_AddNotifyCallback(&rpl_loaded, nullptr);
}

size_t olv_applet_visits() { return applet_visits; }
size_t olv_applet_url_swaps() { return applet_url_swaps; }
size_t olv_applet_allowlist_swaps() { return applet_allowlist_swaps; }
size_t olv_foreground_url_swaps() { return foreground_url_swaps; }
size_t olv_late_loads_seen() { return late_loads_seen; }
size_t olv_late_load_swaps() { return late_load_swaps; }

size_t olv_tld_opens() { return tld_opens; }
size_t olv_tld_reads() { return tld_reads; }
size_t olv_tld_rewrites() { return tld_rewrites; }

// Installed statically, before any setting has been read, which is why every body
// above starts by checking the setting instead of assuming it.
WUPS_MUST_REPLACE_FOR_PROCESS(FSOpenFile, WUPS_LOADER_LIBRARY_COREINIT, FSOpenFile,
                              WUPS_FP_TARGET_PROCESS_MIIVERSE);

// Same process, same reasoning. Both sit on the applet's own file path and both cost
// one comparison against a bool that is false on every setting but Roseverse, so the
// applet pays nothing to have them installed.
WUPS_MUST_REPLACE_FOR_PROCESS(FSReadFile, WUPS_LOADER_LIBRARY_COREINIT, FSReadFile,
                              WUPS_FP_TARGET_PROCESS_MIIVERSE);
WUPS_MUST_REPLACE_FOR_PROCESS(FSCloseFile, WUPS_LOADER_LIBRARY_COREINIT, FSCloseFile,
                              WUPS_FP_TARGET_PROCESS_MIIVERSE);
