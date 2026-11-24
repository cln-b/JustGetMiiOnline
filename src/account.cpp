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

#include "account.h"
#include "iosu.h"
#include "logger.h"

#include <coreinit/dynload.h>

#include <cstdint>
#include <cstring>

namespace {

// The account URL lives at a fixed address in IOSU. Writing it from a plugin
// after a module wrote its own at boot is confirmed on hardware: Wii U Chat
// connects on one setting and errors on the other, which it couldn't do if the
// console cached the boot value.
constexpr uint32_t ACCOUNT_URL_ADDRESS = 0xE31930D4;
constexpr const char *PRETENDO_ACCOUNT_URL = "https://account.pretendo.cc/v1/api/";

// What Protarium's build writes there, and therefore how to tell it's installed.
//
// Part of a host rather than a whole one, because their builds don't agree on the host.
// The older two write https://account.protarium.lol/v1/api/ and r17 writes
// https://account.protarium.online/v1/api/, so the marker is the one piece of the name
// every generation writes. The .lol half stays recognized until #29 drops it.
//
// INVARIANT: finding this marker proves their build is installed. Not finding it
// proves nothing on its own, because this plugin writes Pretendo's URL to the same
// address, and because Nintendo's own URL sits there until their plugin has asked
// their module to write. So look_for_protarium() answers Unknown, not Other, for
// either of those.
//
// When the read happens matters as much as what it looks for. Their module doesn't
// write this address when it loads. Their plugin calls the module's Inkay_Initialize
// from its own INITIALIZE_PLUGIN, and the URL goes in then. Two plugins' init hooks
// run in the order the SD card's directory lists the files, which nobody controls, so
// a read from this plugin's INITIALIZE_PLUGIN found Nintendo's URL whenever this
// plugin happened to go first, and called their build somebody else's for the whole
// boot. That's #45. Every plugin has had its turn by the first application start, so
// the read is made there, in account_observe(), and before account_apply() writes.
//
// After the first application start the read only repeats if it got no answer, and by
// then the address may hold this plugin's own write, which is the Unknown answer above.
constexpr const char *PROTARIUM_MARKER = "protarium.";

// What the console holds there before any Inkay has written. Part of a host for the
// same reason as the marker: finding it says nothing about which build is installed,
// only that none has written yet.
constexpr const char *NINTENDO_ACCOUNT_HOST = "account.nintendo.net";

// Three states, not two: it may simply not be knowable.
enum class Host { Unknown, Protarium, Other };
Host host = Host::Unknown;

// The module their plugin talks to, and the export that answers what it's doing.
// Both names are upstream Inkay's, so every fork carries them: Protarium's shipped
// module exports these three symbols and so does Project Rose's, under the same
// module name. Which is exactly why this can't identify a build, and why the marker
// above still has to.
//
// What it can do is say whether an Inkay is loaded and patching, which the marker
// can't say at all once this plugin has written over the evidence. Two readings, two
// halves, neither one replaces the other.
constexpr const char *INKAY_MODULE = "inkay";
constexpr const char *INKAY_STATUS_EXPORT = "Inkay_GetStatus";

// Their return values, which are upstream's InkayStatus. Taken as a plain int rather
// than an enum of mine, because what crosses the call boundary is whatever their
// compiler left in the register, and assuming a width for somebody else's enum is how
// that goes wrong quietly.
constexpr int32_t INKAY_UNINITIALIZED = 0;
constexpr int32_t INKAY_NINTENDO = 1;
constexpr int32_t INKAY_PRETENDO = 2;

// What the module said, which is not the same question as which build it is.
//
// Unrecognized is a module answering to that name without the export, worth telling
// apart from absent: one means no Inkay, the other means an Inkay this plugin can't
// interrogate.
enum class Patcher { Unknown, Absent, Unrecognized, Idle, Patching };
Patcher patcher = Patcher::Unknown;

const char *status = "left alone";

// The one exception to leaving this address as the setting put it: lent to the other
// account server for the length of Miiverse's token request, then taken back. Juxt on
// Protarium lends it to Pretendo, Protaverse on Pretendo lends it to Protarium.
//
// Narrowed the way the other writes here are. The slot has to hold the selected
// account server's string before anything is written, the write is no longer than what
// was matched, and what goes back is the bytes that were read, not a string this file
// believes was there.
//
// While it's lent, every account request on the console goes to the other server, not
// just the one being routed. Protarium's account server forwards everything it does
// not answer itself to Pretendo, so in either direction the requests that land
// differently are the ones it answers, asked for in that same instant: service tokens
// for the clients it lists and NEX tokens for the games it runs servers for. Lent to
// Pretendo, Pretendo issues those instead. Lent to Protarium, Protarium issues them.
constexpr size_t LEND_MAX = 64;

char lent_original[LEND_MAX];
size_t lent_length = 0;
bool lent = false;

const char *lend_status = "not lent yet";

// Protarium's account URL as their module wrote it this boot, terminator included, so
// lending to Protarium writes back their own bytes instead of a string spelled out
// here. Zero length until look_for_protarium() has seen it.
//
// Kept from the one read that can see it. On Pretendo, account_apply() writes over it
// at the first application start and nothing puts it back until their module does at
// the next boot, so the read at plugin initialization is the only moment it's there
// to copy.
char protarium_url[LEND_MAX];
size_t protarium_url_length = 0;

// Check the address really holds a URL before overwriting it.
//
// An address that's right on one console firmware can be wrong on another. The cheap
// defense: by the time this runs, Protarium's module has already written its own URL
// here at boot. Finding a URL proves the address is good on this console. Finding
// anything else means this isn't where it thinks it is, and then the only safe move is
// to leave it alone. Writing into the wrong place in the security processor is the one
// thing here that could take a console down rather than just fail.
bool looks_like_url(const IosuHandle &iosu, uint32_t address) {
    char head[9] = {};
    if (!iosu.read_bytes(address, head, sizeof(head) - 1)) {
        return false;
    }
    return std::strncmp(head, "https://", 8) == 0;
}

// How long the string already at the address is, terminator included. Used instead of
// a hardcoded capacity, because how much room the console allocated here isn't
// knowable, and guessing wrong overruns whatever sits next door. What is knowable:
// something is already there, so writing no more than it occupies can't reach past
// it.
//
// This measures the incumbent string, not the slot, and that's only right here because
// Pretendo's account URL is shorter than Protarium's. It's not a general room
// measurement. spotpass.cpp goes into why the tasksheet slots needed a different one,
// and lending the account server to Protarium needs a different one too, for the same
// reason. prepare_lend_to_protarium() has it.
size_t existing_length(const IosuHandle &iosu, uint32_t address, size_t ceiling) {
    for (size_t offset = 0; offset < ceiling; offset += 4) {
        char bytes[4];
        if (!iosu.read_bytes(address + offset, bytes, sizeof(bytes))) {
            return 0;
        }
        for (size_t i = 0; i < 4; ++i) {
            if (bytes[i] == '\0') {
                return offset + i + 1;
            }
        }
    }
    return 0;
}

bool write_string(const IosuHandle &iosu, uint32_t address, const char *text, size_t room) {
    const size_t length = std::strlen(text) + 1;

    if (length > room) {
        return false;
    }
    return iosu.write_bytes(address, text, length);
}

// Keep their URL out of the buffer look_for_protarium() read, terminator included. A
// string with no terminator inside what was read is dropped: its length isn't known,
// and lending to Protarium writes exactly as many bytes as this records.
void keep_protarium_url(const char *buffer, size_t read) {
    const size_t length = strnlen(buffer, read);
    if (length >= read) {
        return;
    }
    std::memcpy(protarium_url, buffer, length + 1);
    protarium_url_length = length + 1;
}

Host look_for_protarium() {
    char buffer[64] = {};
    const IosuHandle iosu;
    if (!iosu.read_bytes(ACCOUNT_URL_ADDRESS, buffer, sizeof(buffer) - 1)) {
        LOG("could not read IOSU at %08x, cannot tell which Inkay is installed",
            ACCOUNT_URL_ADDRESS);
        return Host::Unknown;
    }
    buffer[sizeof(buffer) - 1] = '\0';

    if (std::strstr(buffer, PROTARIUM_MARKER) != nullptr) {
        keep_protarium_url(buffer, sizeof(buffer) - 1);
        return Host::Protarium;
    }

    // Asked before falling through to Other, because this is what account_apply()
    // leaves at this address. Finding it means the first title already applied the
    // setting, not that Protarium's build is missing: the read is just too late to
    // identify anybody. Other would put a warning on screen that contradicts the
    // account line right beside it.
    if (std::strcmp(buffer, PRETENDO_ACCOUNT_URL) == 0) {
        return Host::Unknown;
    }

    // Nintendo's own URL means no Inkay has written yet, which can't name a build.
    // Calling that Other is what put #45's warning on screen.
    if (std::strstr(buffer, NINTENDO_ACCOUNT_HOST) != nullptr) {
        return Host::Unknown;
    }

    return Host::Other;
}

// Ask their module directly.
//
// The one place this plugin calls into their code instead of reading memory they left
// behind, so it's worth being exact about what that costs. The export takes no
// arguments, returns one integer and has no side effect on the console. Nothing here
// writes, so this runs on every setting including the defaults.
//
// The handle is acquired and released around the one call rather than held, and
// releasing it doesn't take their module away: their own plugin releases the same
// handle at deinitialization and their module keeps running afterwards, which its
// application-start hook doing its work proves. Holding it instead would mean parking
// a reference to somebody else's module in a plugin global that outlives the process
// it was made in, which is the worse of the two.
//
// If a later Inkay changes what the export means, the damage is a wrong answer on one
// menu line, not a wrong write: nothing downstream of this decides what goes into
// memory. Only account_warning() and the menu read it.
Patcher ask_the_module() {
    OSDynLoad_Module module = nullptr;
    if (OSDynLoad_Acquire(INKAY_MODULE, &module) != OS_DYNLOAD_OK || module == nullptr) {
        // Nothing answers to that name. A module can't show up later in a
        // session, so this is the one answer here that's final the moment it's
        // given.
        return Patcher::Absent;
    }

    int32_t (*get_status)() = nullptr;
    const OSDynLoad_Error found = OSDynLoad_FindExport(
        module, OS_DYNLOAD_EXPORT_FUNC, INKAY_STATUS_EXPORT,
        reinterpret_cast<void **>(&get_status));

    Patcher answer = Patcher::Unrecognized;
    if (found == OS_DYNLOAD_OK && get_status != nullptr) {
        switch (get_status()) {
            case INKAY_PRETENDO:
                answer = Patcher::Patching;
                break;
            case INKAY_NINTENDO:
                answer = Patcher::Idle;
                break;
            case INKAY_UNINITIALIZED:
                // Their module is loaded and their plugin hasn't initialized it
                // yet. That's a moment, not a state, and it's why this probe
                // doesn't run from plugin initialization: two plugins initializing
                // have no order between them. By an application start every plugin
                // has had its turn, so seeing this here means something did go
                // wrong, just not something this can name.
                answer = Patcher::Unknown;
                break;
            default:
                answer = Patcher::Unknown;
                break;
        }
    }

    OSDynLoad_Release(module);
    return answer;
}

} // namespace

