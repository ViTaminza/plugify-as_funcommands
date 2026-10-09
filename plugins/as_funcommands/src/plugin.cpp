// as_funcommands — Plugify port of the Pisex cs2-admin_system module "FunCommands"
// (ref/pisex/cs2-admin_system_modules/FunCommands/as_funcommands.cpp).
//
// Behaviour kept identical:
//   * Console commands (needs @admin/<name>, the server console always passes; the original's
//     CON_COMMAND_F + ConVar_Register flags are reused so clients can execute them):
//       mm_gravity/mm_hp/mm_armor/mm_scale/mm_speed <name|steamid|slot> <value>,
//       mm_god <target> [0/1], mm_freezy <target> [time], mm_unfreezy <target>,
//       mm_blind <target> [time], mm_unblind <target>, mm_bury <target>, mm_unbury <target>.
//     Usage/error lines go to the caller's console (a negative slot = the server console).
//   * The admin menu: category "players" with the items freeze, gravity, scale, hp, armor, speed,
//     god, bury, unbury, blind (identities, phrase keys and flags are unchanged); selecting one
//     shows the alive players (with the per-item state), selecting a player shows the value menu
//     ("1..1000", "0.1x..10x"), back (item 7) goes back to the admin menu (player list) or to the
//     player list (value menus), and everything performs the same action + broadcast as before.
//   * The public chat messages (AdminGravityMessage/GravityMessage/...), GetMessageType() 1/2
//     semantics and SendAction(action, param) payloads are unchanged.
//
// Port notes:
//   * m_flGravityScale / m_iHealth / m_ArmorValue / m_flVelocityModifier / m_flScale and the
//     engine's move type are set through s2sdk's client helpers (SetClientGravity, SetClientHealth,
//     SetClientArmor, SetClientSpeed, SetClientAbsScale, SetClientMoveType); each of them performs
//     the same schema write + network update (ChainNetworkStateChanged) the original did with the
//     typed field and g_pUtils->SetStateChanged(). SetClientMoveType calls the engine's
//     CBaseEntity::SetMoveType (cs2-menus' UTIL_SetMoveType), exactly like the original.
//   * Freeze uses MOVETYPE_OBSOLETE (value 1 of the engine's MoveType_t, verified against the
//     server schema dump); s2sdk's MoveType enum names that value "Isometric" but passes it
//     through unchanged. The original read m_nActualMoveType for the ON/OFF display; s2sdk's
//     GetClientMoveType reads m_MoveType, the field its own setter writes - so the plugin stays
//     consistent with itself (and keeps working when the engine re-derives m_nActualMoveType).
//   * m_bTakesDamage (god mode) and the blind fields (m_flFlashDuration, m_flFlashMaxAlpha,
//     m_blindUntilTime, m_blindStartTime on CCSPlayerPawnBase) have no s2sdk helper, so they are
//     read/written by name through pisex/schema.hpp with the original's changeState flags.
//   * Bury/unbury use the pawn's abs origin/angles/velocity + pisex::Teleport (IPlayersApi's
//     TeleportEntity equivalent).
//   * The core calls the item-select callback from *inside* its own s2sdk menu handler, which
//     holds the MenuManager lock; creating our value/player menus there would re-enter the menu
//     subsystem (see libs/pisexcompat's notes on s2sdk callbacks holding locks), so the list is
//     opened from the next frame (pisex::NextFrame -> pisex::Poll). Menus opened from our own
//     callbacks are deferred by pisexcompat itself.
//   * The original leaked one strdup() per selection (g_szLastItem); the port keeps a std::string
//     per slot instead.
//   * Nobody is queried without a map: commands/menu callbacks only run for a live client and
//     pisexcompat's player/schema wrappers return neutral values before the first map.

#include <admin_api.hpp>
#include <pisex/pisex.hpp>
#include <plg/numerics.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace {

admin_system::AdminApi* g_pAdminApi = &admin_system::AdminApi::Get();

// CS2 has 64 player slots.
constexpr int kMaxSlots = 64;

// MoveType_t of the server schema: MOVETYPE_OBSOLETE = 1 (the "frozen" value the original used),
// MOVETYPE_WALK = 2.
constexpr int kMoveTypeObsolete = 1;
constexpr int kMoveTypeWalk = 2;

// The flags of the Metamod CON_COMMAND_F + ConVar_Register pair of the original: a client can
// execute the mm_* commands from its console.
constexpr s2sdk::ConVarFlag kCommandFlags =
        static_cast<s2sdk::ConVarFlag>(static_cast<int64_t>(s2sdk::ConVarFlag::GameDll) |
                                       static_cast<int64_t>(s2sdk::ConVarFlag::ServerCanExecute) |
                                       static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute) |
                                       static_cast<int64_t>(s2sdk::ConVarFlag::Notify));

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

// --- small string helpers -----------------------------------------------------

std::string ToString(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// Argument i of the s2sdk argv (0 is the command name, like CCommand::Arg(i)); empty when absent.
std::string Arg(const plg::vector<plg::string>& args, std::size_t index) {
	return index < args.size() ? ToString(args[index]) : std::string();
}

char ToLower(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// strcasestr of the original: empty needle = "matches anything".
bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
	if (needle.empty()) {
		return true;
	}
	if (needle.size() > haystack.size()) {
		return false;
	}
	for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
		std::size_t j = 0;
		while (j < needle.size() && ToLower(haystack[i + j]) == ToLower(needle[j])) {
			++j;
		}
		if (j == needle.size()) {
			return true;
		}
	}
	return false;
}

bool IsAllDigits(std::string_view text) {
	return !text.empty() && text.find_first_not_of("0123456789") == std::string_view::npos;
}

bool ParseUint64(std::string_view text, uint64_t& out) {
	if (text.empty()) {
		return false;
	}
	const char* first = text.data();
	const char* last = text.data() + text.size();
	uint64_t value = 0;
	const auto [ptr, ec] = std::from_chars(first, last, value);
	if (ec != std::errc() || ptr != last) {
		return false;
	}
	out = value;
	return true;
}

// --- guarded s2sdk client access (pisexcompat's style: a missing binding is a no-op) ----------

bool TargetUsable(int iSlot) {
	// CCSPlayerController::FromSlot() + GetPlayerPawn() of the original.
	return iSlot >= 0 && iSlot < kMaxSlots && pisex::GetPlayerPawn(iSlot) != 0;
}

