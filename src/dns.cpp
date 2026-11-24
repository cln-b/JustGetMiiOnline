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

#include "dns.h"
#include "youtube.h"
#include "youtube/proxy.h"

#include <wups.h>

#include <coreinit/debug.h>
#include <netdb.h>
#include <nsysnet/_netdb.h>
#include <cstdio>
#include <cstring>

namespace {

// Refuse the lookup rather than trying to undo the redirect.
//
// Undoing it can't be expressed. Their redirects aren't one to one, so a destination
// doesn't tell you what the game asked for. Their build also redirects any name
// containing one of three publisher domains, which makes the set of originals
// open-ended. There is no function from what arrives back to what was asked.
//
// Refusing needs none of that. The games this covers have no server on Pretendo,
// so the honest answer to the lookup is that the name doesn't resolve, which is
// what the console would be told if nobody had patched anything. It's also the
// fast answer: this runs on the thread the game is waiting on, and resolving a
// long-dead Nintendo, Activision, Ubisoft or Bandai Namco name would burn the
// resolver's whole timeout to arrive at the same place.

// What Protarium's build produces, for the case where their hook ran first and
// this one is looking at their output.
//
// A rule with a few exceptions, and it used to be a list of seven game hosts. That
// list was the wrong way round: it grew every time they added a game, so keeping it
// current meant tracking their releases, and any name they added that nobody here
// had heard of stayed redirected.
//
// Inverted, the maintenance follows the half that barely moves. Everything they
// redirect lands on their domain, so the rule is the domain and the exceptions are
// their own infrastructure, which is a short list and changes rarely. A game they
// add tomorrow is covered the day they ship it, with no release from here.
//
// The risk runs the other way now: a new infrastructure host of theirs gets refused
// until it's listed. That's visible rather than silent, because every refusal shows
// up in the recent lookups in the config menu with a cross beside it, and nothing is
// refused at all unless the account server is Pretendo.
//
// Two domains, because their builds are on two. The older two redirect to protarium.lol
// and r17 to protarium.online, and the rule is the same on both. Spelled with the
// leading dot the suffix test needs, and the bare domain is the same string past it. The
// .lol one goes with #29.
constexpr const char *PROTARIUM_DOMAINS[] = {
    ".protarium.lol",
    ".protarium.online",
};

// The names left resolving: Protaverse's three hosts and their account server on either
// domain, and on protarium.online their CDN and SpotPass hosts as well.
//
// Protaverse's are disc. for discovery, api. for the API and portal. for the portal, on
// protarium.lol for the older builds and protarium.online for r17. Protaverse is offered
// on a Pretendo account, where token.cpp routes its token request to their account
// server, so these are a destination a setting here selects, and refusing them would
// leave that pairing signed in with nowhere to go. An earlier version of this comment
// said no setting selected any of them on Pretendo, which was true until Protaverse was
// offered there.
//
// The portal is the one nothing has measured being asked for here. These hooks don't
// run in the Miiverse applet, which is what opens it, so it only matters to a game or
// the Wii U Menu that asks, and it's kept so that one wouldn't be refused.
// api.protarium.lol is also the tasksheet host in their older module. Their newer one
// moved SpotPass to npts.app.protarium.lol and nppl.app.protarium.lol, and neither is
// kept, because Protarium's SpotPass is not an answer a Pretendo account is offered.
//
// r17 is different, and on purpose. It redirects Nintendo's own CDN and SpotPass names
// (npdi.cdn., npdl.cdn., npfl.c.app., nppl.app., npts.app. and service.spr.app.) to
// same-named hosts on protarium.online, and refusing those would take content downloads
// away from a Pretendo account along with Protarium's SpotPass. So they're kept: the
// SpotPass addresses this plugin writes still decide whose tasksheets the console asks
// for, and the CDN names are content rather than a service. The services r17 added,
// ama., nfs. and demaecan., are not kept, so the domain rule refuses them like any game.
//
// The account server is kept because it's the subject of a write rather than of a
// refusal, which was once the whole reason for all of these. The account URL, and on
// the other pairings the Miiverse slot and the SpotPass addresses, are decided by
// moving a string somewhere else, so a name hook that refuses those names is a second,
// blunter enforcement of decisions those writes already made. Blunter matters:
// account.cpp alone reports four ways its write can fail, and in each of them the
// console is still signed in against their server with the menu saying so, which is
// this plugin degrading to doing nothing rather than breaking. Refusing the name on
// top turns a reported refusal into a console that can't sign in anywhere. One more
// reason only points forward: work that rewrites where a lookup goes, rather than
// refusing it, needs the name to reach this hook at all.
//
// The trade, written down so nobody has to rediscover it: if one of those writes
// doesn't land, the console reaches their service quietly instead of failing. On Juxt
// or Roseverse that means a Miiverse swap that didn't land reaches Protaverse. It's
// not silent in the menu, though. The discovery host line names whichever host was
// actually resolved, and the swap counters beside it say whether anything was written,
// so the pair reads as the fault it is.
//
// Exact matches, never suffixes: disc.protarium.lol keeps working while
// jd.protarium.lol doesn't, and only the whole name separates them. The first four go
// with #29.
constexpr const char *PROTARIUM_KEEP[] = {
    "disc.protarium.lol",
    "api.protarium.lol",
    "portal.protarium.lol",
    "account.protarium.lol",
    "disc.protarium.online",
    "api.protarium.online",
    "portal.protarium.online",
    "account.protarium.online",
    "npdi.cdn.protarium.online",
    "npdl.cdn.protarium.online",
    "npfl.c.app.protarium.online",
    "nppl.app.protarium.online",
    "npts.app.protarium.online",
    "spr.protarium.online",
};

// What the games ask for, for the case where this hook runs first and theirs hasn't
// had its turn yet.
//
// The Black Ops II and Just Dance names are deliberately absent here and handled
// by the domain test below instead. Their build catches every name on those
// publishers' domains by rule, so listing the ones this plugin happens to know
// would leave exactly the names nobody predicted still redirected. The two Need for
// Speed names are listed because their build matches those two exactly and nothing
// wider.
//
// The Nintendo CDN and SpotPass names r17 redirects are deliberately absent too. Their
// redirected spellings are kept above, so refusing the originals here would refuse on
// one nesting what the other lets through.
constexpr const char *GAME_ORIGINALS[] = {
    "ssl.wahp.wah.wup.app.nintendo.net",
    "goshawk.capcom.co.jp",
    "mars.project-treasure.bng.jp", // Lost Reavers, added in the r10 build
    "gosredirector.ea.com",         // Need for Speed, added in the September 29 build
    "gosca.ea.com",                 // Need for Speed, added in the September 29 build
    "wup-ama.app.nintendo.net",     // added in the r17 build
    "nws.demae-can.com",            // added in the r17 build
    // Disney Infinity 1.0, 2.0 and 3.0, on their disney-infinity-url branch and not in
    // a release yet. They send it to api.protarium.online, which is kept above, so
    // this only catches it on the nesting where this hook sees the original name.
    "api.disney.com",
};

// The domains their build catches by rule, tested as substrings because their
// rule is.
//
// Black Ops II was the only one of these when this was written. The r10 build
// added Just Dance the same way, and for the same reason it has to be matched
// the same way: the set of names caught is open-ended, so listing the few this
// plugin happens to know would leave exactly the names nobody predicted still
// redirected.
//
// Nothing outside these domains reaches the test, so no other game's lookups can
// be caught by them.
constexpr const char *RULE_DOMAINS[] = {
    "demonware.net",
    "just-dance.com",
    "ubiservices.ubi.com",
};

Account account = Account::Protarium;

size_t seen = 0;
size_t refused = 0;

// Which nesting was observed, and only ever set from a name that proves it.
enum class Nesting { Unknown, Outer, Inner };
Nesting nesting = Nesting::Unknown;

constexpr size_t RECENT_SLOTS = 6;
char recent[RECENT_SLOTS][96] = {};
bool recent_refused[RECENT_SLOTS] = {};
size_t recent_next = 0;
size_t recent_held = 0;
size_t last_slot = 0;

// ASCII case folding, written out rather than reached for in a header. A game may
// ask for a name in any case it likes, and a hostname never contains a byte where
// ASCII folding and locale folding disagree.
char fold(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ci(const char *a, const char *b) {
    while (fold(*a) == fold(*b)) {
        if (*a == '\0') {
            return true;
        }
        ++a;
        ++b;
    }
    return false;
}

// A suffix test rather than a substring one, because the domain rule below turns
// on where the domain sits in the name. "protarium.lol.example.com" contains one of
// their domains and is not theirs, and only the end of the name can tell those apart.
bool ends_with_ci(const char *name, const char *suffix) {
    const size_t name_length = std::strlen(name);
    const size_t suffix_length = std::strlen(suffix);
    if (name_length < suffix_length) {
        return false;
    }

    const char *tail = name + name_length - suffix_length;
    for (size_t i = 0; i < suffix_length; ++i) {
        if (fold(tail[i]) != fold(suffix[i])) {
            return false;
        }
    }
    return true;
}

bool contains_ci(const char *haystack, const char *needle) {
    for (; *haystack != '\0'; ++haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*n != '\0' && fold(*h) == fold(*n)) {
            ++h;
            ++n;
        }
        if (*n == '\0') {
            return true;
        }
    }
    return false;
}

// One of their domains, or any name under one.
bool is_protarium(const char *name) {
    for (const char *domain : PROTARIUM_DOMAINS) {
        if (equals_ci(name, domain + 1) || ends_with_ci(name, domain)) {
            return true;
        }
    }
    return false;
}

// Which Miiverse discovery host the console actually resolved, and in which
// process, kept separately from the list above and never reset.
//
// The list above answers "what did this title ask for" and is cleared per title
// so it stays about the title you're in. That makes it useless for the one
// question that matters here, which is whether a Roseverse host is ever reached
// at all. The answer can be produced inside the Miiverse applet, and the config
// menu can't be opened from there, so anything that gets cleared on the way back
// to the menu can't carry it. This is five bits and five process ids, so keeping
// it for the whole boot costs nothing.
//
// The swap only ever moves this one string, so these spellings are the whole space:
// nobody's, Protarium's on either of their domains, Juxt's, Rose's. Both of Protarium's
// say Protaverse, so the line reads the same whichever build is installed, and the .lol
// one goes with #29.
struct DiscoveryHost {
    const char *name;
    const char *label;
};

constexpr DiscoveryHost DISCOVERY_HOSTS[] = {
    {"discovery.olv.nintendo.net", "Nintendo"},
    {"disc.protarium.lol", "Protaverse"},
    {"disc.protarium.online", "Protaverse"},
    {"discovery.olv.pretendo.cc", "Juxt"},
    {"disco.olv.projectrose.cafe", "Roseverse"},
};
constexpr size_t DISCOVERY_COUNT = sizeof(DISCOVERY_HOSTS) / sizeof(DISCOVERY_HOSTS[0]);

bool discovery_seen[DISCOVERY_COUNT] = {};
uint32_t discovery_pid[DISCOVERY_COUNT] = {};
char discovery_line[128];

void note_discovery(const char *name) {
    for (size_t i = 0; i < DISCOVERY_COUNT; ++i) {
        if (!equals_ci(name, DISCOVERY_HOSTS[i].name)) continue;
        if (!discovery_seen[i]) {
            discovery_seen[i] = true;
            discovery_pid[i] = static_cast<uint32_t>(OSGetUPID());
        }
        return;
    }
}

void remember(const char *name) {
    ++seen;
    note_discovery(name);

    // Kept always, and it used to follow the debug setting. That was wrong twice
    // over. This list is the only place you can see a name that reaches their
    // redirect and not this one, and anybody can read it in the config menu, while
    // the setting it followed governs a log that needs a debugger. So the one
    // diagnostic that can actually be read was off by default, behind a switch
    // whose own output nobody on a console can see.
    //
    // Six names and six flags cost nothing to keep. The refusal marker below
    // depends on last_slot being current, which is the other half of why the gate
    // had to go: with it closed, last_slot went stale and a refusal marked a slot
    // that nothing displayed.
    last_slot = recent_next;
    recent_refused[recent_next] = false;
    std::strncpy(recent[recent_next], name, sizeof(recent[0]) - 1);
    recent[recent_next][sizeof(recent[0]) - 1] = '\0';
    recent_next = (recent_next + 1) % RECENT_SLOTS;
    if (recent_held < RECENT_SLOTS) {
        ++recent_held;
    }
}

// Whether this name should be refused, recording on the way what its spelling
// says about which hook ran first.
bool should_refuse(const char *name) {
    // Their domain, minus their own infrastructure. Seeing one of these at all
    // means their hook rewrote the name before this one saw it, which is the
    // nesting this records.
    if (is_protarium(name)) {
        for (const char *keep : PROTARIUM_KEEP) {
            if (equals_ci(name, keep)) {
                return false;
            }
        }
        nesting = Nesting::Inner;
        return true;
    }

    for (const char *original : GAME_ORIGINALS) {
        if (equals_ci(name, original)) {
            nesting = Nesting::Outer;
            return true;
        }
    }
    for (const char *domain : RULE_DOMAINS) {
        if (contains_ci(name, domain)) {
            nesting = Nesting::Outer;
            return true;
        }
    }
    return false;
}

// Record every lookup even on the default setting, so you can run this as a plain
// observer alongside their module and compare a configuration that works against
// one that doesn't.
bool refuse(const char *name) {
    if (name == nullptr) {
        return false;
    }

    remember(name);

    if (account != Account::Pretendo) {
        return false;
    }

    if (!should_refuse(name)) {
        return false;
    }

    ++refused;
    recent_refused[last_slot] = true;
    return true;
}

} // namespace