const char *account_warning() {
    // Asked in this order because each answer makes the next one moot. No point
    // telling somebody their Inkay is the wrong build when no Inkay is loaded, or
    // that the build is wrong when the one they have is switched off.
    switch (patcher) {
        case Patcher::Absent:
            return "Inkay is not installed. This plugin supplements that build.";
        case Patcher::Idle:
            return "Inkay is installed with its network off. Nothing here applies.";
        case Patcher::Unrecognized:
        case Patcher::Patching:
        case Patcher::Unknown:
            break;
    }

    // Only now does whose-build-is-it matter, and only the marker can answer it.
    // Host::Other means the address held a URL that's neither Protarium's nor
    // this plugin's own, which with a module patching means some other fork.
    if (host == Host::Other) {
        return "The Inkay installed is not Protarium's. This does nothing.";
    }

    return nullptr;
}

const char *account_host_name() {
    switch (host) {
        case Host::Protarium: return "Protarium";
        case Host::Other:     return "not recognized";
        case Host::Unknown:   break;
    }
    return "not checked";
}

const char *account_module_state() {
    switch (patcher) {
        case Patcher::Patching:     return "loaded, patching";
        case Patcher::Idle:         return "loaded, network off";
        case Patcher::Absent:       return "not loaded";
        case Patcher::Unrecognized: return "loaded, no status export";
        case Patcher::Unknown:      break;
    }
    return "not answered";
}