float GetClientGravity(int iSlot) {
	if (!pisex::IsGameReady() || !__s2sdk_GetClientGravity) {
		return 0.0f;
	}
	try {
		return s2sdk::GetClientGravity(iSlot);
	} catch (...) {
		return 0.0f;
	}
}

float GetClientSpeed(int iSlot) {
	if (!pisex::IsGameReady() || !__s2sdk_GetClientSpeed) {
		return 0.0f;
	}
	try {
		return s2sdk::GetClientSpeed(iSlot);
	} catch (...) {
		return 0.0f;
	}
}

float GetClientScale(int iSlot) {
	// m_CBodyComponent->m_pSceneNode->m_flScale of the original (the engine's GetAbsScale).
	if (!pisex::IsGameReady() || !__s2sdk_GetClientAbsScale) {
		return 0.0f;
	}
	try {
		return s2sdk::GetClientAbsScale(iSlot);
	} catch (...) {
		return 0.0f;
	}
}

int GetClientMoveType(int iSlot) {
	if (!pisex::IsGameReady() || !__s2sdk_GetClientMoveType) {
		return kMoveTypeWalk;
	}
	try {
		return static_cast<int>(s2sdk::GetClientMoveType(iSlot));
	} catch (...) {
		return kMoveTypeWalk;
	}
}

void SetClientGravity(int iSlot, float value) {
	if (!pisex::IsGameReady() || !__s2sdk_SetClientGravity) {
		return;
	}
	try {
		s2sdk::SetClientGravity(iSlot, value);
	} catch (...) {
	}
}

void SetClientHealth(int iSlot, int value) {
	if (!pisex::IsGameReady() || !__s2sdk_SetClientHealth) {
		return;
	}
	try {
		s2sdk::SetClientHealth(iSlot, value);
	} catch (...) {
	}
}

void SetClientArmor(int iSlot, int value) {
	if (!pisex::IsGameReady() || !__s2sdk_SetClientArmor) {
		return;
	}
	try {
		s2sdk::SetClientArmor(iSlot, value);
	} catch (...) {
	}
}

void SetClientSpeed(int iSlot, float value) {
	if (!pisex::IsGameReady() || !__s2sdk_SetClientSpeed) {
		return;
	}
	try {
		s2sdk::SetClientSpeed(iSlot, value);
	} catch (...) {
	}
}

void SetClientScale(int iSlot, float value) {
	if (!pisex::IsGameReady() || !__s2sdk_SetClientAbsScale) {
		return;
	}
	try {
		s2sdk::SetClientAbsScale(iSlot, value);
	} catch (...) {
	}
}

void SetClientMoveType(int iSlot, int value) {
	// pisexcompat's IPlayersApi::SetMoveType: the engine's CBaseEntity::SetMoveType.
	pisex::g_pPlayers->SetMoveType(iSlot, value);
}

// --- schema fields without an s2sdk helper ----------------------------------------------------

float GetFlashDuration(int iSlot) {
	return pisex::GetEntFloat(pisex::GetPlayerPawn(iSlot), "CCSPlayerPawnBase", "m_flFlashDuration", 0.0f);
}

void SetPawnFloat(int iSlot, const char* field, float value, bool bChangeState) {
	const int pawn = pisex::GetPlayerPawn(iSlot);
	if (pawn == 0) {
		return;
	}
	pisex::SetEntFloat(pawn, "CCSPlayerPawnBase", field, value, bChangeState);
}

bool TakesDamage(int iSlot) {
	return pisex::GetEntBool(pisex::GetPlayerPawn(iSlot), "CBaseEntity", "m_bTakesDamage", false);
}

void SetTakesDamage(int iSlot, bool bTakesDamage) {
	const int pawn = pisex::GetPlayerPawn(iSlot);
	if (pawn == 0) {
		return;
	}
	// The original wrote the field without a network update (damage is processed server-side).
	pisex::SetEntBool(pawn, "CBaseEntity", "m_bTakesDamage", bTakesDamage, false);
}

// --- shared bits of the action/message code ---------------------------------------------------

bool IsFrozen(int iSlot) {
	return GetClientMoveType(iSlot) == kMoveTypeObsolete;
}

bool IsBlinded(int iSlot) {
	return GetFlashDuration(iSlot) > 0.0f;
}

const char* AdminName(int iSlot) {
	return iSlot == -1 ? "Console" : pisex::g_pPlayers->GetPlayerName(iSlot);
}

int GetMessageType() {
	return g_pAdminApi->GetMessageType();
}

// Prints a line to the caller's console (a negative slot = the server console). The text is passed
// as an argument, so a translation containing '%' can never be read as a format string here.
void PrintLine(int iSlot, const char* text) {
	pisex::PrintToConsole(iSlot, "%s\n", text ? text : "");
}

void PrintUsage(int iSlot, const char* key, const std::string& command) {
	const std::string usage = pisex::FormatString(g_pAdminApi->GetTranslation(key), command.c_str());
	PrintLine(iSlot, usage.c_str());
}

// --- the plugin --------------------------------------------------------------------------------

class FunCommandsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// --- admin menu entry point (admin_system item-select callback) ---
	void OnItemSelected(int iSlot, const char* szIdentity);

	// --- console command dispatch ---
	s2sdk::ResultType DispatchCommand(int32_t caller, const plg::vector<plg::string>& args);

	using CommandHandler = s2sdk::ResultType (FunCommandsPlugin::*)(int32_t, const plg::vector<plg::string>&);
	s2sdk::ResultType CommandGravity(int32_t caller, const plg::vector<plg::string>& args);
	s2sdk::ResultType CommandHP(int32_t caller, const plg::vector<plg::string>& args);
	s2sdk::ResultType CommandArmor(int32_t caller, const plg::vector<plg::string>& args);
	s2sdk::ResultType CommandScale(int32_t caller, const plg::vector<plg::string>& args);
	s2sdk::ResultType CommandSpeed(int32_t caller, const plg::vector<plg::string>& args);
	s2sdk::ResultType CommandGod(int32_t caller, const plg::vector<plg::string>& args);
	s2sdk::ResultType CommandFreezy(int32_t caller, const plg::vector<plg::string>& args);
	s2sdk::ResultType CommandUnfreezy(int32_t caller, const plg::vector<plg::string>& args);
	s2sdk::ResultType CommandBlind(int32_t caller, const plg::vector<plg::string>& args);
	s2sdk::ResultType CommandUnblind(int32_t caller, const plg::vector<plg::string>& args);
	s2sdk::ResultType CommandBury(int32_t caller, const plg::vector<plg::string>& args);
	s2sdk::ResultType CommandUnbury(int32_t caller, const plg::vector<plg::string>& args);

	// --- menus ---
	void ShowPlayerList(int iSlot, const std::string& identity);
	void ShowBaseStatMenu(int iSlot, int iTarget, bool bHealth);
	void ShowSpeedMenu(int iSlot, int iTarget, bool bSpeed);
	void ShowGravityMenu(int iSlot, int iTarget);

	const std::string& LastItem(int iSlot) const { return m_lastItem[static_cast<std::size_t>(iSlot)]; }
	void SetLastItem(int iSlot, const char* identity) {
		if (iSlot >= 0 && iSlot < kMaxSlots && identity) {
			m_lastItem[static_cast<std::size_t>(iSlot)] = identity;
		}
	}

	// --- actions (the Set*/Blind*/Bury* family of the original) ---
	void SetGravity(int iSlot, int iTarget, float fGravity);
	void SetHP(int iSlot, int iTarget, int iHP);
	void SetArmor(int iSlot, int iTarget, int iArmor);
	void SetScale(int iSlot, int iTarget, float fScale);
	void SetSpeed(int iSlot, int iTarget, float fSpeed);
	void SetGod(int iSlot, int iTarget, bool bGod);
	void SetFreezy(int iSlot, int iTarget, bool bFreeze);
	void BlindPlayer(int iSlot, int iTarget, bool bBlind);
	void BuryPlayer(int iSlot, int iTarget);
	void UnburyPlayer(int iSlot, int iTarget);

	// --- helpers ---
	void RegisterMenus();
	int FindUser(const std::string& identity) const;
	void ReopenPlayerList(int iSlot, const char* identity);

private:
	std::array<std::string, kMaxSlots> m_lastItem{};
	bool m_menusRegistered = false;
	std::array<std::string, 12> m_registeredCommands{};
	int m_registeredCommandCount = 0;
};

FunCommandsPlugin g_funCommandsPlugin;

struct CommandSpec {
	const char* name;
	const char* permission; // nullptr = no permission check (besides the console bypass)
	const char* usageKey;	// phrase key for the "<command>" usage line
	int minArgs;			// including the command name, like CCommand::ArgC()
	FunCommandsPlugin::CommandHandler handler;
};

constexpr CommandSpec kCommands[] = {
		{"mm_gravity", "@admin/gravity", "Usage_Gravity", 3, &FunCommandsPlugin::CommandGravity},
		{"mm_hp", "@admin/hp", "Usage_HP", 3, &FunCommandsPlugin::CommandHP},
		{"mm_armor", "@admin/armor", "Usage_Armor", 3, &FunCommandsPlugin::CommandArmor},
		{"mm_scale", "@admin/scale", "Usage_Scale", 3, &FunCommandsPlugin::CommandScale},
		{"mm_speed", "@admin/speed", "Usage_Speed", 3, &FunCommandsPlugin::CommandSpeed},
		{"mm_god", "@admin/god", "Usage_God", 2, &FunCommandsPlugin::CommandGod},
		{"mm_freezy", "@admin/freeze", "Usage_Freeze", 2, &FunCommandsPlugin::CommandFreezy},
		{"mm_unfreezy", "@admin/freeze", "Usage_Unfreeze", 2, &FunCommandsPlugin::CommandUnfreezy},
		{"mm_blind", "@admin/blind", "Usage_Blind", 2, &FunCommandsPlugin::CommandBlind},
		{"mm_unblind", "@admin/blind", "Usage_Unblind", 2, &FunCommandsPlugin::CommandUnblind},
		{"mm_bury", "@admin/bury", "Usage_Bury", 2, &FunCommandsPlugin::CommandBury},
		{"mm_unbury", "@admin/bury", "Usage_Unbury", 2, &FunCommandsPlugin::CommandUnbury},
};

// One trampoline for all of them (the dispatcher looks the name up in the table above).
s2sdk::ResultType OnFunCommand(int32_t caller, s2sdk::ConCommandContext context,
							   const plg::vector<plg::string>& args) {
	(void)context;
	try {
		return g_funCommandsPlugin.DispatchCommand(caller, args);
	} catch (const std::exception& ex) {
		LogError(std::string("as_funcommands: exception in a console command: ") + ex.what());
	} catch (...) {
		LogError("as_funcommands: unknown exception in a console command");
	}
	return s2sdk::ResultType::Handled;
}

// admin_system item-select callback ("players" category).
void OnItemSelect(int iSlot, const char* szCategory, const char* szIdentity, const char* szItem) {
	(void)szCategory;
	(void)szItem;
	try {
		g_funCommandsPlugin.OnItemSelected(iSlot, szIdentity);
	} catch (const std::exception& ex) {
		LogError(std::string("as_funcommands: exception in the item-select callback: ") + ex.what());
	} catch (...) {
		LogError("as_funcommands: unknown exception in the item-select callback");
	}
}