void dns_set_account(Account chosen) {
    account = chosen;
}

void reset_dns_counters() {
    seen = 0;
    refused = 0;
    recent_next = 0;
    recent_held = 0;
    for (bool &flag : recent_refused) {
        flag = false;
    }
    // Deliberately not cleared. Which way the hooks nest is a property of the
    // boot rather than of the title, and clearing it would throw away the answer
    // every time a game starts.
}

// Names every Miiverse discovery host that was actually resolved, with the process
// each was first seen in. "none" after a Miiverse attempt is the finding rather
// than a gap: it says the console never asked for any of them, so whatever was
// going to do the asking had already done it before the swap landed.
const char *dns_discovery_seen() {
    char *p = discovery_line;
    const char *const end = discovery_line + sizeof(discovery_line);
    bool any = false;
    for (size_t i = 0; i < DISCOVERY_COUNT && p < end; ++i) {
        if (!discovery_seen[i]) continue;
        p += std::snprintf(p, end - p, "%s%s(pid %u)", any ? " " : "",
                           DISCOVERY_HOSTS[i].label, discovery_pid[i]);
        any = true;
    }
    return any ? discovery_line : "none";
}

size_t dns_lookups_seen() {
    return seen;
}

size_t dns_lookups_refused() {
    return refused;
}

