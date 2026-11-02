// as_funcommands — Plugify C++ port of the Pisex cs2-admin_system module "FunCommands".
//
// Admin fun commands (each one needs its @admin/<name> permission from the admin_system core;
// the server console always passes):
//   mm_gravity/mm_hp/mm_armor/mm_scale/mm_speed <name|steamid|slot> <value>,
//   mm_god <target> [0/1], mm_freezy <target> [time], mm_unfreezy <target>,
//   mm_blind <target> [time], mm_unblind <target>, mm_bury <target>, mm_unbury <target>.
// Usage/error lines go to the caller's console (slot -1 = the server console).
//
// The admin menu: category "players" with the ten items freeze, gravity, scale, hp, armor,
// speed, god, bury, unbury, blind (identities, phrase keys, permissions and item order are kept).
// Selecting one shows the alive players (with the per-item state), selecting a player shows the
// value menu ("1..1000", "0.1x..10x"), "back" goes back to the admin menu (player list) or to the
// player list (value menus), and every selection performs the same action + broadcast + SendAction
// payload as the original.
//
// Pure s2sdk, no pisexcompat and no s2guard (docs/PLUGIN_STANDARD.md §7). Every guard the shared
// libraries used to provide lives in this file:
//
//   * PITFALLS B1/B2 — the map gate g_mapReady. s2sdk client functions dereference null before the
//     first map and, worse, an emptied client list between OnMapEnd and the next OnMapStart. The
//     map listeners are registered in OnPluginStart (only as a complete Register+Unregister
//     pair); OnMapStart latches readiness, OnMapEnd drops it, and a plugin loaded into a running
//     map latches it on its first OnPluginUpdate — but only when all four map-listener bindings
//     were registered, no OnMapEnd was seen since OnPluginStart and s2sdk::GetCurrentMap() names a
//     real map (non-empty, not "<empty>"). No client, entity or schema call is made while the
//     gate is closed. OnMapEnd only clears this plugin's own bookkeeping (no client query).
//   * PITFALLS C2 — every client query goes through HasClient(): slot 0..63,
//     IsClientConnected && IsClientInGame && !IsClientSourceTV. Before a controller/pawn is used
//     its entity handle is validated; CEntityHandle is a *signed* int32 (index | serial << 15)
//     whose serial may set the sign bit, so negative values are valid handles. Only 0 and -1 are
//     sentinels (PITFALLS C1); entity indices and player slots stay positive. A bot's SteamID64 is
//     never read (it is 0) and s2sdk's "<unknown>" name becomes "".
//   * Permissions — every client-originated action (command or menu selection) goes through
//     HasReadyPermission(): IsClientAdminDataReady(slot, live SteamID64) must be true before
//     HasPermission() is trusted, because until the admin core's asynchronous SELECT answers (or
//     after a failed one, or on an admin_system older than 1.1.0 where the export is missing)
//     HasPermission() can still hold the rights of the slot's previous occupant. Pending/failed/
//     missing denies (fail soft), it never opens an action up (plugins/admin_system/API.md §4.1).
//     Only slot -1 is the server-console bypass; -2 and out-of-range callers fail closed.
//   * Slot identity — every action that outlives a command/menu selection (the delayed unfreeze
//     and unblind of mm_freezy/mm_blind <time>, and every menu selection) carries the target's
//     SteamID64 *and* its controller handle (the entity serial is the connection generation) and
//     is re-resolved against the live client table right before it runs, so a player who left and
//     a slot reused by somebody else can never receive the action/effect meant for the previous
//     occupant (PITFALLS C2/B4). The acting admin is re-checked the same way: a delayed action
//     whose admin slot now holds another player still unfreezes/unblinds the target but is not
//     announced or logged on behalf of the new occupant.
//   * PITFALLS C11 — the twelve commands are console commands of this plugin
//     (ClientCanExecute|Release, so a client console reaches the permission check exactly like the
//     Pisex CON_COMMAND_F they replace). s2sdk dispatches a chat "!mm_god" to the same callback
//     with ConCommandContext::Chat; these commands were console-only in the original, so the
//     callback answers Continue for chat and lets s2sdk treat the line as ordinary chat.
//     AddConsoleCommand answers false when the name already existed and then only attaches a
//     listener: OnPluginEnd removes a name this plugin created with RemoveCommand and detaches a
//     listener with RemoveCommandListener otherwise. No command at all is registered unless all
//     three bindings exist (a callback that cannot be removed would outlive the .so).
//   * PITFALLS C6/C8 — the five schema fields without an s2sdk helper (m_flFlashDuration,
//     m_flFlashMaxAlpha, m_blindUntilTime, m_blindStartTime on CCSPlayerPawnBase, m_bTakesDamage
//     on CBaseEntity) are read and written through *raw* cached offsets: GetSchemaOffset /
//     GetSchemaChainOffset are called only while the gate is open (before the first map they
//     segfault inside s2sdk's serializer map) and only successful lookups (>= 0, chain > 0) are
//     cached, so a field that was unavailable at the first probe is retried instead of poisoned.
//     The raw writes match the original's widths and changeState flags (the flash fields are
//     networked and get a chain state change; the two blind times and m_bTakesDamage are written
//     without one, exactly like the Metamod module did).
//   * PITFALLS C9 — no s2sdk timers at all (CreateTimer never runs a repeat timer inside the timer
//     mutex and a game action under it is the vip_duckspeed hang). The two timed commands use
//     deadline accumulators driven by OnPluginUpdate(dt); each entry carries the identities above
//     and its effect is applied outside every s2sdk lock.
//   * PITFALLS C10 — native s2sdk menus. MenuId 0 is also "no menu" in the client state, so a menu
//     created with id 0 is burnt and re-created. Every menu this plugin opens is tracked per slot
//     and destroyed after its session ends (the manager never destroys a closed menu), and
//     DestroyMenu is never called from inside a menu handler: the handler only records a Pending
//     action which OnPluginUpdate applies outside the manager's mutex. On OnMapEnd/unload every
//     display is ended with CancelClientMenu(Disconnect), re-checked with GetClientMenu and, when
//     a display survives in a map-down window, switched to the builtin "chat"/"console" no-op-close
//     type (verified through GetMenuType) before DestroyMenu; if even that is impossible the
//     request is parked and A7_UNRESOLVED is logged instead of a blind backend close on the
//     torn-down client. Menus use the exact Pisex table for the player's own "!menu" choice
//     (hudmenu::GetClientMenuType, an optional plugin API dependency): "hud" only while its
//     backend is registered, otherwise "button"; without a player choice the module's HUD_LAYOUT
//     default maps to "hud" when registered, otherwise "button".
//   * PITFALLS A1/A7 — OnPluginStart never returns Failed (that stops every Plugify plugin), and
//     every registration is removed in OnPluginEnd: the map listeners, the commands, the admin
//     core listener and the ten "players" items. The admin core has no item-unregister API, so the
//     items are re-registered through the *low-level* binding with null callbacks before the
//     library goes away (the consumer wrapper would install its own trampolines, i.e. pointers
//     into the .so being unloaded); the category was registered with a null display callback from
//     the start, also through the low-level binding. `plg reload` therefore leaves nothing
//     pointing into the library.
//
// No config, translation file or database of its own: phrases come from admin_system's
// GetTranslation() (the original carried them in the core's admin_system.phrases.txt). No game
// event is hooked, so no event slot conversion is needed here.

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <hudmenu/hudmenu.hpp>

#include <admin_api.hpp>

#include "text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr const char* kLogTag = "as_funcommands";
// CS2 has 64 player slots; the original iterated 0..63 without asking for a maximum.
constexpr int32_t kSlots = 64;
// The original's cs2-menus menus are paginated five items per page (the chat menu of Pisex).
constexpr int32_t kItemsPerPage = 5;
// The cs2-menus core.ini "MenuTime" default of the original: a menu closes itself after 60 s.
constexpr int32_t kMenuTimeoutSeconds = 60;
// MoveType_t of the server schema: MOVETYPE_OBSOLETE = 1 is the "frozen" value the original used
// (s2sdk's MoveType enum calls the same value "Isometric" but passes it to the engine unchanged),
// MOVETYPE_WALK = 2 the value unfreeze restores.
constexpr int32_t kMoveTypeObsolete = 1;
constexpr int32_t kMoveTypeWalk = 2;
// A long value argument is rejected before atof/atoi, exactly like the original's strlen > 10.
constexpr std::size_t kMaxValueLength = 10;
// Widths of the raw schema writes (checked against the server schema).
constexpr int32_t kFloatSize = 4;
constexpr int32_t kBoolSize = 1;
// s2sdk's sentinel for "the object itself is the entity" in the changeState chain argument.
constexpr int32_t kNoChainOffset = -1;

// ---------------------------------------------------------------------------------------------
// Interfaces (the original's g_pAdminApi global).
// ---------------------------------------------------------------------------------------------
admin_system::AdminApi* g_pAdminApi = nullptr;
// True when every admin_system export this module calls has a binding. A missing admin_system
// must not crash the module; it degrades to "no permission, no menu items" (PITFALLS A1/A4).
bool g_adminBindingsAvailable = false;
// True when every s2sdk menu export this module calls has a binding. Without them the admin menu
// items are not registered at all (rather than showing entries that do nothing).
bool g_menuBindingsAvailable = false;

// ---------------------------------------------------------------------------------------------
// PITFALLS B1/B2: the map gate.
// ---------------------------------------------------------------------------------------------
bool g_mapReady = false;		   // a map is up: client/entity/schema calls are safe
bool g_mapEndedSinceStart = false; // OnMapEnd was seen since OnPluginStart
bool g_firstUpdateDone = false;	   // the mid-map latch has already been decided
bool g_mapListeners = false;	   // OnMapStart/OnMapEnd are registered with s2sdk

// True while the ten "players" items are registered in the admin core. OnPluginEnd uses it to
// retire them (the core has no unregister API, see RetireAdminMenuItems below).
bool g_adminMenuItemsRegistered = false;

// ---------------------------------------------------------------------------------------------
// Logging (engine-free when plg::Log is unavailable; see PLUGIFY_PLUGIN).
// ---------------------------------------------------------------------------------------------
void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	LogMessage(std::string("[") + kLogTag + "] " + std::string(message), plg::Severity::Error);
}

void LogInfo(std::string_view message) {
	LogMessage(std::string("[") + kLogTag + "] " + std::string(message), plg::Severity::Info);
}

// Translated phrase from the admin core; the key itself when the core is not loaded (the
// original's g_vecPhrases lookup answered with the key on a miss).
const char* T(const char* key) {
	if (g_pAdminApi != nullptr) {
		return g_pAdminApi->GetTranslation(key);
	}
	return key != nullptr ? key : "";
}

const char* Safe(const char* text) {
	return text != nullptr ? text : "";
}

plg::string Plg(std::string_view text) {
	return text.empty() ? plg::string() : plg::string(text);
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// ---------------------------------------------------------------------------------------------
// Binding availability
// ---------------------------------------------------------------------------------------------
// The exports used by this module. A dependency is loaded before its consumer, so normally all
// of them are filled; the checks keep the module alive (and honest about what it can do) when a
// plugin was loaded in an unexpected order or an older admin_system is installed.
bool AdminApiAvailable() {
	return __admin_system_GetPluginVersion != nullptr && __admin_system_GetTranslation != nullptr &&
		   __admin_system_GetMessageType != nullptr && __admin_system_IsCoreLoaded != nullptr &&
		   __admin_system_HasPermission != nullptr &&
		   __admin_system_IsClientAdminDataReady != nullptr &&
		   __admin_system_SendAction != nullptr && __admin_system_RegisterCategory != nullptr &&
		   __admin_system_RegisterItem != nullptr &&
		   __admin_system_OnCoreLoaded_Register != nullptr &&
		   __admin_system_OnCoreLoaded_Unregister != nullptr &&
		   __admin_system_ShowAdminLastCategoryMenu != nullptr;
}

bool MenuApiAvailable() {
	// DestroyMenu/GetClientMenu/CancelClientMenu are required for every menu handle, and
	// SetMenuType/GetMenuType are required *before* any menu is created: they are the fail-safe
	// that makes a still-displayed menu closable with a no-op backend (see DestroyMenuChecked).
	return __s2sdk_CreateMenu != nullptr && __s2sdk_DestroyMenu != nullptr &&
		   __s2sdk_AddMenuItem != nullptr && __s2sdk_DisplayMenu != nullptr &&
		   __s2sdk_SetMenuPagination != nullptr && __s2sdk_SetMenuExitButton != nullptr &&
		   __s2sdk_SetMenuExitBackButton != nullptr && __s2sdk_SetMenuCloseOnSelect != nullptr &&
		   __s2sdk_GetClientMenu != nullptr && __s2sdk_GetClientMenuOffset != nullptr &&
		   __s2sdk_CancelClientMenu != nullptr && __s2sdk_SetMenuType != nullptr &&
		   __s2sdk_GetMenuType != nullptr;
}

// ---------------------------------------------------------------------------------------------
// PITFALLS C2: the client guards.
// ---------------------------------------------------------------------------------------------
// The map gate and the slot range are the only requirements of a function that addresses a slot
// without reading its state (a print).
bool CanQueryClient(int32_t iSlot) {
	return g_mapReady && iSlot >= 0 && iSlot < kSlots;
}

// A live, human-or-bot slot: connected, in game and not the SourceTV proxy.
bool HasClient(int32_t iSlot) {
	if (!CanQueryClient(iSlot) || !__s2sdk_IsClientConnected || !__s2sdk_IsClientInGame ||
		!__s2sdk_IsClientSourceTV) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(iSlot) && s2sdk::IsClientInGame(iSlot) &&
			   !s2sdk::IsClientSourceTV(iSlot);
	} catch (...) {
		return false;
	}
}

bool IsBot(int32_t iSlot) {
	if (!HasClient(iSlot) || __s2sdk_IsFakeClient == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsFakeClient(iSlot);
	} catch (...) {
		return false;
	}
}

bool ClientAlive(int32_t iSlot) {
	if (!CanQueryClient(iSlot) || __s2sdk_IsClientAlive == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientAlive(iSlot);
	} catch (...) {
		return false;
	}
}

// The controller entity handle of the slot, or -1 when there is none. CEntityHandle is a SIGNED
// int32: index | serial << 15, where the serial may legitimately set the sign bit, so negative
// values are valid handles. Only 0 and -1 are sentinels (s2sdk's invalid handle and the world
// entity, which is never a player controller); every other int32 is handed to the engine's
// IsValidEntHandle (PITFALLS C1/C2). The same signed value is used as the identity of a deferred
// action: a slot whose controller handle changed belongs to a new connection, even when the
// SteamID64 happens to be the same.
int32_t ControllerHandle(int32_t iSlot) {
	if (!HasClient(iSlot) || !__s2sdk_PlayerSlotToEntHandle || !__s2sdk_IsValidEntHandle) {
		return -1;
	}
	try {
		const int32_t iHandle = s2sdk::PlayerSlotToEntHandle(iSlot);
		if (iHandle == 0 || iHandle == -1 || !s2sdk::IsValidEntHandle(iHandle)) {
			return -1;
		}
		return iHandle;
	} catch (...) {
		return -1;
	}
}