plg::PluginResult FunCommandsPlugin::OnPluginStart() {
	try {
		// Never fatal: pisexcompat only logs when the s2sdk bindings are missing.
		pisex::Init("as_funcommands");

		if (!__s2sdk_AddConsoleCommand) {
			LogError("as_funcommands: the s2sdk bindings are unavailable - no commands are registered");
		} else {
			for (const CommandSpec& spec : kCommands) {
				if (m_registeredCommandCount >= static_cast<int>(m_registeredCommands.size())) {
					break; // cannot happen while the table and the array have the same size
				}
				if (s2sdk::AddConsoleCommand(spec.name, "Admin fun command", kCommandFlags, &OnFunCommand,
											 s2sdk::HookMode::Pre)) {
					m_registeredCommands[static_cast<std::size_t>(m_registeredCommandCount++)] = spec.name;
				} else {
					LogError(std::string("as_funcommands: registering '") + spec.name +
							 "' failed (continuing without it)");
				}
			}
		}

		// The menu entries: the core fires OnCoreLoaded when its database is up. A plugin that
		// starts after that (reload) must run its registration immediately, the event will not
		// fire a second time.
		g_pAdminApi->OnCoreLoaded([]() {
			try {
				g_funCommandsPlugin.RegisterMenus();
			} catch (const std::exception& ex) {
				LogError(std::string("as_funcommands: menu registration failed: ") + ex.what());
			} catch (...) {
				LogError("as_funcommands: menu registration failed with an unknown exception");
			}
		});
		if (g_pAdminApi->IsCoreLoaded()) {
			RegisterMenus();
		}

		LogInfo("as_funcommands: ready; " + std::to_string(m_registeredCommandCount) + " console commands");
	} catch (const std::exception& ex) {
		LogError(std::string("as_funcommands: OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogError("as_funcommands: OnPluginStart failed with an unknown exception");
	}
	return {};
}

plg::PluginResult FunCommandsPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	(void)dt;
	// The plugify update callback must not let an exception through: pisexcompat wraps its own
	// paths, this is the outer net for the bookkeeping around them.
	try {
		pisex::Poll(); // timers, next-frame queue (the deferred player list), maps of pisexcompat
	} catch (const std::exception& ex) {
		LogError(std::string("as_funcommands: update failed: ") + ex.what());
	} catch (...) {
		LogError("as_funcommands: update failed with an unknown exception");
	}
	return {};
}

plg::PluginResult FunCommandsPlugin::OnPluginEnd() {
	try {
		for (int i = 0; i < m_registeredCommandCount; ++i) {
			if (__s2sdk_RemoveCommand) {
				s2sdk::RemoveCommand(plg::string(m_registeredCommands[static_cast<std::size_t>(i)]), &OnFunCommand);
			}
		}
		m_registeredCommandCount = 0;
		// The item registration itself has no unregister in the admin_system API (the original
		// could not drop its items either); the core replaces our entries if we come back.
		g_pAdminApi->OnCoreLoadedUnregister();
	} catch (const std::exception& ex) {
		LogError(std::string("as_funcommands: OnPluginEnd failed: ") + ex.what());
	} catch (...) {
		LogError("as_funcommands: OnPluginEnd failed with an unknown exception");
	}
	pisex::Shutdown();
	return {};
}

void FunCommandsPlugin::RegisterMenus() {
	if (m_menusRegistered) {
		return;
	}
	m_menusRegistered = true;
	// Same identities, phrase keys, categories and flags as the original (and the same order).
	g_pAdminApi->RegisterCategory("players", "Category_Players", nullptr);
	g_pAdminApi->RegisterItem("freeze", "Item_Freeze", "players", "@admin/freeze", nullptr, &OnItemSelect);
	g_pAdminApi->RegisterItem("gravity", "Item_Gravity", "players", "@admin/gravity", nullptr, &OnItemSelect);
	g_pAdminApi->RegisterItem("scale", "Item_Scale", "players", "@admin/scale", nullptr, &OnItemSelect);
	g_pAdminApi->RegisterItem("hp", "Item_HP", "players", "@admin/hp", nullptr, &OnItemSelect);
	g_pAdminApi->RegisterItem("armor", "Item_Armor", "players", "@admin/armor", nullptr, &OnItemSelect);
	g_pAdminApi->RegisterItem("speed", "Item_Speed", "players", "@admin/speed", nullptr, &OnItemSelect);
	g_pAdminApi->RegisterItem("god", "Item_God", "players", "@admin/god", nullptr, &OnItemSelect);
	g_pAdminApi->RegisterItem("bury", "Item_Bury", "players", "@admin/bury", nullptr, &OnItemSelect);
	g_pAdminApi->RegisterItem("unbury", "Item_Unbury", "players", "@admin/unbury", nullptr, &OnItemSelect);
	g_pAdminApi->RegisterItem("blind", "Item_Blind", "players", "@admin/blind", nullptr, &OnItemSelect);
	LogInfo("as_funcommands: the 'players' category (10 items) is registered");
}

void FunCommandsPlugin::OnItemSelected(int iSlot, const char* szIdentity) {
	if (iSlot < 0 || iSlot >= kMaxSlots || !szIdentity || !*szIdentity) {
		return;
	}
	// g_szLastItem[iSlot] = strdup(szIdentity) of the original, without the leak.
	SetLastItem(iSlot, szIdentity);
	const std::string identity = szIdentity;
	// The admin core runs this callback inside its own s2sdk menu handler (MenuManager lock held),
	// so the player list is built on the next frame, from pisex::Poll().
	pisex::NextFrame([iSlot, identity]() {
		try {
			g_funCommandsPlugin.ShowPlayerList(iSlot, identity);
		} catch (const std::exception& ex) {
			LogError(std::string("as_funcommands: opening the player list failed: ") + ex.what());
		} catch (...) {
			LogError("as_funcommands: opening the player list failed with an unknown exception");
		}
	});
}

// --- console commands ---------------------------------------------------------------------------

s2sdk::ResultType FunCommandsPlugin::DispatchCommand(int32_t caller, const plg::vector<plg::string>& args) {
	if (args.empty() || args[0].empty()) {
		return s2sdk::ResultType::Continue;
	}
	const std::string name = ToString(args[0]);
	for (const CommandSpec& spec : kCommands) {
		if (name != spec.name) {
			continue;
		}
		const bool bConsole = caller == -1;
		if (!bConsole && spec.permission && !g_pAdminApi->HasPermission(caller, spec.permission)) {
			return s2sdk::ResultType::Handled; // the original returned without a word
		}
		if (static_cast<int>(args.size()) < spec.minArgs) {
			PrintUsage(caller, spec.usageKey, name);
			return s2sdk::ResultType::Handled;
		}
		return (this->*(spec.handler))(caller, args);
	}
	return s2sdk::ResultType::Continue; // not one of ours (cannot happen: we registered them)
}

s2sdk::ResultType FunCommandsPlugin::CommandGravity(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	const std::string value = Arg(args, 2);
	if (value.size() > 10) {
		PrintLine(caller, "Invalid gravity value");
		return s2sdk::ResultType::Handled;
	}
	SetGravity(caller, iTarget, static_cast<float>(std::atof(value.c_str())));
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType FunCommandsPlugin::CommandHP(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	const std::string value = Arg(args, 2);
	if (value.size() > 10) {
		PrintLine(caller, "Invalid HP value");
		return s2sdk::ResultType::Handled;
	}
	SetHP(caller, iTarget, std::atoi(value.c_str()));
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType FunCommandsPlugin::CommandArmor(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	const std::string value = Arg(args, 2);
	if (value.size() > 10) {
		PrintLine(caller, "Invalid armor value");
		return s2sdk::ResultType::Handled;
	}
	SetArmor(caller, iTarget, std::atoi(value.c_str()));
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType FunCommandsPlugin::CommandScale(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	const std::string value = Arg(args, 2);
	if (value.size() > 10) {
		PrintLine(caller, "Invalid scale value");
		return s2sdk::ResultType::Handled;
	}
	SetScale(caller, iTarget, static_cast<float>(std::atof(value.c_str())));
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType FunCommandsPlugin::CommandSpeed(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	const std::string value = Arg(args, 2);
	if (value.size() > 10) {
		PrintLine(caller, "Invalid speed value");
		return s2sdk::ResultType::Handled;
	}
	SetSpeed(caller, iTarget, static_cast<float>(std::atof(value.c_str())));
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType FunCommandsPlugin::CommandGod(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	if (args.size() > 2 && Arg(args, 2).size() > 10) {
		PrintLine(caller, "Invalid god value");
		return s2sdk::ResultType::Handled;
	}
	const bool bGod = args.size() > 2 ? std::atoi(Arg(args, 2).c_str()) != 0 : true;
	SetGod(caller, iTarget, bGod);
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType FunCommandsPlugin::CommandFreezy(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	if (args.size() > 2 && Arg(args, 2).size() > 10) {
		PrintLine(caller, "Invalid freeze value");
		return s2sdk::ResultType::Handled;
	}
	const float fTime = args.size() > 2 ? static_cast<float>(std::atof(Arg(args, 2).c_str())) : 0.0f;
	SetFreezy(caller, iTarget, true);
	if (fTime > 0.0f) {
		pisex::CreateTimer(fTime, [caller, iTarget]() -> float {
			g_funCommandsPlugin.SetFreezy(caller, iTarget, false);
			return -1.0f;
		});
	}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType FunCommandsPlugin::CommandUnfreezy(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	SetFreezy(caller, iTarget, false);
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType FunCommandsPlugin::CommandBlind(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	if (args.size() > 2 && Arg(args, 2).size() > 10) {
		PrintLine(caller, "Invalid blind value");
		return s2sdk::ResultType::Handled;
	}
	const float fTime = args.size() > 2 ? static_cast<float>(std::atof(Arg(args, 2).c_str())) : 0.0f;
	BlindPlayer(caller, iTarget, true);
	if (fTime > 0.0f) {
		pisex::CreateTimer(fTime, [caller, iTarget]() -> float {
			g_funCommandsPlugin.BlindPlayer(caller, iTarget, false);
			return -1.0f;
		});
	}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType FunCommandsPlugin::CommandUnblind(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	BlindPlayer(caller, iTarget, false);
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType FunCommandsPlugin::CommandBury(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	BuryPlayer(caller, iTarget);
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType FunCommandsPlugin::CommandUnbury(int32_t caller, const plg::vector<plg::string>& args) {
	const int iTarget = FindUser(Arg(args, 1));
	if (iTarget == -1) {
		return s2sdk::ResultType::Handled;
	}
	UnburyPlayer(caller, iTarget);
	return s2sdk::ResultType::Handled;
}

// --- target lookup (the original's FindUser) ----------------------------------------------------

int FunCommandsPlugin::FindUser(const std::string& identity) const {
	if (identity.empty()) {
		return -1;
	}
	const bool numeric = IsAllDigits(identity);
	uint64_t numericValue = 0;
	const bool haveNumeric = numeric && ParseUint64(identity, numericValue);

	for (int i = 0; i < kMaxSlots; ++i) {
		// CCSPlayerController::FromSlot() of the original.
		if (!pisex::g_pPlayers->IsConnected(i)) {
			continue;
		}
		// NOTE: the original stored m_steamID in a 32-bit uint, so its SteamID64 comparison could
		// never match; here the full SteamID64 is used (the value users actually paste).
		const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(i);
		if (steamid == 0) {
			continue;
		}
		const char* name = pisex::g_pPlayers->GetPlayerName(i);
		if (ContainsIgnoreCase(name ? name : "", identity) ||
			(haveNumeric && steamid == numericValue) ||
			(haveNumeric && numericValue == static_cast<uint64_t>(i))) {
			return i;
		}
	}
	return -1;
}

// --- actions ------------------------------------------------------------------------------------

void FunCommandsPlugin::SetGravity(int iSlot, int iTarget, float fGravity) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	SetClientGravity(iTarget, fGravity);
	if (GetMessageType() == 2) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("AdminGravityMessage"), AdminName(iSlot),
							  pisex::g_pPlayers->GetPlayerName(iTarget), fGravity);
	} else if (GetMessageType() == 1) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("GravityMessage"),
							  pisex::g_pPlayers->GetPlayerName(iTarget), fGravity);
	}
	const std::string param = pisex::FormatString("%i %f", iTarget, fGravity);
	g_pAdminApi->SendAction(iSlot, "gravity", param.c_str());
}

void FunCommandsPlugin::SetHP(int iSlot, int iTarget, int iHP) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	SetClientHealth(iTarget, iHP);
	if (GetMessageType() == 2) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("AdminHPMessage"), AdminName(iSlot),
							  pisex::g_pPlayers->GetPlayerName(iTarget), iHP);
	} else if (GetMessageType() == 1) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("HPMessage"), pisex::g_pPlayers->GetPlayerName(iTarget), iHP);
	}
	const std::string param = pisex::FormatString("%i %i", iTarget, iHP);
	g_pAdminApi->SendAction(iSlot, "hp", param.c_str());
}

void FunCommandsPlugin::SetArmor(int iSlot, int iTarget, int iArmor) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	SetClientArmor(iTarget, iArmor);
	if (GetMessageType() == 2) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("AdminArmorMessage"), AdminName(iSlot),
							  pisex::g_pPlayers->GetPlayerName(iTarget), iArmor);
	} else if (GetMessageType() == 1) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("ArmorMessage"), pisex::g_pPlayers->GetPlayerName(iTarget), iArmor);
	}
	const std::string param = pisex::FormatString("%i %i", iTarget, iArmor);
	g_pAdminApi->SendAction(iSlot, "armor", param.c_str());
}