const char *account_status() {
    return status;
}

const char *account_protarium_url() {
    return protarium_url_length > 0 ? protarium_url : nullptr;
}

void account_init() {
    // Protarium's module initializes Mocha too. This is a client library talking
    // to an IOSU module, so a second client is expected, not a conflict, and
    // failing here costs the account setting and nothing else.
    if (!iosu_init()) {
        status = "refused (no Mocha)";
    }

    // Deliberately no read of the account URL here. See the note by PROTARIUM_MARKER:
    // at this point their plugin may not have asked their module to write it yet.
}

namespace {

// Which build wrote the account URL. Asked once and kept.
//
// A plugin global outlives the process it was set in, so one definite answer serves
// the whole session. Unknown isn't an answer, so it isn't latched: a read that failed,
// or found nothing that names a build, gets another turn at the next application start.
void identify_host() {
    if (host != Host::Unknown || !iosu_ready()) {
        return;
    }

    host = look_for_protarium();
    switch (host) {
        case Host::Protarium:
            LOG("Protarium's Inkay looks present");
            break;
        case Host::Other:
            LOG("Protarium's Inkay was not found, this plugin supplements that build");
            break;
        case Host::Unknown:
            break;
    }
}

} // namespace

void account_observe() {
    // First, because account_apply() runs right after this and may write over what it
    // reads.
    identify_host();

    // Latched the same way, and for the same reason, as the host above. A probe that
    // came too early, or found their plugin still uninitialized, gets another turn at
    // the next application start.
    if (patcher != Patcher::Unknown) {
        return;
    }

    patcher = ask_the_module();
    switch (patcher) {
        case Patcher::Patching:
            LOG("an Inkay module is loaded and patching");
            break;
        case Patcher::Idle:
            LOG("an Inkay module is loaded with its network setting off");
            break;
        case Patcher::Absent:
            LOG("no Inkay module is loaded, this plugin supplements one");
            break;
        case Patcher::Unrecognized:
            LOG("a module named %s is loaded but does not export %s",
                INKAY_MODULE, INKAY_STATUS_EXPORT);
            break;
        case Patcher::Unknown:
            break;
    }
}