const char *dns_nesting() {
    switch (nesting) {
        case Nesting::Outer: return "ours runs first";
        case Nesting::Inner: return "theirs runs first";
        case Nesting::Unknown: break;
    }
    return "no redirected name yet";
}

size_t dns_recent_count() {
    return recent_held;
}

// Oldest first, so the menu reads in the order the game asked.
const char *dns_recent(size_t index) {
    if (index >= recent_held) {
        return "";
    }
    const size_t start = (recent_held == RECENT_SLOTS) ? recent_next : 0;
    return recent[(start + index) % RECENT_SLOTS];
}

bool dns_recent_refused(size_t index) {
    if (index >= recent_held) {
        return false;
    }
    const size_t start = (recent_held == RECENT_SLOTS) ? recent_next : 0;
    return recent_refused[(start + index) % RECENT_SLOTS];
}

DECL_FUNCTION(struct hostent *, gethostbyname, const char *name) {
    if (refuse(name)) {
        // This call reports failure through h_errno as well as through the return
        // value, and since nothing downstream runs, a caller that reads it would
        // otherwise get whatever the last lookup left there. After a lookup that
        // succeeded that value is "no error", which is the worst of the two ways to
        // be wrong: a null result alongside a success code.
        //
        // Set through the accessor rather than by assigning h_errno, which isn't
        // the same variable. The plain name binds to the copy inside this plugin's
        // own image, while the game reads the one the network library owns, so the
        // obvious spelling compiles, runs, stores a 1 somewhere nobody reads, and
        // leaves the hazard above exactly as it was.
        *RPLWRAP(get_h_errno)() = HOST_NOT_FOUND;
        return nullptr;
    }
    return real_gethostbyname(name);
}

// The YouTube Patcher answers two names here too, which youtube_answer hands back as what to
// resolve instead. Every other name comes back as it was asked.
DECL_FUNCTION(int, getaddrinfo, const char *node, const char *service,
              const struct addrinfo *hints, struct addrinfo **res) {
    if (refuse(node)) {
        return EAI_NONAME;
    }
    return real_getaddrinfo(youtube_answer(node), service, hints, res);
}

// The YouTube proxy's own lookups, which must not come back through the answer above. Asking for
// www.youtube.com through it would get the revival host, and the proxy would forward to it in a
// circle.
int proxy_resolve(const char *node, const char *service, const struct addrinfo *hints,
                  struct addrinfo **res) {
    return real_getaddrinfo(node, service, hints, res);
}

// Game and menu. Anything narrower would leave Protarium's redirects in place in
// whichever process this one didn't cover.
WUPS_MUST_REPLACE_FOR_PROCESS(gethostbyname, WUPS_LOADER_LIBRARY_NSYSNET, gethostbyname,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(getaddrinfo, WUPS_LOADER_LIBRARY_NSYSNET, getaddrinfo,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);