void FunCommandsPlugin::SetScale(int iSlot, int iTarget, float fScale) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	SetClientScale(iTarget, fScale);
	if (GetMessageType() == 2) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("AdminScaleMessage"), AdminName(iSlot),
							  pisex::g_pPlayers->GetPlayerName(iTarget), fScale);
	} else if (GetMessageType() == 1) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("ScaleMessage"), pisex::g_pPlayers->GetPlayerName(iTarget), fScale);
	}
	const std::string param = pisex::FormatString("%i %f", iTarget, fScale);
	g_pAdminApi->SendAction(iSlot, "scale", param.c_str());
}

void FunCommandsPlugin::SetSpeed(int iSlot, int iTarget, float fSpeed) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	SetClientSpeed(iTarget, fSpeed);
	if (GetMessageType() == 2) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("AdminSpeedMessage"), AdminName(iSlot),
							  pisex::g_pPlayers->GetPlayerName(iTarget), fSpeed);
	} else if (GetMessageType() == 1) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("SpeedMessage"), pisex::g_pPlayers->GetPlayerName(iTarget), fSpeed);
	}
	const std::string param = pisex::FormatString("%i %f", iTarget, fSpeed);
	g_pAdminApi->SendAction(iSlot, "speed", param.c_str());
}

void FunCommandsPlugin::SetGod(int iSlot, int iTarget, bool bGod) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	// NOTE: the original's messages are inverted on purpose ("...GodMessageDisable" is used when
	// god is *enabled*, because god mode means m_bTakesDamage == false); kept as is.
	SetTakesDamage(iTarget, bGod);
	if (GetMessageType() == 2) {
		const char* key = bGod ? "AdminGodMessageDisable" : "AdminGodMessageEnable";
		const std::string text = pisex::FormatString(g_pAdminApi->GetTranslation(key), AdminName(iSlot),
													 pisex::g_pPlayers->GetPlayerName(iTarget));
		pisex::PrintToChatAll("%s", text.c_str());
	} else if (GetMessageType() == 1) {
		const char* key = bGod ? "GodMessageDisable" : "GodMessageEnable";
		const std::string text = pisex::FormatString(g_pAdminApi->GetTranslation(key),
													 pisex::g_pPlayers->GetPlayerName(iTarget));
		pisex::PrintToChatAll("%s", text.c_str());
	}
	const std::string param = pisex::FormatString("%i %i", iTarget, bGod ? 1 : 0);
	g_pAdminApi->SendAction(iSlot, "god", param.c_str());
}