void account_shutdown() {
    iosu_shutdown();
}

void account_apply(Account chosen) {
    if (chosen != Account::Pretendo) {
        // Leaving the address alone is how Protarium stays selected. It is never
        // written back: their module already restores it on the next boot, and a
        // plugin racing a module to own one address is worse than deferring to it.
        // So switching back takes a reboot, which is what the menu says.
        return;
    }

    if (!iosu_ready()) {
        return;
    }

    // One handle for the whole check and write, opened in this title's own process. A
    // process that can't open one leaves the status alone rather than reporting a
    // refusal: whatever an earlier title wrote is still in place, and the line should
    // keep saying so.
    const IosuHandle iosu;
    if (!iosu.is_open()) {
        return;
    }

    const size_t length = std::strlen(PRETENDO_ACCOUNT_URL) + 1;

    if (iosu.matches(ACCOUNT_URL_ADDRESS, PRETENDO_ACCOUNT_URL, length)) {
        status = "pointed at Pretendo";
        return;
    }

    if (!looks_like_url(iosu, ACCOUNT_URL_ADDRESS)) {
        LOG("%08x does not hold a URL, refusing to write the account server there",
            ACCOUNT_URL_ADDRESS);
        status = "refused (not a URL)";
        return;
    }

    const size_t room = existing_length(iosu, ACCOUNT_URL_ADDRESS, 256);
    if (room == 0 || length > room) {
        LOG("account URL needs %u bytes and the slot holds %u, leaving it alone",
            static_cast<unsigned>(length), static_cast<unsigned>(room));
        status = "refused (no room)";
        return;
    }

    if (write_string(iosu, ACCOUNT_URL_ADDRESS, PRETENDO_ACCOUNT_URL, room)) {
        LOG("account server pointed at Pretendo");
        status = "pointed at Pretendo";
    } else {
        status = "refused (write failed)";
    }
}

