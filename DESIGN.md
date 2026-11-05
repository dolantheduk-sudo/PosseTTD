# PosseTTD design document

Base: OpenTTD **15.3** (savegame version SLV 362 = `SLV_ENGINE_MULTI_RAILTYPE`).
Written before any feature code. Everything here was checked against the 15.3 source; places marked **(verify)** are things I could not fully confirm by reading and will confirm when that phase starts.

This is written for a non-C++ reader, so each section starts with the plain-language idea, then lists the files.

---

## 0. Ground rules recap (and how this design meets them)

| Rule | How we meet it |
|---|---|
| Synced state only via commands | Every change = a new `CMD_*` (see §1). GUI code only *posts* commands. |
| Synced RNG | Use `Random()` / `RandomRange()` / `Chance16()` from `core/random_func.hpp`. Never `InteractiveRandom()`, never `<random>`. |
| Savegames | PosseTTD data lives in its **own savegame chunk** with its own version number (§2). |
| Settings | New `.ini` entries in `table/settings/` (§3). |
| Upstream-friendly | New code in `src/possettd/` (new folder, new files). Existing files get 1-5 line hooks marked `// PosseTTD:`. |
| No desyncs | Per-phase two-client test + `-d desync=1` logging (§8). |

New code folder: **`src/possettd/`** with its own `CMakeLists.txt` added by one line to `src/CMakeLists.txt`.

---

## 1. How the multiplayer command system works (the pattern we copy)

**Idea.** A player clicks something. The client does not change the game itself. It sends a "command" to the server; the server stamps it with a tick and sends it to everyone; every machine runs the same function at the same tick. Because all machines start from the same state and run the same commands and same RNG, they stay identical.

**Real example in the code:** buying exclusive rights.
- `town_gui.cpp:312` : button click calls `Command<CMD_DO_TOWN_ACTION>::Post(...)`.
- `command_type.h` : `CMD_DO_TOWN_ACTION` is one entry in the `Commands` enum.
- `town_cmd.h:34` : `DEF_CMD_TRAIT(CMD_DO_TOWN_ACTION, CmdDoTownAction, flags, CommandType::...)` registers it. `command.cpp:97` builds the dispatch table automatically from these traits.
- `town_cmd.cpp:3728` : `CmdDoTownAction(DoCommandFlags flags, ...)` is the function every machine runs. It is called twice: once with no `Execute` flag (just "would this work, what does it cost?") and once with `Execute` (actually do it). **All state changes must be inside `if (flags.Test(DoCommandFlag::Execute))`.**

**What a new feature needs, always:**
1. Add `CMD_xxx` to the enum in `command_type.h` (**at the end, before `CMD_END`** to keep merges tidy).
2. Add `DEF_CMD_TRAIT` + function declaration in a new `src/possettd/xxx_cmd.h`, and `#include` it from `command.cpp` (one line).
3. Write `CmdXxx()` in `src/possettd/xxx_cmd.cpp`.
4. GUI calls `Command<CMD_XXX>::Post(...)`.

Command arguments can be ints, enums, bools and `std::string` (company rename does this), so profile text edits are fine. Command *order* in the enum is part of the network protocol, which is another reason all players must run the same build.

**Who is "the actor" in a command?** `_current_company`. Covert ops and contracts use this, so a player can never act as another company.

**Random in commands:** `Chance16(1, 14)` in `TownActionBribe` (`town_cmd.cpp:3631`) is the exact pattern: roll the synced RNG *inside* `Execute`. Success/caught/crash rolls in Phase 4 will do the same.

---

## 2. Savegames

**Idea.** A savegame is a set of named chunks. Joining a multiplayer game downloads a savegame, so anything not saved = lost on join.

**Problem with a normal "version bump".** The version list (`SLV_*` in `saveload/saveload.h`) is a single global counter. If we add `SLV_POSSE_1` after 362 and upstream later adds 363 for something else, every future merge conflicts *and* the numbers mean different things.