void FunCommandsPlugin::SetFreezy(int iSlot, int iTarget, bool bFreeze) {
	// IPlayersApi::SetMoveType of the original (MOVETYPE_OBSOLETE / MOVETYPE_WALK). The pawn guard
	// keeps s2sdk's "Cannot execute 'void SetClientMoveType(int, MoveType_t)' on invalid player
	// pawn" warning out of the console on the command paths, where FindUser can hand back a slot
	// whose pawn is gone (a dead player); the engine call does nothing there either.
	if (TargetUsable(iTarget)) {
		SetClientMoveType(iTarget, bFreeze ? kMoveTypeObsolete : kMoveTypeWalk);
	}
	if (GetMessageType() == 2) {
		const char* key = bFreeze ? "AdminFreezeMessage" : "AdminUnfreezeMessage";
		const std::string text = pisex::FormatString(g_pAdminApi->GetTranslation(key), AdminName(iSlot),
													 pisex::g_pPlayers->GetPlayerName(iTarget));
		pisex::PrintToChatAll("%s", text.c_str());
	} else if (GetMessageType() == 1) {
		const char* key = bFreeze ? "FreezeMessage" : "UnfreezeMessage";
		const std::string text = pisex::FormatString(g_pAdminApi->GetTranslation(key),
													 pisex::g_pPlayers->GetPlayerName(iTarget));
		pisex::PrintToChatAll("%s", text.c_str());
	}
	const std::string param = std::to_string(iTarget);
	g_pAdminApi->SendAction(iSlot, bFreeze ? "freeze" : "unfreeze", param.c_str());
}

void FunCommandsPlugin::BlindPlayer(int iSlot, int iTarget, bool bBlind) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	const float curtime = pisex::GetCGlobalVars()->curtime;
	if (bBlind) {
		// m_flFlashDuration/m_flFlashMaxAlpha are networked (the original marked them changed);
		// the two blind times are plain (non-networked) GameTime_t fields.
		SetPawnFloat(iTarget, "m_flFlashDuration", 9999.0f, true);
		SetPawnFloat(iTarget, "m_flFlashMaxAlpha", 255.0f, true);
		SetPawnFloat(iTarget, "m_blindUntilTime", curtime + 9999.0f, false);
		SetPawnFloat(iTarget, "m_blindStartTime", curtime, false);
		if (GetMessageType() == 2) {
			pisex::PrintToChatAll(g_pAdminApi->GetTranslation("AdminBlindMessage"), AdminName(iSlot),
								  pisex::g_pPlayers->GetPlayerName(iTarget));
		} else if (GetMessageType() == 1) {
			pisex::PrintToChatAll(g_pAdminApi->GetTranslation("BlindMessage"),
								  pisex::g_pPlayers->GetPlayerName(iTarget));
		}
	} else {
		SetPawnFloat(iTarget, "m_blindUntilTime", curtime - 1.0f, false);
		if (GetMessageType() == 2) {
			pisex::PrintToChatAll(g_pAdminApi->GetTranslation("AdminUnblindMessage"), AdminName(iSlot),
								  pisex::g_pPlayers->GetPlayerName(iTarget));
		} else if (GetMessageType() == 1) {
			pisex::PrintToChatAll(g_pAdminApi->GetTranslation("UnblindMessage"),
								  pisex::g_pPlayers->GetPlayerName(iTarget));
		}
	}
	const std::string param = pisex::FormatString("%i %i", iTarget, IsBlinded(iTarget) ? 1 : 0);
	g_pAdminApi->SendAction(iSlot, "blind", param.c_str());
}