bool ControllerValid(int32_t iSlot) {
	return ControllerHandle(iSlot) != -1;
}

// The pawn's entity handle of the slot, or -1 when there is no usable pawn. Same signed-handle
// rules as ControllerHandle: only 0 and -1 are sentinels, negative serials are valid.
int32_t PawnHandle(int32_t iSlot) {
	if (!ControllerValid(iSlot) || __s2sdk_GetClientPawn == nullptr ||
		__s2sdk_EntPointerToEntHandle == nullptr) {
		return -1;
	}
	try {
		void* pPawn = s2sdk::GetClientPawn(iSlot);
		if (pPawn == nullptr) {
			return -1;
		}
		if (__s2sdk_IsValidEntPointer != nullptr && !s2sdk::IsValidEntPointer(pPawn)) {
			return -1;
		}
		const int32_t iHandle = s2sdk::EntPointerToEntHandle(pPawn);
		if (iHandle == 0 || iHandle == -1) {
			return -1;
		}
		if (__s2sdk_IsValidEntHandle != nullptr && !s2sdk::IsValidEntHandle(iHandle)) {
			return -1;
		}
		return iHandle;
	} catch (...) {
		return -1;
	}
}

// The original's TargetUsable(): CCSPlayerController::FromSlot() + GetPlayerPawn() != nullptr.
bool TargetUsable(int32_t iSlot) {
	return PawnHandle(iSlot) != -1;
}

// s2sdk answers "<unknown>" for a name it could not resolve; the original printed "".
std::string ClientName(int32_t iSlot) {
	if (!ControllerValid(iSlot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	try {
		const plg::string szName = s2sdk::GetClientName(iSlot);
		std::string out = ToStd(szName);
		if (out == "<unknown>") {
			out.clear();
		}
		return out;
	} catch (...) {
		return {};
	}
}

// 0 for a bot, an unauthenticated client, a disconnected slot and while no map is up.
uint64_t ClientSteamID64(int32_t iSlot) {
	if (!HasClient(iSlot) || IsBot(iSlot) || __s2sdk_GetClientSteamID64 == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(iSlot);
	} catch (...) {
		return 0;
	}
}

// The admin-load readiness of API 1.1.0 (see plugins/admin_system/API.md §4.1): false means
// "ask again later", not "not an admin". Showing admin data or acting on a permission while the
// SELECT is still pending would trust a record that may belong to the slot's previous occupant.
bool AdminDataReady(int32_t iSlot, uint64_t iSteamID64) {
	if (g_pAdminApi == nullptr || iSteamID64 == 0 ||
		__admin_system_IsClientAdminDataReady == nullptr) {
		return false;
	}
	return g_pAdminApi->IsClientAdminDataReady(iSlot, iSteamID64);
}

bool HasPermission(int32_t iSlot, const char* szPermission) {
	if (!g_adminBindingsAvailable || g_pAdminApi == nullptr || szPermission == nullptr) {
		return false;
	}
	return g_pAdminApi->HasPermission(iSlot, szPermission);
}

// The permission check every client-originated action uses. IsClientAdminDataReady() must be
// true for the *live* SteamID64 of the slot first: until the asynchronous admin SELECT answers
// (or after a failed one, or on an admin_system older than 1.1.0 where the export is missing)
// HasPermission() may still hold the previous occupant's rights. Pending/failed/missing means
// "deny now, ask again later" - never a fail-open (API.md §4.1).
bool HasReadyPermission(int32_t iSlot, const char* szPermission) {
	if (iSlot < 0 || iSlot >= kSlots || szPermission == nullptr ||
		__admin_system_IsClientAdminDataReady == nullptr) {
		return false;
	}
	const uint64_t iSteamID64 = ClientSteamID64(iSlot);
	if (iSteamID64 == 0 || !AdminDataReady(iSlot, iSteamID64)) {
		return false;
	}
	return HasPermission(iSlot, szPermission);
}

// The output name of the admin who acted: "Console" for the server console, the live name
// otherwise (empty when the client is gone - the message is then shown with an empty name,
// exactly like the original's GetPlayerName()).
std::string ActorName(int32_t iSlot) {
	if (iSlot == -1) {
		return "Console";
	}
	if (iSlot < 0) {
		return {}; // -2/out-of-range is not a caller and not the console
	}
	return ClientName(iSlot);
}

// ---------------------------------------------------------------------------------------------
// Output: the Pisex printers over s2sdk (chat, the caller's console, the server console).
// ---------------------------------------------------------------------------------------------
// The server console: colour tags are meaningless there and are stripped (pisexcompat did the
// same); PrintToServer() is ConMsg("%s\n"), so one trailing newline is dropped to keep one line.
void PrintServerLine(std::string_view text) {
	if (__s2sdk_PrintToServer == nullptr) {
		return;
	}
	std::string line = as_funcommands::StripColorTags(text);
	if (!line.empty() && line.back() == '\n') {
		line.pop_back();
	}
	try {
		s2sdk::PrintToServer(Plg(line));
	} catch (...) {
	}
}

// A console message: the server console for slot -1, the caller's own console otherwise. A client
// console gets the text as the original sent it (colour tags included, like the Pisex
// PrintToConsole); only the server console is stripped of them.
void PrintConsole(int32_t iSlot, std::string_view text) {
	if (iSlot == -1) {
		PrintServerLine(text);
		return;
	}
	if (iSlot < 0 || !CanQueryClient(iSlot) || __s2sdk_PrintToConsole == nullptr) {
		return; // -2/out-of-range is invalid, not the server console (fail closed)
	}
	try {
		s2sdk::PrintToConsole(iSlot, Plg(text));
	} catch (...) {
	}
}

void PrintChatAll(std::string_view text) {
	if (!g_mapReady || __s2sdk_PrintToChatAll == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToChatAll(Plg(as_funcommands::Colorize(text)));
	} catch (...) {
	}
}

// A translated phrase with printf arguments, colourised for chat (the original's
// pisex::PrintToChatAll(phrase, args...)).
void PrintChatAllPhrase(const char* szKey, ...) {
	va_list args;
	va_start(args, szKey);
	const std::string text = as_funcommands::FormatV(T(szKey), args);
	va_end(args);
	PrintChatAll(text);
}

// The original's "usage message to a console or a player" block. Only slot -1 is the server
// console: any other negative slot is invalid and gets no output at all.
void PrintUsage(int32_t iSlot, const char* szPhrase, const char* szCommand) {
	PrintConsole(iSlot, as_funcommands::Format(T(szPhrase), Safe(szCommand)));
}

// The original's pcss/Pisex "Invalid <x> value" line (a plain string, not a phrase).
void PrintLine(int32_t iSlot, const char* szText) {
	PrintConsole(iSlot, szText != nullptr ? szText : "");
}

int GetMessageType() {
	return g_pAdminApi != nullptr ? g_pAdminApi->GetMessageType() : 0;
}

void SendAdminAction(int32_t iSlot, const char* szAction, const std::string& szParam) {
	if (g_pAdminApi == nullptr) {
		return;
	}
	g_pAdminApi->SendAction(iSlot, szAction, szParam.c_str());
}

// ---------------------------------------------------------------------------------------------
// PITFALLS C6/C8: the raw schema fields without an s2sdk helper.
//
// Resolved lazily, only with the gate open, and only successful lookups are cached. The typed
// SetEntSchema*/GetEntSchema* helpers are deliberately not used: a typed call on a class touched
// for the first time during the map-start window can crash inside s2sdk ("schema type poison",
// PITFALLS C6), and the raw offset + width pair is exactly what the original's
// SCHEMA_FIELD/SetStateChanged code did.
// ---------------------------------------------------------------------------------------------
constexpr const char* kPawnBaseClass = "CCSPlayerPawnBase";
constexpr const char* kBaseEntityClass = "CBaseEntity";
constexpr const char* kFlashDurationField = "m_flFlashDuration";
constexpr const char* kFlashMaxAlphaField = "m_flFlashMaxAlpha";
constexpr const char* kBlindUntilField = "m_blindUntilTime";
constexpr const char* kBlindStartField = "m_blindStartTime";
constexpr const char* kTakesDamageField = "m_bTakesDamage";

struct SchemaOffsets {
	int32_t iFlashDuration = -1;
	int32_t iFlashMaxAlpha = -1;
	int32_t iBlindUntil = -1;
	int32_t iBlindStart = -1;
	int32_t iTakesDamage = -1;
	int32_t iPawnChain = -1;
	int32_t iBaseEntityChain = -1;
	bool bFailureLogged = false;
};

SchemaOffsets g_schema;

// -1 while the gate is closed (GetSchemaOffset segfaults before the first map) and for a failed
// lookup; the caller decides whether to cache the answer.
int32_t LookupSchemaOffset(const char* szClass, const char* szField) {
	if (!g_mapReady || szClass == nullptr || szField == nullptr ||
		__s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	try {
		return s2sdk::GetSchemaOffset(plg::string(szClass), plg::string(szField));
	} catch (...) {
		return -1;
	}
}

// -1 for "the object itself is the entity" (no chainer / unknown class) and while the gate is
// closed. s2sdk's SafeNetworkStateChanged() reads a chain offset >= 0 as "the object's
// __m_pChainEntity sits at object + offset", so 0 or a negative answer must become -1.
int32_t LookupSchemaChainOffset(const char* szClass) {
	if (!g_mapReady || szClass == nullptr || __s2sdk_GetSchemaChainOffset == nullptr) {
		return -1;
	}
	try {
		const int32_t iOffset = s2sdk::GetSchemaChainOffset(plg::string(szClass));
		return iOffset > 0 ? iOffset : -1;
	} catch (...) {
		return -1;
	}
}

void ResolveSchemaOffsets() {
	if (g_schema.iFlashDuration < 0) {
		g_schema.iFlashDuration = LookupSchemaOffset(kPawnBaseClass, kFlashDurationField);
	}
	if (g_schema.iFlashMaxAlpha < 0) {
		g_schema.iFlashMaxAlpha = LookupSchemaOffset(kPawnBaseClass, kFlashMaxAlphaField);
	}
	if (g_schema.iBlindUntil < 0) {
		g_schema.iBlindUntil = LookupSchemaOffset(kPawnBaseClass, kBlindUntilField);
	}
	if (g_schema.iBlindStart < 0) {
		g_schema.iBlindStart = LookupSchemaOffset(kPawnBaseClass, kBlindStartField);
	}
	if (g_schema.iTakesDamage < 0) {
		g_schema.iTakesDamage = LookupSchemaOffset(kBaseEntityClass, kTakesDamageField);
	}
	if (g_schema.iPawnChain <= 0) {
		g_schema.iPawnChain = LookupSchemaChainOffset(kPawnBaseClass);
	}
	if (g_schema.iBaseEntityChain <= 0) {
		g_schema.iBaseEntityChain = LookupSchemaChainOffset(kBaseEntityClass);
	}
	if (!g_schema.bFailureLogged &&
		(g_schema.iFlashDuration < 0 || g_schema.iFlashMaxAlpha < 0 || g_schema.iBlindUntil < 0 ||
		 g_schema.iBlindStart < 0 || g_schema.iTakesDamage < 0)) {
		g_schema.bFailureLogged = true;
		std::string missing;
		if (g_schema.iFlashDuration < 0) {
			missing += std::string(kPawnBaseClass) + "::" + kFlashDurationField + " ";
		}
		if (g_schema.iFlashMaxAlpha < 0) {
			missing += std::string(kPawnBaseClass) + "::" + kFlashMaxAlphaField + " ";
		}
		if (g_schema.iBlindUntil < 0) {
			missing += std::string(kPawnBaseClass) + "::" + kBlindUntilField + " ";
		}
		if (g_schema.iBlindStart < 0) {
			missing += std::string(kPawnBaseClass) + "::" + kBlindStartField + " ";
		}
		if (g_schema.iTakesDamage < 0) {
			missing += std::string(kBaseEntityClass) + "::" + kTakesDamageField + " ";
		}
		LogError("schema field(s) not found: " + missing +
				 "- the matching fun command stays off");
	}
}

// Called from OnPluginEnd so a reload resolves the fields against the live map again (C8).
void ClearSchemaOffsets() {
	g_schema = SchemaOffsets{};
}

float ReadEntFloat(int32_t iHandle, int32_t iOffset) {
	if (iHandle == -1 || iOffset < 0 || !g_mapReady || __s2sdk_GetEntDataFloat == nullptr) {
		return 0.0f;
	}
	try {
		return static_cast<float>(s2sdk::GetEntDataFloat(iHandle, iOffset, kFloatSize));
	} catch (...) {
		return 0.0f;
	}
}

bool ReadEntBool(int32_t iHandle, int32_t iOffset) {
	if (iHandle == -1 || iOffset < 0 || !g_mapReady || __s2sdk_GetEntData == nullptr) {
		return false;
	}
	try {
		return s2sdk::GetEntData(iHandle, iOffset, kBoolSize) != 0;
	} catch (...) {
		return false;
	}
}

// One raw float write at a cached offset. changeState is the original's SetStateChanged: the
// networked flash fields send the new value to the clients through the class's chain entity, the
// plain blind times do not.
void WritePawnFloat(int32_t iPawnHandle, int32_t iOffset, float flValue, bool bChangeState,
					int32_t iChainOffset) {
	if (iPawnHandle == -1 || iOffset < 0 || !g_mapReady || __s2sdk_SetEntDataFloat == nullptr) {
		return;
	}
	try {
		// -2 is s2sdk's "this is not a chain-field write"; the argument is only read when
		// changeState is true, but passing it explicitly keeps the intent of a plain write.
		s2sdk::SetEntDataFloat(iPawnHandle, iOffset, flValue, kFloatSize, bChangeState,
							   bChangeState ? iChainOffset : -2);
	} catch (...) {
	}
}

// The original wrote m_bTakesDamage without a network update (damage is processed server-side),
// so no chain offset is read here either.
void WriteBaseEntityBool(int32_t iHandle, int32_t iOffset, bool bValue) {
	if (iHandle == -1 || iOffset < 0 || !g_mapReady || __s2sdk_SetEntData == nullptr) {
		return;
	}
	try {
		s2sdk::SetEntData(iHandle, iOffset, bValue ? 1 : 0, kBoolSize, /*changeState=*/false,
						  /*chainOffset=*/-2);
	} catch (...) {
	}
}

// ---------------------------------------------------------------------------------------------
// The client-schema helpers of the original (each of them is an s2sdk export that dereferences
// the controller/pawn without validating it - the callers validate first, PITFALLS C2).
// ---------------------------------------------------------------------------------------------
float GetClientGravityValue(int32_t iSlot) {
	if (!g_mapReady || __s2sdk_GetClientGravity == nullptr) {
		return 0.0f;
	}
	try {
		return s2sdk::GetClientGravity(iSlot);
	} catch (...) {
		return 0.0f;
	}
}

float GetClientSpeedValue(int32_t iSlot) {
	if (!g_mapReady || __s2sdk_GetClientSpeed == nullptr) {
		return 0.0f;
	}
	try {
		return s2sdk::GetClientSpeed(iSlot);
	} catch (...) {
		return 0.0f;
	}
}

float GetClientScaleValue(int32_t iSlot) {
	// m_CBodyComponent->m_pSceneNode->m_flScale of the original (the engine's GetAbsScale).
	if (!g_mapReady || __s2sdk_GetClientAbsScale == nullptr) {
		return 0.0f;
	}
	try {
		return s2sdk::GetClientAbsScale(iSlot);
	} catch (...) {
		return 0.0f;
	}
}

int32_t GetClientMoveTypeValue(int32_t iSlot) {
	if (!g_mapReady || __s2sdk_GetClientMoveType == nullptr) {
		return kMoveTypeWalk;
	}
	try {
		return static_cast<int32_t>(s2sdk::GetClientMoveType(iSlot));
	} catch (...) {
		return kMoveTypeWalk;
	}
}

int32_t GetClientHealthValue(int32_t iSlot) {
	if (!g_mapReady || __s2sdk_GetClientHealth == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientHealth(iSlot);
	} catch (...) {
		return 0;
	}
}

int32_t GetClientArmorValue(int32_t iSlot) {
	if (!g_mapReady || __s2sdk_GetClientArmor == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientArmor(iSlot);
	} catch (...) {
		return 0;
	}
}

void CallSetClientGravity(int32_t iSlot, float flValue) {
	if (!g_mapReady || __s2sdk_SetClientGravity == nullptr) {
		return;
	}
	try {
		s2sdk::SetClientGravity(iSlot, flValue);
	} catch (...) {
	}
}

void CallSetClientHealth(int32_t iSlot, int32_t iValue) {
	if (!g_mapReady || __s2sdk_SetClientHealth == nullptr) {
		return;
	}
	try {
		s2sdk::SetClientHealth(iSlot, iValue);
	} catch (...) {
	}
}

void CallSetClientArmor(int32_t iSlot, int32_t iValue) {
	if (!g_mapReady || __s2sdk_SetClientArmor == nullptr) {
		return;
	}
	try {
		s2sdk::SetClientArmor(iSlot, iValue);
	} catch (...) {
	}
}

void CallSetClientSpeed(int32_t iSlot, float flValue) {
	if (!g_mapReady || __s2sdk_SetClientSpeed == nullptr) {
		return;
	}
	try {
		s2sdk::SetClientSpeed(iSlot, flValue);
	} catch (...) {
	}
}

void CallSetClientScale(int32_t iSlot, float flValue) {
	if (!g_mapReady || __s2sdk_SetClientAbsScale == nullptr) {
		return;
	}
	try {
		s2sdk::SetClientAbsScale(iSlot, flValue);
	} catch (...) {
	}
}

void CallSetClientMoveType(int32_t iSlot, int32_t iValue) {
	// IPlayersApi::SetMoveType of the original: the engine's CBaseEntity::SetMoveType.
	if (!g_mapReady || __s2sdk_SetClientMoveType == nullptr) {
		return;
	}
	try {
		s2sdk::SetClientMoveType(iSlot, static_cast<s2sdk::MoveType>(iValue));
	} catch (...) {
	}
}

float GetPawnFlashDuration(int32_t iSlot) {
	ResolveSchemaOffsets();
	return ReadEntFloat(PawnHandle(iSlot), g_schema.iFlashDuration);
}

bool GetPawnTakesDamage(int32_t iSlot) {
	ResolveSchemaOffsets();
	return ReadEntBool(PawnHandle(iSlot), g_schema.iTakesDamage);
}

void SetPawnTakesDamage(int32_t iSlot, bool bTakesDamage) {
	ResolveSchemaOffsets();
	WriteBaseEntityBool(PawnHandle(iSlot), g_schema.iTakesDamage, bTakesDamage);
}

void SetPawnFloat(int32_t iSlot, int32_t iOffset, int32_t iChainOffset, float flValue,
				  bool bChangeState) {
	const int32_t iPawnHandle = PawnHandle(iSlot);
	if (iPawnHandle == -1) {
		return;
	}
	WritePawnFloat(iPawnHandle, iOffset, flValue, bChangeState, iChainOffset);
}

// The engine's gpGlobals->curtime of the original (only read with a map up).
float CurrentGameTime() {
	if (!g_mapReady || __s2sdk_GetGameTime == nullptr) {
		return 0.0f;
	}
	try {
		return s2sdk::GetGameTime();
	} catch (...) {
		return 0.0f;
	}
}

bool IsFrozen(int32_t iSlot) {
	return GetClientMoveTypeValue(iSlot) == kMoveTypeObsolete;
}

bool IsBlinded(int32_t iSlot) {
	return GetPawnFlashDuration(iSlot) > 0.0f;
}

// ---------------------------------------------------------------------------------------------
// The actions (port of the original's Set*/Blind*/Bury* free functions).
//
// bNotify is false only for a delayed action whose acting admin's slot has been reused since the
// command: the effect on the target still happens, but the broadcast and the admin-core action
// are not attributed to the new occupant of that slot.
// ---------------------------------------------------------------------------------------------
void SetGravity(int32_t iSlot, int32_t iTarget, float flGravity, bool bNotify) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	CallSetClientGravity(iTarget, flGravity);
	if (!bNotify) {
		return;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminGravityMessage", ActorName(iSlot).c_str(),
						   ClientName(iTarget).c_str(), flGravity);
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("GravityMessage", ClientName(iTarget).c_str(), flGravity);
	}
	SendAdminAction(iSlot, "gravity", as_funcommands::Format("%i %f", iTarget, flGravity));
}

void SetHP(int32_t iSlot, int32_t iTarget, int32_t iHP, bool bNotify) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	CallSetClientHealth(iTarget, iHP);
	if (!bNotify) {
		return;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminHPMessage", ActorName(iSlot).c_str(), ClientName(iTarget).c_str(),
						   iHP);
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("HPMessage", ClientName(iTarget).c_str(), iHP);
	}
	SendAdminAction(iSlot, "hp", as_funcommands::Format("%i %i", iTarget, iHP));
}