**Proposal (needs your OK):** PosseTTD gets **one custom chunk** (4-letter ID `PSSE`, written by `src/possettd/possettd_sl.cpp`, registered by a 1-line addition to the chunk list in `saveload/saveload.cpp`). Inside it we keep our **own version number** (`POSSE_SAVE_VERSION`) that we bump every time we add or change saved fields; the loader handles older versions field by field, exactly how upstream does. Consequences:
- Our data is independent of upstream's counter, so merging future OpenTTD releases stays easy.
- A normal OpenTTD can't load a PosseTTD save that has the chunk (unknown chunk), and PosseTTD loads plain OpenTTD saves (chunk absent -> defaults). Good: no accidental cross-play.
- This still satisfies the intent of your rule 3 (versioned, backwards-compatible, transferred on join).

If you'd rather use real `SLV_` bumps, say so; it works but costs merge pain.

Inside the chunk we use the same table-style `SaveLoad` descriptions upstream uses (e.g. `saveload/company_sl.cpp`), one sub-table per kind of data: company profile, executives, contracts, covert-ops state, cooldowns.

Related: some data is naturally per-company or per-town. Rather than editing the `Company`/`Town` structs (which would conflict upstream), we store **side tables** in our own file, keyed by `CompanyID`/`TownID`: `std::array<PosseCompanyData, MAX_COMPANIES>` and a `std::map<TownID, ...>`. Company/town deletion events need a 1-line hook each to clean the side tables (**verify** hook points: `company_cmd.cpp` `CompanyCtrl` delete path, `town_cmd.cpp` `Town::~Town`).

---

## 3. Settings

Each setting is a block in a `.ini` under `src/table/settings/` (e.g. `economy.exclusive_rights` in `economy_settings.ini:154`): variable, default, string ID, help text, optional callback. They become game settings that the server controls and that are saved in the savegame (so they sync to joiners and can be changed mid-game from the settings window).

Plan: a new `possettd_settings.ini` (added to `table/settings/CMakeLists.txt` and the settings struct in `settings_type.h` as one `PosseSettings posse;` member; **verify** the exact struct-add pattern when starting Phase 1). Per phase: master toggle + key numbers (listed per phase below).
Settings missing from an older save take their `def` value. **(verify** in Phase 1 by loading a vanilla 15.3 save.)

Strings go in `src/lang/english.txt`. Rule: new strings are appended in a clearly marked `# PosseTTD` section at the end of the file to keep merges clean. Other languages fall back to English.

---

## 4. Phase 1: Territory overlay (client-only)

**Idea.** Pure drawing. Never touches game state, so cannot desync.

**Data source (all existing, all available on every client):**
- `Station::catchment_tiles` (`station_base.h:540`): the set of tiles each station covers; rebuilt on load, so it's valid for joiners.
- `Station::goods[cargo].rating` (`station_base.h:235`): service quality 0-255.
- Station owner, facility type (`st->facilities`: rail/road/air/ship), and "how much used": per-cargo statistics on `GoodsEntry` (**verify** which field gives recent throughput; fallback: `Station::goods[].link_graph`/supply, or `time_since_pickup`).

**Calculation** (new `src/possettd/territory.cpp`): for each tile, `influence[company] = sum over stations covering the tile of (rating * usage-weight)`. Winner = highest. If second place is within X% of the winner (tunable constant), the tile is "contested". Result: a per-tile owner byte (+ contested flag) in a plain array, one per filter mode. Filters (All/Rail/Road/Air/Water) just restrict which stations count; Contracts is a stub until Phase 3.

**Caching.** Rebuilt at most every N game ticks (e.g. ~2 in-game days) or when a station is built/removed/changes, never per frame. Computed per client from local data, so even if clients disagree on timing it can't matter, because nothing reads it back into the game.

**Rendering:**
- *Viewport*: precedent is `LinkGraphOverlay` (`viewport.cpp:1857` calls `vp.overlay->Draw(&dp)` after the world is drawn). We add a sibling hook next to it that draws, for visible tiles, a translucent company-colour tint and a border line where owner changes between neighbouring tiles. Contested tiles: draw alternating diagonal stripes in two company colours. Uses plain `GfxFillRect`/`GfxDrawLine`, so **no new sprites needed**.
- *Smallmap*: new map type `SMT_TERRITORY` in `smallmap_gui.cpp` (existing types at `:615`, modelled on `SMT_OWNER`) that colours each pixel by the cached owner. Needs one new legend/button entry.
- *Toolbar button*: the toolbar's buttons are sprites and OpenGFX has no spare one. **Simpler proposal:** a hotkey + an entry in the existing "Map" toolbar dropdown + the filter dropdown shown as a small window. If you really want a dedicated button, I'd reuse an existing sprite. Tell me if that matters.