namespace {

// Lending to Pretendo, from Protarium. Returns how many bytes were read into
// lent_original, which is also how many the take-back writes, or zero with
// lend_status saying why nothing may be written.
size_t prepare_lend_to_pretendo(const IosuHandle &iosu) {
    // How long the string already there is, terminator included, which is the most
    // this may write. It's measured by reading, so a process IOSU won't answer
    // stops here, before anything has been written.
    const size_t length = existing_length(iosu, ACCOUNT_URL_ADDRESS, LEND_MAX);
    if (length == 0 || !iosu.read_bytes(ACCOUNT_URL_ADDRESS, lent_original, length)) {
        lend_status = "refused (unreadable)";
        return 0;
    }

    // Protarium's string and nothing else. Pretendo's already in the slot would mean
    // an earlier take-back failed, and anything else isn't the build this was
    // written against.
    if (std::strncmp(lent_original, "https://", 8) != 0 ||
        std::strstr(lent_original, PROTARIUM_MARKER) == nullptr) {
        lend_status = "refused (not Protarium's)";
        return 0;
    }

    if (std::strlen(PRETENDO_ACCOUNT_URL) + 1 > length) {
        lend_status = "refused (no room)";
        return 0;
    }
    return length;
}

// Lending to Protarium, from Pretendo, which is the direction existing_length() can't
// measure. Protarium's URL is longer than Pretendo's, so the string in the slot is
// shorter than what goes in, and measuring to its terminator would refuse a write their
// own module makes at every boot.
//
// So the room gets proved the way spotpass.cpp proves the tasksheet slots. Pretendo's
// shorter write left the end of Protarium's string standing past its terminator,
// because the writes here keep every byte past the run they write. Matching Pretendo's
// string and then that leftover, as far as Protarium's string reached, proves three
// things at once: the address is right, account_apply()'s write was the last thing to
// touch it, and the slot held Protarium's full length this boot, because their module
// put it there. What goes in is exactly that length, so nothing is written past what
// was matched.
//
// Measured holding on hardware. The first run of Protaverse on Pretendo read "lent and
// taken back", which it could only do with the copy from boot in place and this match
// passing.
//
// Returns how many bytes were read into lent_original, or zero with lend_status saying
// why nothing may be written.
size_t prepare_lend_to_protarium(const IosuHandle &iosu) {
    if (protarium_url_length == 0) {
        lend_status = "refused (Protarium's not seen)";
        return 0;
    }

    const size_t pretendo_length = std::strlen(PRETENDO_ACCOUNT_URL) + 1;
    const size_t width =
        protarium_url_length > pretendo_length ? protarium_url_length : pretendo_length;

    char expected[LEND_MAX];
    std::memcpy(expected, PRETENDO_ACCOUNT_URL, pretendo_length);
    if (width > pretendo_length) {
        std::memcpy(expected + pretendo_length, protarium_url + pretendo_length,
                    width - pretendo_length);
    }

    if (!iosu.read_bytes(ACCOUNT_URL_ADDRESS, lent_original, width)) {
        lend_status = "refused (unreadable)";
        return 0;
    }

    // Anything else is refused the same way, and one case is worth naming. Protarium's
    // own string still in the slot means account_apply() never wrote, so the request
    // goes to Protarium without any help and the account server line says why the write
    // didn't happen.
    if (std::memcmp(lent_original, expected, width) != 0) {
        lend_status = "refused (not Pretendo's)";
        return 0;
    }
    return width;
}

} // namespace

bool account_lend(const IosuHandle &iosu, Account to) {
    // One at a time. A second request arriving while the first is out would find the
    // other server's string in the slot and be refused below anyway, but saying so
    // here keeps the reason readable.
    if (lent) {
        lend_status = "refused (already lent)";
        return false;
    }

    if (!iosu_ready()) {
        lend_status = "refused (no Mocha)";
        return false;
    }

    // Mocha is installed but the asking process couldn't open it. Different fault
    // from the address holding something unexpected, and it says nothing about the
    // address.
    if (!iosu.is_open()) {
        lend_status = "refused (no Mocha here)";
        return false;
    }

    const bool to_pretendo = to == Account::Pretendo;
    const size_t length =
        to_pretendo ? prepare_lend_to_pretendo(iosu) : prepare_lend_to_protarium(iosu);
    if (length == 0) {
        return false;
    }

    const char *replacement = to_pretendo ? PRETENDO_ACCOUNT_URL : protarium_url;
    const size_t replacement_length =
        to_pretendo ? std::strlen(PRETENDO_ACCOUNT_URL) + 1 : protarium_url_length;

    lent_length = length;
    if (!iosu.write_bytes(ACCOUNT_URL_ADDRESS, replacement, replacement_length)) {
        // A write that stops partway leaves part of each string, which signs in
        // nowhere. Put the original back rather than leave that.
        const bool restored = iosu.write_bytes(ACCOUNT_URL_ADDRESS, lent_original, lent_length);
        LOG("lending the account server failed, %s", restored ? "restored" : "not restored");
        lend_status = restored ? "refused (write failed)" : "write failed, reboot";
        return false;
    }

    lent = true;
    return true;
}

void account_take_back(const IosuHandle &iosu) {
    if (!lent) {
        return;
    }
    lent = false;

    // The bytes that were read, not either URL spelled out here, which is what keeps
    // this right for whatever was there. It is also exactly as many bytes as were
    // matched before the lend, and it goes through the handle the lend opened, so
    // putting the address back never depends on opening anything.
    if (iosu.write_bytes(ACCOUNT_URL_ADDRESS, lent_original, lent_length)) {
        lend_status = "lent and taken back";
        return;
    }
    LOG("could not put the account server back, it points at the other network until a reboot");
    lend_status = "take-back failed, reboot";
}

const char *account_lend_status() {
    return lend_status;
}
