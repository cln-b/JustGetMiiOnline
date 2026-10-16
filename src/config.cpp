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

#include "config.h"
#include "account.h"
#include "dns.h"
#include "logger.h"
#include "notify.h"
#include "olv.h"
#include "plaza.h"
#include "secret.h"
#include "spotpass.h"
#include "token.h"
#include "youtube.h"
#include "youtube/settings.h"

#include <wups.h>
#include <wups/storage.h>
#include <wups/config_api.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config/WUPSConfigItemStub.h>

// Both for the relaunch at the foot of this file. Already covered by -lwut, so the
// Makefile is unchanged.
#include <coreinit/launch.h>
#include <sysapp/launch.h>

#include <array>
#include <cstdio>

Miiverse Config::miiverse = Miiverse::Protaverse;
Account Config::account = Account::Protarium;
bool Config::jdmo_enabled = false;
SpotPass Config::spotpass = SpotPass::Protarium;
bool Config::change_needs_reboot = false;
bool Config::advanced_overrides = false;

namespace {

constexpr const char *KEY_MIIVERSE = "miiverse";
constexpr const char *KEY_ACCOUNT = "account";
constexpr const char *KEY_JDMO = "jdmo_enabled";
constexpr const char *KEY_SPOTPASS = "spotpass";
constexpr const char *KEY_DEBUG_LOG = "debug_log";
constexpr const char *KEY_ADVANCED_OVERRIDES = "advanced_overrides";

void log_storage(WUPSStorageError err, const char *what) {
    if (err != WUPS_STORAGE_ERROR_SUCCESS) {
        LOG("storage %s: %s", what, WUPSStorageAPI_GetStatusStr(err));
    }
}

WUPSConfigAPICallbackStatus log_config(WUPSConfigAPIStatus err, int line) {
    LOG("config error at line %d: %s", line, WUPSConfigAPI_GetStatusStr(err));
    return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
}

// Move the SpotPass choice to what the new pairing wants, after either of the two
// settings above changes.
//
// The pairing's default, not just a legal value, and hardware is what found the
// difference: on Pretendo, changing the Miiverse from Juxt to Roseverse left SpotPass
// on Pretendo. Pretendo's own SpotPass is still a legal answer there, so a validity
// check had nothing to say, but the pairing had just gained Rose's and that's the one
// it wants. What changed was the answer, not the legality.
//
// A default, not a lock. A SpotPass choice belongs to the pairing it was made in, so
// changing that pairing re-derives it, and the item is right there to set it again.
// The Miiverse isn't re-derived this way at all: an account switch leaves it where it
// is, which account_changed sets out.
//
// Boot deliberately isn't this. Config::Init honors a stored value that's legal for
// its pairing, because a choice made and stored isn't a pairing change.
void sync_spotpass() {
    const SpotPass wanted = spotpass_default(Config::account, Config::miiverse);
    if (Config::spotpass == wanted) {
        return;
    }

    LOG("SpotPass follows this pairing from %s to %s", spotpass_name(Config::spotpass),
        spotpass_name(wanted));

    Config::spotpass = wanted;
    Config::change_needs_reboot = true;
    log_storage(WUPSStorageAPI_StoreU32(nullptr, KEY_SPOTPASS, static_cast<uint32_t>(wanted)),
                KEY_SPOTPASS);
}

// The only place the Miiverse setting gets written, so the stored value, the snapshot
// the applet process reads and what the menu shows can't drift apart.
void set_miiverse(Miiverse chosen) {
    if (chosen != Config::miiverse) {
        Config::change_needs_reboot = true;
    }
    Config::miiverse = chosen;

    // The applet process can't read storage, so the snapshots it does read have to be
    // kept level with the setting here. Both of them: the URL swap and the token hook
    // run in that process and neither can look the setting up itself.
    olv_set_target(chosen);
    token_set_target(chosen);

    log_storage(WUPSStorageAPI_StoreU32(nullptr, KEY_MIIVERSE, static_cast<uint32_t>(chosen)),
                KEY_MIIVERSE);

    // Last, so it decides against the Miiverse just stored and not the one being
    // replaced. Leaving Roseverse is what takes Rose's SpotPass off the menu.
    sync_spotpass();
}

void spotpass_changed(ConfigItemMultipleValues *, uint32_t value) {
    const auto chosen = static_cast<SpotPass>(value);
    if (chosen == Config::spotpass) {
        return;
    }

    // The item only offers what was legal for the pairing the menu opened on, and this
    // runs after everything else changed in the same visit: Aroma calls the Debug
    // page's items first and then these in the order they were added, so Advanced
    // Overrides, the account item and the Miiverse item have already had their say. A
    // pick the pairing no longer allows gets dropped here. It's also the only check on
    // a number that came from outside this file.
    if (!spotpass_valid(Config::account, Config::miiverse, chosen, Config::advanced_overrides)) {
        LOG("ignoring a SpotPass value this pairing does not offer: %u", value);
        return;
    }

    Config::change_needs_reboot = true;
    Config::spotpass = chosen;
    log_storage(WUPSStorageAPI_StoreU32(nullptr, KEY_SPOTPASS, value), KEY_SPOTPASS);
}

// Turning it off takes Rose's SpotPass away from Protarium, so a console using it there
// moves to Protarium's, the way a pairing change would move it, and backing out
// relaunches the console for that. Turning it on moves nothing, because Protarium's
// stays the default: the SpotPass selector shows up the next time the menu is opened,
// since the menu's items are built when it opens.
void advanced_overrides_changed(ConfigItemBoolean *, bool value) {
    Config::advanced_overrides = value;
    log_storage(WUPSStorageAPI_StoreU32(nullptr, KEY_ADVANCED_OVERRIDES, value ? 1u : 0u),
                KEY_ADVANCED_OVERRIDES);

    if (!spotpass_valid(Config::account, Config::miiverse, Config::spotpass, value)) {
        sync_spotpass();
    }
}

void miiverse_changed(ConfigItemMultipleValues *, uint32_t value) {
    const auto chosen = static_cast<Miiverse>(value);

    // The item only ever offers the three, so this can't fall through from the menu.
    // It's here for the same reason the SpotPass check is: the number came from
    // outside this file and is trusted nowhere else. Both account servers offer all
    // three now, so which of the three is the only question left to ask of it.
    switch (chosen) {
        case Miiverse::Protaverse:
        case Miiverse::Juxt:
        case Miiverse::Roseverse:
            set_miiverse(chosen);
            return;
    }
    LOG("ignoring an unrecognized Miiverse: %u", value);
}

void jdmo_changed(ConfigItemMultipleValues *, uint32_t value) {
    if (value > 1 || Config::jdmo_enabled == (value != 0)) return;
    Config::jdmo_enabled = value != 0;
    Config::change_needs_reboot = true;
    log_storage(WUPSStorageAPI_StoreU32(nullptr, KEY_JDMO, value), KEY_JDMO);
}

void account_changed(ConfigItemMultipleValues *, uint32_t value) {
    const auto chosen = static_cast<Account>(value);
    if (chosen == Config::account) {
        return;
    }
    Config::change_needs_reboot = true;
    Config::account = chosen;
    log_storage(WUPSStorageAPI_StoreU32(nullptr, KEY_ACCOUNT, value), KEY_ACCOUNT);

    // The name hooks deliberately do NOT get told here, and that asymmetry with
    // set_miiverse, which tells the applet's snapshots right away, is the point, not an
    // oversight.
    //
    // This callback runs in the process the title is already running in, so telling
    // them now would change what gets refused from the next lookup onwards. The other
    // two things this setting moves can't follow: both are written into the security
    // processor at application start and neither is written back, so they keep their
    // old value until a reboot. Acting on one third of a setting now and two thirds at
    // reboot would leave the console in a state nobody selected.
    //
    // The hooks pick the new value up from Config::Init after the relaunch menu_closed
    // forces, which is the same boot the other two act on it in. Not on the next title:
    // Config::Init runs once per boot, so without that relaunch nothing would bring the
    // value in until somebody restarted the console by hand. Nothing is lost by
    // waiting.

    // The Miiverse stays exactly where it is, whichever way the account server moves.
    // Both are chosen, not derived: every pairing is selectable, and on the two that
    // aren't an account server's own, Juxt on Protarium and Protaverse on Pretendo,
    // token.cpp sends the sign-in request to the Miiverse's own account server. So a
    // switch has nothing to repair, and moving somebody's Miiverse because they touched
    // the account item would override a choice instead of completing one.
    //
    // This has been two other rules. It used to send the Miiverse to whichever one the
    // new account server signs into by itself, which turned Protaverse into Juxt on a
    // switch to Pretendo. Then it moved only Juxt, and only to Protaverse on a switch to
    // Protarium, so a console landed on Protaverse wherever it could. Both moved a
    // selection somebody had made. This one moves nothing.
    //
    // What the account server still decides is the Miiverse a console with nothing
    // stored starts on. That's Config::Init's fallback, by miiverse_for in choice.h.

    // SpotPass does still follow, because this setting moves its options by itself:
    // the account server's own SpotPass is always one of them, and that's a different
    // server than it was a moment ago.
    sync_spotpass();
}

// One list for both account servers. Pretendo had a second one without Protaverse
// until Protaverse's token request could be routed to Protarium's account server. The
// menu still looks a value's position up instead of casting the enum, so nothing
// depends on this order.
constexpr std::array<ConfigItemMultipleValuesPair, 3> MIIVERSE_VALUES = {{
    {static_cast<uint32_t>(Miiverse::Protaverse), "Protaverse"},
    {static_cast<uint32_t>(Miiverse::Juxt), "Juxt"},
    {static_cast<uint32_t>(Miiverse::Roseverse), "Roseverse"},
}};

constexpr std::array<ConfigItemMultipleValuesPair, 2> JDMO_VALUES = {{
    {0, "Default"}, {1, "Melody Online"},
}};

constexpr std::array<ConfigItemMultipleValuesPair, 2> ACCOUNT_VALUES = {{
    {static_cast<uint32_t>(Account::Protarium), "Protarium"},
    {static_cast<uint32_t>(Account::Pretendo), "Pretendo"},
}};

// One pair per account server, because the account server's own SpotPass is always the
// other option and it's never both. Static rather than built when the menu opens: the
// config API is handed the pointer, and an array living on the stack of the function
// that created the item would be long gone by the time anybody scrolled to it.
constexpr std::array<ConfigItemMultipleValuesPair, 2> SPOTPASS_PROTARIUM_VALUES = {{
    {static_cast<uint32_t>(SpotPass::Protarium), "Protarium"},
    {static_cast<uint32_t>(SpotPass::Roseverse), "Roseverse"},
}};

constexpr std::array<ConfigItemMultipleValuesPair, 2> SPOTPASS_PRETENDO_VALUES = {{
    {static_cast<uint32_t>(SpotPass::Pretendo), "Pretendo"},
    {static_cast<uint32_t>(SpotPass::Roseverse), "Roseverse"},
}};

// Position 1 is Roseverse in both, which is what the index arithmetic below
// assumes. Make the compiler hold it.
static_assert(SPOTPASS_PROTARIUM_VALUES[1].value == static_cast<uint32_t>(SpotPass::Roseverse));
static_assert(SPOTPASS_PRETENDO_VALUES[1].value == static_cast<uint32_t>(SpotPass::Roseverse));
static_assert(SPOTPASS_PROTARIUM_VALUES[0].value == static_cast<uint32_t>(SpotPass::Protarium));
static_assert(SPOTPASS_PRETENDO_VALUES[0].value == static_cast<uint32_t>(SpotPass::Pretendo));

// Off by default, and stored, so a console that was left with it on says so rather
// than quietly costing whatever it costs.
void debug_log_changed(ConfigItemBoolean *, bool value) {
    log_set_enabled(value);
    log_storage(WUPSStorageAPI_StoreU32(nullptr, KEY_DEBUG_LOG, value ? 1u : 0u), KEY_DEBUG_LOG);
}

// Turning it on is the action. Turning it off isn't, so it does nothing: there's no
// un-importing, and a stored constant is only ever replaced by a later import that
// succeeded.
void import_requested(ConfigItemBoolean *, bool value) {
    if (value) {
        secret_import();
    }
}

// Turning it on is the action, same shape as the import above, and turning it off
// isn't, so it does nothing. There's no un-resetting a plaza.
void plaza_reset_requested(ConfigItemBoolean *, bool value) {
    if (!value) {
        return;
    }

    // Only when something was actually unregistered. The reset takes effect when the
    // console registers the task again, which is at the next boot, so the relaunch
    // below is part of the action, not a suggestion tacked onto it. Forcing one after a
    // reset that found no network accounts would reboot a console to apply nothing,
    // which is the same fault menu_closed already refuses to commit when no setting
    // changed.
    if (plaza_reset() > 0) {
        Config::change_needs_reboot = true;
    }
}

WUPSConfigAPICallbackStatus add_stub(WUPSConfigCategoryHandle category, const char *text) {
    WUPSConfigItemHandle item;
    WUPSConfigAPIStatus err = WUPSConfigItemStub_Create(text, &item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    err = WUPSConfigAPI_Category_AddItem(category, item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

WUPSConfigAPICallbackStatus menu_opened(WUPSConfigCategoryHandle root) {
    // The log says this too, but the log isn't where anyone looks. This plugin
    // supplements one particular build and does nothing beside any other, and there's
    // more than one way to end up in that situation.
    if (const char *warning = account_warning()) {
        if (add_stub(root, warning) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    // Three settings and one action at the root, with everything diagnostic on a page
    // of its own below them. The third setting is a line of text rather than an item on
    // the two pairings that have only one answer for it. What each choice costs is in
    // the README, which is where a paragraph can be read properly. A menu that scrolls
    // is a menu nobody reads to the end of, and everything that used to be here got
    // read once at most and then scrolled past forever.
    WUPSConfigItemHandle account_item;
    WUPSConfigAPIStatus err = WUPSConfigItemMultipleValues_Create(
        KEY_ACCOUNT, "Network Server", 0,
        Config::account == Account::Pretendo ? 1 : 0,
        const_cast<ConfigItemMultipleValuesPair *>(ACCOUNT_VALUES.data()),
        ACCOUNT_VALUES.size(), &account_changed, &account_item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    err = WUPSConfigAPI_Category_AddItem(root, account_item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    WUPSConfigItemHandle jd_item;
    err = WUPSConfigItemMultipleValues_Create(
        KEY_JDMO, "Just Dance Server", 0, Config::jdmo_enabled ? 1 : 0,
        const_cast<ConfigItemMultipleValuesPair *>(JDMO_VALUES.data()),
        JDMO_VALUES.size(), &jdmo_changed, &jd_item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);
    err = WUPSConfigAPI_Category_AddItem(root, jd_item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    // Second, and independent of the setting above: an account switch leaves this where
    // it is, so what it shows is what was chosen. Both account servers offer all three.
    // Juxt on Protarium and Protaverse on Pretendo route their token request, and
    // Roseverse builds its own, which choice.h sets out by miiverse_routed.
    //
    // Positions, not enum values, looked up instead of cast, so the item shows what's
    // actually selected whatever order the list is in.
    const auto miiverse_position = [](Miiverse wanted) {
        for (size_t i = 0; i < MIIVERSE_VALUES.size(); ++i) {
            if (MIIVERSE_VALUES[i].value == static_cast<uint32_t>(wanted)) {
                return static_cast<int>(i);
            }
        }
        return 0;
    };

    WUPSConfigItemHandle item;
    err = WUPSConfigItemMultipleValues_Create(
        KEY_MIIVERSE, "Miiverse Service", miiverse_position(miiverse_for(Config::account)),
        miiverse_position(Config::miiverse),
        const_cast<ConfigItemMultipleValuesPair *>(MIIVERSE_VALUES.data()),
        MIIVERSE_VALUES.size(), &miiverse_changed, &item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    err = WUPSConfigAPI_Category_AddItem(root, item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    // Third, and a choice only where there is one to make. The two Roseverse pairings
    // can offer both their own network's SpotPass and Rose's, and each position gives
    // the other up: Protarium's SpotPass carries Splatfests, Conquest and 100 Mario,
    // Rose's carries the WaraWara Plaza of the Miiverse actually in use, and one server
    // per console means having either is not having the other. On Protarium, Rose's is
    // offered only with Advanced Overrides on, which choice.h explains.
    //
    // Everywhere else there's exactly one answer, so this is a line of text and not an
    // item. A selector with one position looks like a choice, isn't one, and invites
    // somebody to spend an evening toggling it.
    if (spotpass_offers_roseverse(Config::account, Config::miiverse,
                                  Config::advanced_overrides)) {
        const auto &values = Config::account == Account::Pretendo ? SPOTPASS_PRETENDO_VALUES
                                                                  : SPOTPASS_PROTARIUM_VALUES;
        const int roseverse_index = 1;
        const int own_index = 0;
        const int default_index =
            spotpass_default(Config::account, Config::miiverse) == SpotPass::Roseverse
                ? roseverse_index
                : own_index;
        const int current_index =
            Config::spotpass == SpotPass::Roseverse ? roseverse_index : own_index;

        WUPSConfigItemHandle spotpass_item;
        err = WUPSConfigItemMultipleValues_Create(
            KEY_SPOTPASS, "SpotPass Server", default_index, current_index,
            const_cast<ConfigItemMultipleValuesPair *>(values.data()), values.size(),
            &spotpass_changed, &spotpass_item);
        if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

        err = WUPSConfigAPI_Category_AddItem(root, spotpass_item);
        if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);
    } else {
        // Protarium's SpotPass reads "Protarium with Pretendo Pass-thru" wherever it is the
        // one answer: with Protaverse, with Juxt, and with Roseverse while Advanced
        // Overrides is off, where the line mentions no override. Pretendo's reads just
        // "Pretendo".
        const char *after_name =
            Config::spotpass == SpotPass::Protarium ? " with Pretendo Pass-thru" : "";
        char spotpass_line[128];
        std::snprintf(spotpass_line, sizeof(spotpass_line), "SpotPass Server: %s%s",
                      spotpass_name(Config::spotpass), after_name);
        if (add_stub(root, spotpass_line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    // Reported rather than chosen, so a stub and not a selectable item. Three
    // values and no fourth, which is why the spelling lives in secret.h.
    char status[128];
    std::snprintf(status, sizeof(status), "Roseverse Import Status: %s",
                  import_state_name(secret_state()));
    if (add_stub(root, status) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // The one conditional line left in this menu, and the only prose. Every way an
    // import fails calls for something different to be done about it, the log needs a
    // debugger to read, and "Import Failed" with the reason nowhere would send somebody
    // back to a menu that can't tell them anything. It only shows up when there's a
    // failure to explain.
    if (const char *why = secret_failure()) {
        if (add_stub(root, why) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    WUPSConfigItemHandle import_item;
    err = WUPSConfigItemBoolean_CreateEx(
        "roseverse_import", "Import from Roseverse", false, false, &import_requested,
        "Yes", "No", &import_item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    err = WUPSConfigAPI_Category_AddItem(root, import_item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    // The last item at the root, and deliberately not described as being above Debug.
    // The menu backend draws every sub-category before any item, so the YouTube Patcher
    // and Debug pages sit at the top of this screen whatever order things get added in.
    // This is last among the items, which is as low as anything can go.
    //
    // Only the control lives here now. What it did is on the Debug page with every
    // other readout, which is also where the not-offered case is reported, so nothing
    // here has to explain its own absence.
    if (plaza_available()) {
        WUPSConfigItemHandle plaza_item;
        err = WUPSConfigItemBoolean_CreateEx(
            "plaza_reset", "Reset WaraWara Plaza", false, false, &plaza_reset_requested,
            "Yes", "No", &plaza_item);
        if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

        err = WUPSConfigAPI_Category_AddItem(root, plaza_item);
        if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);
    }

    // The YouTube Patcher's four settings, on a page of their own. Pages draw in the order
    // they're added, so this one sits above Debug. youtube.cpp builds what's on it, through
    // youtube/settings.h, and none of it sets change_needs_reboot: YouTube reads it when it
    // starts, so a change takes effect the next time it does.
    WUPSConfigCategoryHandle youtube;
    err = WUPSConfigAPI_Category_Create({"YouTube Patcher"}, &youtube);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    if (!settings_menu(youtube)) {
        LOG("config error: the YouTube Patcher page could not be built");
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // Adding a category transfers ownership of it, so this handle is spent here.
    err = WUPSConfigAPI_Category_AddCategory(root, youtube);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    // Nothing in this plugin can log from the Miiverse applet, and the system log needs
    // a debugger to read even where logging does work. These counters are the only
    // place a result can be read, so don't delete them without putting something else
    // in their place first.
    //
    // Counts, not yes or no. Zero and two are different faults: zero with Protarium's
    // build present means their string isn't what this plugin looks for, and two means
    // there's a second copy that stopping at the first would have hidden.
    WUPSConfigCategoryHandle activity;
    err = WUPSConfigAPI_Category_Create({"Debug"}, &activity);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    char line[128];

    std::snprintf(line, sizeof(line), "Miiverse in use: %s", miiverse_name(Config::miiverse));
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    std::snprintf(line, sizeof(line), "Inkay build: %s", account_host_name());
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // Beside the line above, not instead of it. That one says whose build is installed
    // and can only be read before this plugin writes; this one says what the module is
    // doing and can be read at any point, but says nothing about whose it is, because
    // every fork exports it under the same name. "Protarium" with "not loaded" beside
    // it is the pair that says their module is gone and only its leftovers were seen.
    std::snprintf(line, sizeof(line), "Inkay module: %s", account_module_state());
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // The trigger that runs between their module's write and the title's own code.
    // Runs and swaps are separate numbers because they're separate faults: zero runs
    // means the loader never called it and the whole approach is wrong on this console,
    // while runs without swaps means it ran and their leftover wasn't there to find.
    // The process id says which of the two the loader called it in.
    std::snprintf(line, sizeof(line), "Pre-main: %u run, %u swapped, pid %d",
                  static_cast<unsigned>(olv_premain_runs()),
                  static_cast<unsigned>(olv_premain_swaps()),
                  static_cast<int>(olv_premain_pid()));
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (olv_applet_visits() == 0) {
        std::snprintf(line, sizeof(line), "Applet: not opened this session");
    } else {
        std::snprintf(line, sizeof(line), "Applet: %u visit(s), %u url, %u allowlist",
                      static_cast<unsigned>(olv_applet_visits()),
                      static_cast<unsigned>(olv_applet_url_swaps()),
                      static_cast<unsigned>(olv_applet_allowlist_swaps()));
    }
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // Beside the applet line, because it happens in the same process and the same
    // visit. Roseverse only, since on anything else all three are zero forever and
    // that's a line of menu saying nothing.
    //
    // Read them in order. No opens means the applet never asked for the file and the
    // hook is watching for the wrong name. Opens without reads means the handle was
    // captured and never matched. Reads without rewrites means the four bytes got split
    // across two chunks, which olv.cpp explains and this doesn't handle.
    if (Config::miiverse == Miiverse::Roseverse) {
        std::snprintf(line, sizeof(line), "Suffix list: %u open, %u read, %u rewritten",
                      static_cast<unsigned>(olv_tld_opens()),
                      static_cast<unsigned>(olv_tld_reads()),
                      static_cast<unsigned>(olv_tld_rewrites()));
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    // Two numbers because there are two triggers now and which one did the work is the
    // whole question. The foreground one measured too late in the Wii U Menu, so a swap
    // credited to the token request is the fix working.
    std::snprintf(line, sizeof(line), "Menu/games: %u fg, %u tok, %s",
                  static_cast<unsigned>(olv_foreground_url_swaps()),
                  static_cast<unsigned>(olv_token_trigger_swaps()),
                  olv_sweep_bounded() ? "bounded" : "full sweep");
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    std::snprintf(line, sizeof(line), "Late nn_olv loads: %u seen, %u swapped",
                  static_cast<unsigned>(olv_late_loads_seen()),
                  static_cast<unsigned>(olv_late_load_swaps()));
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    std::snprintf(line, sizeof(line), "Account server: %s", account_status());
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // Both tasksheet slots, reported separately. They move independently, and one of
    // them refusing is a different fault from both refusing, which a single line would
    // hide. A line reading "tail" followed by four bytes is the one assumption in
    // spotpass.cpp that nothing off-console could settle, answering itself: those bytes
    // are what's actually in the slot.
    //
    // Read the four addresses now, so everything below reports the console and not the
    // setting. Those aren't the same statement and the difference is the point: a
    // setting changed since the last boot hasn't moved anything yet, and only the read
    // says so.
    spotpass_observe();

    // Which of Protarium's modules the long slot shows to the read just above. "older"
    // has the tasksheets on api.protarium.lol and the policy pair on Pretendo, "newer"
    // has all four on Protarium's own hosts.
    std::snprintf(line, sizeof(line), "SpotPass layout: %s", spotpass_layout_name());
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    std::snprintf(line, sizeof(line), "SpotPass %s, now %s", spotpass_long_status(),
                  spotpass_long_seen());
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    std::snprintf(line, sizeof(line), "SpotPass %s, now %s", spotpass_short_status(),
                  spotpass_short_seen());
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // The policy pair, shown only where it can move. Everywhere else these two would
    // read "left alone" on every boot forever, which is two lines of menu saying nothing.
    // Roseverse moves them on either of Protarium's modules and Pretendo only on the
    // newer one, so Pretendo shows them once that module is proved. They have never been
    // read on a console, so this is where they're worth the space.
    if (spotpass_moves_policy(Config::spotpass)) {
        std::snprintf(line, sizeof(line), "SpotPass %s, now %s", spotpass_policy_status(),
                      spotpass_policy_seen());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        std::snprintf(line, sizeof(line), "SpotPass %s, now %s", spotpass_policy_host_status(),
                      spotpass_policy_host_seen());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    // This used to say the token was somebody else's to mint. It isn't any more, so
    // these report it the way everything else here is reported: as counts, plus one
    // line naming the last thing that stopped a token being built.
    //
    // Seen, answered and passed are three numbers instead of one because all three
    // interesting failures look identical in a single count. Zero seen means the hook
    // never ran. Seen but never answered means it ran and declined, and the status line
    // says why. Answered with a sign-in that still failed means the token was built and
    // Roseverse refused it, which is the one result that says the remaining problem
    // isn't on this console.
    if (Config::miiverse == Miiverse::Roseverse) {
        std::snprintf(line, sizeof(line), "Token: %u seen, %u answered, %u passed on",
                      static_cast<unsigned>(token_requests_seen()),
                      static_cast<unsigned>(token_requests_answered()),
                      static_cast<unsigned>(token_requests_passed()));
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        std::snprintf(line, sizeof(line), "Token state: %s", token_status());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        // Which process asked. Beside the counts, not instead of them: the counts say
        // whether anything asked at all, and this says where, which is the difference
        // between watching the wrong process and not being installed anywhere.
        std::snprintf(line, sizeof(line), "Token seen in: %s", token_where());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        // What the applet install did. The applet fetches its own token, in a process
        // the static replacement never reached, so this is installed by address from
        // the applet's own file open. "installed in pid 9" here with the applet named
        // in the line above is the fix working; anything else is where it stopped.
        std::snprintf(line, sizeof(line), "Applet token hook: %s", token_applet_status());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        // The same thing for the in-game overlay, process 7, which was measured
        // untouched before this existed. "installed in pid 7" is the working reading.
        // "nn_act not loaded here" that never changes means the overlay's file opens
        // all arrive before nn_act does, and the trigger is in the wrong place, since
        // this retries on every one of them until it takes. Read it beside the line
        // above: `overlay` there is what says the overlay actually asked for a token.
        std::snprintf(line, sizeof(line), "Overlay token hook: %s", token_overlay_status());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        // How many times the console loaded an account, each of which rebuilds the
        // token. Zero here is the reading that says the hook isn't installed, which
        // matters because the rebuild is what stops a token built too early from
        // standing.
        std::snprintf(line, sizeof(line), "Account loads: %u",
                      static_cast<unsigned>(token_account_loads()));
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        // Which moment built the token that's cached now, and where. A token built
        // before the account finished loading is well formed and wrong, and on hardware
        // that made Roseverse behave as though no account were set up while every other
        // line here still read as success. "account load" is what should be standing by
        // the time the applet asks. "app start" beside a launch that failed that way is
        // the thing to report.
        std::snprintf(line, sizeof(line), "Token built: %s", token_build_origin());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        // Where nn_act's token function sits in each process that looked, and what
        // was sitting at that address. Process 2 is the Wii U Menu, 15 a game, 9 the
        // Miiverse applet, 7 the in-game overlay.
        //
        // A row each, not one line for all of them. Four on one line ran off the edge
        // of the menu and cost a console run to find out, and the lookup list further
        // down has always been a row each for the same reason.
        //
        // Read the last field first, and read it in the past tense. It's captured once,
        // when the process is first seen, and in the applet and the overlay that
        // happens before this plugin installs its own patch there. So `found patched`
        // means something had already replaced that function before this looked, and
        // `found unpatched` means nothing had. A process showing `found unpatched`
        // beside a hook line saying installed is the two of them agreeing. The raw
        // instruction is printed beside it so the verdict can be checked instead of
        // believed. Nothing is patched to read any of this.
        for (size_t i = 0; i < token_act_sighting_count(); ++i) {
            std::snprintf(line, sizeof(line), "nn_act %s", token_act_sighting(i));
            if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
                return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
            }
        }
    }

    // The routed pairings, Juxt on Protarium and Protaverse on Pretendo, where
    // Miiverse's request gets routed instead of answered or passed on. Shown only
    // there, for the reason the Roseverse lines above are shown only on Roseverse.
    //
    // Read the route line by its time first. A call that took a few milliseconds never
    // left the console, so act handed back a token it already held. One that took
    // hundreds went to a server, and the code beside it says whether the server said
    // yes. The lend line says whether the account server moved at all, and whether it
    // got put back.
    if (miiverse_routed(Config::account, Config::miiverse)) {
        std::snprintf(line, sizeof(line), "Token: %u seen, %u routed, %u declined",
                      static_cast<unsigned>(token_requests_seen()),
                      static_cast<unsigned>(token_requests_routed()),
                      static_cast<unsigned>(token_routes_declined()));
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        std::snprintf(line, sizeof(line), "Token route: %s", token_route_status());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        std::snprintf(line, sizeof(line), "Account lend: %s", account_lend_status());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        std::snprintf(line, sizeof(line), "Token seen in: %s", token_where());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        // The applet's own request, which is lent like the others. "installed in pid 9" is
        // the patch in place. The request line says whether the lend happened and what it
        // cost. A few milliseconds is act handing over a token a routed fetch already got.
        // Seconds with "sent 0" and "lent" is a fresh fetch from the Miiverse's own account
        // server, which is the working reading when the applet asks first on a cold boot.
        // "not lent" is the one to report, beside the lend line above saying why.
        std::snprintf(line, sizeof(line), "Applet token hook: %s", token_applet_status());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        std::snprintf(line, sizeof(line), "Applet request: %s", token_applet_request_status());
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    std::snprintf(line, sizeof(line), "Lookups: %u refused of %u",
                  static_cast<unsigned>(dns_lookups_refused()),
                  static_cast<unsigned>(dns_lookups_seen()));
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // Which Miiverse discovery host the console actually resolved, and where. This is
    // the one line that says whether anything ever tried to reach the selected Miiverse
    // at all, as opposed to being handed a token for it. It survives the trip back from
    // the applet, which the lookup list below doesn't, because the applet is a process
    // the config menu can't be opened from.
    std::snprintf(line, sizeof(line), "Discovery host: %s", dns_discovery_seen());
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    std::snprintf(line, sizeof(line), "Name hooks: %s", dns_nesting());
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // Every name the running title asked for, oldest first, a cross marking one that
    // was refused. A name here that isn't marked and should have been is the only way
    // to spot a redirect this plugin doesn't cover.
    for (size_t i = 0; i < dns_recent_count(); ++i) {
        std::snprintf(line, sizeof(line), "%s %s", dns_recent_refused(i) ? "x" : " ",
                      dns_recent(i));
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    // The plaza reset's own readout, on the page with every other readout instead of
    // beside the control at the root. The control is a one-press action whose result
    // nobody watches happen, so what it did belongs where results get read.
    //
    // Both cases are reported here, including the one where the action isn't on offer.
    // At the root that case had to be either a line contradicting an absent item or
    // nothing at all; here it's just another status.
    if (plaza_available()) {
        std::snprintf(line, sizeof(line), "WaraWara Plaza: %s", plaza_status());
    } else {
        std::snprintf(line, sizeof(line), "WaraWara Plaza: open from the Wii U Menu to reset");
    }
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // What the toast at boot did. A toast that never appeared looks the same whether the
    // module is missing, refused the defaults, refused the text, or took it and the
    // overlay never showed it, and only the last of those reads `queued`.
    std::snprintf(line, sizeof(line), "Boot toast: %s", notify_status());
    if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    // What the YouTube Patcher did the last time YouTube started this boot, up to three lines,
    // and none with the patcher off and nothing started. youtube_report_line has how they read.
    for (size_t i = 0; youtube_report_line(i, line, sizeof(line)); ++i) {
        if (add_stub(activity, line) != WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    // Advanced Overrides, just above the logging switch. A setting, not a readout, and
    // down here instead of beside the three at the root because what it unlocks isn't
    // meant to be found by accident: Rose's SpotPass with Protarium as the account
    // server, which choice.h holds back. Off by default, and stored. What it changes
    // shows up the next time the menu is opened, because the root items were built
    // alongside this one.
    WUPSConfigItemHandle overrides_item;
    err = WUPSConfigItemBoolean_CreateEx(KEY_ADVANCED_OVERRIDES, "Advanced Overrides", false,
                                         Config::advanced_overrides, &advanced_overrides_changed,
                                         "Yes", "No", &overrides_item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    err = WUPSConfigAPI_Category_AddItem(activity, overrides_item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    // Last on this page, under everything it might explain.
    //
    // What it switches on is the Cafe OS system log, which needs a debugger or an
    // emulator to read, so on an ordinary console turning it on writes lines nobody
    // will ever see. The name says that outright, because what people want from a
    // switch called Debug is the readout above it, and that isn't behind this or behind
    // anything now.
    //
    // Off by default, and stored. It stopped being obviously free when a console
    // started taking twenty-five seconds to change title: an unconditional syscall on a
    // path the system is waiting on is cheap right up until it isn't.
    WUPSConfigItemHandle log_item;
    err = WUPSConfigItemBoolean_CreateEx("debug_log", "Enable Debugger Logging", false,
                                         log_enabled(), &debug_log_changed, "Yes", "No",
                                         &log_item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    err = WUPSConfigAPI_Category_AddItem(activity, log_item);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    // Added last, and it makes no difference where this page appears: the menu backend
    // renders every sub-category before any item, from two separate lists, so a page
    // can't be placed below a setting no matter when it gets added. An earlier version
    // of this comment claimed the opposite and was wrong. See CategoryRenderer's
    // constructor, which walks getCategories() to exhaustion and only then starts on
    // getItems().
    //
    // Adding a category transfers ownership of it, so this handle is spent here and
    // nothing may touch it afterwards.
    err = WUPSConfigAPI_Category_AddCategory(root, activity);
    if (err != WUPSCONFIG_API_RESULT_SUCCESS) return log_config(err, __LINE__);

    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void menu_closed() {
    log_storage(WUPSStorageAPI_SaveStorage(false), "save");

    // Nothing changed, so nothing happens. This is the whole reason the relaunch keys
    // on the flag and not on the menu having been open: reading the Debug page is the
    // most common thing anybody opens this menu for, and a console that reboots every
    // time somebody looks at it is worse than one that needs telling.
    if (!Config::change_needs_reboot) {
        return;
    }

    LOG("selection changed, relaunching to apply it");

    // Cleared before the calls, not after, because there is no after: normally neither
    // of them returns here. Clearing it below would be dead code that reads as though
    // the flag were being managed.
    Config::change_needs_reboot = false;

    // Both, in this order.
    //
    // Why relaunch at all, instead of the line this used to print: every setting here
    // is written into the security processor at application start and none is written
    // back, so a change made mid-session applies at the next boot and not before.
    // spotpass.cpp goes further and refuses to move anything once the destination has
    // changed under it, printing "reboot to move" on all four slots. Telling somebody
    // that and leaving them to do it by hand is asking them to finish the job the menu
    // started.
    //
    // The first call is what makes the second a reboot instead of a return to the menu:
    // without it the system resumes what it has cached, plugin state included, and the
    // setting that was just stored isn't the one running.
    OSForceFullRelaunch();
    SYSLaunchMenu();
}

// An unrecognized stored value means storage was written by a different version.
// Both settings fall back to doing nothing, which is the same answer an absent
// file gets.
uint32_t load(const char *key, uint32_t fallback) {
    uint32_t stored = fallback;
    const WUPSStorageError err = WUPSStorageAPI_GetU32(nullptr, key, &stored);
    if (err == WUPS_STORAGE_ERROR_NOT_FOUND) {
        log_storage(WUPSStorageAPI_StoreU32(nullptr, key, stored), key);
        return fallback;
    }
    if (err != WUPS_STORAGE_ERROR_SUCCESS) {
        log_storage(err, key);
        return fallback;
    }
    return stored;
}

} // namespace

void Config::Init() {
    WUPSConfigAPIStatus cres =
        WUPSConfigAPI_Init({.name = "JustGetMiiOnline"}, menu_opened, menu_closed);
    if (cres != WUPSCONFIG_API_RESULT_SUCCESS) {
        log_config(cres, __LINE__);
        return;
    }

    // The account server is read first because the Miiverse default is derived
    // from it. A console that has never been told otherwise gets Protarium and
    // Protaverse, which together mean this plugin writes nothing anywhere.
    const uint32_t stored_account = load(KEY_ACCOUNT, static_cast<uint32_t>(Account::Protarium));
    if (stored_account == static_cast<uint32_t>(Account::Pretendo)) {
        account = Account::Pretendo;
    } else {
        if (stored_account != static_cast<uint32_t>(Account::Protarium)) {
            LOG("unrecognized stored account server %u, using Protarium", stored_account);
        }
        account = Account::Protarium;
    }

    jdmo_enabled = load(KEY_JDMO, 0) == 1;

    // No stored Miiverse means follow the account server instead of assuming
    // Protaverse, so an account setting carried over from an older build arrives with
    // the Miiverse that server signs into by itself and not a routed one.
    const uint32_t matching = static_cast<uint32_t>(miiverse_for(account));
    const uint32_t stored_miiverse = load(KEY_MIIVERSE, matching);
    switch (stored_miiverse) {
        case static_cast<uint32_t>(Miiverse::Protaverse):
            miiverse = Miiverse::Protaverse;
            break;
        case static_cast<uint32_t>(Miiverse::Juxt):
            miiverse = Miiverse::Juxt;
            break;
        case static_cast<uint32_t>(Miiverse::Roseverse):
            miiverse = Miiverse::Roseverse;
            break;
        default:
            LOG("unrecognized stored Miiverse %u, following the account server", stored_miiverse);
            miiverse = miiverse_for(account);
            break;
    }

    // Before SpotPass, because it decides whether Rose's SpotPass is allowed with
    // Protarium as the account server.
    advanced_overrides = load(KEY_ADVANCED_OVERRIDES, 0) != 0;

    // Third, and validated against the pair above instead of trusted. A stored value
    // this pairing doesn't offer is one left behind by a pairing that moved after it,
    // or Rose's on Protarium with Advanced Overrides off, and the honest answer is this
    // pairing's default and not a server nobody can see selected.
    //
    // The default is stored as well as used. Advanced Overrides can make a stored value
    // legal again with no pairing change to re-derive it, so leaving Rose's in storage
    // under a Protarium default would bring it back at a later boot while the menu had
    // shown Protarium.
    const uint32_t stored_spotpass =
        load(KEY_SPOTPASS, static_cast<uint32_t>(spotpass_default(account, miiverse)));
    const auto wanted_spotpass = static_cast<SpotPass>(stored_spotpass);
    if (spotpass_valid(account, miiverse, wanted_spotpass, advanced_overrides)) {
        spotpass = wanted_spotpass;
    } else {
        spotpass = spotpass_default(account, miiverse);
        LOG("stored SpotPass server %u is not offered with this pairing, using %s",
            stored_spotpass, spotpass_name(spotpass));
        log_storage(
            WUPSStorageAPI_StoreU32(nullptr, KEY_SPOTPASS, static_cast<uint32_t>(spotpass)),
            KEY_SPOTPASS);
    }

    // Before anything that might log, so the setting governs this boot instead of
    // kicking in only after the first thing worth logging has already happened.
    log_set_enabled(load(KEY_DEBUG_LOG, 0) != 0);

    olv_set_target(miiverse);
    token_set_target(miiverse);
    token_set_account(account);
    dns_set_account(account);

    // Read here and nowhere else, for the same reason olv_set_target is called
    // here: this runs where storage is readable and the applet process is not.
    // Whatever this finds is what the token hook will have to work from.
    secret_init();

    // The YouTube Patcher's four, stored beside these under youtube_ keys.
    settings_load();

    log_storage(WUPSStorageAPI_SaveStorage(false), "save");

    LOG("account: %s, miiverse: %s", account == Account::Pretendo ? "pretendo" : "protarium",
        miiverse_name(miiverse));
}