void FunCommandsPlugin::BuryPlayer(int iSlot, int iTarget) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	if (!__s2sdk_GetClientAbsOrigin || !__s2sdk_GetClientAbsAngles || !__s2sdk_GetClientAbsVelocity) {
		return; // without the pawn transform the teleport would be a jump to the origin
	}
	plg::vec3 origin{};
	plg::vec3 angles{};
	plg::vec3 velocity{};
	try {
		origin = s2sdk::GetClientAbsOrigin(iTarget);
		angles = s2sdk::GetClientAbsAngles(iTarget);
		velocity = s2sdk::GetClientAbsVelocity(iTarget);
	} catch (...) {
		return;
	}
	origin.z -= 10.0f;
	pisex::Teleport(iTarget, &origin, &angles, &velocity);
	if (GetMessageType() == 2) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("AdminBuryMessage"), AdminName(iSlot),
							  pisex::g_pPlayers->GetPlayerName(iTarget));
	} else if (GetMessageType() == 1) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("BuryMessage"), pisex::g_pPlayers->GetPlayerName(iTarget));
	}
	const std::string param = std::to_string(iTarget);
	g_pAdminApi->SendAction(iSlot, "bury", param.c_str());
}

void FunCommandsPlugin::UnburyPlayer(int iSlot, int iTarget) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	if (!__s2sdk_GetClientAbsOrigin || !__s2sdk_GetClientAbsAngles || !__s2sdk_GetClientAbsVelocity) {
		return;
	}
	plg::vec3 origin{};
	plg::vec3 angles{};
	plg::vec3 velocity{};
	try {
		origin = s2sdk::GetClientAbsOrigin(iTarget);
		angles = s2sdk::GetClientAbsAngles(iTarget);
		velocity = s2sdk::GetClientAbsVelocity(iTarget);
	} catch (...) {
		return;
	}
	origin.z += 10.0f;
	pisex::Teleport(iTarget, &origin, &angles, &velocity);
	if (GetMessageType() == 2) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("AdminUnburyMessage"), AdminName(iSlot),
							  pisex::g_pPlayers->GetPlayerName(iTarget));
	} else if (GetMessageType() == 1) {
		pisex::PrintToChatAll(g_pAdminApi->GetTranslation("UnburyMessage"), pisex::g_pPlayers->GetPlayerName(iTarget));
	}
	const std::string param = std::to_string(iTarget);
	g_pAdminApi->SendAction(iSlot, "unbury", param.c_str());
}

// --- menus --------------------------------------------------------------------------------------

void FunCommandsPlugin::ReopenPlayerList(int iSlot, const char* identity) {
	if (iSlot < 0 || iSlot >= kMaxSlots || !identity) {
		return;
	}
	SetLastItem(iSlot, identity);
	ShowPlayerList(iSlot, identity);
}

void FunCommandsPlugin::ShowPlayerList(int iSlot, const std::string& identity) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	pisex::Menu hMenu;
	pisex::SetTitleMenu(hMenu, g_pAdminApi->GetTranslation("SelectPlayer"));

	for (int i = 0; i < kMaxSlots; ++i) {
		// CCSPlayerController::FromSlot() + GetPlayerPawn() + IsAlive() of the original.
		if (pisex::GetPlayerPawn(i) == 0 || !pisex::IsAlive(i)) {
			continue;
		}
		const char* name = pisex::g_pPlayers->GetPlayerName(i);
		const std::string slot = std::to_string(i);
		std::string label;
		if (identity == "freeze") {
			label = pisex::FormatString("%s [%s]", name, IsFrozen(i) ? "ON" : "OFF");
		} else if (identity == "gravity") {
			label = pisex::FormatString("%s [%.1fx]", name, GetClientGravity(i));
		} else if (identity == "hp") {
			label = pisex::FormatString("%s [%i]", name, pisex::g_pPlayers->GetHealth(i));
		} else if (identity == "armor") {
			label = pisex::FormatString("%s [%i]", name, pisex::g_pPlayers->GetArmor(i));
		} else if (identity == "speed") {
			label = pisex::FormatString("%s [%.1fx]", name, GetClientSpeed(i));
		} else if (identity == "scale") {
			label = pisex::FormatString("%s [%.1fx]", name, GetClientScale(i));
		} else if (identity == "god") {
			label = pisex::FormatString("%s [%s]", name, TakesDamage(i) ? "OFF" : "ON");
		} else if (identity == "blind") {
			label = pisex::FormatString("%s [%s]", name, IsBlinded(i) ? "ON" : "OFF");
		} else if (identity == "bury" || identity == "unbury") {
			// bury/unbury show the plain name, like the original.
			label = name ? name : "";
		} else {
			continue; // unknown identity: the original added no item at all
		}
		pisex::AddItemMenu(hMenu, slot.c_str(), label.c_str());
	}

	pisex::SetBackMenu(hMenu, true);
	pisex::SetExitMenu(hMenu, true);
	pisex::SetCallback(hMenu, [](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (iSlot < 0 || iSlot >= kMaxSlots) {
			return;
		}
		if (iItem < 7) {
			const int iTarget = szBack ? std::atoi(szBack) : -1;
			if (!TargetUsable(iTarget)) {
				return;
			}
			const std::string identity = g_funCommandsPlugin.LastItem(iSlot);
			if (identity == "freeze") {
				g_funCommandsPlugin.SetFreezy(iSlot, iTarget, !IsFrozen(iTarget));
				g_funCommandsPlugin.ReopenPlayerList(iSlot, "freeze");
			} else if (identity == "gravity") {
				g_funCommandsPlugin.ShowGravityMenu(iSlot, iTarget);
			} else if (identity == "hp") {
				g_funCommandsPlugin.ShowBaseStatMenu(iSlot, iTarget, true);
			} else if (identity == "armor") {
				g_funCommandsPlugin.ShowBaseStatMenu(iSlot, iTarget, false);
			} else if (identity == "speed") {
				g_funCommandsPlugin.ShowSpeedMenu(iSlot, iTarget, true);
			} else if (identity == "scale") {
				g_funCommandsPlugin.ShowSpeedMenu(iSlot, iTarget, false);
			} else if (identity == "god") {
				g_funCommandsPlugin.SetGod(iSlot, iTarget, !TakesDamage(iTarget));
				g_funCommandsPlugin.ReopenPlayerList(iSlot, "god");
			} else if (identity == "bury") {
				g_funCommandsPlugin.BuryPlayer(iSlot, iTarget);
				g_funCommandsPlugin.ReopenPlayerList(iSlot, "bury");
			} else if (identity == "unbury") {
				g_funCommandsPlugin.UnburyPlayer(iSlot, iTarget);
				g_funCommandsPlugin.ReopenPlayerList(iSlot, "unbury");
			} else if (identity == "blind") {
				g_funCommandsPlugin.BlindPlayer(iSlot, iTarget, !IsBlinded(iTarget));
				g_funCommandsPlugin.ReopenPlayerList(iSlot, "blind");
			}
		} else if (iItem == 7) {
			// Back: the original's ShowAdminLastCategoryMenu(slot). The admin core opens its own
			// s2sdk menu, and this callback may still run inside the MenuManager lock, so the call
			// is made from the next frame.
			pisex::NextFrame([iSlot]() {
				try {
					g_pAdminApi->ShowAdminLastCategoryMenu(iSlot);
				} catch (...) {
				}
			});
		}
	});
	pisex::DisplayPlayerMenu(hMenu, iSlot);
}