**Settings:** `posse.territory_enabled` (client-visible toggle; actually mostly a UI preference, saved in `openttd.cfg`, not the savegame), `territory_recalc_interval`, `territory_contest_threshold`.

**Risks:** performance on huge maps (mitigate: only compute tiles in station bounding rects; draw only visible tiles); catchment tiles with `modified_catchment` off use a different radius (use the existing `TileIsInCatchment`, don't reimplement).

---

## 5. Phase 2: Company customization

**Idea.** A set of profile fields per company, edited via commands, shown in a new window.

**What exists:** `CompanyProperties` (`company_base.h`) has `name`, `president_name`, `face` (CEO), `colour`, `livery[]`. Commands: `CMD_RENAME_COMPANY`, `CMD_RENAME_PRESIDENT`, `CMD_SET_COMPANY_MANAGER_FACE`, `CMD_SET_COMPANY_COLOUR` (`company_cmd.h`). Face editor window = `SelectCompanyManagerFaceWindow` (`company_gui.cpp:1208`).

**New data** (side table, `PosseCompanyData`): ticker (<=5 chars), slogan, founding story, HQ town id, industry-focus enum, 3 executives `{name, CompanyManagerFace}` for CFO/COO/Security, emblem `{shape, colour1, colour2}`, fleet-name prefix.

**New commands:** `CMD_POSSE_SET_PROFILE_TEXT` (field id + string), `CMD_POSSE_SET_EXEC` (role + name + face style/bits), `CMD_POSSE_SET_EMBLEM`. Validation: length caps (reuse the `MAX_LENGTH_*` pattern), HQ town must exist, only for `_current_company`.

**UI:** new `CompanyProfileWindow` (`src/possettd/profile_gui.cpp`), openable for any company ID from the company window and from a rival's company list. Face-editor for executives: reuse `SelectCompanyManagerFaceWindow` by generalising which face it edits. This is an edit to upstream's window (moderate size, the riskiest edit in this phase); alternative is a copy of the class in our folder (more code, zero merge risk). **I recommend the copy.**

**Emblem:** built-in shapes drawn with fill-rect/line primitives (circle, shield, star, chevron, ...) in two colours. No sprites, no uploads, tiny to sync (3 numbers).

**Fleet prefix:** applied at *display time* where vehicle names are formatted, not stored in each vehicle's name. So no per-vehicle saved state, existing names untouched. **(verify** the formatting function, `vehicle_gui.cpp` / `STR_VEHICLE_NAME`).

**Settings:** `posse.profile_enabled`, max slogan/story lengths.
**Risks:** news with emblems needs custom drawing in `NewsStyle::Company` (`CompanyNewsInformation`, `news_type.h:162`), so emblem-in-news may be a small hook in `news_gui.cpp`; I'd defer emblem-in-news if it's awkward, and say so.

---

## 6. Phase 3: City contracts

**Idea.** A contract = (town, type, holder company, expiry, yearly fee, targets). It layers on top of the existing "exclusive transport rights".

**What exists to hook:**
- Exclusivity today: `Town::exclusivity` + `exclusive_counter` (`town.h:85`), set in `TownActionBuyRights` (`town_cmd.cpp:3585`), ticks down monthly (`town_cmd.cpp:4123`). Effect: rival stations get no cargo from the town: `station_cmd.cpp:4581` (station is ignored for the town's houses) and `MoveGoodsToStation(... exclusivity)` (`:4599`).
- Town rating: `t->ratings[company]` (`town.h:86`), `ChangeTownRating()` (`town_cmd.cpp:4040`), `CheckforTownRating()` (`:4077`).
- Station build gate: `CheckIfAuthorityAllowsNewStation()` (`town_cmd.cpp:3920`, called from `station_cmd.cpp:1441, 2108, 2637`). **This is where "new rival stations are blocked" goes** (plus depots; **verify** the depot build path).
- Yearly/monthly timers: `IntervalTimer<TimerGameEconomy>` like `_economy_towns_monthly` (`town_cmd.cpp:4116`).

**Data** (own chunk): `Contract {id, town, type(6 kinds), holder, start, end, yearly_fee, targets{min_transported_pct, min_rating}, strikes}`; `Bid {contract/town, type, company, amount}` list + auction window end date.

**Commands:** `CMD_POSSE_SIGN_CONTRACT`, `CMD_POSSE_BID`, `CMD_POSSE_CANCEL_CONTRACT`, `CMD_POSSE_OPEN_FOR_BIDS` (maybe server/town-triggered via the monthly timer instead). Fees via the existing `CommandCost` + expense type (**verify** whether a new `ExpensesType` is needed; adding one alters the finance window and savegame, so I'd reuse "Other"/"Property maintenance" to avoid it).

**Enforcement without destruction:** for restricted types, the monthly timer marks rival stations of that type as "penalised"; the penalty is applied where cargo is generated/rated (`UpdateStationRating`, `station_cmd.cpp:3973`, and the `MoveGoodsToStation` ratio). This reuses the existing exclusivity mechanism rather than inventing a second one.

**Obligations:** monthly check of `Town::supplied` history (`transported/production`, see `town_cmd.cpp` `SumHistory`) vs targets; miss -> `ChangeTownRating` down, strike; 3 strikes cancel + news.

**Bidding:** all deterministic (highest amount, ties broken by earlier bid then lower CompanyID; **no RNG needed**). Resolved by the monthly timer on every machine identically. News via `AddNewsItem` like `TownActionBuyRights`.

**Windows:** town view + company view (new files).
**Settings:** master toggle, max term, fee multiplier, min rating, bid window length, penalty strength.
**Risks (largest of phases 1-3):** (a) interaction with the vanilla exclusive-rights action; plan: contracts *replace* it when contracts are on and it stays when off. (b) AIs/GameScripts: leave them unaware. (c) fee deduction for a company that can't afford it: needs bankruptcy-safe handling.

---

## 7. Phase 4: Sabotage

**Idea.** Each operation is one command that rolls the synced RNG inside `Execute` and applies an effect through *existing* code paths.

**Hooks found:**
- Road vehicle crash: `RoadVehCrash()` (`roadveh_cmd.cpp:555`) is `static`. We add a tiny public wrapper or call it via a function in our file that's declared `friend`; **I'd add a 3-line exported function in `roadveh_cmd.cpp`**.
- Breakdown: `Vehicle::HandleBreakdown` / `CheckVehicleBreakdown` (`vehicle.cpp:1287,1332`): set `breakdown_ctr` the way the game does.
- Bad press: `ChangeTownRating` (`town_cmd.cpp:4040`) on the victim.
- Strike: needs a new per-station "loading blocked until date" flag in our side table, checked in the loading code (`economy.cpp` `LoadUnloadVehicle`): the one place needing a hot-path edit. **(verify** cost/feasibility; fallback is a rating penalty instead of full stop.)
- Executive scandal: temporary modifier on company value / loan interest, stored in our side table; applied where interest is charged (`economy.cpp` `CompaniesGenStatistics`/`CompaniesMonthlyLoop`, **verify**) and where company value is computed.
- Spy: purely client-side *view* of rival finances. But the **permission** to view must be synced state (so the spy window knows it's allowed), while finance data is already readable by all clients. This one is trivial technically.
- News: `AddNewsItem` with `CompanyNewsInformation`. **Visibility rule** (attacker sees result; victim sees effect; attacker named only if caught) means news must be addressed to specific companies: use `NewsFlag`/company filter, **verify** per-company news support. Fallback: all clients receive the item but each client hides items not addressed to its `_local_company` when *displaying* (safe, display-only), with a sync-neutral approach to the news history list.

**State:** per-company cooldowns, op counts per year, security budget (%), insurance on/off, "caught" records (for lawsuits). All in the `PSSE` chunk.
**Commands:** `CMD_POSSE_COVERT_OP {op, target company, target object}`, `CMD_POSSE_SET_SECURITY`, `CMD_POSSE_SET_INSURANCE`, `CMD_POSSE_SUE`.
**Safety:** target validation rejects own company, under-age companies, anything beyond the per-year cap, severity cap on money damage. All in the command's test pass (before `Execute`).
**Settings:** master toggle, ops/year, protection years, severity cap, base success/catch chances, fines, cooldown.
**Risk:** the roll order. Every roll must happen in a fixed order inside `Execute`, with no conditional extra `Random()` call that depends on client-local data (e.g. "is the local company the victim"). I'll write the ops so that RNG calls depend only on synced state.

---

## 8. Testing & desync strategy (every phase)

1. **Two-client local test** (exact steps in each phase checklist): instance A: Start server from the menu (no `-D`, that flag means headless dedicated server). Instance B: `openttd.exe -n 127.0.0.1` (or Multiplayer menu -> LAN/Internet -> join).
2. Run both with `-d desync=2` (cache debugging: after every tick, cached values are compared to freshly recomputed ones; slow but catches missed updates) and, when chasing a real desync, `-d desync=3` on the server (records `commands.log` and periodic savegames for replay, see `docs/desync.md` §2.1-2.2 and `network.cpp:279,1107`). A desync shows as the client being disconnected with a desync error.
3. Leave both running with a few thousand ticks of fast-forward after exercising the feature.
4. For each new command: play it from the *joining* client, the *host*, and then **save/reload and join a late client** (tests save/load of our chunk).
5. Phase 1 additionally: verify that toggling the overlay does not change the sync lines (it must not).

**Caveat:** I can't play the game for you. I can build, launch two instances and read the logs once a local toolchain exists (§9); the "does it look and feel right" checks are yours.

---

## 9. Phase 0 status and open items

Done (branch `phase0-setup`):
- Forked 15.3. Rebranded window title, main-menu version label, About window, exe properties, package name, personal directory (`Documents\PosseTTD`), README notice. Original copyright/licence files untouched.
- Version string = git tag (e.g. `v0.1.0`), so a PosseTTD client can never match an official server's version string (network protocol compares it).
- Upstream CI workflows moved to `.github/upstream-workflows/` (they'd fail on a fork: they need OpenTTD's secrets). New `.github/workflows/possettd-release.yml`: tag `v*` -> Windows x64 build via vcpkg+MSVC -> CPack zip -> OpenGFX 0.6.1 added in `baseset/` -> GitHub Release.

**Not yet done / needs you:**
1. **Local build is impossible right now:** this PC has Visual Studio 2022 Community but **no C++ workload**, plus no CMake/Ninja/vcpkg/`gh`. Options: (a) I walk you through installing "Desktop development with C++" (~6-8 GB, needs admin, then CMake + vcpkg), enabling local testing; or (b) build only on GitHub Actions (slow loop: push, wait ~20-40 min per build, download zip). **I recommend (a)**, because Phase 4-style work with only a cloud build is painful.
2. **The workflow is untested** until it runs on GitHub. It needs the repo pushed to your GitHub account. I haven't pushed anything or created any repo (that's outward-facing, your call).
3. **Project name:** your brief had a `[YOUR NAME HERE]` placeholder; I used **PosseTTD** from the folder name. One search-and-replace if you want another.
4. The exe is still called `openttd.exe` inside the zip (renaming touches many build files). Window title/version make it unmistakable; say if you want the rename.
5. "Confirm vanilla multiplayer works with the rebranded build" is **not verified** yet; it needs a built exe.

## 10. Feasibility notes / where I'd simplify (per your "propose simpler" rule)

- **Toolbar button** -> hotkey + dropdown entry (§4).
- **Shares/hostile takeovers (Phase 5):** I checked: **share trading no longer exists in 15.3** (no buy/sell-share command or `allow_shares` setting anywhere in `src`). Phase 5 takeovers would be built from scratch, a big feature; flagging early.
- **Strike (Phase 4)** might be reduced to a rating/income penalty if the loading code is too hot to touch.
- **Per-company-addressed news** may need to be display-filtered (§7).
- **Emblem in news** may be deferred (§5).

## 11. Decisions I need from you before Phase 1

1. Own savegame chunk with own version (recommended) vs. real `SLV_` bumps (§2)?
2. Install the C++ toolchain locally (recommended) or cloud-only builds (§9.1)?
3. Name PosseTTD OK? Rename `openttd.exe`? (§9.3-4)
4. Toolbar: hotkey + menu entry instead of a new button OK? (§4)
5. OK to push to a GitHub repo you create (private), so the workflow can run?