void SetArmor(int32_t iSlot, int32_t iTarget, int32_t iArmor, bool bNotify) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	CallSetClientArmor(iTarget, iArmor);
	if (!bNotify) {
		return;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminArmorMessage", ActorName(iSlot).c_str(),
						   ClientName(iTarget).c_str(), iArmor);
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("ArmorMessage", ClientName(iTarget).c_str(), iArmor);
	}
	SendAdminAction(iSlot, "armor", as_funcommands::Format("%i %i", iTarget, iArmor));
}

void SetScale(int32_t iSlot, int32_t iTarget, float flScale, bool bNotify) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	CallSetClientScale(iTarget, flScale);
	if (!bNotify) {
		return;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminScaleMessage", ActorName(iSlot).c_str(),
						   ClientName(iTarget).c_str(), flScale);
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("ScaleMessage", ClientName(iTarget).c_str(), flScale);
	}
	SendAdminAction(iSlot, "scale", as_funcommands::Format("%i %f", iTarget, flScale));
}

void SetSpeed(int32_t iSlot, int32_t iTarget, float flSpeed, bool bNotify) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	CallSetClientSpeed(iTarget, flSpeed);
	if (!bNotify) {
		return;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminSpeedMessage", ActorName(iSlot).c_str(),
						   ClientName(iTarget).c_str(), flSpeed);
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("SpeedMessage", ClientName(iTarget).c_str(), flSpeed);
	}
	SendAdminAction(iSlot, "speed", as_funcommands::Format("%i %f", iTarget, flSpeed));
}

void SetGod(int32_t iSlot, int32_t iTarget, bool bGod, bool bNotify) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	// NOTE: the original's messages are inverted on purpose ("...GodMessageDisable" is used when
	// god is *enabled*, because god mode means m_bTakesDamage == false); kept as is.
	SetPawnTakesDamage(iTarget, bGod);
	if (!bNotify) {
		return;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		const char* szKey = bGod ? "AdminGodMessageDisable" : "AdminGodMessageEnable";
		PrintChatAllPhrase(szKey, ActorName(iSlot).c_str(), ClientName(iTarget).c_str());
	} else if (iMessageType == 1) {
		const char* szKey = bGod ? "GodMessageDisable" : "GodMessageEnable";
		PrintChatAllPhrase(szKey, ClientName(iTarget).c_str());
	}
	SendAdminAction(iSlot, "god", as_funcommands::Format("%i %i", iTarget, bGod ? 1 : 0));
}

void SetFreezy(int32_t iSlot, int32_t iTarget, bool bFreeze, bool bNotify) {
	// IPlayersApi::SetMoveType of the original (MOVETYPE_OBSOLETE / MOVETYPE_WALK). The pawn guard
	// keeps s2sdk's "Cannot execute 'void SetClientMoveType(int, MoveType_t)' on invalid player
	// pawn" warning out of the console on the command paths, where FindUser can hand back a slot
	// whose pawn is gone (a dead player); the engine call does nothing there either. Unlike the
	// other setters the original still announced the freeze/unfreeze of such a slot, so the
	// messages and the admin action are not behind the guard.
	if (TargetUsable(iTarget)) {
		CallSetClientMoveType(iTarget, bFreeze ? kMoveTypeObsolete : kMoveTypeWalk);
	}
	if (!bNotify) {
		return;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		const char* szKey = bFreeze ? "AdminFreezeMessage" : "AdminUnfreezeMessage";
		PrintChatAllPhrase(szKey, ActorName(iSlot).c_str(), ClientName(iTarget).c_str());
	} else if (iMessageType == 1) {
		const char* szKey = bFreeze ? "FreezeMessage" : "UnfreezeMessage";
		PrintChatAllPhrase(szKey, ClientName(iTarget).c_str());
	}
	SendAdminAction(iSlot, bFreeze ? "freeze" : "unfreeze", std::to_string(iTarget));
}

void BlindPlayer(int32_t iSlot, int32_t iTarget, bool bBlind, bool bNotify) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	ResolveSchemaOffsets();
	const float flCurtime = CurrentGameTime();
	if (bBlind) {
		// m_flFlashDuration/m_flFlashMaxAlpha are networked (the original marked them changed);
		// the two blind times are plain (non-networked) GameTime_t fields.
		SetPawnFloat(iTarget, g_schema.iFlashDuration, g_schema.iPawnChain, 9999.0f, true);
		SetPawnFloat(iTarget, g_schema.iFlashMaxAlpha, g_schema.iPawnChain, 255.0f, true);
		SetPawnFloat(iTarget, g_schema.iBlindUntil, kNoChainOffset, flCurtime + 9999.0f, false);
		SetPawnFloat(iTarget, g_schema.iBlindStart, kNoChainOffset, flCurtime, false);
	} else {
		// IsBlinded() (and with it the menu label and the toggle) reads m_flFlashDuration, which
		// the engine does not clear server-side when the blind expires: the plugin has to zero it
		// (the same write vip_af makes, without a network state change).
		SetPawnFloat(iTarget, g_schema.iFlashDuration, kNoChainOffset, 0.0f, false);
		SetPawnFloat(iTarget, g_schema.iFlashMaxAlpha, kNoChainOffset, 0.0f, false);
		SetPawnFloat(iTarget, g_schema.iBlindUntil, kNoChainOffset, flCurtime - 1.0f, false);
	}
	if (!bNotify) {
		return;
	}
	const int iMessageType = GetMessageType();
	if (bBlind) {
		if (iMessageType == 2) {
			PrintChatAllPhrase("AdminBlindMessage", ActorName(iSlot).c_str(),
							   ClientName(iTarget).c_str());
		} else if (iMessageType == 1) {
			PrintChatAllPhrase("BlindMessage", ClientName(iTarget).c_str());
		}
	} else {
		if (iMessageType == 2) {
			PrintChatAllPhrase("AdminUnblindMessage", ActorName(iSlot).c_str(),
							   ClientName(iTarget).c_str());
		} else if (iMessageType == 1) {
			PrintChatAllPhrase("UnblindMessage", ClientName(iTarget).c_str());
		}
	}
	SendAdminAction(iSlot, "blind",
					as_funcommands::Format("%i %i", iTarget, IsBlinded(iTarget) ? 1 : 0));
}

// Reads the pawn transform for the bury/unbury teleport; false when a binding is missing.
bool ReadPawnTransform(int32_t iTarget, plg::vec3& origin, plg::vec3& angles,
					   plg::vec3& velocity) {
	if (__s2sdk_GetClientAbsOrigin == nullptr || __s2sdk_GetClientAbsAngles == nullptr ||
		__s2sdk_GetClientAbsVelocity == nullptr || __s2sdk_TeleportClient == nullptr) {
		return false;
	}
	try {
		origin = s2sdk::GetClientAbsOrigin(iTarget);
		angles = s2sdk::GetClientAbsAngles(iTarget);
		velocity = s2sdk::GetClientAbsVelocity(iTarget);
		return true;
	} catch (...) {
		return false;
	}
}

void TeleportPawn(int32_t iTarget, const plg::vec3& origin, const plg::vec3& angles,
				  const plg::vec3& velocity) {
	try {
		s2sdk::TeleportClient(iTarget, origin, angles, velocity);
	} catch (...) {
	}
}

void BuryPlayer(int32_t iSlot, int32_t iTarget, bool bNotify) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	plg::vec3 origin{};
	plg::vec3 angles{};
	plg::vec3 velocity{};
	if (!ReadPawnTransform(iTarget, origin, angles, velocity)) {
		return; // without the pawn transform the teleport would be a jump to the origin
	}
	origin.z -= 10.0f;
	TeleportPawn(iTarget, origin, angles, velocity);
	if (!bNotify) {
		return;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminBuryMessage", ActorName(iSlot).c_str(),
						   ClientName(iTarget).c_str());
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("BuryMessage", ClientName(iTarget).c_str());
	}
	SendAdminAction(iSlot, "bury", std::to_string(iTarget));
}