void FunCommandsPlugin::ShowBaseStatMenu(int iSlot, int iTarget, bool bHealth) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	pisex::Menu hMenu;
	const int iValue = bHealth ? pisex::g_pPlayers->GetHealth(iTarget) : pisex::g_pPlayers->GetArmor(iTarget);
	pisex::SetTitleMenu(hMenu, pisex::FormatString("%s [%i]", pisex::g_pPlayers->GetPlayerName(iTarget), iValue).c_str());
	for (const char* value : {"1", "5", "10", "20", "50", "100", "200", "500", "1000"}) {
		pisex::AddItemMenu(hMenu, value, value);
	}
	pisex::SetBackMenu(hMenu, true);
	pisex::SetExitMenu(hMenu, true);
	pisex::SetCallback(hMenu, [iTarget, bHealth](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (iSlot < 0 || iSlot >= kMaxSlots) {
			return;
		}
		if (iItem < 7) {
			const int iNewValue = szBack ? std::atoi(szBack) : 0;
			if (bHealth) {
				g_funCommandsPlugin.SetHP(iSlot, iTarget, iNewValue);
			} else {
				g_funCommandsPlugin.SetArmor(iSlot, iTarget, iNewValue);
			}
			g_funCommandsPlugin.ShowBaseStatMenu(iSlot, iTarget, bHealth);
		} else if (iItem == 7) {
			// Back: re-open the player list (the original re-entered OnItemSelect).
			g_funCommandsPlugin.ReopenPlayerList(iSlot, bHealth ? "hp" : "armor");
		}
	});
	pisex::DisplayPlayerMenu(hMenu, iSlot);
}

void FunCommandsPlugin::ShowSpeedMenu(int iSlot, int iTarget, bool bSpeed) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	pisex::Menu hMenu;
	const float flValue = bSpeed ? GetClientSpeed(iTarget) : GetClientScale(iTarget);
	pisex::SetTitleMenu(hMenu, pisex::FormatString("%s [%.1fx]", pisex::g_pPlayers->GetPlayerName(iTarget), flValue).c_str());
	// value, label - same pairs as the original.
	struct Option {
		const char* value;
		const char* label;
	};
	for (const Option& option :
		 std::array<Option, 10>{{{"0.1", "0.1x"}, {"0.2", "0.2x"}, {"0.5", "0.5x"}, {"1.0", "1x"}, {"1.1", "1.1x"},
								 {"1.2", "1.2x"}, {"1.5", "1.5x"}, {"2.0", "2x"}, {"5.0", "5x"}, {"10.0", "10x"}}}) {
		pisex::AddItemMenu(hMenu, option.value, option.label);
	}
	pisex::SetBackMenu(hMenu, true);
	pisex::SetExitMenu(hMenu, true);
	pisex::SetCallback(hMenu, [iTarget, bSpeed](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (iSlot < 0 || iSlot >= kMaxSlots) {
			return;
		}
		if (iItem < 7) {
			const float flNewValue = szBack ? static_cast<float>(std::atof(szBack)) : 0.0f;
			if (bSpeed) {
				g_funCommandsPlugin.SetSpeed(iSlot, iTarget, flNewValue);
			} else {
				g_funCommandsPlugin.SetScale(iSlot, iTarget, flNewValue);
			}
			g_funCommandsPlugin.ShowSpeedMenu(iSlot, iTarget, bSpeed);
		} else if (iItem == 7) {
			g_funCommandsPlugin.ReopenPlayerList(iSlot, bSpeed ? "speed" : "scale");
		}
	});
	pisex::DisplayPlayerMenu(hMenu, iSlot);
}

void FunCommandsPlugin::ShowGravityMenu(int iSlot, int iTarget) {
	if (!TargetUsable(iTarget)) {
		return;
	}
	pisex::Menu hMenu;
	pisex::SetTitleMenu(hMenu, pisex::FormatString("%s [%.1fx]", pisex::g_pPlayers->GetPlayerName(iTarget),
												   GetClientGravity(iTarget))
									  .c_str());
	struct Option {
		const char* value;
		const char* label;
	};
	for (const Option& option :
		 std::array<Option, 12>{{{"0.1", "0.1x"}, {"0.2", "0.2x"}, {"0.5", "0.5x"}, {"0.7", "0.7x"}, {"0.8", "0.8x"},
								 {"1.0", "1x"}, {"1.1", "1.1x"}, {"1.2", "1.2x"}, {"1.5", "1.5x"}, {"2.0", "2x"},
								 {"5.0", "5x"}, {"10.0", "10x"}}}) {
		pisex::AddItemMenu(hMenu, option.value, option.label);
	}
	pisex::SetBackMenu(hMenu, true);
	pisex::SetExitMenu(hMenu, true);
	pisex::SetCallback(hMenu, [iTarget](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (iSlot < 0 || iSlot >= kMaxSlots) {
			return;
		}
		if (iItem < 7) {
			const float flNewValue = szBack ? static_cast<float>(std::atof(szBack)) : 0.0f;
			g_funCommandsPlugin.SetGravity(iSlot, iTarget, flNewValue);
			g_funCommandsPlugin.ShowGravityMenu(iSlot, iTarget);
		} else if (iItem == 7) {
			g_funCommandsPlugin.ReopenPlayerList(iSlot, "gravity");
		}
	});
	pisex::DisplayPlayerMenu(hMenu, iSlot);
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_funCommandsPlugin)
