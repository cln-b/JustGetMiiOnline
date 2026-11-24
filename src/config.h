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

#pragma once

#include "choice.h"

class Config {
public:
    static void Init();

    static Miiverse miiverse;
    static Account account;
    static bool jdmo_enabled;

    // Which network serves SpotPass. Constrained by the two above, not free: choice.h
    // holds which values are legal for a given pairing and which way the two-answer
    // combinations lean, and Init() falls back to that default whenever the stored
    // value isn't one this pairing allows.
    static SpotPass spotpass;

    // Set when a selection changes, or when the WaraWara Plaza reset actually
    // unregisters something. All of these get applied once per boot and the console
    // keeps its own state afterwards, so a mid-session change can't be honored in
    // place.
    //
    // The menu used to just say so. Now it acts on it: menu_closed relaunches the
    // console when this is set, and does nothing at all when it isn't, so opening the
    // menu to read the Debug page never costs a reboot.
    static bool change_needs_reboot;

    // Advanced Overrides, near the foot of the Debug page. Off unless you turn it on.
    // It does exactly one thing: puts Rose's SpotPass on offer with Protarium as the
    // account server, which choice.h otherwise keeps off and says why.
    static bool advanced_overrides;
};