void UnburyPlayer(int32_t iSlot, int32_t iTarget, bool bNotify) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	plg::vec3 origin{};
	plg::vec3 angles{};
	plg::vec3 velocity{};
	if (!ReadPawnTransform(iTarget, origin, angles, velocity)) {
		return;
	}
	origin.z += 10.0f;
	TeleportPawn(iTarget, origin, angles, velocity);
	if (!bNotify) {
		return;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminUnburyMessage", ActorName(iSlot).c_str(),
						   ClientName(iTarget).c_str());
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("UnburyMessage", ClientName(iTarget).c_str());
	}
	SendAdminAction(iSlot, "unbury", std::to_string(iTarget));
}

// ---------------------------------------------------------------------------------------------
// The admin menu items (the original's RegisterCategory/RegisterItem block).
// ---------------------------------------------------------------------------------------------
struct ItemDef {
	const char* szIdentity;
	const char* szPhrase;
	const char* szPermission;
};

// Registry order of the original's OnCoreLoaded block.
constexpr std::array<ItemDef, 10> kItems = {{{"freeze", "Item_Freeze", "@admin/freeze"},
											 {"gravity", "Item_Gravity", "@admin/gravity"},
											 {"scale", "Item_Scale", "@admin/scale"},
											 {"hp", "Item_HP", "@admin/hp"},
											 {"armor", "Item_Armor", "@admin/armor"},
											 {"speed", "Item_Speed", "@admin/speed"},
											 {"god", "Item_God", "@admin/god"},
											 {"bury", "Item_Bury", "@admin/bury"},
											 {"unbury", "Item_Unbury", "@admin/unbury"},
											 {"blind", "Item_Blind", "@admin/blind"}}};

const ItemDef* FindItem(std::string_view identity) {
	for (const ItemDef& item : kItems) {
		if (identity == item.szIdentity) {
			return &item;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------------------------
// PITFALLS C10: the menu layer.
//
// One menu per player slot. A selection never performs an engine action inside the s2sdk menu
// handler (the handler runs under the menu manager's mutex): it stores a Pending action that
// OnPluginUpdate applies on the next frame, outside every s2sdk lock. Every menu whose session
// has ended is destroyed through the deferred queue - s2sdk never destroys a menu on its own.
// ---------------------------------------------------------------------------------------------
enum class MenuKind { None, Player, Health, Armor, Speed, Scale, Gravity };

const char* IdentityForKind(MenuKind kind) {
	switch (kind) {
	case MenuKind::Health:
		return "hp";
	case MenuKind::Armor:
		return "armor";
	case MenuKind::Speed:
		return "speed";
	case MenuKind::Scale:
		return "scale";
	case MenuKind::Gravity:
		return "gravity";
	case MenuKind::Player:
	case MenuKind::None:
		break;
	}
	return "";
}

struct MenuItemSpec {
	std::string info;
	std::string display;
	s2sdk::MenuItemStyle style = s2sdk::MenuItemStyle::Default;
};

struct OpenMenu {
	s2sdk::MenuId id = 0;
	MenuKind kind = MenuKind::None;
	std::string lastItem;		  // the admin item the menu belongs to
	uint64_t ownerSteamID64 = 0;  // the admin the menu was shown to (slot-reuse guard)
	int32_t ownerController = -1; // and the controller generation of that connection
	int32_t targetSlot = -1;	  // the slot the menu acts on (value menus)
	uint64_t targetSteamID64 = 0;
	int32_t targetController = -1;
	std::vector<std::string> values; // item info by absolute index (aligned with the added items)
};

enum class PendingKind {
	None,
	AdminItem,	  // a selection of an item of the "players" category -> open the player list
	PlayerItem,	  // a selection inside the player list -> run the action / open a value menu
	ValueItem,	  // a selection inside a value menu -> apply the value
	BackToList,	  // "back" of a value menu -> the player list of the same admin item
	AdminCategory // "back" of the player list -> the admin core's last category
};

struct Pending {
	PendingKind kind = PendingKind::None;
	MenuKind menu = MenuKind::None;
	std::string lastItem;
	std::string value;			  // ValueItem: the selected item's info string
	uint64_t actorSteamID64 = 0;  // the admin that made the selection (slot-reuse guard)
	int32_t actorController = -1; // and the controller generation of that connection
	int32_t targetSlot = -1;
	uint64_t targetSteamID64 = 0;
	int32_t targetController = -1;
};

std::array<OpenMenu, kSlots> g_openMenus;
std::array<Pending, kSlots> g_pending;

struct DestroyRequest {
	s2sdk::MenuId id = 0;
	int32_t slot = -1;
};

std::vector<DestroyRequest> g_destroyQueue;

// Defined with the rest of the menu layer below; forward declared because the menu builder and
// CloseSlotMenu refer to each other.
void OnMenuAction(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t iSlot, int32_t iParam);
bool DisplayMenu(int32_t iSlot, MenuKind kind, const std::string& szTitle,
				 std::vector<MenuItemSpec> items, const std::string& lastItem, int32_t iTargetSlot,
				 uint64_t iTargetSteamID64, int32_t iTargetController);

// "slot:steamid:controllerHandle" - the identity a player list entry carries. The controller
// handle includes s2sdk's serial (generation), so a new connection in the same slot - even with
// the same SteamID64 - is not the entry the admin clicked. CEntityHandle is signed: the handle is
// written as a signed decimal and parsed back through a safe int64 range (INT32_MIN..INT32_MAX);
// negative handles are valid, only 0 and -1 are sentinels.
std::string EncodeTarget(int32_t iSlot, uint64_t iSteamID64, int32_t iController) {
	return std::to_string(iSlot) + ":" + std::to_string(iSteamID64) + ":" +
		   std::to_string(iController);
}

bool DecodeTarget(const std::string& value, int32_t& iSlotOut, uint64_t& iSteamID64Out,
				  int32_t& iControllerOut) {
	const std::size_t iFirst = value.find(':');
	const std::size_t iSecond =
		iFirst == std::string::npos ? std::string::npos : value.find(':', iFirst + 1);
	if (iFirst == std::string::npos || iSecond == std::string::npos) {
		return false;
	}
	try {
		const long long iSlot = std::stoll(value.substr(0, iFirst));
		const unsigned long long iSteamID =
			std::stoull(value.substr(iFirst + 1, iSecond - iFirst - 1));
		// The serial is stored as a signed decimal CEntityHandle: parse it as int64 and reject
		// only values outside the int32 range (and the two sentinels). Negative handles are valid.
		const long long iController = std::stoll(value.substr(iSecond + 1));
		if (iSlot < 0 || iSlot >= kSlots || iController < INT32_MIN ||
			iController > INT32_MAX || iController == 0 || iController == -1) {
			return false;
		}
		iSlotOut = static_cast<int32_t>(iSlot);
		iSteamID64Out = static_cast<uint64_t>(iSteamID);
		iControllerOut = static_cast<int32_t>(iController);
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

// A delayed action / menu selection that named a slot earlier: the slot still holds the same
// connection only when both the SteamID64 and the controller handle (entity serial = connection
// generation) match, and the pawn still exists. A bot has no id, so it is accepted only while the
// slot still holds the same bot. -1 = do not act (PITFALLS C2/B4).
int32_t ResolveTarget(int32_t iSlot, uint64_t iSteamID64, int32_t iController) {
	if (iController == 0 || iController == -1 || !HasClient(iSlot) || !TargetUsable(iSlot)) {
		return -1;
	}
	if (ControllerHandle(iSlot) != iController) {
		return -1; // the slot now holds a different connection
	}
	if (iSteamID64 != 0) {
		if (IsBot(iSlot)) {
			return -1;
		}
		return ClientSteamID64(iSlot) == iSteamID64 ? iSlot : -1;
	}
	return IsBot(iSlot) ? iSlot : -1;
}

// The menu type of the new menu, with exactly the table pisexcompat used for every Pisex menu
// (libs/pisexcompat/src/menu.cpp ResolveS2sdkMenuTypeName/MappedMenuTypeName):
//
//   1. the player's own "!menu" choice from the hudmenu plugin (the original's cs2-menus
//      MenuType, persisted in the permissions cookie "Utils.MenuType") - an optional plugin API
//      dependency, not a wrapper library; "hud"/"wasd" are only used while the matching backend
//      is registered, otherwise "button" (even when the other custom backend is registered);
//      an unknown/foreign answer falls through to the mapped type;
//   2. otherwise the slot's Pisex MenuType. as_funcommands never calls SetMenuType, so this is
//      pisexcompat's default HUD_LAYOUT: "hud" when the backend is registered, otherwise
//      "button" - NOT "centerhtml" (that is Pisex's CENTER type, which this module never uses).
enum class PisexMenuType : int32_t {
	Chat = 0,
	Center = 1,
	CenterWasd = 2,
	HudLayout = 3,
};

bool IsMenuTypeRegisteredSafe(const char* szName) {
	if (__s2sdk_IsMenuTypeRegistered == nullptr || szName == nullptr) {
		return false; // pisexcompat's IsHudTypeRegistered/IsWasdTypeRegistered answer false too
	}
	try {
		return s2sdk::IsMenuTypeRegistered(plg::string(szName));
	} catch (...) {
		return false;
	}
}

// Maps a Pisex MenuType onto an s2sdk type name (used when no player choice applies).
std::string MappedMenuTypeName(PisexMenuType type, bool bHudRegistered, bool bWasdRegistered) {
	switch (type) {
	case PisexMenuType::Chat:
		return "chat";
	case PisexMenuType::Center:
		return "centerhtml";
	// Pisex's CENTER_WASD: the hudmenu "wasd" backend; s2sdk's built-in "button" is the fallback
	// while it is not registered.
	case PisexMenuType::CenterWasd:
		return bWasdRegistered ? "wasd" : "button";
	case PisexMenuType::HudLayout:
		return bHudRegistered ? "hud" : "button"; // without the backend, "button" is the closest
	}
	return "chat";
}

// The exact pisexcompat resolution: the hudmenu answer first, then the slot's Pisex MenuType.
// Only the known builtin names are taken from the player choice; anything else is ignored and
// the mapped type is used.
std::string ResolveMenuTypeName(std::string_view playerTypeChoice, PisexMenuType slotType,
								bool bHudRegistered, bool bWasdRegistered) {
	if (!playerTypeChoice.empty()) {
		if (playerTypeChoice == "chat" || playerTypeChoice == "centerhtml" ||
			playerTypeChoice == "button") {
			return std::string(playerTypeChoice);
		}
		if (playerTypeChoice == "hud") {
			return bHudRegistered ? "hud" : "button";
		}
		if (playerTypeChoice == "wasd") {
			return bWasdRegistered ? "wasd" : "button";
		}
	}
	return MappedMenuTypeName(slotType, bHudRegistered, bWasdRegistered);
}

std::string ChooseMenuType(int32_t iSlot) {
	std::string playerChoice;
	if (__hudmenu_GetClientMenuType != nullptr && HasClient(iSlot)) {
		try {
			playerChoice = ToStd(hudmenu::GetClientMenuType(iSlot));
		} catch (...) {
			playerChoice.clear();
		}
	}
	// No forced per-slot type: as_funcommands never calls SetMenuType/SetDefaultMenuType, so the
	// mapped fallback is always pisexcompat's HUD_LAYOUT default.
	return ResolveMenuTypeName(playerChoice, PisexMenuType::HudLayout,
							   IsMenuTypeRegisteredSafe("hud"), IsMenuTypeRegisteredSafe("wasd"));
}

// MenuId 0 is valid for the manager but also means "no menu" in its client state, so a menu
// created as id 0 could never be shown or tracked (PITFALLS C10). Burn it and create again.
//
// The three menu-handle exports are required *before* anything is created, including the burnt
// id 0: without DestroyMenu/GetClientMenu/CancelClientMenu a menu handle could never be shown,
// tracked or closed safely (the whole menu layer is disabled when one of them is missing).
s2sdk::MenuId CreateNativeMenu(int32_t iSlot, const std::string& title) {
	if (!g_menuBindingsAvailable) {
		return 0;
	}
	const std::string typeName = ChooseMenuType(iSlot);
	s2sdk::MenuId id = s2sdk::CreateMenu(Plg(title), &OnMenuAction, Plg(typeName));
	if (id == 0) {
		s2sdk::DestroyMenu(0);
		id = s2sdk::CreateMenu(Plg(title), &OnMenuAction, Plg(typeName));
	}
	return id;
}

void ClearOpenMenu(int32_t iSlot) {
	g_openMenus[static_cast<std::size_t>(iSlot)] = OpenMenu{};
}

void QueueMenuDestroy(s2sdk::MenuId id, int32_t iSlot) {
	if (id != 0) {
		g_destroyQueue.push_back(DestroyRequest{id, iSlot});
	}
}

// --- safe menu destruction (PITFALLS B2 + A7) -------------------------------------------------
//
// s2sdk's menu manager runs GetClientMenu/CancelClientMenu/DestroyMenu on the game thread under
// one recursive mutex, so a display checked with GetClientMenu() cannot change between two calls
// of this plugin. CancelClientMenu(slot, Disconnect) ends the session without calling the backend
// close; it answers false only when the slot has no display any more. DestroyMenu(id) ends a
// still-active display with reason Destroyed and *does* call the backend close - which, between
// OnMapEnd and the next OnMapStart, reaches the client/entity layer being torn down (B2).
//
// The builtin "chat"/"console" closers are no-ops (menu_types_builtin.cpp). Therefore a menu that
// is still displayed while no map is up is switched to one of those types (verified through
// GetMenuType) before DestroyMenu: the close becomes a no-op and the destroy is safe. If even
// that switch is impossible, the request is parked (retried when a map is back) and reported as
// A7_UNRESOLVED - the plugin never pretends a menu it could not release was cleaned up.

std::vector<s2sdk::MenuId> g_unresolvedMenus;

void ReportUnresolvedMenu(s2sdk::MenuId id, int32_t iSlot, bool bForce) {
	if (!bForce) {
		if (std::find(g_unresolvedMenus.begin(), g_unresolvedMenus.end(), id) !=
			g_unresolvedMenus.end()) {
			return;
		}
		g_unresolvedMenus.push_back(id);
	}
	LogError("A7_UNRESOLVED: menu " + std::to_string(id) + " on slot " + std::to_string(iSlot) +
			 " is still displayed and cannot be switched to a no-op-close type; the handle (and "
			 "this module's handler pointer) could not be released");
}

// Replaces the menu type with a builtin whose close() is a no-op, so a DestroyMenu that still has
// to end a display cannot reach the client layer.
bool MakeMenuCloseNoOp(s2sdk::MenuId id) {
	if (__s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr) {
		return false;
	}
	for (const char* szType : {"chat", "console"}) {
		if (__s2sdk_IsMenuTypeRegistered != nullptr && !IsMenuTypeRegisteredSafe(szType)) {
			continue;
		}
		try {
			if (!s2sdk::SetMenuType(id, plg::string(szType))) {
				continue;
			}
			if (ToStd(s2sdk::GetMenuType(id)) == szType) {
				return true;
			}
		} catch (...) {
		}
	}
	return false;
}

// Ends every display of `id` and destroys the menu. bMapReady == false is the changelevel window,
// where a backend close must not touch a client. Returns true when the handle is gone.
bool DestroyMenuChecked(s2sdk::MenuId id, int32_t iSlot, bool bMapReady) {
	if (__s2sdk_DestroyMenu == nullptr) {
		return false;
	}
	if (id == 0) {
		// Client state 0 is s2sdk's "no menu": id 0 can never be displayed, so a plain destroy is
		// safe (it only frees the burnt handle).
		try {
			s2sdk::DestroyMenu(0);
		} catch (...) {
			return false;
		}
		return true;
	}
	if (__s2sdk_IsValidMenu != nullptr) {
		bool bValid = true;
		try {
			bValid = s2sdk::IsValidMenu(id);
		} catch (...) {
			bValid = true;
		}
		if (!bValid) {
			return true; // already gone
		}
	}
	// Find every slot that still displays the menu (the manager allows one menu on several
	// clients; this plugin shows one per slot, but the cleanup must not depend on its own
	// bookkeeping being right).
	auto collectDisplays = [id]() {
		std::vector<int32_t> slots;
		if (__s2sdk_GetClientMenu == nullptr) {
			return slots;
		}
		for (int32_t slot = 0; slot < kSlots; ++slot) {
			try {
				if (s2sdk::GetClientMenu(slot) == id) {
					slots.push_back(slot);
				}
			} catch (...) {
			}
		}
		return slots;
	};
	std::vector<int32_t> displayed = collectDisplays();
	if (!displayed.empty()) {
		if (__s2sdk_CancelClientMenu != nullptr) {
			for (const int32_t slot : displayed) {
				try {
					(void)s2sdk::CancelClientMenu(slot, s2sdk::MenuCancelReason::Disconnect);
				} catch (...) {
				}
			}
		}
		// Re-query whether the cancel answered true or false: only the manager's own state
		// decides, and a display that survived must not be destroyed with a backend close in a
		// map-down window.
		displayed = collectDisplays();
		if (!displayed.empty() && !bMapReady) {
			if (!MakeMenuCloseNoOp(id)) {
				ReportUnresolvedMenu(id, displayed.front(), false);
				return false; // parked: a direct DestroyMenu would reach the torn-down client
			}
		}
	}
	try {
		s2sdk::DestroyMenu(id);
	} catch (...) {
		ReportUnresolvedMenu(id, iSlot, false);
		return false;
	}
	if (__s2sdk_IsValidMenu != nullptr) {
		bool bStillValid = false;
		try {
			bStillValid = s2sdk::IsValidMenu(id);
		} catch (...) {
			bStillValid = false;
		}
		if (bStillValid) {
			ReportUnresolvedMenu(id, iSlot, false);
			return false;
		}
	}
	g_unresolvedMenus.erase(std::remove(g_unresolvedMenus.begin(), g_unresolvedMenus.end(), id),
							g_unresolvedMenus.end());
	return true;
}

// Ends a still-displayed menu of the slot right now (the caller is outside every s2sdk handler:
// OnPluginUpdate / ApplyPending / OnPluginEnd). Cancel is used so the backend closes its UI; the
// manager's own End then reaches this plugin's handler, which only queues the destroy.
void CloseSlotMenu(int32_t iSlot) {
	OpenMenu& menu = g_openMenus[static_cast<std::size_t>(iSlot)];
	if (menu.id == 0) {
		return;
	}
	const s2sdk::MenuId id = menu.id;
	if (DestroyMenuChecked(id, iSlot, g_mapReady)) {
		ClearOpenMenu(iSlot);
	}
	// If the handle had to be parked (only possible outside a map), the state is kept so the next
	// cleanup pass - and OnPluginEnd - can try again.
}

// Destroys every menu whose session has ended. A menu the manager still shows for its slot
// (which should not happen) is cancelled first - with Disconnect while no map is up, so the
// backend never touches a torn-down client (PITFALLS B2).
void DrainDestroyQueue() {
	if (g_destroyQueue.empty()) {
		return;
	}
	std::vector<DestroyRequest> queue = std::move(g_destroyQueue);
	g_destroyQueue.clear();
	for (const DestroyRequest& request : queue) {
		if (!DestroyMenuChecked(request.id, request.slot, g_mapReady)) {
			// Only a map-down display that could not be switched to a no-op backend gets here:
			// keep the request for the next update / the next map instead of a blind DestroyMenu.
			g_destroyQueue.push_back(request);
			continue;
		}
		if (request.slot >= 0 && request.slot < kSlots &&
			g_openMenus[static_cast<std::size_t>(request.slot)].id == request.id) {
			ClearOpenMenu(request.slot);
		}
	}
}

// OnMapEnd: drop the bookkeeping only. No client query is made from here; the queued menus are
// destroyed by OnPluginUpdate, which runs outside the map teardown (PITFALLS B2).
void DropMenusForMapEnd() {
	for (int32_t i = 0; i < kSlots; ++i) {
		const s2sdk::MenuId id = g_openMenus[static_cast<std::size_t>(i)].id;
		if (id != 0) {
			// End the display session through the menu manager only: Disconnect skips the backend
			// close, so neither the HUD backend nor a client is touched while the map is being
			// torn down (PITFALLS B2). The handle itself is destroyed on the next update; if the
			// cancel is refused and the display survives, DestroyMenuChecked() re-queries it and
			// switches the menu to a no-op-close type before the destroy (A7).
			try {
				if (__s2sdk_GetClientMenu != nullptr && __s2sdk_CancelClientMenu != nullptr &&
					s2sdk::GetClientMenu(i) == id) {
					(void)s2sdk::CancelClientMenu(i, s2sdk::MenuCancelReason::Disconnect);
				}
			} catch (...) {
			}
			QueueMenuDestroy(id, i);
			ClearOpenMenu(i);
		}
		g_pending[static_cast<std::size_t>(i)] = Pending{};
	}
}

// --- building and showing the menus -----------------------------------------------------------
bool HasPermissionForItem(int32_t iSlot, const ItemDef* pItem) {
	// Same canonical, current-SteamID gate as the commands: a selection that outlived its menu
	// must not run on a stale record of the slot (API.md §4.1).
	return pItem != nullptr && HasReadyPermission(iSlot, pItem->szPermission);
}

// The Pisex "DisplayPlayerMenu" of the ported module: five items per page, exit and back
// buttons, no auto-close on selection. Runs from OnPluginUpdate only.
bool DisplayMenu(int32_t iSlot, MenuKind kind, const std::string& szTitle,
				 std::vector<MenuItemSpec> items, const std::string& lastItem, int32_t iTargetSlot,
				 uint64_t iTargetSteamID64, int32_t iTargetController) {
	if (!g_menuBindingsAvailable || !g_mapReady || !HasClient(iSlot)) {
		return false;
	}
	const uint64_t ownerSteamID64 = ClientSteamID64(iSlot);
	if (ownerSteamID64 == 0) {
		// A menu outlives the frame of its selection; without an identity a reused slot could
		// inherit the next player's selection.
		LogInfo("not showing a menu on slot " + std::to_string(iSlot) +
				": the client has no SteamID64 yet");
		return false;
	}
	const int32_t ownerController = ControllerHandle(iSlot);
	if (ownerController == -1) {
		LogInfo("not showing a menu on slot " + std::to_string(iSlot) +
				": the controller is not valid");
		return false;
	}
	CloseSlotMenu(iSlot);
	s2sdk::MenuId id = 0;
	try {
		id = CreateNativeMenu(iSlot, szTitle);
	} catch (...) {
		id = 0;
	}
	if (id == 0) {
		LogError("s2sdk refused to create a menu");
		return false;
	}
	OpenMenu state;
	state.kind = kind;
	state.lastItem = lastItem;
	state.ownerSteamID64 = ownerSteamID64;
	state.ownerController = ownerController;
	state.targetSlot = iTargetSlot;
	state.targetSteamID64 = iTargetSteamID64;
	state.targetController = iTargetController;
	bool bOk = true;
	try {
		(void)s2sdk::SetMenuPagination(id, kItemsPerPage);
		(void)s2sdk::SetMenuExitButton(id, true);
		(void)s2sdk::SetMenuExitBackButton(id, true);
		(void)s2sdk::SetMenuCloseOnSelect(id, false);
		for (const MenuItemSpec& item : items) {
			const int32_t iIndex =
				s2sdk::AddMenuItem(id, Plg(item.info), Plg(item.display), item.style);
			if (iIndex < 0) {
				bOk = false;
				break;
			}
			state.values.push_back(item.info);
		}
	} catch (...) {
		bOk = false;
	}
	if (!bOk) {
		// A half-built menu must not be displayed (its index/values alignment is gone).
		(void)DestroyMenuChecked(id, iSlot, g_mapReady);
		LogError("s2sdk refused to add a menu item");
		return false;
	}
	state.id = id;
	g_openMenus[static_cast<std::size_t>(iSlot)] = std::move(state);
	bool bDisplayed = false;
	try {
		bDisplayed = s2sdk::DisplayMenu(id, iSlot, kMenuTimeoutSeconds);
	} catch (...) {
		bDisplayed = false;
	}
	if (!bDisplayed) {
		ClearOpenMenu(iSlot);
		(void)DestroyMenuChecked(id, iSlot, g_mapReady);
		LogError("s2sdk could not display a menu on slot " + std::to_string(iSlot));
		return false;
	}
	return true;
}

void OpenAdminCategoryMenu(int32_t iSlot) {
	if (g_pAdminApi != nullptr) {
		g_pAdminApi->ShowAdminLastCategoryMenu(iSlot);
	}
}

// The player picker of one admin item (the original's ShowPlayerList). Only slots whose name and
// identity can really be read are offered: the original required the controller's pawn and a live
// player, and an entry without a SteamID64 could only point at the next occupant of the slot.
bool ShowPlayerList(int32_t iSlot, const std::string& identity) {
	if (identity.empty() || !g_mapReady || !HasClient(iSlot)) {
		return false;
	}
	std::vector<MenuItemSpec> items;
	items.reserve(kSlots);
	for (int32_t i = 0; i < kSlots; ++i) {
		// CCSPlayerController::FromSlot() + GetPlayerPawn() + IsAlive() of the original.
		if (!TargetUsable(i) || !ClientAlive(i)) {
			continue;
		}
		const bool bBot = IsBot(i);
		uint64_t iSteamID64 = 0;
		if (!bBot) {
			iSteamID64 = ClientSteamID64(i);
			if (iSteamID64 == 0) {
				continue; // no identity: the selection could hit the next occupant of the slot
			}
		}
		const std::string name = ClientName(i);
		std::string label;
		if (identity == "freeze") {
			label = as_funcommands::Format("%s [%s]", name.c_str(), IsFrozen(i) ? "ON" : "OFF");
		} else if (identity == "gravity") {
			label = as_funcommands::Format("%s [%.1fx]", name.c_str(), GetClientGravityValue(i));
		} else if (identity == "hp") {
			label = as_funcommands::Format("%s [%i]", name.c_str(), GetClientHealthValue(i));
		} else if (identity == "armor") {
			label = as_funcommands::Format("%s [%i]", name.c_str(), GetClientArmorValue(i));
		} else if (identity == "speed") {
			label = as_funcommands::Format("%s [%.1fx]", name.c_str(), GetClientSpeedValue(i));
		} else if (identity == "scale") {
			label = as_funcommands::Format("%s [%.1fx]", name.c_str(), GetClientScaleValue(i));
		} else if (identity == "god") {
			label = as_funcommands::Format("%s [%s]", name.c_str(),
										   GetPawnTakesDamage(i) ? "OFF" : "ON");
		} else if (identity == "blind") {
			label = as_funcommands::Format("%s [%s]", name.c_str(), IsBlinded(i) ? "ON" : "OFF");
		} else if (identity == "bury" || identity == "unbury") {
			// bury/unbury show the plain name, like the original.
			label = name;
		} else {
			return false; // unknown identity: the original added no item at all
		}
		MenuItemSpec item;
		item.info = EncodeTarget(i, iSteamID64, ControllerHandle(i));
		item.display = std::move(label);
		items.push_back(std::move(item));
	}
	// No placeholder item: the original showed an empty page with the back/exit buttons only.
	return DisplayMenu(iSlot, MenuKind::Player, T("SelectPlayer"), std::move(items), identity, -1,
					   0, -1);
}

// The value menus of hp/armor/speed/scale/gravity (the original's ShowBaseStatMenu/ShowSpeedMenu/
// ShowGravityMenu). The target is re-resolved from its identity on every click, so a menu that
// outlives the target's connection can never act on the next occupant.
bool ShowValueMenu(MenuKind kind, int32_t iSlot, int32_t iTarget, uint64_t iSteamID64,
				   int32_t iController, const std::string& lastItem) {
	if (!TargetUsable(iTarget)) {
		return false;
	}
	const std::string name = ClientName(iTarget);
	std::string title;
	std::vector<MenuItemSpec> items;
	switch (kind) {
	case MenuKind::Health:
	case MenuKind::Armor: {
		const int32_t iValue = kind == MenuKind::Health ? GetClientHealthValue(iTarget)
														: GetClientArmorValue(iTarget);
		title = as_funcommands::Format("%s [%i]", name.c_str(), iValue);
		for (const char* szValue : {"1", "5", "10", "20", "50", "100", "200", "500", "1000"}) {
			items.push_back(MenuItemSpec{szValue, szValue, s2sdk::MenuItemStyle::Default});
		}
		break;
	}
	case MenuKind::Speed:
	case MenuKind::Scale: {
		struct Option {
			const char* szValue;
			const char* szLabel;
		};
		const float flValue = kind == MenuKind::Speed ? GetClientSpeedValue(iTarget)
													  : GetClientScaleValue(iTarget);
		title = as_funcommands::Format("%s [%.1fx]", name.c_str(), flValue);
		// value, label - same pairs as the original.
		for (const Option& option :
			 std::array<Option, 10>{{{"0.1", "0.1x"}, {"0.2", "0.2x"}, {"0.5", "0.5x"},
									 {"1.0", "1x"},	 {"1.1", "1.1x"}, {"1.2", "1.2x"},
									 {"1.5", "1.5x"}, {"2.0", "2x"},   {"5.0", "5x"},
									 {"10.0", "10x"}}}) {
			items.push_back(
				MenuItemSpec{option.szValue, option.szLabel, s2sdk::MenuItemStyle::Default});
		}
		break;
	}
	case MenuKind::Gravity: {
		struct Option {
			const char* szValue;
			const char* szLabel;
		};
		title = as_funcommands::Format("%s [%.1fx]", name.c_str(), GetClientGravityValue(iTarget));
		for (const Option& option :
			 std::array<Option, 12>{{{"0.1", "0.1x"}, {"0.2", "0.2x"}, {"0.5", "0.5x"},
									 {"0.7", "0.7x"}, {"0.8", "0.8x"}, {"1.0", "1x"},
									 {"1.1", "1.1x"}, {"1.2", "1.2x"}, {"1.5", "1.5x"},
									 {"2.0", "2x"},   {"5.0", "5x"},   {"10.0", "10x"}}}) {
			items.push_back(
				MenuItemSpec{option.szValue, option.szLabel, s2sdk::MenuItemStyle::Default});
		}
		break;
	}
	case MenuKind::Player:
	case MenuKind::None:
		return false;
	}
	return DisplayMenu(iSlot, kind, title, std::move(items), lastItem, iTarget, iSteamID64,
					   iController);
}

// Re-opens the player list of one admin item for its admin.
bool ReopenPlayerList(int32_t iSlot, const std::string& identity) {
	return ShowPlayerList(iSlot, identity);
}

// The action of one player-picker item. Runs outside the menu handler.
void ExecutePlayerItem(int32_t iSlot, const ItemDef* pItem, int32_t iTarget, uint64_t iSteamID64,
					   int32_t iController) {
	const std::string identity = pItem->szIdentity;
	if (identity == "freeze") {
		SetFreezy(iSlot, iTarget, !IsFrozen(iTarget), true);
		(void)ReopenPlayerList(iSlot, "freeze");
	} else if (identity == "gravity") {
		if (!ShowValueMenu(MenuKind::Gravity, iSlot, iTarget, iSteamID64, iController, "gravity")) {
			(void)ReopenPlayerList(iSlot, "gravity");
		}
	} else if (identity == "hp") {
		if (!ShowValueMenu(MenuKind::Health, iSlot, iTarget, iSteamID64, iController, "hp")) {
			(void)ReopenPlayerList(iSlot, "hp");
		}
	} else if (identity == "armor") {
		if (!ShowValueMenu(MenuKind::Armor, iSlot, iTarget, iSteamID64, iController, "armor")) {
			(void)ReopenPlayerList(iSlot, "armor");
		}
	} else if (identity == "speed") {
		if (!ShowValueMenu(MenuKind::Speed, iSlot, iTarget, iSteamID64, iController, "speed")) {
			(void)ReopenPlayerList(iSlot, "speed");
		}
	} else if (identity == "scale") {
		if (!ShowValueMenu(MenuKind::Scale, iSlot, iTarget, iSteamID64, iController, "scale")) {
			(void)ReopenPlayerList(iSlot, "scale");
		}
	} else if (identity == "god") {
		SetGod(iSlot, iTarget, !GetPawnTakesDamage(iTarget), true);
		(void)ReopenPlayerList(iSlot, "god");
	} else if (identity == "bury") {
		BuryPlayer(iSlot, iTarget, true);
		(void)ReopenPlayerList(iSlot, "bury");
	} else if (identity == "unbury") {
		UnburyPlayer(iSlot, iTarget, true);
		(void)ReopenPlayerList(iSlot, "unbury");
	} else if (identity == "blind") {
		BlindPlayer(iSlot, iTarget, !IsBlinded(iTarget), true);
		(void)ReopenPlayerList(iSlot, "blind");
	}
}

// The value of a value-menu item, applied to the (re-resolved) target.
void ApplyValueItem(int32_t iSlot, MenuKind kind, const std::string& value, int32_t iTarget) {
	switch (kind) {
	case MenuKind::Health:
		SetHP(iSlot, iTarget, std::atoi(value.c_str()), true);
		break;
	case MenuKind::Armor:
		SetArmor(iSlot, iTarget, std::atoi(value.c_str()), true);
		break;
	case MenuKind::Speed:
		SetSpeed(iSlot, iTarget, static_cast<float>(std::atof(value.c_str())), true);
		break;
	case MenuKind::Scale:
		SetScale(iSlot, iTarget, static_cast<float>(std::atof(value.c_str())), true);
		break;
	case MenuKind::Gravity:
		SetGravity(iSlot, iTarget, static_cast<float>(std::atof(value.c_str())), true);
		break;
	case MenuKind::Player:
	case MenuKind::None:
		break;
	}
}

void HandleMenuSelect(int32_t iSlot, const OpenMenu& menu, int32_t iParam) {
	if (iParam < 0 || static_cast<std::size_t>(iParam) >= menu.values.size()) {
		return;
	}
	if (menu.ownerSteamID64 == 0 || menu.ownerController == 0 || menu.ownerController == -1 ||
		ClientSteamID64(iSlot) != menu.ownerSteamID64 ||
		ControllerHandle(iSlot) != menu.ownerController) {
		return; // the slot's owner (or its connection generation) changed: not their selection
	}
	// s2sdk maps digits 1..7 to currentOffset + digit - 1 even when the page shows five items,
	// so a digit that belongs to the next (not displayed) page must be ignored, exactly like
	// pisexcompat's guard did.
	int32_t iOffset = iParam - (iParam % kItemsPerPage);
	if (__s2sdk_GetClientMenuOffset != nullptr) {
		try {
			iOffset = s2sdk::GetClientMenuOffset(iSlot);
		} catch (...) {
		}
	}
	if (iParam < iOffset || iParam >= iOffset + kItemsPerPage) {
		return;
	}
	const std::string& value = menu.values[static_cast<std::size_t>(iParam)];
	Pending pending;
	pending.lastItem = menu.lastItem;
	pending.menu = menu.kind;
	pending.actorSteamID64 = menu.ownerSteamID64;
	pending.actorController = menu.ownerController;
	pending.targetSlot = menu.targetSlot;
	pending.targetSteamID64 = menu.targetSteamID64;
	pending.targetController = menu.targetController;
	if (menu.kind == MenuKind::Player) {
		int32_t iTarget = -1;
		uint64_t iSteamID64 = 0;
		int32_t iController = -1;
		if (!DecodeTarget(value, iTarget, iSteamID64, iController)) {
			return;
		}
		pending.kind = PendingKind::PlayerItem;
		pending.targetSlot = iTarget;
		pending.targetSteamID64 = iSteamID64;
		pending.targetController = iController;
	} else if (menu.kind != MenuKind::None) {
		pending.kind = PendingKind::ValueItem;
		pending.value = value;
	} else {
		return;
	}
	g_pending[static_cast<std::size_t>(iSlot)] = std::move(pending);
}

// The s2sdk menu handler. It runs under the menu manager's mutex and must not perform engine
// actions; it only records state and a Pending action for OnPluginUpdate (PITFALLS C10).
void OnMenuAction(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t iSlot, int32_t iParam) {
	try {
		if (iSlot < 0 || iSlot >= kSlots) {
			return;
		}
		OpenMenu& menu = g_openMenus[static_cast<std::size_t>(iSlot)];
		if (menu.id == 0 || menu.id != id) {
			return; // stale (the plugin already replaced or destroyed this menu)
		}
		switch (action) {
		case s2sdk::MenuAction::Start:
			return;
		case s2sdk::MenuAction::Select:
			HandleMenuSelect(iSlot, menu, iParam);
			return;
		case s2sdk::MenuAction::Cancel: {
			const auto reason = static_cast<s2sdk::MenuCancelReason>(iParam);
			const MenuKind kind = menu.kind;
			const std::string lastItem = menu.lastItem;
			const uint64_t ownerSteamID64 = menu.ownerSteamID64;
			const int32_t ownerController = menu.ownerController;
			QueueMenuDestroy(id, iSlot);
			ClearOpenMenu(iSlot);
			if (reason == s2sdk::MenuCancelReason::ExitBack) {
				Pending pending;
				pending.actorSteamID64 = ownerSteamID64;
				pending.actorController = ownerController;
				if (kind == MenuKind::Player) {
					pending.kind = PendingKind::AdminCategory;
				} else if (kind != MenuKind::None) {
					// Back from a value menu: the player list of the same admin item.
					pending.kind = PendingKind::BackToList;
					pending.lastItem = lastItem;
				} else {
					return;
				}
				g_pending[static_cast<std::size_t>(iSlot)] = std::move(pending);
			}
			return;
		}
		case s2sdk::MenuAction::End:
			QueueMenuDestroy(id, iSlot);
			ClearOpenMenu(iSlot);
			return;
		}
	} catch (const std::exception& ex) {
		LogError(std::string("menu handler failed: ") + ex.what());
	} catch (...) {
		LogError("menu handler failed with an unknown exception");
	}
}

// Applies one deferred selection, outside every s2sdk menu handler.
void ApplyPending(int32_t iSlot) {
	const Pending pending = g_pending[static_cast<std::size_t>(iSlot)];
	if (pending.kind == PendingKind::None) {
		return;
	}
	g_pending[static_cast<std::size_t>(iSlot)] = Pending{};
	if (!g_mapReady || !HasClient(iSlot)) {
		return; // the admin is gone; the map-end path already ended the menu session
	}
	if (pending.actorSteamID64 == 0 || pending.actorController == 0 ||
		pending.actorController == -1 || ClientSteamID64(iSlot) != pending.actorSteamID64 ||
		ControllerHandle(iSlot) != pending.actorController) {
		// The slot was reused (or reconnected) after the selection: never run an admin action on
		// behalf of the new connection. The menu this plugin still tracks belongs to the old
		// occupant, so it is closed rather than left on the new player's screen.
		CloseSlotMenu(iSlot);
		return;
	}
	CloseSlotMenu(iSlot);
	switch (pending.kind) {
	case PendingKind::None:
		return;
	case PendingKind::AdminCategory:
		OpenAdminCategoryMenu(iSlot);
		return;
	case PendingKind::AdminItem:
	case PendingKind::BackToList: {
		const ItemDef* pItem = FindItem(pending.lastItem);
		if (!HasPermissionForItem(iSlot, pItem)) {
			LogInfo("the permission for '" + pending.lastItem + "' is not ready/held for slot " +
					std::to_string(iSlot));
			return;
		}
		(void)ShowPlayerList(iSlot, pItem->szIdentity);
		return;
	}
	case PendingKind::PlayerItem: {
		const ItemDef* pItem = FindItem(pending.lastItem);
		if (!HasPermissionForItem(iSlot, pItem)) {
			LogInfo("the permission for '" + pending.lastItem + "' is not ready/held for slot " +
					std::to_string(iSlot));
			return;
		}
		const int32_t iTarget = ResolveTarget(pending.targetSlot, pending.targetSteamID64,
											  pending.targetController);
		if (iTarget < 0) {
			// The player left, reconnected or the slot was reused: never act on the new
			// connection (PITFALLS C2/B4). A fresh list shows what is possible now.
			(void)ReopenPlayerList(iSlot, pItem->szIdentity);
			return;
		}
		ExecutePlayerItem(iSlot, pItem, iTarget, pending.targetSteamID64, pending.targetController);
		return;
	}
	case PendingKind::ValueItem: {
		const ItemDef* pItem = FindItem(IdentityForKind(pending.menu));
		if (!HasPermissionForItem(iSlot, pItem)) {
			LogInfo("the permission for '" + std::string(IdentityForKind(pending.menu)) +
					"' is not ready/held for slot " + std::to_string(iSlot));
			return;
		}
		const int32_t iTarget = ResolveTarget(pending.targetSlot, pending.targetSteamID64,
											  pending.targetController);
		if (iTarget < 0) {
			(void)ReopenPlayerList(iSlot, IdentityForKind(pending.menu));
			return;
		}
		ApplyValueItem(iSlot, pending.menu, pending.value, iTarget);
		if (!ShowValueMenu(pending.menu, iSlot, iTarget, pending.targetSteamID64,
						   pending.targetController, IdentityForKind(pending.menu))) {
			(void)ReopenPlayerList(iSlot, IdentityForKind(pending.menu));
		}
		return;
	}
	}
}

// Called by the admin core when an item of the "players" category is selected. It runs inside
// the core's own s2sdk menu handler, so it only queues the next menu for OnPluginUpdate.
void OnAdminItemSelect(int32_t iSlot, const char* szCategory, const char* szIdentity,
					   const char* /*szItem*/) {
	try {
		if (iSlot < 0 || iSlot >= kSlots || szCategory == nullptr || szIdentity == nullptr) {
			return;
		}
		if (std::string_view(szCategory) != "players") {
			return;
		}
		const ItemDef* pItem = FindItem(szIdentity);
		if (pItem == nullptr) {
			return;
		}
		const uint64_t actorSteamID64 = ClientSteamID64(iSlot);
		const int32_t actorController = ControllerHandle(iSlot);
		if (actorSteamID64 == 0 || actorController == -1) {
			return; // no identity: a reused slot could otherwise inherit this request
		}
		Pending pending;
		pending.kind = PendingKind::AdminItem;
		pending.lastItem = pItem->szIdentity;
		pending.actorSteamID64 = actorSteamID64;
		pending.actorController = actorController;
		g_pending[static_cast<std::size_t>(iSlot)] = std::move(pending);
	} catch (const std::exception& ex) {
		LogError(std::string("admin item callback failed: ") + ex.what());
	} catch (...) {
		LogError("admin item callback failed with an unknown exception");
	}
}

// ---------------------------------------------------------------------------------------------
// PITFALLS C9: the delayed unfreeze/unblind of the two timed commands.
//
// No s2sdk timer: a deadline accumulator runs in OnPluginUpdate (a plain host callback, no timer
// mutex), at most one pass per update. Every entry re-validates the target's identity and its own
// admin's identity when it fires; a map change drops the entries (a client query must not run in
// the map-change window - PITFALLS B4).
// ---------------------------------------------------------------------------------------------
enum class DelayedKind { Unfreeze, Unblind };

struct DelayedAction {
	double flRemaining = 0.0;
	DelayedKind kind = DelayedKind::Unfreeze;
	int32_t iActorSlot = -1;	  // -1 = the server console
	uint64_t iActorSteamID64 = 0; // 0 for the console
	int32_t iActorController = -1;
	int32_t iTargetSlot = -1;
	uint64_t iTargetSteamID64 = 0;
	int32_t iTargetController = -1;
};

std::vector<DelayedAction> g_delayedActions;

void AddDelayedAction(DelayedKind kind, int32_t iActor, int32_t iTarget, double flSeconds) {
	if (flSeconds <= 0.0) {
		return;
	}
	DelayedAction action;
	action.flRemaining = flSeconds;
	action.kind = kind;
	if (iActor == -1) {
		action.iActorSlot = -1;
		action.iActorSteamID64 = 0;
		action.iActorController = -1;
	} else {
		action.iActorSlot = iActor;
		action.iActorSteamID64 = ClientSteamID64(iActor);
		action.iActorController = ControllerHandle(iActor);
	}
	action.iTargetSlot = iTarget;
	action.iTargetSteamID64 = ClientSteamID64(iTarget);
	action.iTargetController = ControllerHandle(iTarget);
	g_delayedActions.push_back(action);
}

// The permission a delayed action runs under (the command that armed it).
const char* PermissionForDelayedKind(DelayedKind kind) {
	return kind == DelayedKind::Unfreeze ? "@admin/freeze" : "@admin/blind";
}

// True while the delayed action may still be *announced* as the acting admin's action: the slot
// must hold the very same connection (SteamID64 and controller-handle serial) and the admin must
// still hold the permission the command ran under, with the admin data ready (API.md §4.1).
//
// False does not cancel the effect on the target - that is re-validated separately in
// ApplyDelayedAction - it only suppresses the chat broadcast and the admin-core SendAction: an
// action whose admin left, reconnected, had the permission revoked or whose admin data is no
// longer ready must never be reported in somebody else's name. The server console (-1) cannot be
// reused and bypasses the permission exactly like the command does.
bool ActorStillCurrent(const DelayedAction& action) {
	if (action.iActorSlot == -1) {
		return true; // the server console cannot be reused
	}
	if (action.iActorSlot < 0 || action.iActorSlot >= kSlots) {
		return false;
	}
	// A connection whose identity could not be proven when the deadline was armed (a bot or an
	// unauthenticated client has no SteamID64; a missing binding leaves the controller handle
	// invalid) is never announced on later. 0 and -1 are the only invalid CEntityHandle values.
	if (action.iActorSteamID64 == 0 || action.iActorController == 0 ||
		action.iActorController == -1) {
		return false;
	}
	if (!HasClient(action.iActorSlot) ||
		ControllerHandle(action.iActorSlot) != action.iActorController ||
		ClientSteamID64(action.iActorSlot) != action.iActorSteamID64) {
		return false; // the slot now holds a different connection
	}
	// The permission must still be held and ready when the deadline fires: a revoked permission or
	// a pending/re-failed admin SELECT must not produce an announcement in the admin's name.
	return HasReadyPermission(action.iActorSlot, PermissionForDelayedKind(action.kind));
}

void ApplyDelayedAction(const DelayedAction& action) {
	const int32_t iTarget = ResolveTarget(action.iTargetSlot, action.iTargetSteamID64,
										  action.iTargetController);
	if (iTarget < 0) {
		return; // the target left / the slot holds somebody else now: never touch the new player
	}
	// The acting admin is not part of the effect on the target, only of its announcement: a slot
	// that now holds another player, a revoked permission or admin data that is no longer ready
	// must not broadcast/act (or report to the admin core) in the admin's name.
	const bool bNotify = ActorStillCurrent(action);
	if (action.kind == DelayedKind::Unfreeze) {
		SetFreezy(action.iActorSlot, iTarget, false, bNotify);
	} else {
		BlindPlayer(action.iActorSlot, iTarget, false, bNotify);
	}
}

void UpdateDelayedActions(double flSeconds) {
	if (g_delayedActions.empty()) {
		return;
	}
	if (!g_mapReady) {
		g_delayedActions.clear();
		return;
	}
	const double flDelta = flSeconds > 0.0 ? flSeconds : 0.0;
	for (std::size_t i = 0; i < g_delayedActions.size();) {
		g_delayedActions[i].flRemaining -= flDelta;
		if (g_delayedActions[i].flRemaining > 0.0) {
			++i;
			continue;
		}
		const DelayedAction action = g_delayedActions[i];
		g_delayedActions.erase(g_delayedActions.begin() + static_cast<std::ptrdiff_t>(i));
		ApplyDelayedAction(action);
	}
}

// ---------------------------------------------------------------------------------------------
// The original's FindUser(): a case-insensitive substring of the name, the full steam id or the
// slot number - tried for every slot with a controller and a non-zero SteamID64. Bots and
// unauthenticated clients have no id and stay untargetable by name, exactly like the original
// (whose GetSteamID64 answered 0 for them).
// ---------------------------------------------------------------------------------------------
std::string Arg(const plg::vector<plg::string>& args, std::size_t iIndex) {
	return iIndex < args.size() ? ToStd(args[iIndex]) : std::string();
}

bool ContainsOnlyDigits(std::string_view text) {
	return !text.empty() && text.find_first_not_of("0123456789") == std::string_view::npos;
}

// strcasestr() of the original.
bool NameContains(std::string_view haystack, std::string_view needle) {
	if (needle.empty() || needle.size() > haystack.size()) {
		return false;
	}
	for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
		std::size_t j = 0;
		while (j < needle.size() &&
			   std::tolower(static_cast<unsigned char>(haystack[i + j])) ==
				   std::tolower(static_cast<unsigned char>(needle[j]))) {
			++j;
		}
		if (j == needle.size()) {
			return true;
		}
	}
	return false;
}

int32_t FindUser(const std::string& identity) {
	if (identity.empty()) {
		return -1;
	}
	const bool bDigits = ContainsOnlyDigits(identity);
	unsigned long long iValue = 0;
	if (bDigits) {
		try {
			iValue = std::stoull(identity);
		} catch (const std::exception&) {
			return -1;
		}
	}
	for (int32_t i = 0; i < kSlots; ++i) {
		// CCSPlayerController::FromSlot() of the original.
		if (!ControllerValid(i)) {
			continue;
		}
		// NOTE: the original stored m_steamID in a 32-bit uint, so its SteamID64 comparison could
		// never match; here the full SteamID64 is used (the value users actually paste).
		const uint64_t iSteamID64 = ClientSteamID64(i);
		if (iSteamID64 == 0) {
			continue;
		}
		if (NameContains(ClientName(i), identity)) {
			return i;
		}
		if (bDigits && (static_cast<unsigned long long>(iSteamID64) == iValue ||
						iValue == static_cast<unsigned long long>(i))) {
			return i;
		}
	}
	return -1;
}

// ---------------------------------------------------------------------------------------------
// The console commands (the original's CON_COMMAND_F bodies).
// ---------------------------------------------------------------------------------------------
struct CommandDef;

using CommandHandler = void (*)(int32_t iSlot, const plg::vector<plg::string>& args);

struct CommandDef {
	const char* szName;
	const char* szPermission;
	const char* szUsageKey;
	int32_t iMinArgs; // including the command name, like CCommand::ArgC()
	CommandHandler handler;
	bool registered = false;
	bool created = false;
};

void HandleGravityCommand(int32_t iSlot, const plg::vector<plg::string>& args);
void HandleHPCommand(int32_t iSlot, const plg::vector<plg::string>& args);
void HandleArmorCommand(int32_t iSlot, const plg::vector<plg::string>& args);
void HandleScaleCommand(int32_t iSlot, const plg::vector<plg::string>& args);
void HandleSpeedCommand(int32_t iSlot, const plg::vector<plg::string>& args);
void HandleGodCommand(int32_t iSlot, const plg::vector<plg::string>& args);
void HandleFreezyCommand(int32_t iSlot, const plg::vector<plg::string>& args);
void HandleUnfreezyCommand(int32_t iSlot, const plg::vector<plg::string>& args);
void HandleBlindCommand(int32_t iSlot, const plg::vector<plg::string>& args);
void HandleUnblindCommand(int32_t iSlot, const plg::vector<plg::string>& args);
void HandleBuryCommand(int32_t iSlot, const plg::vector<plg::string>& args);
void HandleUnburyCommand(int32_t iSlot, const plg::vector<plg::string>& args);

std::array<CommandDef, 12> g_commands = {{
	{"mm_gravity", "@admin/gravity", "Usage_Gravity", 3, &HandleGravityCommand},
	{"mm_hp", "@admin/hp", "Usage_HP", 3, &HandleHPCommand},
	{"mm_armor", "@admin/armor", "Usage_Armor", 3, &HandleArmorCommand},
	{"mm_scale", "@admin/scale", "Usage_Scale", 3, &HandleScaleCommand},
	{"mm_speed", "@admin/speed", "Usage_Speed", 3, &HandleSpeedCommand},
	{"mm_god", "@admin/god", "Usage_God", 2, &HandleGodCommand},
	{"mm_freezy", "@admin/freeze", "Usage_Freeze", 2, &HandleFreezyCommand},
	{"mm_unfreezy", "@admin/freeze", "Usage_Unfreeze", 2, &HandleUnfreezyCommand},
	{"mm_blind", "@admin/blind", "Usage_Blind", 2, &HandleBlindCommand},
	{"mm_unblind", "@admin/blind", "Usage_Unblind", 2, &HandleUnblindCommand},
	{"mm_bury", "@admin/bury", "Usage_Bury", 2, &HandleBuryCommand},
	{"mm_unbury", "@admin/bury", "Usage_Unbury", 2, &HandleUnburyCommand},
}};

s2sdk::ResultType RunCommand(const CommandDef& def, int32_t iSlot, s2sdk::ConCommandContext eContext,
							 const plg::vector<plg::string>& args) {
	// The ported commands were console commands: a chat "!mm_god" reaches the same callback with
	// ConCommandContext::Chat and is left to s2sdk's normal chat path (Continue), exactly like
	// pisexcompat's chat-name filter did (PITFALLS C11).
	if (eContext == s2sdk::ConCommandContext::Chat) {
		return s2sdk::ResultType::Continue;
	}
	try {
		// Only slot -1 is the server console; -2 and out-of-range callers fail closed with no
		// output at all (never a permission bypass, never the console).
		if (iSlot < -1 || iSlot >= kSlots) {
			return s2sdk::ResultType::Handled;
		}
		if (iSlot != -1 && !HasReadyPermission(iSlot, def.szPermission)) {
			return s2sdk::ResultType::Handled; // the original returned without a word
		}
		if (static_cast<int32_t>(args.size()) < def.iMinArgs) {
			PrintUsage(iSlot, def.szUsageKey, def.szName);
			return s2sdk::ResultType::Handled;
		}
		if (def.handler != nullptr) {
			def.handler(iSlot, args);
		}
	} catch (const std::exception& ex) {
		LogError(std::string(def.szName) + " failed: " + ex.what());
	} catch (...) {
		LogError(std::string(def.szName) + " failed with an unknown exception");
	}
	// A handler that threw still owns the command (PITFALLS A: never unwind into the engine).
	return s2sdk::ResultType::Handled;
}

void HandleGravityCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	const std::string value = Arg(args, 2);
	if (value.size() > kMaxValueLength) {
		PrintLine(iSlot, "Invalid gravity value");
		return;
	}
	SetGravity(iSlot, iTarget, static_cast<float>(std::atof(value.c_str())), true);
}

void HandleHPCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	const std::string value = Arg(args, 2);
	if (value.size() > kMaxValueLength) {
		PrintLine(iSlot, "Invalid HP value");
		return;
	}
	SetHP(iSlot, iTarget, std::atoi(value.c_str()), true);
}

void HandleArmorCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	const std::string value = Arg(args, 2);
	if (value.size() > kMaxValueLength) {
		PrintLine(iSlot, "Invalid armor value");
		return;
	}
	SetArmor(iSlot, iTarget, std::atoi(value.c_str()), true);
}

void HandleScaleCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	const std::string value = Arg(args, 2);
	if (value.size() > kMaxValueLength) {
		PrintLine(iSlot, "Invalid scale value");
		return;
	}
	SetScale(iSlot, iTarget, static_cast<float>(std::atof(value.c_str())), true);
}

void HandleSpeedCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	const std::string value = Arg(args, 2);
	if (value.size() > kMaxValueLength) {
		PrintLine(iSlot, "Invalid speed value");
		return;
	}
	SetSpeed(iSlot, iTarget, static_cast<float>(std::atof(value.c_str())), true);
}

void HandleGodCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	if (args.size() > 2 && Arg(args, 2).size() > kMaxValueLength) {
		PrintLine(iSlot, "Invalid god value");
		return;
	}
	const bool bGod = args.size() > 2 ? std::atoi(Arg(args, 2).c_str()) != 0 : true;
	SetGod(iSlot, iTarget, bGod, true);
}

void HandleFreezyCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	if (args.size() > 2 && Arg(args, 2).size() > kMaxValueLength) {
		PrintLine(iSlot, "Invalid freeze value");
		return;
	}
	const float flTime = args.size() > 2 ? static_cast<float>(std::atof(Arg(args, 2).c_str())) : 0.0f;
	SetFreezy(iSlot, iTarget, true, true);
	if (flTime > 0.0f) {
		// The delayed action outlives the slot: the target (and the acting admin) are resolved
		// again when it fires, so a new player in the slot is not unfrozen (and announced).
		AddDelayedAction(DelayedKind::Unfreeze, iSlot, iTarget, flTime);
	}
}

void HandleUnfreezyCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	SetFreezy(iSlot, iTarget, false, true);
}

void HandleBlindCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	if (args.size() > 2 && Arg(args, 2).size() > kMaxValueLength) {
		PrintLine(iSlot, "Invalid blind value");
		return;
	}
	const float flTime = args.size() > 2 ? static_cast<float>(std::atof(Arg(args, 2).c_str())) : 0.0f;
	BlindPlayer(iSlot, iTarget, true, true);
	if (flTime > 0.0f) {
		// Same as the unfreeze timer: the slot may carry another player by the time it fires.
		AddDelayedAction(DelayedKind::Unblind, iSlot, iTarget, flTime);
	}
}

void HandleUnblindCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	BlindPlayer(iSlot, iTarget, false, true);
}

void HandleBuryCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	BuryPlayer(iSlot, iTarget, true);
}

void HandleUnburyCommand(int32_t iSlot, const plg::vector<plg::string>& args) {
	const int32_t iTarget = FindUser(Arg(args, 1));
	if (iTarget < 0) {
		return;
	}
	UnburyPlayer(iSlot, iTarget, true);
}

// One callback per command so s2sdk can match the exact function pointer in RemoveCommand.
s2sdk::ResultType OnGravityCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								   const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[0], iSlot, eContext, args);
}

s2sdk::ResultType OnHPCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
							  const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[1], iSlot, eContext, args);
}

s2sdk::ResultType OnArmorCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								 const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[2], iSlot, eContext, args);
}

s2sdk::ResultType OnScaleCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								 const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[3], iSlot, eContext, args);
}

s2sdk::ResultType OnSpeedCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								 const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[4], iSlot, eContext, args);
}

s2sdk::ResultType OnGodCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
							   const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[5], iSlot, eContext, args);
}

s2sdk::ResultType OnFreezyCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								  const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[6], iSlot, eContext, args);
}

s2sdk::ResultType OnUnfreezyCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
									const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[7], iSlot, eContext, args);
}

s2sdk::ResultType OnBlindCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								 const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[8], iSlot, eContext, args);
}

s2sdk::ResultType OnUnblindCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								   const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[9], iSlot, eContext, args);
}

s2sdk::ResultType OnBuryCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[10], iSlot, eContext, args);
}

s2sdk::ResultType OnUnburyCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								  const plg::vector<plg::string>& args) {
	return RunCommand(g_commands[11], iSlot, eContext, args);
}

constexpr std::array<s2sdk::ConCommandCallback, 12> kCommandCallbacks = {
	{&OnGravityCommand, &OnHPCommand, &OnArmorCommand, &OnScaleCommand, &OnSpeedCommand,
	 &OnGodCommand, &OnFreezyCommand, &OnUnfreezyCommand, &OnBlindCommand, &OnUnblindCommand,
	 &OnBuryCommand, &OnUnburyCommand}};

void RegisterCommands() {
	// A7, paired bindings: AddConsoleCommand alone is not enough. Whether a name is ours (created)
	// or somebody else's (only a listener was attached) is known only after the call, so without
	// both RemoveCommand (for a created name) and RemoveCommandListener (for a pre-existing one)
	// OnPluginEnd could leave a callback pointing into the unloaded library. No registration at
	// all is the only safe answer when any of the three is missing.
	if (__s2sdk_AddConsoleCommand == nullptr || __s2sdk_RemoveCommand == nullptr ||
		__s2sdk_RemoveCommandListener == nullptr) {
		LogError("s2sdk AddConsoleCommand/RemoveCommand/RemoveCommandListener are not all "
				 "available - no mm_* command is registered (its callback could not be removed "
				 "on unload)");
		return;
	}
	// PITFALLS C11: our own commands must be client-executable so a client console reaches the
	// permission check, and Release so they are available on a normal server.
	constexpr s2sdk::ConVarFlag kFlags = static_cast<s2sdk::ConVarFlag>(
		static_cast<int64_t>(s2sdk::ConVarFlag::Release) |
		static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute));
	for (std::size_t i = 0; i < g_commands.size(); ++i) {
		CommandDef& def = g_commands[i];
		if (def.registered) {
			continue;
		}
		// false = the name already existed (engine or another plugin) and s2sdk only attached a
		// listener to it: OnPluginEnd must then detach the listener and never RemoveCommand a
		// command this plugin did not create (PITFALLS C11).
		bool bCreated = false;
		try {
			bCreated = s2sdk::AddConsoleCommand(def.szName, "Admin fun command", kFlags,
												kCommandCallbacks[i], s2sdk::HookMode::Pre);
		} catch (const std::exception& ex) {
			LogError(std::string("registering '") + def.szName + "' failed: " + ex.what());
			continue;
		} catch (...) {
			LogError(std::string("registering '") + def.szName + "' failed");
			continue;
		}
		def.created = bCreated;
		def.registered = true;
		if (!bCreated) {
			LogInfo(std::string("the command '") + def.szName +
					"' already existed; only a Pre listener was attached to it");
		}
	}
}

void UnregisterCommands() {
	for (std::size_t i = 0; i < g_commands.size(); ++i) {
		CommandDef& def = g_commands[i];
		if (!def.registered) {
			continue;
		}
		if (def.created) {
			if (__s2sdk_RemoveCommand != nullptr) {
				try {
					(void)s2sdk::RemoveCommand(plg::string(def.szName), kCommandCallbacks[i]);
				} catch (...) {
				}
			}
		} else if (__s2sdk_RemoveCommandListener != nullptr) {
			// The name existed before this plugin: detach our listener, leave the command.
			try {
				(void)s2sdk::RemoveCommandListener(plg::string(def.szName), kCommandCallbacks[i],
												   s2sdk::HookMode::Pre);
			} catch (...) {
			}
		}
		def.registered = false;
		def.created = false;
	}
}

// ---------------------------------------------------------------------------------------------
// The admin core listener and the menu items.
// ---------------------------------------------------------------------------------------------
void RegisterAdminMenuItems() {
	if (!g_adminBindingsAvailable || !g_menuBindingsAvailable || g_pAdminApi == nullptr) {
		return;
	}
	if (g_adminMenuItemsRegistered) {
		return;
	}
	// The category itself is registered through the generated binding with a null display
	// callback: the consumer wrapper always installs its CategoryDisplayTrampoline (a pointer into
	// this .so), and the category would then be a callback the core holds after an unload. Only
	// the module's own "players" category is registered here; other plugins' categories and items
	// are never touched (PITFALLS A7).
	if (__admin_system_RegisterCategory == nullptr) {
		LogError("the admin_system RegisterCategory binding is unavailable - the players category "
				 "is not registered");
		return;
	}
	g_adminMenuItemsRegistered = true;
	try {
		::admin_system::RegisterCategory(plg::string("players"), plg::string("Category_Players"),
										 nullptr);
	} catch (const std::exception& ex) {
		LogError(std::string("registering the players category failed: ") + ex.what());
	}
	for (const ItemDef& item : kItems) {
		g_pAdminApi->RegisterItem(item.szIdentity, item.szPhrase, "players", item.szPermission,
								  nullptr, &OnAdminItemSelect);
	}
	LogInfo("the 'players' category (10 items) is registered");
}

// The admin core has no unregister API for categories/items. Re-register every item this module
// registered with the same identity/name/category/permissions and null callbacks, through the
// *generated binding*: the consumer wrapper cannot be used here because it always installs its
// own ItemDisplayTrampoline/ItemSelectTrampoline, i.e. pointers into the .so that is about to be
// unloaded. The core explicitly handles null callbacks (ItemLabel skips a null display and
// ShowAdminItemMenu refuses a null select), so the entries stay listed but do nothing until a
// reload registers the real ones. Only the identities this module registered are touched here -
// no other plugin's items or categories are re-registered, overwritten or removed (PITFALLS A7).
void RetireAdminMenuItems() {
	if (!g_adminMenuItemsRegistered) {
		return;
	}
	g_adminMenuItemsRegistered = false;
	if (__admin_system_RegisterItem == nullptr) {
		// Fail soft: without the binding the stale pointers cannot be cleared. Plugify unloads a
		// consumer before its dependency, so this only happens with an already-destroyed core.
		LogError("the admin_system RegisterItem binding is unavailable - the menu item callbacks "
				 "cannot be retired");
		return;
	}
	for (const ItemDef& item : kItems) {
		try {
			::admin_system::RegisterItem(plg::string(item.szIdentity), plg::string(item.szPhrase),
										 plg::string("players"), plg::string(item.szPermission),
										 nullptr, nullptr);
		} catch (const std::exception& ex) {
			LogError(std::string("could not retire the item '") + item.szIdentity + "': " +
					 ex.what());
		} catch (...) {
			LogError(std::string("could not retire the item '") + item.szIdentity);
		}
	}
}

// The original registered the menu items when the admin core signalled that it is loaded. A
// module loaded after the core has to run the same block immediately: the event does not fire
// twice (plugins/admin_system/API.md).
void OnAdminCoreLoaded() {
	try {
		RegisterAdminMenuItems();
	} catch (const std::exception& ex) {
		LogError(std::string("RegisterAdminMenuItems failed: ") + ex.what());
	} catch (...) {
		LogError("RegisterAdminMenuItems failed with an unknown exception");
	}
}

// ---------------------------------------------------------------------------------------------
// Map listeners (PITFALLS B1/B2). They only touch the gate and this plugin's own bookkeeping.
// ---------------------------------------------------------------------------------------------
// s2sdk::GetCurrentMap answers "<empty>" (and "" for a server without a map) while no map is up;
// the mid-map latch must not open the gate on such an answer.
bool MapNameIsReal() {
	if (__s2sdk_GetCurrentMap == nullptr) {
		return false;
	}
	try {
		const std::string map = ToStd(s2sdk::GetCurrentMap());
		return !map.empty() && map != "<empty>";
	} catch (...) {
		return false;
	}
}

void OnMapStartListener() {
	g_mapReady = true;
	g_mapEndedSinceStart = false;
}

void OnMapEndListener() {
	g_mapReady = false;
	g_mapEndedSinceStart = true;
	DropMenusForMapEnd();
	// A delayed action cannot run without a map (a client query in the map-change window
	// segfaults - PITFALLS B2/B4); carrying it into the next map would hit a slot whose
	// connection is gone.
	g_delayedActions.clear();
	for (Pending& pending : g_pending) {
		pending = Pending{};
	}
}

// ---------------------------------------------------------------------------------------------
// Plugin glue
// ---------------------------------------------------------------------------------------------
// Every menu whose session is still open when the plugin unloads is torn down here. While a map
// is up DestroyMenu() closes the backend UI properly (the backend lives in another plugin that
// is still loaded); outside a map the session is ended with Disconnect first, which skips the
// backend and never touches the client list being torn down (PITFALLS B2/C10).
void CloseAllMenusForUnload() {
	const bool bMapReady = g_mapReady;
	std::vector<DestroyRequest> requests;
	for (int32_t i = 0; i < kSlots; ++i) {
		const s2sdk::MenuId id = g_openMenus[static_cast<std::size_t>(i)].id;
		if (id != 0) {
			requests.push_back(DestroyRequest{id, i});
		}
	}
	for (const DestroyRequest& request : g_destroyQueue) {
		requests.push_back(request);
	}
	g_destroyQueue.clear();
	for (const DestroyRequest& request : requests) {
		if (!DestroyMenuChecked(request.id, request.slot, bMapReady)) {
			// The last chance: report it loudly instead of pretending the unload was clean.
			ReportUnresolvedMenu(request.id, request.slot, true);
		}
	}
	for (OpenMenu& menu : g_openMenus) {
		menu = OpenMenu{};
	}
	g_unresolvedMenus.clear();
	for (Pending& pending : g_pending) {
		pending = Pending{};
	}
}

class FunCommandsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			g_pAdminApi = &admin_system::AdminApi::Get();
			// A reload (plg reload) starts from a clean sheet; OnPluginEnd already unregistered
			// everything, so the flags are simply re-armed here (PITFALLS A7).
			g_mapReady = false;
			g_mapEndedSinceStart = false;
			g_firstUpdateDone = false;
			g_adminMenuItemsRegistered = false;
			for (OpenMenu& menu : g_openMenus) {
				menu = OpenMenu{};
			}
			for (Pending& pending : g_pending) {
				pending = Pending{};
			}
			g_destroyQueue.clear();
			g_unresolvedMenus.clear();
			g_delayedActions.clear();
			ClearSchemaOffsets();

			g_adminBindingsAvailable = AdminApiAvailable();
			g_menuBindingsAvailable = MenuApiAvailable();
			if (!g_adminBindingsAvailable) {
				LogError("the admin_system bindings are unavailable - permissions can only be "
						 "checked for nobody, so only the server console can use the commands");
			}
			if (!g_menuBindingsAvailable) {
				LogError("the s2sdk menu bindings are unavailable - the admin menu items are not "
						 "registered");
			}

			// PITFALLS B1 + A7: registering the map listeners touches no client, and nothing
			// could ever open the gate again without them. Only register when BOTH pairs exist -
			// the register halves and the matching unregister halves: a missing unregister would
			// leave a callback inside the unloaded library, so the gate simply stays shut.
			const bool bMapListenerBindings =
				__s2sdk_OnMapStart_Register != nullptr && __s2sdk_OnMapStart_Unregister != nullptr &&
				__s2sdk_OnMapEnd_Register != nullptr && __s2sdk_OnMapEnd_Unregister != nullptr;
			if (!g_mapListeners && bMapListenerBindings) {
				s2sdk::OnMapStart_Register(&OnMapStartListener);
				s2sdk::OnMapEnd_Register(&OnMapEndListener);
				g_mapListeners = true;
			} else if (!bMapListenerBindings) {
				LogError("the s2sdk map listener bindings are incomplete (register/unregister "
						 "pairs) - client calls stay disabled");
			}

			RegisterCommands();

			if (g_adminBindingsAvailable && g_pAdminApi != nullptr) {
				g_pAdminApi->OnCoreLoaded(&OnAdminCoreLoaded);
				if (g_pAdminApi->IsCoreLoaded()) {
					OnAdminCoreLoaded(); // late load: the event will not fire twice
				}
			}

			LogInfo("ready");
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed (PITFALLS A1: that stops every Plugify plugin)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override {
		try {
			const double flSeconds = static_cast<double>(dt.count()) / 1000.0;
			// A plugin loaded into a running map never saw OnMapStart: its first update is the
			// "a map is up" signal (PITFALLS B1) - but only when the four map-listener bindings
			// really were registered, no OnMapEnd was seen since OnPluginStart and s2sdk names a
			// real map (never "<empty>"/"").
			if (!g_firstUpdateDone) {
				g_firstUpdateDone = true;
				if (g_mapListeners && !g_mapEndedSinceStart && MapNameIsReal()) {
					g_mapReady = true;
				}
			}
			DrainDestroyQueue();
			if (!g_mapReady) {
				// No client, entity or schema call in the map-change window; a deadline that
				// survived would fire on the next map's first frame against a reused slot.
				g_delayedActions.clear();
				return {};
			}
			// Menu selections and delayed actions are applied here, outside the s2sdk menu/timer
			// locks (PITFALLS C9/C10): no engine action and no admin callback runs under them.
			UpdateDelayedActions(flSeconds);
			for (int32_t i = 0; i < kSlots; ++i) {
				ApplyPending(i);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginUpdate failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginUpdate failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// Menus first: DestroyMenu reaches this plugin's handler, which must still be loaded.
			CloseAllMenusForUnload();
			// Then clear the admin core's item callbacks: they point into this .so (PITFALLS A7).
			RetireAdminMenuItems();

			g_mapReady = false;
			g_mapEndedSinceStart = true;
			g_firstUpdateDone = false;
			g_delayedActions.clear();

			// A7, paired binding: only remove the listener this plugin actually registered, and
			// only while the unregister export exists. The consumer wrapper checks the plugin's
			// availability but not this individual slot, so the pair is guarded here.
			if (g_pAdminApi != nullptr && g_adminBindingsAvailable &&
				__admin_system_OnCoreLoaded_Unregister != nullptr) {
				g_pAdminApi->OnCoreLoadedUnregister();
			}

			UnregisterCommands();

			if (g_mapListeners) {
				if (__s2sdk_OnMapStart_Unregister != nullptr) {
					s2sdk::OnMapStart_Unregister(&OnMapStartListener);
				}
				if (__s2sdk_OnMapEnd_Unregister != nullptr) {
					s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
				}
				g_mapListeners = false;
			}

			ClearSchemaOffsets();
			// A stale callback (the engine still holds the pointer for a moment) can no longer
			// reach a client or an admin check.
			g_pAdminApi = nullptr;
			g_adminBindingsAvailable = false;
			g_menuBindingsAvailable = false;
			for (OpenMenu& menu : g_openMenus) {
				menu = OpenMenu{};
			}
			for (Pending& pending : g_pending) {
				pending = Pending{};
			}
			g_destroyQueue.clear();
			g_delayedActions.clear();
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginEnd failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

FunCommandsPlugin g_funCommandsPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_funCommandsPlugin)
