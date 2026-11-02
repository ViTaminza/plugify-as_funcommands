// as_funcommands tests — no engine: the plugin TU is compiled in (its handlers live in an
// anonymous namespace) and every __s2sdk_* / __admin_system_* / __hudmenu_* binding slot is a
// recording fake.
//
// Covered:
//   * start and command ownership (PITFALLS C11/A7): the twelve commands are created with
//     ClientCanExecute|Release (a client console reaches the permission check), a chat "!mm_god"
//     is left to s2sdk (Continue), the console bypass is strictly caller == -1 (-2/out-of-range
//     fail closed with no output and no client call), OnPluginEnd removes a name this plugin
//     created with RemoveCommand but only detaches a listener from a pre-existing name, and a
//     start without the removal bindings registers nothing at all;
//   * permissions and the admin-readiness gate (API 1.1.0): a denied client never reaches an
//     action or SendAction; IsClientAdminDataReady(slot, live SteamID64) must be true first, a
//     pending/failed/missing readiness denies, a revoked permission stops an already-open menu
//     selection, and only slot -1 is the server-console bypass;
//   * signed CEntityHandle semantics (PITFALLS C1): negative controller and pawn handles are valid
//     and reach commands, menus and delayed actions end-to-end; only 0/-1 (and engine-rejected
//     handles) fail closed; the menu encode/decode round-trips the full int32 range; a bot's
//     SteamID64 is never read and s2sdk's "<unknown>" name becomes "";
//   * all twelve commands end-to-end: FindUser by name/SteamID64/slot, value-length rejection,
//     usage lines from the core's phrases, the exact chat phrases (message types 0/1/2), the
//     SendAction payloads, the move type / health / armor / speed / gravity / scale setters, the
//     raw m_bTakesDamage / flash / blind field writes with their widths, changeState flags and
//     chain offsets, and the bury/unbury teleport (z -/+ 10, angles and velocity preserved);
//   * both timed commands through the OnPluginUpdate accumulators (PITFALLS C9): no s2sdk
//     CreateTimer call ever, the effect fires only after the deadline, a reused target slot keeps
//     its new occupant untouched, a disconnected target is skipped, a reused admin slot still
//     unfreezes the target but announces nothing in the new occupant's name, and a map change
//     drops the pending deadlines (no client call in the map-change window);
//   * the native menus (PITFALLS C10): the player list shows exactly the live, alive, identified
//     players with the per-item state labels, pagination 5, exit+back buttons and no auto-close;
//     MenuId 0 is burnt and re-created; selections never act inside the handler (they are applied
//     by OnPluginUpdate); the freeze/god/bury/unbury/blind items toggle and reopen the list; the
//     five value menus carry the original values and re-open themselves; back from the list calls
//     the admin core's ShowAdminLastCategoryMenu and back from a value menu reopens the list; the
//     exact pisexcompat menu-type table (hudmenu choice first, HUD_LAYOUT -> hud/button);
//   * PITFALLS B1/B2 — the map gate: zero client/entity/schema calls before the first map, after
//     OnMapEnd and when the plugin was loaded into the map-change window (an OnMapEnd seen before
//     the first update must not latch readiness); the mid-map latch requires all four map-listener
//     bindings and a real s2sdk::GetCurrentMap ("" / "<empty>" do not open it); OnMapEnd only
//     drops bookkeeping and the queued menus are destroyed outside the teardown;
//   * PITFALLS C6/C8 — schema offsets are resolved only with the gate open, cached only when
//     >= 0 (a failed lookup is retried; a chain offset > 0 only) and cleared on unload, so a
//     reload resolves them against the live map again;
//   * PITFALLS A7 — paired registration: no map listeners without both register/unregister pairs,
//     no commands without AddConsoleCommand + both removal paths, no admin core listener without
//     its Register/Unregister pair, no menu items without the s2sdk menu API; the category is
//     registered with a null display callback and every one of the ten items is re-registered
//     with null callbacks through the low-level binding on unload (a missing binding fails soft);
//   * safe destruction (PITFALLS B2/A7): a CancelClientMenu(Disconnect) refused (or lying) in a
//     map-down window is re-queried and the menu is switched to a builtin no-op-close type before
//     DestroyMenu (no backend close reaches the client layer); when even that is impossible the
//     request is parked and A7_UNRESOLVED is logged instead of blindly destroying or pretending;
//   * unload/reload: symmetric removal, open menus destroyed, a null admin_system, a null s2sdk
//     menu API and all bindings null at once still start, update and unload without a crash, and
//     stale callbacks are inert.
//
// Run by ./test.sh (build/tests/as_funcommands_tests).

#include "../src/plugin.cpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool bCondition, const std::string& what) {
	++g_checks;
	if (!bCondition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

plg::string PlgTest(const std::string& text) {
	return plg::string(text);
}

bool HasFlag(s2sdk::ConVarFlag flags, s2sdk::ConVarFlag flag) {
	return (static_cast<int64_t>(flags) & static_cast<int64_t>(flag)) != 0;
}

// --- the fake world -------------------------------------------------------------------------

constexpr int32_t kFakeSlots = kSlots;

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool fake = false;
	bool alive = true;
	bool hasController = true;
	bool hasPawn = true;
	bool hasHandleOverride = false;
	int32_t handleOverride = 0; // explicit PlayerSlotToEntHandle answer (0/-1/negative tests)
	uint32_t generation = 0;	// s2sdk's controller serial: a reconnect gets a new one
	bool hasPawnHandleOverride = false;
	int32_t pawnHandleOverride = 0;
	uint32_t pawnGeneration = 0;
	std::string name;
	uint64_t steamid = 0;
	s2sdk::MoveType moveType = s2sdk::MoveType::Walk;
	int32_t health = 100;
	int32_t armor = 0;
	float gravity = 1.0f;
	float speed = 1.0f;
	float scale = 1.0f;
	plg::vec3 origin{0.0f, 0.0f, 0.0f};
	plg::vec3 angles{0.0f, 0.0f, 0.0f};
	plg::vec3 velocity{0.0f, 0.0f, 0.0f};
	// raw schema fields
	float flashDuration = 0.0f;
	float flashMaxAlpha = 0.0f;
	float blindUntil = 0.0f;
	float blindStart = 0.0f;
	bool takesDamage = true;
};

std::vector<FakeClient> g_clients(kFakeSlots);

const FakeClient& FakeSlot(int32_t slot) {
	static const FakeClient empty;
	return slot >= 0 && slot < kFakeSlots ? g_clients[static_cast<std::size_t>(slot)] : empty;
}

FakeClient& MutableSlot(int32_t slot) {
	return g_clients[static_cast<std::size_t>(slot)];
}

// Every fake client/entity-layer call counts into this; the map-gate tests assert it stays 0.
int g_clientCalls = 0;
// Schema-layer calls are separate: GetSchemaOffset may not run in the map-change window either.
int g_schemaCalls = 0;
// Only the GetSchemaOffset/GetSchemaChainOffset lookups (not the field reads/writes).
int g_offsetLookups = 0;
int g_createTimerCalls = 0;
bool g_throwOnClientName = false;
bool g_throwOnSchemaOffset = false;
bool g_throwOnSetGravity = false;
bool g_steamidReadForBot = false;
bool g_nameReadForDisconnected = false;

void ResetClientCounters() {
	g_clientCalls = 0;
	g_steamidReadForBot = false;
	g_nameReadForDisconnected = false;
}

void ResetSchemaCounters() {
	g_schemaCalls = 0;
	g_offsetLookups = 0;
}

FakeClient& MakeHuman(int32_t slot, const std::string& name, uint64_t steamid, bool alive = true) {
	FakeClient& client = MutableSlot(slot);
	client = FakeClient{};
	client.connected = true;
	client.inGame = true;
	client.alive = alive;
	client.name = name;
	client.steamid = steamid;
	client.hasPawn = alive;
	return client;
}

FakeClient& MakeBot(int32_t slot, const std::string& name, bool alive = true) {
	FakeClient& client = MakeHuman(slot, name, 0, alive);
	client.fake = true;
	return client;
}

void ClearClients() {
	g_clients.assign(kFakeSlots, FakeClient{});
}

// index | serial << 15, the s2sdk CEntityHandle layout. The serial's bit 31 makes the handle
// negative and that is still a valid handle, so the bit pattern is built unsigned-bitcast.
int32_t MakeHandle(int32_t index, uint32_t serial) {
	const uint32_t value = static_cast<uint32_t>(index) | (serial << 15);
	return std::bit_cast<int32_t>(value);
}

int32_t HandleIndex(int32_t handle) {
	return static_cast<int32_t>(std::bit_cast<uint32_t>(handle) & 0x7FFFu);
}

int32_t HandleSlot(int32_t handle) {
	const int32_t index = HandleIndex(handle);
	if (index >= 1 && index <= kFakeSlots) {
		return index - 1; // a controller handle
	}
	if (index > 100 && index <= 100 + kFakeSlots) {
		return index - 101; // a pawn handle (kPawnIndexBase + slot + 1)
	}
	return -1;
}

constexpr uintptr_t kPawnPointerBase = 0x10000;
constexpr int32_t kPawnIndexBase = 100;

void* PawnPointerOfSlot(int32_t slot) {
	return reinterpret_cast<void*>(kPawnPointerBase + static_cast<uintptr_t>(slot));
}

int32_t SlotOfPawnPointer(void* pointer) {
	const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
	if (value < kPawnPointerBase || value >= kPawnPointerBase + static_cast<uintptr_t>(kFakeSlots)) {
		return -1;
	}
	return static_cast<int32_t>(value - kPawnPointerBase);
}

// --- the fake map lifecycle ------------------------------------------------------------------

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;
std::string g_currentMap = "de_dust2";

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_onMapStart = callback;
	++g_mapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
	++g_mapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_onMapEnd = callback;
	++g_mapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
	++g_mapEndUnregisters;
}

void FakeMapStart() {
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void FakeMapEnd() {
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

plg::string FakeGetCurrentMap() {
	return PlgTest(g_currentMap);
}

// --- the fake client/entity layer ------------------------------------------------------------

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).fake;
}

bool FakeIsClientAlive(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).alive;
}

plg::string FakeGetClientName(int32_t slot) {
	++g_clientCalls;
	if (g_throwOnClientName) {
		throw std::runtime_error("fake name failure");
	}
	const FakeClient& client = FakeSlot(slot);
	if (!client.connected) {
		g_nameReadForDisconnected = true;
	}
	return PlgTest(client.name.empty() ? "<unknown>" : client.name);
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_clientCalls;
	const FakeClient& client = FakeSlot(slot);
	if (client.fake) {
		g_steamidReadForBot = true;
	}
	return client.steamid;
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_clientCalls;
	const FakeClient& client = FakeSlot(slot);
	if (!client.connected || !client.hasController) {
		return -1;
	}
	if (client.hasHandleOverride) {
		return client.handleOverride;
	}
	return MakeHandle(slot + 1, client.generation);
}

std::set<int32_t> g_rejectedHandles;
std::set<uintptr_t> g_rejectedPointers;

bool FakeIsValidEntHandle(int32_t handle) {
	++g_clientCalls;
	if (handle == 0 || handle == -1) {
		return false;
	}
	return g_rejectedHandles.count(handle) == 0;
}

bool FakeIsValidEntPointer(void* pointer) {
	++g_clientCalls;
	if (pointer == nullptr) {
		return false;
	}
	return g_rejectedPointers.count(reinterpret_cast<uintptr_t>(pointer)) == 0;
}

void* FakeGetClientPawn(int32_t slot) {
	++g_clientCalls;
	const FakeClient& client = FakeSlot(slot);
	if (!client.connected || !client.hasController || !client.hasPawn) {
		return nullptr;
	}
	return PawnPointerOfSlot(slot);
}

int32_t FakeEntPointerToEntHandle(void* pointer) {
	++g_clientCalls;
	const int32_t slot = SlotOfPawnPointer(pointer);
	if (slot < 0) {
		return -1;
	}
	const FakeClient& client = FakeSlot(slot);
	if (!client.connected || !client.hasController || !client.hasPawn) {
		return -1;
	}
	if (client.hasPawnHandleOverride) {
		return client.pawnHandleOverride;
	}
	return MakeHandle(kPawnIndexBase + slot + 1, client.pawnGeneration);
}

int32_t FakeEntPointerToEntIndex(void* pointer) {
	++g_clientCalls;
	const int32_t slot = SlotOfPawnPointer(pointer);
	return slot >= 0 ? kPawnIndexBase + slot + 1 : -1;
}

int32_t FakeEntIndexToEntHandle(int32_t index) {
	++g_clientCalls;
	if (index <= kPawnIndexBase) {
		return -1;
	}
	const int32_t slot = index - kPawnIndexBase - 1;
	return slot >= 0 && slot < kFakeSlots ? FakeEntPointerToEntHandle(PawnPointerOfSlot(slot)) : -1;
}

float FakeGetClientGravity(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).gravity;
}

void FakeSetClientGravity(int32_t slot, float value) {
	++g_clientCalls;
	if (g_throwOnSetGravity) {
		throw std::runtime_error("fake gravity failure");
	}
	if (slot >= 0 && slot < kFakeSlots) {
		MutableSlot(slot).gravity = value;
	}
}

float FakeGetClientSpeed(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).speed;
}

void FakeSetClientSpeed(int32_t slot, float value) {
	++g_clientCalls;
	if (slot >= 0 && slot < kFakeSlots) {
		MutableSlot(slot).speed = value;
	}
}

float FakeGetClientAbsScale(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).scale;
}

void FakeSetClientAbsScale(int32_t slot, float value) {
	++g_clientCalls;
	if (slot >= 0 && slot < kFakeSlots) {
		MutableSlot(slot).scale = value;
	}
}

int32_t FakeGetClientHealth(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).health;
}

void FakeSetClientHealth(int32_t slot, int32_t value) {
	++g_clientCalls;
	if (slot >= 0 && slot < kFakeSlots) {
		MutableSlot(slot).health = value;
	}
}

int32_t FakeGetClientArmor(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).armor;
}

void FakeSetClientArmor(int32_t slot, int32_t value) {
	++g_clientCalls;
	if (slot >= 0 && slot < kFakeSlots) {
		MutableSlot(slot).armor = value;
	}
}

s2sdk::MoveType FakeGetClientMoveType(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).moveType;
}

void FakeSetClientMoveType(int32_t slot, s2sdk::MoveType type) {
	++g_clientCalls;
	if (slot >= 0 && slot < kFakeSlots) {
		MutableSlot(slot).moveType = type;
	}
}

plg::vec3 FakeGetClientAbsOrigin(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).origin;
}

plg::vec3 FakeGetClientAbsAngles(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).angles;
}

plg::vec3 FakeGetClientAbsVelocity(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).velocity;
}

struct FakeTeleport {
	int32_t slot = 0;
	plg::vec3 origin{};
	plg::vec3 angles{};
	plg::vec3 velocity{};
};

std::vector<FakeTeleport> g_teleports;

void FakeTeleportClient(int32_t slot, const plg::vec3& origin, const plg::vec3& angles,
						const plg::vec3& velocity) {
	++g_clientCalls;
	g_teleports.push_back(FakeTeleport{slot, origin, angles, velocity});
	if (slot >= 0 && slot < kFakeSlots) {
		MutableSlot(slot).origin = origin;
	}
}

float g_gameTime = 100.0f;

float FakeGetGameTime() {
	return g_gameTime;
}

// --- the fake raw schema layer ---------------------------------------------------------------

std::map<std::string, int32_t> g_fieldOffsets;
std::map<std::string, int32_t> g_chainOffsets;
int g_flashDurationLookups = 0;

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& memberName) {
	++g_clientCalls;
	++g_schemaCalls;
	++g_offsetLookups;
	if (g_throwOnSchemaOffset) {
		throw std::runtime_error("fake schema offset failure");
	}
	const std::string key = ToStd(className) + "::" + ToStd(memberName);
	if (key == "CCSPlayerPawnBase::m_flFlashDuration") {
		++g_flashDurationLookups;
	}
	const auto it = g_fieldOffsets.find(key);
	return it != g_fieldOffsets.end() ? it->second : -1;
}

int32_t FakeGetSchemaChainOffset(const plg::string& className) {
	++g_clientCalls;
	++g_schemaCalls;
	++g_offsetLookups;
	if (g_throwOnSchemaOffset) {
		throw std::runtime_error("fake schema chain failure");
	}
	const auto it = g_chainOffsets.find(ToStd(className));
	return it != g_chainOffsets.end() ? it->second : -1;
}

struct FakeSchemaWrite {
	bool bFloat = true;
	int32_t handle = 0;
	int32_t offset = 0;
	double flValue = 0.0;
	int64_t iValue = 0;
	int32_t size = 0;
	bool changeState = false;
	int32_t chainOffset = 0;
};

std::vector<FakeSchemaWrite> g_schemaWrites;

enum class FakeField { None, FlashDuration, FlashMaxAlpha, BlindUntil, BlindStart, TakesDamage };

FakeField FieldForOffset(int32_t offset) {
	const auto at = [offset](const char* key) {
		const auto it = g_fieldOffsets.find(key);
		return it != g_fieldOffsets.end() && it->second == offset;
	};
	if (at("CCSPlayerPawnBase::m_flFlashDuration")) {
		return FakeField::FlashDuration;
	}
	if (at("CCSPlayerPawnBase::m_flFlashMaxAlpha")) {
		return FakeField::FlashMaxAlpha;
	}
	if (at("CCSPlayerPawnBase::m_blindUntilTime")) {
		return FakeField::BlindUntil;
	}
	if (at("CCSPlayerPawnBase::m_blindStartTime")) {
		return FakeField::BlindStart;
	}
	if (at("CBaseEntity::m_bTakesDamage")) {
		return FakeField::TakesDamage;
	}
	return FakeField::None;
}

double FakeGetEntDataFloat(int32_t handle, int32_t offset, int32_t size) {
	++g_clientCalls;
	++g_schemaCalls;
	const int32_t slot = HandleSlot(handle);
	if (slot < 0 || size != static_cast<int32_t>(sizeof(float))) {
		return 0.0;
	}
	const FakeClient& client = FakeSlot(slot);
	switch (FieldForOffset(offset)) {
	case FakeField::FlashDuration:
		return client.flashDuration;
	case FakeField::FlashMaxAlpha:
		return client.flashMaxAlpha;
	case FakeField::BlindUntil:
		return client.blindUntil;
	case FakeField::BlindStart:
		return client.blindStart;
	default:
		return 0.0;
	}
}

int64_t FakeGetEntData(int32_t handle, int32_t offset, int32_t size) {
	++g_clientCalls;
	++g_schemaCalls;
	const int32_t slot = HandleSlot(handle);
	if (slot < 0) {
		return 0;
	}
	const FakeClient& client = FakeSlot(slot);
	if (FieldForOffset(offset) == FakeField::TakesDamage &&
		size == static_cast<int32_t>(sizeof(bool))) {
		return client.takesDamage ? 1 : 0;
	}
	return 0;
}

void FakeSetEntDataFloat(int32_t handle, int32_t offset, double value, int32_t size,
						 bool changeState, int32_t chainOffset) {
	++g_clientCalls;
	++g_schemaCalls;
	g_schemaWrites.push_back(
		FakeSchemaWrite{true, handle, offset, value, 0, size, changeState, chainOffset});
	const int32_t slot = HandleSlot(handle);
	if (slot < 0) {
		return;
	}
	FakeClient& client = MutableSlot(slot);
	switch (FieldForOffset(offset)) {
	case FakeField::FlashDuration:
		client.flashDuration = static_cast<float>(value);
		break;
	case FakeField::FlashMaxAlpha:
		client.flashMaxAlpha = static_cast<float>(value);
		break;
	case FakeField::BlindUntil:
		client.blindUntil = static_cast<float>(value);
		break;
	case FakeField::BlindStart:
		client.blindStart = static_cast<float>(value);
		break;
	default:
		break;
	}
}

void FakeSetEntData(int32_t handle, int32_t offset, int64_t value, int32_t size, bool changeState,
					int32_t chainOffset) {
	++g_clientCalls;
	++g_schemaCalls;
	g_schemaWrites.push_back(
		FakeSchemaWrite{false, handle, offset, 0.0, value, size, changeState, chainOffset});
	const int32_t slot = HandleSlot(handle);
	if (slot < 0) {
		return;
	}
	FakeClient& client = MutableSlot(slot);
	if (FieldForOffset(offset) == FakeField::TakesDamage &&
		size == static_cast<int32_t>(sizeof(bool))) {
		client.takesDamage = value != 0;
	}
}

// --- the fake output ------------------------------------------------------------------------

struct FakePrint {
	int32_t slot = 0;
	std::string text;
};

std::vector<FakePrint> g_chatPrints;
std::vector<std::string> g_chatAllPrints;
std::vector<FakePrint> g_consolePrints;
std::vector<std::string> g_serverPrints;

void FakePrintToChat(int32_t slot, const plg::string& text) {
	++g_clientCalls;
	g_chatPrints.push_back(FakePrint{slot, ToStd(text)});
}

void FakePrintToChatAll(const plg::string& text) {
	++g_clientCalls;
	g_chatAllPrints.push_back(ToStd(text));
}

void FakePrintToConsole(int32_t slot, const plg::string& text) {
	++g_clientCalls;
	g_consolePrints.push_back(FakePrint{slot, ToStd(text)});
}

void FakePrintToServer(const plg::string& text) {
	// Not a client call: it must stay reachable even while no map is up.
	g_serverPrints.push_back(ToStd(text));
}

void ResetPrints() {
	g_chatPrints.clear();
	g_chatAllPrints.clear();
	g_consolePrints.clear();
	g_serverPrints.clear();
}

bool HasChatAllPrint(const std::string& needle) {
	for (const std::string& line : g_chatAllPrints) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool HasConsolePrint(const std::string& needle) {
	for (const FakePrint& print : g_consolePrints) {
		if (print.text.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool HasServerPrint(const std::string& needle) {
	for (const std::string& line : g_serverPrints) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// --- the fake command manager ---------------------------------------------------------------

struct FakeCommand {
	std::string name;
	std::string description;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

struct FakeListener {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

std::vector<FakeCommand> g_addedCommands;
std::set<std::string> g_preexistingCommands;
std::vector<std::pair<std::string, s2sdk::ConCommandCallback>> g_removedCommands;
std::vector<FakeListener> g_addedListeners;
std::vector<FakeListener> g_removedListeners;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& description,
						   s2sdk::ConVarFlag flags, s2sdk::ConCommandCallback callback,
						   s2sdk::HookMode mode) {
	const std::string key = ToStd(name);
	g_addedCommands.push_back(FakeCommand{key, ToStd(description), flags, callback, mode});
	// false = the name already existed (the engine or another plugin); s2sdk then only attaches
	// the listener to that command.
	return g_preexistingCommands.count(key) == 0;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback callback) {
	g_removedCommands.emplace_back(ToStd(name), callback);
	return true;
}

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							s2sdk::HookMode mode) {
	g_addedListeners.push_back(FakeListener{ToStd(name), callback, mode});
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							   s2sdk::HookMode mode) {
	g_removedListeners.push_back(FakeListener{ToStd(name), callback, mode});
	return true;
}

const FakeCommand* FindAddedCommand(const std::string& name) {
	const FakeCommand* found = nullptr;
	for (const FakeCommand& command : g_addedCommands) {
		if (command.name == name) {
			found = &command; // the most recent registration wins
		}
	}
	return found;
}

bool RemovedCommand(const std::string& name) {
	for (const auto& [removedName, callback] : g_removedCommands) {
		(void)callback;
		if (removedName == name) {
			return true;
		}
	}
	return false;
}

bool RemovedCommandWith(const std::string& name, s2sdk::ConCommandCallback callback) {
	for (const auto& [removedName, removedCallback] : g_removedCommands) {
		if (removedName == name && removedCallback == callback) {
			return true;
		}
	}
	return false;
}

bool RemovedListener(const std::string& name, s2sdk::ConCommandCallback callback,
					 s2sdk::HookMode mode) {
	for (const FakeListener& listener : g_removedListeners) {
		if (listener.name == name && listener.callback == callback && listener.mode == mode) {
			return true;
		}
	}
	return false;
}

plg::vector<plg::string> ToArgs(std::initializer_list<const char*> words) {
	plg::vector<plg::string> args;
	for (const char* word : words) {
		args.emplace_back(word);
	}
	return args;
}

s2sdk::ResultType DispatchCommand(const std::string& name, int32_t caller,
								  s2sdk::ConCommandContext context,
								  std::initializer_list<const char*> args) {
	const FakeCommand* command = FindAddedCommand(name);
	Check(command != nullptr, "command \"" + name + "\" is registered with s2sdk");
	if (command == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	const plg::vector<plg::string> argv = ToArgs(args);
	return command->callback(caller, context, argv);
}

// --- the fake menu manager ------------------------------------------------------------------

struct FakeMenuItem {
	std::string info;
	std::string display;
	s2sdk::MenuItemStyle style = s2sdk::MenuItemStyle::Default;
};

struct FakeMenu {
	s2sdk::MenuId id = 0;
	std::string title;
	std::string type;
	s2sdk::MenuHandlerCallback handler = nullptr;
	std::vector<FakeMenuItem> items;
	int32_t pagination = 7;
	bool exitButton = true;
	bool backButton = false;
	bool closeOnSelect = true;
};

std::vector<FakeMenu> g_menus;
std::array<s2sdk::MenuId, kFakeSlots> g_clientMenu{};
std::array<int32_t, kFakeSlots> g_clientOffset{};
s2sdk::MenuId g_nextMenuId = 1;
bool g_burnFirstMenuId = false;
int g_createMenuCalls = 0;
bool g_createMenuFails = false;   // CreateMenu answers 0 even after the burn
bool g_addMenuItemFails = false;  // AddMenuItem refuses
bool g_displayMenuFails = false;  // DisplayMenu refuses
bool g_throwOnDisplayMenu = false;
int g_destroyMenuCalls = 0;
std::vector<s2sdk::MenuId> g_destroyedMenuIds;
std::set<std::string> g_menuTypes{"chat", "console", "centerhtml", "button"};
std::vector<std::pair<int32_t, s2sdk::MenuCancelReason>> g_cancelledMenus;
bool g_cancelFails = false;
bool g_cancelLies = false;
int g_liveBackendCloses = 0; // backend close() that would reach the client/entity layer
int g_setMenuTypeCalls = 0;
bool g_setMenuTypeFails = false;
int g_handlerCalls = 0;
int32_t g_lastMenuTimeout = -1;
bool g_menuZeroBurned = false;

FakeMenu* FindMenu(s2sdk::MenuId id) {
	for (FakeMenu& menu : g_menus) {
		if (menu.id == id) {
			return &menu;
		}
	}
	return nullptr;
}

void EndClientMenuFake(int32_t slot, s2sdk::MenuCancelReason reason) {
	const s2sdk::MenuId id = g_clientMenu[static_cast<std::size_t>(slot)];
	if (id == 0) {
		return;
	}
	g_clientMenu[static_cast<std::size_t>(slot)] = 0;
	g_clientOffset[static_cast<std::size_t>(slot)] = 0;
	const FakeMenu* menu = FindMenu(id);
	// s2sdk's CloseClientDisplay(notifyBackend = reason != Disconnect) calls the menu type's
	// close(); the builtin chat/console closers are no-ops (menu_types_builtin.cpp).
	if (reason != s2sdk::MenuCancelReason::Disconnect && menu != nullptr && menu->type != "chat" &&
		menu->type != "console") {
		++g_liveBackendCloses;
	}
	const s2sdk::MenuHandlerCallback handler = menu != nullptr ? menu->handler : nullptr;
	if (handler != nullptr) {
		++g_handlerCalls;
		handler(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(reason));
		handler(id, s2sdk::MenuAction::End, slot, 0);
	}
}

bool FakeDestroyMenu(s2sdk::MenuId id) {
	for (int32_t slot = 0; slot < kFakeSlots; ++slot) {
		if (g_clientMenu[static_cast<std::size_t>(slot)] == id) {
			EndClientMenuFake(slot, s2sdk::MenuCancelReason::Destroyed);
		}
	}
	for (auto it = g_menus.begin(); it != g_menus.end(); ++it) {
		if (it->id == id) {
			g_menus.erase(it);
			++g_destroyMenuCalls;
			g_destroyedMenuIds.push_back(id);
			if (id == 0) {
				g_menuZeroBurned = true;
			}
			return true;
		}
	}
	return false;
}

s2sdk::MenuId FakeCreateMenu(const plg::string& title, s2sdk::MenuHandlerCallback handler,
							 const plg::string& type) {
	++g_createMenuCalls;
	FakeMenu menu;
	menu.id = g_nextMenuId;
	if (g_createMenuFails) {
		return 0;
	}
	if (g_burnFirstMenuId && g_createMenuCalls == 1) {
		menu.id = 0; // the manager really hands out 0 for the first menu of the process
	} else {
		++g_nextMenuId;
	}
	menu.title = ToStd(title);
	menu.type = ToStd(type);
	menu.handler = handler;
	g_menus.push_back(std::move(menu));
	return g_menus.back().id;
}

int32_t FakeAddMenuItem(s2sdk::MenuId id, const plg::string& info, const plg::string& display,
						s2sdk::MenuItemStyle style) {
	if (g_addMenuItemFails) {
		return -1;
	}
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return -1;
	}
	menu->items.push_back(FakeMenuItem{ToStd(info), ToStd(display), style});
	return static_cast<int32_t>(menu->items.size()) - 1;
}

bool FakeSetMenuPagination(s2sdk::MenuId id, int32_t itemsPerPage) {
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return false;
	}
	menu->pagination = itemsPerPage;
	return true;
}

bool FakeSetMenuExitButton(s2sdk::MenuId id, bool enabled) {
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return false;
	}
	menu->exitButton = enabled;
	return true;
}

bool FakeSetMenuExitBackButton(s2sdk::MenuId id, bool enabled) {
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return false;
	}
	menu->backButton = enabled;
	return true;
}

bool FakeSetMenuCloseOnSelect(s2sdk::MenuId id, bool enabled) {
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return false;
	}
	menu->closeOnSelect = enabled;
	return true;
}

bool FakeDisplayMenu(s2sdk::MenuId id, int32_t slot, int32_t time) {
	g_lastMenuTimeout = time;
	if (g_throwOnDisplayMenu) {
		throw std::runtime_error("fake display failure");
	}
	if (g_displayMenuFails) {
		return false;
	}
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr || slot < 0 || slot >= kFakeSlots) {
		return false;
	}
	if (g_clientMenu[static_cast<std::size_t>(slot)] != 0) {
		EndClientMenuFake(slot, s2sdk::MenuCancelReason::Interrupted);
	}
	g_clientMenu[static_cast<std::size_t>(slot)] = id;
	g_clientOffset[static_cast<std::size_t>(slot)] = 0;
	if (menu->handler != nullptr) {
		++g_handlerCalls;
		menu->handler(id, s2sdk::MenuAction::Start, slot, 0);
	}
	return true;
}

s2sdk::MenuId FakeGetClientMenu(int32_t slot) {
	if (slot < 0 || slot >= kFakeSlots) {
		return 0;
	}
	return g_clientMenu[static_cast<std::size_t>(slot)];
}

int32_t FakeGetClientMenuOffset(int32_t slot) {
	if (slot < 0 || slot >= kFakeSlots) {
		return 0;
	}
	return g_clientOffset[static_cast<std::size_t>(slot)];
}

bool FakeCancelClientMenu(int32_t slot, s2sdk::MenuCancelReason reason) {
	if (slot < 0 || slot >= kFakeSlots || g_clientMenu[static_cast<std::size_t>(slot)] == 0) {
		return false;
	}
	if (g_cancelFails) {
		return false; // the manager refused but the display is still active
	}
	if (g_cancelLies) {
		return true; // contract violation: true, but the display stays active
	}
	g_cancelledMenus.emplace_back(slot, reason);
	EndClientMenuFake(slot, reason);
	return true;
}

bool FakeIsValidMenu(s2sdk::MenuId id) {
	return FindMenu(id) != nullptr;
}

bool FakeIsMenuTypeRegistered(const plg::string& name) {
	return g_menuTypes.count(ToStd(name)) != 0;
}

bool FakeSetMenuType(s2sdk::MenuId id, const plg::string& typeName) {
	++g_setMenuTypeCalls;
	if (g_setMenuTypeFails) {
		return false;
	}
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return false;
	}
	menu->type = ToStd(typeName);
	return true;
}

plg::string FakeGetMenuType(s2sdk::MenuId id) {
	const FakeMenu* menu = FindMenu(id);
	return menu != nullptr ? PlgTest(menu->type) : plg::string();
}

// --- the optional hudmenu plugin API --------------------------------------------------------

std::array<std::string, kFakeSlots> g_clientMenuTypes;
int g_hudmenuCalls = 0;

plg::string FakeGetClientMenuType(int32_t slot) {
	++g_hudmenuCalls;
	if (slot < 0 || slot >= kFakeSlots) {
		return {};
	}
	return PlgTest(g_clientMenuTypes[static_cast<std::size_t>(slot)]);
}

// --- the fake plg log -----------------------------------------------------------------------

std::vector<std::string> g_logs;

void FakeLog(std::string_view message, plg::Severity severity,
			 const plg::source_location& location) {
	(void)severity;
	(void)location;
	g_logs.emplace_back(message);
}

bool HasLog(const std::string& needle) {
	for (const std::string& line : g_logs) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// --- the fake admin_system exports ----------------------------------------------------------

std::map<int32_t, std::set<std::string>> g_permissions;
// slot -> the SteamID64 the core's asynchronous admin SELECT answered for.
std::map<int32_t, uint64_t> g_adminReadyFor;
std::map<std::string, std::string> g_translations;
std::set<int32_t> g_adminDataNotReady;
int32_t g_messageType = 0;
bool g_coreLoaded = true;

admin_system::CoreLoadedCallback g_coreLoadedCallback = nullptr;
int g_coreLoadedRegisters = 0;
int g_coreLoadedUnregisters = 0;

struct FakeCategory {
	std::string identity;
	std::string name;
	admin_system::CategoryDisplayCallback display = nullptr;
};

struct FakeItem {
	std::string identity;
	std::string name;
	std::string category;
	std::string flags;
	admin_system::ItemDisplayCallback display = nullptr;
	admin_system::ItemSelectCallback select = nullptr;
};

std::vector<FakeCategory> g_categories;
std::vector<FakeItem> g_items;
std::vector<int32_t> g_lastCategoryCalls;

struct FakeAction {
	int32_t slot = 0;
	std::string action;
	std::string param;
};

std::vector<FakeAction> g_actions;

float FakeGetPluginVersion() {
	return 1.1f;
}

plg::string FakeGetTranslation(const plg::string& key) {
	const auto it = g_translations.find(ToStd(key));
	return it != g_translations.end() ? PlgTest(it->second) : key;
}

int32_t FakeGetMessageType() {
	return g_messageType;
}

bool FakeIsCoreLoaded() {
	return g_coreLoaded;
}

bool FakeHasPermission(int32_t slot, const plg::string& permission) {
	const auto it = g_permissions.find(slot);
	return it != g_permissions.end() && it->second.count(ToStd(permission)) != 0;
}

bool FakeIsClientAdminDataReady(int32_t slot, uint64_t steamid) {
	if (slot < 0 || slot >= kFakeSlots || steamid == 0 || g_adminDataNotReady.count(slot) != 0) {
		return false;
	}
	// The core answered for the SteamID64 that was live when the load ran: a reclaimed slot is
	// not ready until the next SELECT.
	const auto it = g_adminReadyFor.find(slot);
	return it != g_adminReadyFor.end() && it->second == steamid &&
		   FakeSlot(slot).steamid == steamid;
}

void FakeSendAction(int32_t slot, const plg::string& action, const plg::string& param) {
	g_actions.push_back(FakeAction{slot, ToStd(action), ToStd(param)});
}

void FakeRegisterCategory(const plg::string& identity, const plg::string& name,
						  admin_system::CategoryDisplayCallback callback) {
	// The real core overwrites the entry with the same identity (AdminPlugin::RegisterCategory).
	for (FakeCategory& category : g_categories) {
		if (category.identity == ToStd(identity)) {
			category = FakeCategory{ToStd(identity), ToStd(name), callback};
			return;
		}
	}
	g_categories.push_back(FakeCategory{ToStd(identity), ToStd(name), callback});
}

void FakeRegisterItem(const plg::string& identity, const plg::string& name,
					  const plg::string& category, const plg::string& flags,
					  admin_system::ItemDisplayCallback display,
					  admin_system::ItemSelectCallback select) {
	// The core overwrites an item registered again (a reload path).
	for (FakeItem& item : g_items) {
		if (item.identity == ToStd(identity) && item.category == ToStd(category)) {
			item = FakeItem{ToStd(identity), ToStd(name), ToStd(category), ToStd(flags), display,
							select};
			return;
		}
	}
	g_items.push_back(
		FakeItem{ToStd(identity), ToStd(name), ToStd(category), ToStd(flags), display, select});
}

void FakeOnCoreLoadedRegister(admin_system::CoreLoadedCallback callback) {
	g_coreLoadedCallback = callback;
	++g_coreLoadedRegisters;
}

void FakeOnCoreLoadedUnregister(admin_system::CoreLoadedCallback callback) {
	if (g_coreLoadedCallback == callback) {
		g_coreLoadedCallback = nullptr;
	}
	++g_coreLoadedUnregisters;
}

void FakeShowAdminLastCategoryMenu(int32_t slot) {
	g_lastCategoryCalls.push_back(slot);
}

// --- binding installation -------------------------------------------------------------------

void InstallFakes() {
	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;
	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;
	__s2sdk_GetGameTime = &FakeGetGameTime;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_EntPointerToEntHandle = &FakeEntPointerToEntHandle;
	__s2sdk_EntPointerToEntIndex = &FakeEntPointerToEntIndex;
	__s2sdk_EntIndexToEntHandle = &FakeEntIndexToEntHandle;
	__s2sdk_GetClientGravity = &FakeGetClientGravity;
	__s2sdk_SetClientGravity = &FakeSetClientGravity;
	__s2sdk_GetClientSpeed = &FakeGetClientSpeed;
	__s2sdk_SetClientSpeed = &FakeSetClientSpeed;
	__s2sdk_GetClientAbsScale = &FakeGetClientAbsScale;
	__s2sdk_SetClientAbsScale = &FakeSetClientAbsScale;
	__s2sdk_GetClientHealth = &FakeGetClientHealth;
	__s2sdk_SetClientHealth = &FakeSetClientHealth;
	__s2sdk_GetClientArmor = &FakeGetClientArmor;
	__s2sdk_SetClientArmor = &FakeSetClientArmor;
	__s2sdk_GetClientMoveType = &FakeGetClientMoveType;
	__s2sdk_SetClientMoveType = &FakeSetClientMoveType;
	__s2sdk_GetClientAbsOrigin = &FakeGetClientAbsOrigin;
	__s2sdk_GetClientAbsAngles = &FakeGetClientAbsAngles;
	__s2sdk_GetClientAbsVelocity = &FakeGetClientAbsVelocity;
	__s2sdk_TeleportClient = &FakeTeleportClient;

	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetSchemaChainOffset = &FakeGetSchemaChainOffset;
	__s2sdk_GetEntDataFloat = &FakeGetEntDataFloat;
	__s2sdk_GetEntData = &FakeGetEntData;
	__s2sdk_SetEntDataFloat = &FakeSetEntDataFloat;
	__s2sdk_SetEntData = &FakeSetEntData;

	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_PrintToChatAll = &FakePrintToChatAll;
	__s2sdk_PrintToConsole = &FakePrintToConsole;
	__s2sdk_PrintToServer = &FakePrintToServer;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_IsValidMenu = &FakeIsValidMenu;
	__s2sdk_SetMenuPagination = &FakeSetMenuPagination;
	__s2sdk_SetMenuExitButton = &FakeSetMenuExitButton;
	__s2sdk_SetMenuExitBackButton = &FakeSetMenuExitBackButton;
	__s2sdk_SetMenuCloseOnSelect = &FakeSetMenuCloseOnSelect;
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_GetClientMenuOffset = &FakeGetClientMenuOffset;
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_IsMenuTypeRegistered = &FakeIsMenuTypeRegistered;
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;

	// A timer must never be created by the plugin (PITFALLS C9): the fake only counts.
	__s2sdk_CreateTimer =
		+[](double, s2sdk::TimerCallback, s2sdk::TimerFlag, const plg::vector<plg::any>&)
		-> s2sdk::TimerId {
		++g_createTimerCalls;
		return 0;
	};

	__hudmenu_GetClientMenuType = &FakeGetClientMenuType;

	plg::Log = &FakeLog;

	__admin_system_GetPluginVersion = &FakeGetPluginVersion;
	__admin_system_GetTranslation = &FakeGetTranslation;
	__admin_system_GetMessageType = &FakeGetMessageType;
	__admin_system_IsCoreLoaded = &FakeIsCoreLoaded;
	__admin_system_HasPermission = &FakeHasPermission;
	__admin_system_IsClientAdminDataReady = &FakeIsClientAdminDataReady;
	__admin_system_SendAction = &FakeSendAction;
	__admin_system_RegisterCategory = &FakeRegisterCategory;
	__admin_system_RegisterItem = &FakeRegisterItem;
	__admin_system_OnCoreLoaded_Register = &FakeOnCoreLoadedRegister;
	__admin_system_OnCoreLoaded_Unregister = &FakeOnCoreLoadedUnregister;
	__admin_system_ShowAdminLastCategoryMenu = &FakeShowAdminLastCategoryMenu;
}

void LoadTestTranslations() {
	g_translations = {
		{"Usage_Gravity", "Usage: {LIGHTRED}%s{DEFAULT} <target> <gravity>"},
		{"Usage_HP", "Usage: {LIGHTRED}%s{DEFAULT} <target> <hp>"},
		{"Usage_Armor", "Usage: {LIGHTRED}%s{DEFAULT} <target> <armor>"},
		{"Usage_Scale", "Usage: {LIGHTRED}%s{DEFAULT} <target> <scale>"},
		{"Usage_Speed", "Usage: {LIGHTRED}%s{DEFAULT} <target> <speed>"},
		{"Usage_God", "Usage: {LIGHTRED}%s{DEFAULT} <target> [0/1]"},
		{"Usage_Freeze", "Usage: {LIGHTRED}%s{DEFAULT} <target> [time]"},
		{"Usage_Unfreeze", "Usage: {LIGHTRED}%s{DEFAULT} <target>"},
		{"Usage_Blind", "Usage: {LIGHTRED}%s{DEFAULT} <target> [time]"},
		{"Usage_Unblind", "Usage: {LIGHTRED}%s{DEFAULT} <target>"},
		{"Usage_Bury", "Usage: {LIGHTRED}%s{DEFAULT} <target>"},
		{"Usage_Unbury", "Usage: {LIGHTRED}%s{DEFAULT} <target>"},
		{"AdminGravityMessage",
		 "Admin {LIGHTRED}%s{DEFAULT} set gravity of {GREEN}%s{DEFAULT} to %.1f"},
		{"GravityMessage", "Admin set gravity of {GREEN}%s{DEFAULT} to %.1f"},
		{"AdminHPMessage", "Admin {LIGHTRED}%s{DEFAULT} set hp of {GREEN}%s{DEFAULT} to %i"},
		{"HPMessage", "Admin set hp of {GREEN}%s{DEFAULT} to %i"},
		{"AdminArmorMessage",
		 "Admin {LIGHTRED}%s{DEFAULT} set armor of {GREEN}%s{DEFAULT} to %i"},
		{"ArmorMessage", "Admin set armor of {GREEN}%s{DEFAULT} to %i"},
		{"AdminScaleMessage",
		 "Admin {LIGHTRED}%s{DEFAULT} set scale of {GREEN}%s{DEFAULT} to %.1f"},
		{"ScaleMessage", "Admin set scale of {GREEN}%s{DEFAULT} to %.1f"},
		{"AdminSpeedMessage",
		 "Admin {LIGHTRED}%s{DEFAULT} set speed of {GREEN}%s{DEFAULT} to %.1f"},
		{"SpeedMessage", "Admin set speed of {GREEN}%s{DEFAULT} to %.1f"},
		{"AdminGodMessageDisable", "Admin {LIGHTRED}%s{DEFAULT} disabled god for {GREEN}%s{DEFAULT}"},
		{"AdminGodMessageEnable", "Admin {LIGHTRED}%s{DEFAULT} enabled god for {GREEN}%s{DEFAULT}"},
		{"GodMessageDisable", "Admin disabled god for {GREEN}%s{DEFAULT}"},
		{"GodMessageEnable", "Admin enabled god for {GREEN}%s{DEFAULT}"},
		{"AdminFreezeMessage", "Admin {LIGHTRED}%s{DEFAULT} froze {GREEN}%s{DEFAULT}"},
		{"AdminUnfreezeMessage", "Admin {LIGHTRED}%s{DEFAULT} unfroze {GREEN}%s{DEFAULT}"},
		{"FreezeMessage", "Admin froze {GREEN}%s{DEFAULT}"},
		{"UnfreezeMessage", "Admin unfroze {GREEN}%s{DEFAULT}"},
		{"AdminBlindMessage", "Admin {LIGHTRED}%s{DEFAULT} blinded {GREEN}%s{DEFAULT}"},
		{"AdminUnblindMessage", "Admin {LIGHTRED}%s{DEFAULT} unblinded {GREEN}%s{DEFAULT}"},
		{"BlindMessage", "Admin blinded {GREEN}%s{DEFAULT}"},
		{"UnblindMessage", "Admin unblinded {GREEN}%s{DEFAULT}"},
		{"AdminBuryMessage", "Admin {LIGHTRED}%s{DEFAULT} buried {GREEN}%s{DEFAULT}"},
		{"AdminUnburyMessage", "Admin {LIGHTRED}%s{DEFAULT} unburied {GREEN}%s{DEFAULT}"},
		{"BuryMessage", "Admin buried {GREEN}%s{DEFAULT}"},
		{"UnburyMessage", "Admin unburied {GREEN}%s{DEFAULT}"},
		{"SelectPlayer", "Select player"},
		{"Category_Players", "Players"},
		{"Item_Freeze", "Freeze"},
		{"Item_Gravity", "Gravity"},
		{"Item_Scale", "Scale"},
		{"Item_HP", "HP"},
		{"Item_Armor", "Armor"},
		{"Item_Speed", "Speed"},
		{"Item_God", "God mode"},
		{"Item_Bury", "Bury"},
		{"Item_Unbury", "Unbury"},
		{"Item_Blind", "Blind"},
	};
}

void ResetWorld() {
	ClearClients();
	g_rejectedHandles.clear();
	g_rejectedPointers.clear();
	g_permissions.clear();
	g_adminReadyFor.clear();
	g_adminDataNotReady.clear();
	g_messageType = 0;
	g_coreLoaded = true;
	g_coreLoadedCallback = nullptr;
	g_coreLoadedRegisters = 0;
	g_coreLoadedUnregisters = 0;
	g_clientMenuTypes = {};
	g_hudmenuCalls = 0;
	g_categories.clear();
	g_items.clear();
	g_lastCategoryCalls.clear();
	g_actions.clear();
	g_teleports.clear();
	g_schemaWrites.clear();
	g_gameTime = 100.0f;
	g_currentMap = "de_dust2";

	ResetClientCounters();
	ResetSchemaCounters();
	ResetPrints();

	g_addedCommands.clear();
	g_preexistingCommands.clear();
	g_removedCommands.clear();
	g_addedListeners.clear();
	g_removedListeners.clear();

	g_menus.clear();
	g_clientMenu.fill(0);
	g_clientOffset.fill(0);
	g_nextMenuId = 1;
	g_burnFirstMenuId = false;
	g_createMenuCalls = 0;
	g_createMenuFails = false;
	g_addMenuItemFails = false;
	g_displayMenuFails = false;
	g_throwOnDisplayMenu = false;
	g_destroyMenuCalls = 0;
	g_destroyedMenuIds.clear();
	g_menuTypes = {"chat", "console", "centerhtml", "button"};
	g_cancelledMenus.clear();
	g_cancelFails = false;
	g_cancelLies = false;
	g_liveBackendCloses = 0;
	g_setMenuTypeCalls = 0;
	g_setMenuTypeFails = false;
	g_handlerCalls = 0;
	g_lastMenuTimeout = -1;
	g_menuZeroBurned = false;
	g_createTimerCalls = 0;
	g_flashDurationLookups = 0;
	g_offsetLookups = 0;
	g_throwOnClientName = false;
	g_throwOnSchemaOffset = false;
	g_throwOnSetGravity = false;

	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;

	g_logs.clear();

	g_fieldOffsets = {
		{"CCSPlayerPawnBase::m_flFlashDuration", 4436},
		{"CCSPlayerPawnBase::m_flFlashMaxAlpha", 4440},
		{"CCSPlayerPawnBase::m_blindUntilTime", 4232},
		{"CCSPlayerPawnBase::m_blindStartTime", 4236},
		{"CBaseEntity::m_bTakesDamage", 1480},
	};
	g_chainOffsets = {
		{"CCSPlayerPawnBase", 8},
		{"CBaseEntity", 4},
	};

	LoadTestTranslations();
	admin_system::AdminApi::Get().ReloadTranslations();
	InstallFakes();
}

// --- test lifecycle -------------------------------------------------------------------------

plg::PluginResult StartPlugin() {
	return g_funCommandsPlugin.OnPluginStart();
}

void Update(int32_t iMillis = 16) {
	g_funCommandsPlugin.OnPluginUpdate(std::chrono::milliseconds(iMillis));
}

void StopPlugin() {
	g_funCommandsPlugin.OnPluginEnd();
}

// Stops whatever a previous test left behind and starts the next test from an empty fake world.
void PrepareTest() {
	StopPlugin();
	ResetWorld();
}

void FinishTest() {
	StopPlugin();
	ResetWorld();
}

// The real map boot: OnMapStart opens the gate immediately.
void StartMap() {
	FakeMapStart();
	Update();
}

// The plugin's own mid-map latch for the tests that do not model a full map boot.
void LatchMap() {
	Update();
}

void Grant(int32_t iSlot, const char* szPermission) {
	g_permissions[iSlot].insert(szPermission);
	// Signalling the fake core that the asynchronous SELECT answered for the live player.
	g_adminReadyFor[iSlot] = FakeSlot(iSlot).steamid;
}

void Revoke(int32_t iSlot, const char* szPermission) {
	g_permissions[iSlot].erase(szPermission);
}

bool HasCategory(const char* szIdentity) {
	for (const FakeCategory& category : g_categories) {
		if (category.identity == szIdentity) {
			return true;
		}
	}
	return false;
}

const FakeCategory* FindFakeCategory(const char* szIdentity) {
	for (const FakeCategory& category : g_categories) {
		if (category.identity == szIdentity) {
			return &category;
		}
	}
	return nullptr;
}

const FakeItem* FindFakeItem(const char* szIdentity) {
	for (const FakeItem& item : g_items) {
		if (item.identity == szIdentity && item.category == "players") {
			return &item;
		}
	}
	return nullptr;
}

bool SelectAdminItem(int32_t iSlot, const char* szIdentity) {
	for (const FakeItem& item : g_items) {
		if (item.identity == szIdentity && item.select != nullptr) {
			item.select(iSlot, PlgTest(item.category), PlgTest(item.identity), PlgTest(item.name));
			return true;
		}
	}
	return false;
}

const FakeMenu* OpenMenuOf(int32_t iSlot) {
	return FindMenu(g_clientMenu[static_cast<std::size_t>(iSlot)]);
}

int32_t IndexOfItem(const FakeMenu& menu, const std::string& display) {
	for (std::size_t i = 0; i < menu.items.size(); ++i) {
		if (menu.items[i].display == display) {
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

// The s2sdk digit input: the item at |offset| + digit - 1. With SetMenuCloseOnSelect(false) the
// manager does not send End after Select, which is what the plugin asks for.
void SelectMenuItemFake(int32_t iSlot, int32_t iAbsoluteIndex) {
	const FakeMenu* menu = OpenMenuOf(iSlot);
	if (menu == nullptr || menu->handler == nullptr) {
		return;
	}
	if (iAbsoluteIndex < 0 || iAbsoluteIndex >= static_cast<int32_t>(menu->items.size())) {
		return;
	}
	menu->handler(menu->id, s2sdk::MenuAction::Select, iSlot, iAbsoluteIndex);
}

void SelectByDisplay(int32_t iSlot, const std::string& display) {
	const FakeMenu* menu = OpenMenuOf(iSlot);
	Check(menu != nullptr, "a menu is open on slot " + std::to_string(iSlot));
	if (menu == nullptr) {
		return;
	}
	const int32_t index = IndexOfItem(*menu, display);
	Check(index >= 0, "menu item \"" + display + "\" exists");
	if (index >= 0) {
		// The client pages to the entry before pressing its digit: s2sdk only accepts a digit
		// that belongs to the visible five-item page.
		g_clientOffset[static_cast<std::size_t>(iSlot)] = index - (index % kItemsPerPage);
		SelectMenuItemFake(iSlot, index);
	}
}

// What the client does when it presses "back" or "exit": the manager sends Cancel(reason) and then
// tears the display down (End).
void CancelMenuFake(int32_t iSlot, s2sdk::MenuCancelReason reason) {
	const s2sdk::MenuId id = g_clientMenu[static_cast<std::size_t>(iSlot)];
	const FakeMenu* menu = FindMenu(id);
	if (menu == nullptr || menu->handler == nullptr) {
		return;
	}
	const s2sdk::MenuHandlerCallback handler = menu->handler;
	g_clientMenu[static_cast<std::size_t>(iSlot)] = 0;
	g_clientOffset[static_cast<std::size_t>(iSlot)] = 0;
	handler(id, s2sdk::MenuAction::Cancel, iSlot, static_cast<int32_t>(reason));
	handler(id, s2sdk::MenuAction::End, iSlot, 0);
}

void BackByUser(int32_t iSlot) {
	CancelMenuFake(iSlot, s2sdk::MenuCancelReason::ExitBack);
}

void ExitByUser(int32_t iSlot) {
	CancelMenuFake(iSlot, s2sdk::MenuCancelReason::Exit);
}

int ActionCount(const std::string& action) {
	int count = 0;
	for (const FakeAction& entry : g_actions) {
		count += entry.action == action ? 1 : 0;
	}
	return count;
}

const FakeAction* LastAction() {
	return g_actions.empty() ? nullptr : &g_actions.back();
}

// Every fake installed again in case a test removed one (missing-binding tests).
void RestoreFakes() {
	InstallFakes();
}

// --- A. start, registration and command ownership (PITFALLS C11/A7) -------------------------

void TestStartCommandsAndContexts() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart returns Ok (never Failed)");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1, "both map listeners are registered");
	Check(!g_mapReady, "the map gate is shut on plugin start");
	Check(g_coreLoadedRegisters == 1, "the admin core listener is registered once");
	Check(HasCategory("players"), "the players category is registered");
	const FakeCategory* category = FindFakeCategory("players");
	Check(category != nullptr && category->display == nullptr,
		  "the players category is registered with a null display callback");
	Check(category != nullptr && category->name == "Category_Players",
		  "the players category keeps its phrase key");
	Check(g_items.size() == 10, "the ten admin items are registered");
	Check(g_addedCommands.size() == 12, "the twelve console commands are created");
	Check(g_clientCalls == 0, "OnPluginStart makes no client/entity call");
	Check(g_schemaCalls == 0, "OnPluginStart resolves no schema field");

	for (const auto& [identity, permission] : std::array<std::pair<const char*, const char*>, 10>{{
			 {"freeze", "@admin/freeze"},
			 {"gravity", "@admin/gravity"},
			 {"scale", "@admin/scale"},
			 {"hp", "@admin/hp"},
			 {"armor", "@admin/armor"},
			 {"speed", "@admin/speed"},
			 {"god", "@admin/god"},
			 {"bury", "@admin/bury"},
			 {"unbury", "@admin/unbury"},
			 {"blind", "@admin/blind"},
		 }}) {
		const FakeItem* item = FindFakeItem(identity);
		Check(item != nullptr, std::string("the item '") + identity + "' is registered");
		if (item == nullptr) {
			continue;
		}
		Check(item->select != nullptr,
			  std::string("the item '") + identity + "' has a select callback");
		// The consumer wrapper installs its display trampoline for every item (a pointer into
		// this .so) even when the module passes a null display - that is exactly why OnPluginEnd
		// has to retire the items through the low-level binding.
		Check(item->display != nullptr,
			  std::string("the item '") + identity + "' carries the consumer display trampoline");
		Check(item->flags == permission,
			  std::string("the item '") + identity + "' keeps its permission");
	}

	for (const CommandDef& def : g_commands) {
		const FakeCommand* command = FindAddedCommand(def.szName);
		Check(command != nullptr, std::string(def.szName) + " is registered");
		if (command == nullptr) {
			continue;
		}
		Check(HasFlag(command->flags, s2sdk::ConVarFlag::ClientCanExecute),
			  std::string(def.szName) +
				  " is client-executable (the permission check needs the caller)");
		Check(HasFlag(command->flags, s2sdk::ConVarFlag::Release),
			  std::string(def.szName) + " is a Release command");
		Check(command->mode == s2sdk::HookMode::Pre, std::string(def.szName) + " hooks Pre");
		Check(command->callback != nullptr, std::string(def.szName) + " keeps its callback");
	}

	StartMap();

	// --- a chat "!mm_god" is not this console command --------------------------------
	for (const CommandDef& def : g_commands) {
		ResetPrints();
		g_actions.clear();
		const auto result = DispatchCommand(def.szName, 10, s2sdk::ConCommandContext::Chat,
										   {def.szName, "Admin"});
		Check(result == s2sdk::ResultType::Continue,
			  std::string(def.szName) + " is left to s2sdk for a chat line (Continue)");
		Check(g_actions.empty(), std::string(def.szName) + " chat line performs no action");
	}
	Check(g_chatAllPrints.empty(), "no chat line was printed for the chat spellings");

	// --- a client console caller needs the permission ---------------------------------
	MakeHuman(11, "Target", 76561198000000011ULL);
	{
		const auto denied =
			DispatchCommand("mm_god", 10, s2sdk::ConCommandContext::Console, {"mm_god", "Target"});
		Check(denied == s2sdk::ResultType::Handled, "the command is owned even when denied");
		Check(g_actions.empty(), "an unpermitted client console caller performs no action");
		Check(FakeSlot(11).takesDamage, "an unpermitted call leaves the target untouched");

		Grant(10, "@admin/god");
		MutableSlot(11).takesDamage = false; // the original's "god mode on"
		const auto allowed =
			DispatchCommand("mm_god", 10, s2sdk::ConCommandContext::Console, {"mm_god", "Target"});
		Check(allowed == s2sdk::ResultType::Handled, "the permitted client console call is handled");
		Check(FakeSlot(11).takesDamage, "the permitted call writes m_bTakesDamage = 1");
		Check(ActionCount("god") == 1 && LastAction()->param == "11 1",
			  "the permitted call sends the original god action");
	}

	// --- the server console is allowed without a permission ---------------------------
	{
		MakeHuman(12, "Second", 76561198000000012ULL);
		g_actions.clear();
		const auto result = DispatchCommand("mm_hp", -1, s2sdk::ConCommandContext::Console,
											{"mm_hp", "Second", "25"});
		Check(result == s2sdk::ResultType::Handled, "the server console command is handled");
		Check(FakeSlot(12).health == 25, "the server console needs no permission");
		Check(ActionCount("hp") == 1, "the server console hp sends its action");
	}

	// --- only caller == -1 is the console ---------------------------------------------
	for (const int32_t caller : {-2, -3, 64, 1000}) {
		ResetPrints();
		g_actions.clear();
		const auto result = DispatchCommand("mm_hp", caller, s2sdk::ConCommandContext::Console,
											{"mm_hp", "Second", "30"});
		Check(result == s2sdk::ResultType::Handled,
			  "caller " + std::to_string(caller) + " is still owned");
		Check(g_actions.empty(), "caller " + std::to_string(caller) + " performs no action");
		Check(g_consolePrints.empty() && g_serverPrints.empty(),
			  "caller " + std::to_string(caller) + " gets no output (it is not the console)");
	}
	Check(FakeSlot(12).health == 25, "the out-of-range callers left the target untouched");

	FinishTest();
}

// A command name that already existed: the plugin must attach a listener and never RemoveCommand it.
void TestPreexistingCommandNames() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	g_preexistingCommands.insert("mm_god");
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Check(FindAddedCommand("mm_god") != nullptr, "mm_god is found");
	const FakeCommand* god = FindAddedCommand("mm_god");
	const s2sdk::ConCommandCallback godCallback = god != nullptr ? god->callback : nullptr;
	StopPlugin();
	Check(!RemovedCommand("mm_god"), "a pre-existing command name is never removed by RemoveCommand");
	Check(godCallback != nullptr &&
			  RemovedListener("mm_god", godCallback, s2sdk::HookMode::Pre),
		  "the listener attached to a pre-existing name is detached on unload");
	FinishTest();
}

// --- B. permissions and the admin-data readiness gate (API 1.1.0) ---------------------------

void TestAdminReadiness() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/hp");

	g_actions.clear();
	const auto call = [&](int32_t slot) {
		return DispatchCommand("mm_hp", slot, s2sdk::ConCommandContext::Console,
							   {"mm_hp", "Target", "50"});
	};

	// Pending admin SELECT: the permission may belong to the previous occupant, so no action.
	g_adminDataNotReady.insert(10);
	Check(call(10) == s2sdk::ResultType::Handled, "the pending-ready caller is still handled");
	Check(g_actions.empty(), "no action runs while the admin data is not ready");
	Check(FakeSlot(11).health == 100, "the target is untouched while the admin data is not ready");

	// After the SELECT answered the same call goes through.
	g_adminDataNotReady.erase(10);
	(void)call(10);
	Check(FakeSlot(11).health == 50, "the action runs when the admin data is ready");
	Check(ActionCount("hp") == 1, "the ready call sends its action");

	// A reclaimed slot is not the admin the record was loaded for.
	MutableSlot(10).steamid = 76561198000000099ULL;
	g_actions.clear();
	(void)call(10);
	Check(g_actions.empty(), "a reclaimed slot does not inherit the previous admin's rights");

	// A bot has no SteamID64 and therefore no admin record.
	MutableSlot(10).steamid = 76561198000000010ULL;
	MutableSlot(10).fake = true;
	g_actions.clear();
	(void)call(10);
	Check(g_actions.empty(), "a bot caller never passes the readiness gate");
	Check(!g_steamidReadForBot, "the plugin never reads a bot's SteamID64");

	FinishTest();
}

void TestMissingAdminBindings() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	__admin_system_IsClientAdminDataReady = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok,
		  "a missing readiness binding still starts");
	StartMap();
	Check(g_coreLoadedRegisters == 0, "no admin core listener without the full API");
	Check(g_items.empty(), "no admin items without the full API");
	Check(HasLog("admin_system bindings are unavailable"), "the missing API is logged");
	g_actions.clear();
	(void)DispatchCommand("mm_hp", 10, s2sdk::ConCommandContext::Console,
						  {"mm_hp", "Target", "50"});
	Check(g_actions.empty(), "a client call is denied without the readiness export");
	g_actions.clear();
	(void)DispatchCommand("mm_hp", -1, s2sdk::ConCommandContext::Console,
						  {"mm_hp", "Target", "50"});
	Check(FakeSlot(11).health == 50, "the server console still works");
	Check(ActionCount("hp") == 1, "the console action still reaches the loaded admin core");
	StopPlugin();
	RestoreFakes();
	FinishTest();
}

// --- C. signed CEntityHandle semantics (PITFALLS C1/C2) ------------------------------------

void TestSignedHandles() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	FakeClient& target = MakeHuman(11, "Target", 76561198000000011ULL);
	// A serial whose bit 16 is set makes the handle's bit 31 set: a negative, valid handle.
	target.generation = 0x10000;
	target.pawnGeneration = 0x10000;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/hp");
	Grant(10, "@admin/freeze");
	Check(ControllerHandle(11) < 0, "the fake controller handle is negative");
	Check(PawnHandle(11) < 0, "the fake pawn handle is negative");
	Check(TargetUsable(11), "a negative pawn handle is a usable target");

	(void)DispatchCommand("mm_hp", 10, s2sdk::ConCommandContext::Console,
						  {"mm_hp", "Target", "75"});
	Check(FakeSlot(11).health == 75, "a negative controller/pawn handle reaches the action");
	Check(LastAction() != nullptr && LastAction()->param == "11 75",
		  "the action of a negative-handle target is sent");

	// Only 0 and -1 (and engine-rejected handles) are invalid.
	target.hasHandleOverride = true;
	target.handleOverride = 0;
	Check(ControllerHandle(11) == -1, "handle 0 is invalid");
	Check(!TargetUsable(11), "a target with handle 0 is unusable");
	target.handleOverride = -1;
	Check(ControllerHandle(11) == -1, "handle -1 is invalid");
	target.handleOverride = 0x12345;
	g_rejectedHandles.insert(0x12345);
	Check(ControllerHandle(11) == -1, "an engine-rejected handle is invalid");
	g_rejectedHandles.clear();
	target.hasHandleOverride = false;

	// The same for the pawn handle.
	target.hasPawnHandleOverride = true;
	target.pawnHandleOverride = 0;
	Check(!TargetUsable(11), "pawn handle 0 is invalid");
	target.pawnHandleOverride = -1;
	Check(!TargetUsable(11), "pawn handle -1 is invalid");
	target.hasPawnHandleOverride = false;

	// The pawn pointer can also be rejected by the engine.
	g_rejectedPointers.insert(reinterpret_cast<uintptr_t>(PawnPointerOfSlot(11)));
	Check(!TargetUsable(11), "a rejected pawn pointer is unusable");
	g_rejectedPointers.clear();
	Check(TargetUsable(11), "a valid pawn pointer is usable again");

	// A dead player has no pawn: no setter action, but the announce-only freeze still works.
	MutableSlot(11).hasPawn = false;
	g_actions.clear();
	(void)DispatchCommand("mm_hp", 10, s2sdk::ConCommandContext::Console,
						  {"mm_hp", "Target", "60"});
	Check(g_actions.empty(), "a pawnless target gets no hp action");
	g_actions.clear();
	(void)DispatchCommand("mm_freezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Target"});
	Check(ActionCount("freeze") == 1, "the original's freeze still announces a pawnless target");

	FinishTest();
}

void TestUnknownNameAndBots() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "", 76561198000000011ULL);
	MakeBot(12, "Botty");
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Check(ClientName(11).empty(), "s2sdk's \"<unknown>\" name becomes an empty name");
	Check(ClientSteamID64(12) == 0, "a bot has no SteamID64");
	Check(!g_steamidReadForBot, "a bot's SteamID64 is never read from s2sdk");
	Check(ClientName(12) == "Botty", "a bot's name is readable");
	// A bot is not targetable by name/steamid (FindUser skips it), like the original.
	Check(FindUser("Botty") == -1, "a bot is not targetable by name");
	Check(FindUser("76561198000000011") == 11, "a human is targetable by SteamID64");
	Check(FindUser("11") == 11, "a human is targetable by slot number");
	Check(FindUser("Admin") == 10, "a human is targetable by name substring");
	Check(FindUser("nobody") == -1, "an unknown name matches nothing");
	FinishTest();
}

// --- D. the value commands (mm_gravity/mm_hp/mm_armor/mm_scale/mm_speed) --------------------

void TestValueCommands() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	for (const char* permission :
		 {"@admin/gravity", "@admin/hp", "@admin/armor", "@admin/scale", "@admin/speed"}) {
		Grant(10, permission);
	}
	g_messageType = 2;

	// --- mm_gravity -------------------------------------------------------------------
	ResetPrints();
	g_actions.clear();
	(void)DispatchCommand("mm_gravity", 10, s2sdk::ConCommandContext::Console,
						  {"mm_gravity", "Target", "0.5"});
	Check(FakeSlot(11).gravity == 0.5f, "mm_gravity sets m_flGravityScale");
	Check(HasChatAllPrint("Admin \017Admin\001 set gravity of \004Target\001 to 0.5"),
		  "mm_gravity prints the colourised admin phrase");
	Check(ActionCount("gravity") == 1 && LastAction()->slot == 10 &&
			  LastAction()->param == "11 0.500000",
		  "mm_gravity sends the original action payload");

	// --- mm_hp ------------------------------------------------------------------------
	ResetPrints();
	g_actions.clear();
	(void)DispatchCommand("mm_hp", 10, s2sdk::ConCommandContext::Console,
						  {"mm_hp", "Target", "25"});
	Check(FakeSlot(11).health == 25, "mm_hp sets m_iHealth");
	Check(HasChatAllPrint("Admin \017Admin\001 set hp of \004Target\001 to 25"),
		  "mm_hp prints the colourised admin phrase");
	Check(ActionCount("hp") == 1 && LastAction()->param == "11 25",
		  "mm_hp sends the original action payload");

	// --- mm_armor ---------------------------------------------------------------------
	ResetPrints();
	g_actions.clear();
	(void)DispatchCommand("mm_armor", 10, s2sdk::ConCommandContext::Console,
						  {"mm_armor", "Target", "30"});
	Check(FakeSlot(11).armor == 30, "mm_armor sets m_ArmorValue");
	Check(HasChatAllPrint("Admin \017Admin\001 set armor of \004Target\001 to 30"),
		  "mm_armor prints the colourised admin phrase");
	Check(ActionCount("armor") == 1 && LastAction()->param == "11 30",
		  "mm_armor sends the original action payload");

	// --- mm_scale ---------------------------------------------------------------------
	ResetPrints();
	g_actions.clear();
	(void)DispatchCommand("mm_scale", 10, s2sdk::ConCommandContext::Console,
						  {"mm_scale", "Target", "1.5"});
	Check(FakeSlot(11).scale == 1.5f, "mm_scale sets the abs scale");
	Check(HasChatAllPrint("Admin \017Admin\001 set scale of \004Target\001 to 1.5"),
		  "mm_scale prints the colourised admin phrase");
	Check(ActionCount("scale") == 1 && LastAction()->param == "11 1.500000",
		  "mm_scale sends the original action payload");

	// --- mm_speed ---------------------------------------------------------------------
	ResetPrints();
	g_actions.clear();
	(void)DispatchCommand("mm_speed", 10, s2sdk::ConCommandContext::Console,
						  {"mm_speed", "Target", "2.0"});
	Check(FakeSlot(11).speed == 2.0f, "mm_speed sets m_flVelocityModifier");
	Check(HasChatAllPrint("Admin \017Admin\001 set speed of \004Target\001 to 2.0"),
		  "mm_speed prints the colourised admin phrase");
	Check(ActionCount("speed") == 1 && LastAction()->param == "11 2.000000",
		  "mm_speed sends the original action payload");

	// --- message type 1: the non-admin phrase, no actor name --------------------------
	g_messageType = 1;
	ResetPrints();
	(void)DispatchCommand("mm_gravity", 10, s2sdk::ConCommandContext::Console,
						  {"mm_gravity", "Target", "1.0"});
	Check(HasChatAllPrint("Admin set gravity of \004Target\001 to 1.0"),
		  "message type 1 prints the non-admin phrase");
	Check(!HasChatAllPrint("Admin \017Admin"), "message type 1 omits the actor name");

	// --- message type 0: no broadcast, the action still goes out ----------------------
	g_messageType = 0;
	ResetPrints();
	g_actions.clear();
	(void)DispatchCommand("mm_hp", 10, s2sdk::ConCommandContext::Console,
						  {"mm_hp", "Target", "40"});
	Check(g_chatAllPrints.empty(), "message type 0 prints nothing");
	Check(ActionCount("hp") == 1, "message type 0 still sends the admin action");

	// --- value length rejection, console and client -----------------------------------
	g_messageType = 0;
	ResetPrints();
	g_actions.clear();
	(void)DispatchCommand("mm_gravity", -1, s2sdk::ConCommandContext::Console,
						  {"mm_gravity", "Target", "12345678901"});
	Check(HasServerPrint("Invalid gravity value"), "a value longer than 10 chars is rejected (console)");
	Check(ActionCount("gravity") == 0, "the rejected gravity value performs no action");
	ResetPrints();
	(void)DispatchCommand("mm_hp", 10, s2sdk::ConCommandContext::Console,
						  {"mm_hp", "Target", "12345678901"});
	Check(HasConsolePrint("Invalid HP value"), "a value longer than 10 chars is rejected (client)");
	ResetPrints();
	(void)DispatchCommand("mm_armor", -1, s2sdk::ConCommandContext::Console,
						  {"mm_armor", "Target", "12345678901"});
	Check(HasServerPrint("Invalid armor value"), "the armor value is checked");
	ResetPrints();
	(void)DispatchCommand("mm_scale", -1, s2sdk::ConCommandContext::Console,
						  {"mm_scale", "Target", "12345678901"});
	Check(HasServerPrint("Invalid scale value"), "the scale value is checked");
	ResetPrints();
	(void)DispatchCommand("mm_speed", -1, s2sdk::ConCommandContext::Console,
						  {"mm_speed", "Target", "12345678901"});
	Check(HasServerPrint("Invalid speed value"), "the speed value is checked");

	// --- usage lines ------------------------------------------------------------------
	ResetPrints();
	(void)DispatchCommand("mm_gravity", -1, s2sdk::ConCommandContext::Console, {"mm_gravity"});
	Check(HasServerPrint("Usage: mm_gravity <target> <gravity>"),
		  "the console usage line is stripped of colour tags");
	ResetPrints();
	(void)DispatchCommand("mm_hp", 10, s2sdk::ConCommandContext::Console, {"mm_hp"});
	Check(HasConsolePrint("{LIGHTRED}mm_hp{DEFAULT}"),
		  "the client usage line keeps the colour tags, like pisexcompat");
	ResetPrints();
	(void)DispatchCommand("mm_armor", -1, s2sdk::ConCommandContext::Console, {"mm_armor", "x"});
	Check(HasServerPrint("Usage: mm_armor"), "a missing second argument prints the usage line");
	ResetPrints();
	(void)DispatchCommand("mm_scale", -1, s2sdk::ConCommandContext::Console, {"mm_scale", "x"});
	Check(HasServerPrint("Usage: mm_scale"), "a missing second argument prints the usage line");
	ResetPrints();
	(void)DispatchCommand("mm_speed", -1, s2sdk::ConCommandContext::Console, {"mm_speed", "x"});
	Check(HasServerPrint("Usage: mm_speed"), "a missing second argument prints the usage line");

	// --- unknown target ---------------------------------------------------------------
	g_actions.clear();
	(void)DispatchCommand("mm_hp", -1, s2sdk::ConCommandContext::Console,
						  {"mm_hp", "nobody", "50"});
	Check(g_actions.empty(), "an unknown target performs no action");

	FinishTest();
}

// --- E. god / freeze / blind / bury ---------------------------------------------------------

void TestGodAndFreeze() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/god");
	Grant(10, "@admin/freeze");
	g_messageType = 2;

	// --- mm_god: m_bTakesDamage is written raw, one byte, without a state change -------
	// The original wrote its bGod argument straight into m_bTakesDamage, so "god mode on" is the
	// argument 0 - the module's messages are inverted for the same reason.
	MutableSlot(11).takesDamage = false; // god mode on
	ResetPrints();
	g_actions.clear();
	g_schemaWrites.clear();
	(void)DispatchCommand("mm_god", 10, s2sdk::ConCommandContext::Console, {"mm_god", "Target"});
	Check(FakeSlot(11).takesDamage, "mm_god writes m_bTakesDamage = 1 (the original's bGod)");
	Check(g_schemaWrites.size() == 1, "mm_god performs exactly one raw write");
	if (!g_schemaWrites.empty()) {
		const FakeSchemaWrite& write = g_schemaWrites.back();
		Check(!write.bFloat && write.size == 1 && write.iValue == 1 && !write.changeState &&
				  write.chainOffset == -2 && write.offset == 1480,
			  "m_bTakesDamage is written at its offset as a 1-byte non-networked value");
		Check(HandleSlot(write.handle) == 11, "m_bTakesDamage is written on the target's pawn");
	}
	Check(HasChatAllPrint("Admin \017Admin\001 disabled god for \004Target\001"),
		  "mm_god uses the original (inverted) phrase");
	Check(ActionCount("god") == 1 && LastAction()->param == "11 1",
		  "mm_god sends the original action payload");

	ResetPrints();
	g_actions.clear();
	(void)DispatchCommand("mm_god", 10, s2sdk::ConCommandContext::Console,
						  {"mm_god", "Target", "0"});
	Check(!FakeSlot(11).takesDamage, "mm_god 0 enables god mode");
	Check(HasChatAllPrint("Admin \017Admin\001 enabled god for \004Target\001"),
		  "mm_god 0 uses the other inverted phrase");
	Check(ActionCount("god") == 1 && LastAction()->param == "11 0",
		  "mm_god 0 sends the original action payload");
	ResetPrints();
	(void)DispatchCommand("mm_god", 10, s2sdk::ConCommandContext::Console,
						  {"mm_god", "Target", "1"});
	Check(FakeSlot(11).takesDamage, "mm_god 1 disables god mode again");
	ResetPrints();
	(void)DispatchCommand("mm_god", -1, s2sdk::ConCommandContext::Console,
						  {"mm_god", "Target", "12345678901"});
	Check(HasServerPrint("Invalid god value"), "the god value length is checked");
	ResetPrints();
	(void)DispatchCommand("mm_god", -1, s2sdk::ConCommandContext::Console, {"mm_god"});
	Check(HasServerPrint("Usage: mm_god <target> [0/1]"), "the god usage line is printed");

	// --- mm_freezy / mm_unfreezy ------------------------------------------------------
	ResetPrints();
	g_actions.clear();
	(void)DispatchCommand("mm_freezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Target"});
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeObsolete,
		  "mm_freezy sets MOVETYPE_OBSOLETE");
	Check(HasChatAllPrint("Admin \017Admin\001 froze \004Target\001"),
		  "mm_freezy prints the freeze phrase");
	Check(ActionCount("freeze") == 1 && LastAction()->param == "11",
		  "mm_freezy sends the original action payload");
	ResetPrints();
	g_actions.clear();
	(void)DispatchCommand("mm_unfreezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_unfreezy", "Target"});
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "mm_unfreezy restores MOVETYPE_WALK");
	Check(HasChatAllPrint("Admin \017Admin\001 unfroze \004Target\001"),
		  "mm_unfreezy prints the unfreeze phrase");
	Check(ActionCount("unfreeze") == 1 && LastAction()->param == "11",
		  "mm_unfreezy sends the original action payload");
	ResetPrints();
	(void)DispatchCommand("mm_freezy", -1, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Target", "12345678901"});
	Check(HasServerPrint("Invalid freeze value"), "the freeze time length is checked");
	ResetPrints();
	(void)DispatchCommand("mm_freezy", -1, s2sdk::ConCommandContext::Console, {"mm_freezy"});
	Check(HasServerPrint("Usage: mm_freezy <target> [time]"), "the freeze usage line is printed");
	ResetPrints();
	(void)DispatchCommand("mm_unfreezy", -1, s2sdk::ConCommandContext::Console, {"mm_unfreezy"});
	Check(HasServerPrint("Usage: mm_unfreezy <target>"), "the unfreeze usage line is printed");

	// --- message type 1 for the toggles -----------------------------------------------
	g_messageType = 1;
	ResetPrints();
	(void)DispatchCommand("mm_god", 10, s2sdk::ConCommandContext::Console,
						  {"mm_god", "Target", "1"});
	Check(HasChatAllPrint("Admin disabled god for \004Target\001"),
		  "the non-admin god phrase is used for message type 1 (bGod=1 disables god)");
	ResetPrints();
	(void)DispatchCommand("mm_freezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Target"});
	Check(HasChatAllPrint("Admin froze \004Target\001"),
		  "the non-admin freeze phrase is used for message type 1");
	ResetPrints();
	(void)DispatchCommand("mm_unfreezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_unfreezy", "Target"});
	Check(HasChatAllPrint("Admin unfroze \004Target\001"),
		  "the non-admin unfreeze phrase is used for message type 1");

	FinishTest();
}

void TestBlindAndBury() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/blind");
	Grant(10, "@admin/bury");
	g_messageType = 2;
	g_gameTime = 100.0f;

	// --- mm_blind ---------------------------------------------------------------------
	ResetPrints();
	g_actions.clear();
	g_schemaWrites.clear();
	(void)DispatchCommand("mm_blind", 10, s2sdk::ConCommandContext::Console, {"mm_blind", "Target"});
	Check(FakeSlot(11).flashDuration == 9999.0f, "mm_blind sets m_flFlashDuration");
	Check(FakeSlot(11).flashMaxAlpha == 255.0f, "mm_blind sets m_flFlashMaxAlpha");
	Check(FakeSlot(11).blindUntil == 10099.0f, "mm_blind sets m_blindUntilTime to curtime + 9999");
	Check(FakeSlot(11).blindStart == 100.0f, "mm_blind sets m_blindStartTime to curtime");
	Check(HasChatAllPrint("Admin \017Admin\001 blinded \004Target\001"),
		  "mm_blind prints the blind phrase");
	Check(ActionCount("blind") == 1 && LastAction()->param == "11 1",
		  "mm_blind sends the original action payload");
	Check(g_schemaWrites.size() == 4, "mm_blind performs the four raw field writes");
	if (g_schemaWrites.size() == 4) {
		const FakeSchemaWrite& duration = g_schemaWrites[0];
		const FakeSchemaWrite& maxAlpha = g_schemaWrites[1];
		const FakeSchemaWrite& until = g_schemaWrites[2];
		const FakeSchemaWrite& start = g_schemaWrites[3];
		Check(duration.offset == 4436 && duration.size == 4 && duration.changeState &&
				  duration.chainOffset == 8 && duration.flValue == 9999.0,
			  "m_flFlashDuration is written as a networked float with the class chain offset");
		Check(maxAlpha.offset == 4440 && maxAlpha.size == 4 && maxAlpha.changeState &&
				  maxAlpha.chainOffset == 8 && maxAlpha.flValue == 255.0,
			  "m_flFlashMaxAlpha is written as a networked float with the class chain offset");
		Check(until.offset == 4232 && until.size == 4 && !until.changeState &&
				  until.chainOffset == -2 && until.flValue == 10099.0,
			  "m_blindUntilTime is written without a state change");
		Check(start.offset == 4236 && start.size == 4 && !start.changeState &&
				  start.flValue == 100.0,
			  "m_blindStartTime is written without a state change");
	}

	// --- mm_unblind -------------------------------------------------------------------
	ResetPrints();
	g_actions.clear();
	g_schemaWrites.clear();
	(void)DispatchCommand("mm_unblind", 10, s2sdk::ConCommandContext::Console,
						  {"mm_unblind", "Target"});
	Check(FakeSlot(11).flashDuration == 0.0f, "mm_unblind clears m_flFlashDuration");
	Check(FakeSlot(11).flashMaxAlpha == 0.0f, "mm_unblind clears m_flFlashMaxAlpha");
	Check(FakeSlot(11).blindUntil == 99.0f, "mm_unblind moves m_blindUntilTime into the past");
	Check(HasChatAllPrint("Admin \017Admin\001 unblinded \004Target\001"),
		  "mm_unblind prints the unblind phrase");
	Check(ActionCount("blind") == 1 && LastAction()->param == "11 0",
		  "mm_unblind sends the original action payload with the new state");
	if (g_schemaWrites.size() == 3) {
		Check(!g_schemaWrites[0].changeState && g_schemaWrites[0].chainOffset == -2,
			  "the unblind flash writes are not networked (like the original)");
	}

	ResetPrints();
	(void)DispatchCommand("mm_blind", -1, s2sdk::ConCommandContext::Console,
						  {"mm_blind", "Target", "12345678901"});
	Check(HasServerPrint("Invalid blind value"), "the blind time length is checked");
	ResetPrints();
	(void)DispatchCommand("mm_blind", -1, s2sdk::ConCommandContext::Console, {"mm_blind"});
	Check(HasServerPrint("Usage: mm_blind <target> [time]"), "the blind usage line is printed");
	ResetPrints();
	(void)DispatchCommand("mm_unblind", -1, s2sdk::ConCommandContext::Console, {"mm_unblind"});
	Check(HasServerPrint("Usage: mm_unblind <target>"), "the unblind usage line is printed");

	g_messageType = 1;
	ResetPrints();
	(void)DispatchCommand("mm_blind", 10, s2sdk::ConCommandContext::Console,
						  {"mm_blind", "Target"});
	Check(HasChatAllPrint("Admin blinded \004Target\001"),
		  "the non-admin blind phrase is used for message type 1");
	ResetPrints();
	(void)DispatchCommand("mm_unblind", 10, s2sdk::ConCommandContext::Console,
						  {"mm_unblind", "Target"});
	Check(HasChatAllPrint("Admin unblinded \004Target\001"),
		  "the non-admin unblind phrase is used for message type 1");

	// --- mm_bury / mm_unbury ----------------------------------------------------------
	MutableSlot(11).origin = plg::vec3{10.0f, 20.0f, 70.0f};
	MutableSlot(11).angles = plg::vec3{1.0f, 2.0f, 3.0f};
	MutableSlot(11).velocity = plg::vec3{4.0f, 5.0f, 6.0f};
	g_teleports.clear();
	g_actions.clear();
	ResetPrints();
	g_messageType = 2;
	(void)DispatchCommand("mm_bury", 10, s2sdk::ConCommandContext::Console, {"mm_bury", "Target"});
	Check(g_teleports.size() == 1, "mm_bury teleports the target");
	if (!g_teleports.empty()) {
		Check(g_teleports[0].origin.z == 60.0f && g_teleports[0].origin.x == 10.0f &&
				  g_teleports[0].origin.y == 20.0f,
			  "mm_bury lowers the target by 10 units");
		Check(g_teleports[0].angles.x == 1.0f && g_teleports[0].velocity.z == 6.0f,
			  "mm_bury keeps the angles and the velocity");
	}
	Check(HasChatAllPrint("Admin \017Admin\001 buried \004Target\001"),
		  "mm_bury prints the bury phrase");
	Check(ActionCount("bury") == 1 && LastAction()->param == "11",
		  "mm_bury sends the original action payload");

	g_teleports.clear();
	g_actions.clear();
	ResetPrints();
	(void)DispatchCommand("mm_unbury", 10, s2sdk::ConCommandContext::Console,
						  {"mm_unbury", "Target"});
	Check(g_teleports.size() == 1 && g_teleports[0].origin.z == 70.0f,
		  "mm_unbury raises the target by 10 units");
	Check(HasChatAllPrint("Admin \017Admin\001 unburied \004Target\001"),
		  "mm_unbury prints the unbury phrase");
	Check(ActionCount("unbury") == 1 && LastAction()->param == "11",
		  "mm_unbury sends the original action payload");
	ResetPrints();
	(void)DispatchCommand("mm_bury", -1, s2sdk::ConCommandContext::Console, {"mm_bury"});
	Check(HasServerPrint("Usage: mm_bury <target>"), "the bury usage line is printed");
	ResetPrints();
	(void)DispatchCommand("mm_unbury", -1, s2sdk::ConCommandContext::Console, {"mm_unbury"});
	Check(HasServerPrint("Usage: mm_unbury <target>"), "the unbury usage line is printed");

	// Without the transform bindings the teleport is skipped, like the original port.
	__s2sdk_TeleportClient = nullptr;
	g_teleports.clear();
	g_actions.clear();
	(void)DispatchCommand("mm_bury", 10, s2sdk::ConCommandContext::Console, {"mm_bury", "Target"});
	Check(g_teleports.empty() && g_actions.empty(),
		  "a missing teleport binding skips the bury silently");
	RestoreFakes();

	FinishTest();
}

// --- F. the delayed unfreeze/unblind through the OnPluginUpdate accumulators (C9) -----------

void TestDelayedActions() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/freeze");
	Grant(10, "@admin/blind");
	g_messageType = 2;

	// --- the freeze expires only after its deadline -----------------------------------
	g_actions.clear();
	ResetPrints();
	(void)DispatchCommand("mm_freezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Target", "2.5"});
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeObsolete, "the target is frozen");
	Check(ActionCount("freeze") == 1, "the freeze action is sent immediately");
	Update(1000);
	Update(1000);
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeObsolete,
		  "the target is still frozen before the deadline");
	Update(400);
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeObsolete,
		  "the target is still frozen shortly before the deadline");
	Update(200);
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "the target is unfrozen after the deadline");
	Check(ActionCount("unfreeze") == 1, "the delayed unfreeze sends its action");
	Check(HasChatAllPrint("Admin \017Admin\001 unfroze \004Target\001"),
		  "the delayed unfreeze announces with the original phrase");
	Check(g_createTimerCalls == 0, "no s2sdk timer was ever created");

	// --- the blind expires the same way -----------------------------------------------
	g_actions.clear();
	ResetPrints();
	(void)DispatchCommand("mm_blind", 10, s2sdk::ConCommandContext::Console,
						  {"mm_blind", "Target", "1.0"});
	Check(FakeSlot(11).flashDuration == 9999.0f, "the target is blinded");
	g_actions.clear();
	Update(500);
	Check(FakeSlot(11).flashDuration == 9999.0f, "the target is still blinded before the deadline");
	Update(600);
	Check(FakeSlot(11).flashDuration == 0.0f, "the target is unblinded after the deadline");
	Check(ActionCount("blind") == 1 && LastAction()->param == "11 0",
		  "the delayed unblind sends the original action payload");
	Check(g_createTimerCalls == 0, "no s2sdk timer was ever created");

	// --- the target's slot is reused: the new occupant is never touched ----------------
	(void)DispatchCommand("mm_freezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Target", "1.0"});
	FakeClient& newTarget = MakeHuman(11, "Other", 76561198000000099ULL);
	newTarget.generation = 3;
	g_actions.clear();
	ResetPrints();
	Update(1100);
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "a reused target slot is not unfrozen");
	Check(g_actions.empty(), "a reused target slot sends no action");
	Check(!HasChatAllPrint("unfroze"), "a reused target slot is not announced");

	// --- the target disconnected: nothing happens -------------------------------------
	(void)DispatchCommand("mm_freezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Other", "1.0"});
	MutableSlot(11).connected = false;
	g_actions.clear();
	ResetPrints();
	Update(1100);
	Check(g_actions.empty(), "a disconnected target gets no delayed action");
	Check(!HasChatAllPrint("unfroze"), "a disconnected target is not announced");

	// --- a dead target has no pawn at the deadline: nothing happens -------------------
	MutableSlot(11).connected = true;
	(void)DispatchCommand("mm_freezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Other", "1.0"});
	MutableSlot(11).hasPawn = false;
	g_actions.clear();
	Update(1100);
	Check(g_actions.empty(), "a pawnless target gets no delayed action");

	// --- the acting admin's slot is reused: the effect still happens, silently --------
	MutableSlot(11).hasPawn = true;
	(void)DispatchCommand("mm_freezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Other", "2.0"});
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeObsolete,
		  "the target is frozen before the admin leaves");
	FakeClient& newAdmin = MakeHuman(10, "NewAdmin", 76561198000000088ULL);
	newAdmin.generation = 3;
	g_actions.clear();
	ResetPrints();
	Update(2100);
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "the effect still happens when the admin's slot was reused");
	Check(g_actions.empty(), "nothing is sent in the new occupant's name");
	Check(g_chatAllPrints.empty(), "nothing is announced in the new occupant's name");

	// --- the permission is revoked before the deadline: effect only, no announcement -----
	FakeClient& freshAdmin = MakeHuman(10, "Admin2", 76561198000000055ULL);
	freshAdmin.generation = 4;
	Grant(10, "@admin/freeze");
	Grant(10, "@admin/blind");
	g_actions.clear();
	ResetPrints();
	(void)DispatchCommand("mm_freezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Other", "2.0"});
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeObsolete,
		  "the target is frozen before the permission is revoked");
	Revoke(10, "@admin/freeze");
	g_actions.clear();
	ResetPrints();
	Update(2100);
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "a revoked permission still lets the target unfreeze");
	Check(g_actions.empty(), "a revoked permission sends no admin action");
	Check(g_chatAllPrints.empty(), "a revoked permission prints no chat line");

	// --- the admin data is no longer ready before the deadline -------------------------
	Grant(10, "@admin/freeze");
	g_actions.clear();
	ResetPrints();
	(void)DispatchCommand("mm_blind", 10, s2sdk::ConCommandContext::Console,
						  {"mm_blind", "Other", "1.0"});
	Check(FakeSlot(11).flashDuration == 9999.0f,
		  "the target is blinded before the admin data goes stale");
	g_adminDataNotReady.insert(10);
	g_actions.clear();
	ResetPrints();
	Update(1100);
	Check(FakeSlot(11).flashDuration == 0.0f,
		  "stale admin data still lets the target unblind");
	Check(g_actions.empty(), "stale admin data sends no admin action");
	Check(g_chatAllPrints.empty(), "stale admin data prints no chat line");
	g_adminDataNotReady.erase(10);

	// --- the same SteamID64 reconnects (a new controller serial) ----------------------
	g_actions.clear();
	ResetPrints();
	(void)DispatchCommand("mm_freezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Other", "1.0"});
	MutableSlot(10).generation += 1; // the same player, a new connection generation
	g_actions.clear();
	ResetPrints();
	Update(1100);
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "a reconnected admin still lets the target unfreeze");
	Check(g_actions.empty(), "a reconnected admin sends no admin action");
	Check(g_chatAllPrints.empty(), "a reconnected admin prints no chat line");

	// --- the console as the acting admin ---------------------------------------------
	(void)DispatchCommand("mm_freezy", -1, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Other", "1.0"});
	g_actions.clear();
	ResetPrints();
	Update(1100);
	Check(ActionCount("unfreeze") == 1 && LastAction()->slot == -1,
		  "the console actor is announced with the console slot");
	Check(HasChatAllPrint("Admin \017Console\001 unfroze"),
		  "the console actor is named \"Console\"");
	Check(g_createTimerCalls == 0, "no s2sdk timer was ever created");

	FinishTest();
}

void TestDelayedActionsDroppedOnMapEnd() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/freeze");
	(void)DispatchCommand("mm_freezy", 10, s2sdk::ConCommandContext::Console,
						  {"mm_freezy", "Target", "5.0"});
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeObsolete, "the target is frozen");

	FakeMapEnd();
	Check(!g_mapReady, "OnMapEnd closes the map gate");
	ResetClientCounters();
	Update();
	Check(g_clientCalls == 0, "the update after OnMapEnd makes no client call");
	FakeMapStart();
	Update(6000);
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeObsolete,
		  "a deadline from the previous map never fires on the next one");

	FinishTest();
}

// --- G. the native menus (PITFALLS C10) -----------------------------------------------------

void SetupMenuWorld() {
	MakeHuman(10, "Admin", 76561198000000010ULL);
	FakeClient& admin = MutableSlot(10);
	admin.generation = 2;
	FakeClient& alpha = MakeHuman(11, "Alpha", 76561198000000011ULL);
	alpha.generation = 4;
	MakeHuman(12, "Dead", 76561198000000012ULL, /*alive=*/false);
	FakeClient& bot = MakeBot(13, "Botty");
	bot.generation = 5;
	FakeClient& tv = MakeHuman(14, "SourceTV", 76561198000000014ULL);
	tv.sourceTV = true;
	tv.connected = true;
	tv.inGame = false;
	MakeHuman(15, "NoId", 0);
	FakeClient& notInGame = MakeHuman(16, "Lobby", 76561198000000016ULL);
	notInGame.inGame = false;
	g_menuTypes.insert("hud");
}

void TestMenuPlayerList() {
	PrepareTest();
	SetupMenuWorld();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/freeze");
	g_messageType = 2;

	// The admin core's select callback only queues: nothing is displayed from the handler.
	Check(SelectAdminItem(10, "freeze"), "the freeze item has a select callback");
	Check(OpenMenuOf(10) == nullptr, "no menu is created inside the admin core's callback");
	Update();
	const FakeMenu* menu = OpenMenuOf(10);
	Check(menu != nullptr, "the player list is displayed on the next update");
	if (menu == nullptr) {
		FinishTest();
		return;
	}
	Check(menu->title == "Select player", "the player list keeps its phrase title");
	Check(menu->type == "hud", "the default HUD_LAYOUT type is used when the hud backend exists");
	Check(menu->pagination == 5, "the player list is paginated five items per page");
	Check(menu->exitButton && menu->backButton, "exit and back buttons are enabled");
	Check(!menu->closeOnSelect, "the menu stays open after a selection");
	Check(g_lastMenuTimeout == kMenuTimeoutSeconds,
		  "the menu uses the original cs2-menus 60 s timeout");
	Check(menu->items.size() == 3,
		  "only the live, alive, identified players are listed (the admin included)");
	if (menu->items.size() == 3) {
		Check(menu->items[0].display == "Admin [OFF]", "the admin itself is listed");
		Check(menu->items[1].display == "Alpha [OFF]", "the freeze label of a live player is OFF");
		Check(menu->items[2].display == "Botty [OFF]", "a bot is listed with the freeze label");
		int32_t targetSlot = -1;
		uint64_t targetSteamID = 0;
		int32_t targetController = 0;
		Check(DecodeTarget(menu->items[1].info, targetSlot, targetSteamID, targetController),
			  "the player item carries a decodable identity");
		Check(targetSlot == 11 && targetSteamID == 76561198000000011ULL &&
				  targetController == ControllerHandle(11),
			  "the player item carries the slot, the SteamID64 and the controller handle");
		Check(DecodeTarget(menu->items[2].info, targetSlot, targetSteamID, targetController) &&
				  targetSlot == 13 && targetSteamID == 0,
			  "a bot item carries the slot and the zero SteamID64");
	}
	Check(!g_steamidReadForBot, "the player list never reads a bot's SteamID64");

	// --- selecting a player defers the action to OnPluginUpdate ------------------------
	ResetPrints();
	g_actions.clear();
	SelectByDisplay(10, "Alpha [OFF]");
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "the selection performs no engine action inside the menu handler");
	Check(g_actions.empty(), "the selection sends nothing inside the menu handler");
	Update();
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeObsolete,
		  "the selection is applied by OnPluginUpdate");
	Check(ActionCount("freeze") == 1 && LastAction()->param == "11",
		  "the freeze selection sends the original action payload");
	Check(HasChatAllPrint("Admin \017Admin\001 froze \004Alpha\001"),
		  "the freeze selection prints the original phrase");
	menu = OpenMenuOf(10);
	Check(menu != nullptr && menu->items.size() > 1 && menu->items[1].display == "Alpha [ON]",
		  "the player list is re-opened with the new state");

	// --- selecting the same player again unfreezes ----------------------------------
	g_actions.clear();
	SelectByDisplay(10, "Alpha [ON]");
	Update();
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "the second selection unfreezes the target");
	Check(ActionCount("unfreeze") == 1, "the second selection sends the unfreeze action");

	// --- a bot can be frozen through the menu ---------------------------------------
	g_actions.clear();
	SelectByDisplay(10, "Botty [OFF]");
	Update();
	Check(static_cast<int32_t>(FakeSlot(13).moveType) == kMoveTypeObsolete,
		  "a bot can be frozen through the menu");
	Check(ActionCount("freeze") == 1, "the bot freeze sends its action");

	// --- back goes to the admin core's last category --------------------------------
	BackByUser(10);
	Check(g_lastCategoryCalls.empty(), "the back press is deferred to OnPluginUpdate");
	Update();
	Check(!g_lastCategoryCalls.empty() && g_lastCategoryCalls.back() == 10,
		  "back from the player list re-opens the admin core's category");

	// --- exit just closes the menu ---------------------------------------------------
	Check(SelectAdminItem(10, "freeze"), "open the list again");
	Update();
	Check(OpenMenuOf(10) != nullptr, "the list is open before the exit");
	ExitByUser(10);
	Update();
	Check(OpenMenuOf(10) == nullptr, "exit closes the menu");
	Check(g_lastCategoryCalls.size() == 1, "exit does not re-open the admin category");

	// --- a menu timeout only closes the menu ----------------------------------------
	Check(SelectAdminItem(10, "freeze"), "open the list again");
	Update();
	CancelMenuFake(10, s2sdk::MenuCancelReason::Timeout);
	Update();
	Check(OpenMenuOf(10) == nullptr, "a timed-out menu is closed");

	FinishTest();
}

void TestMenuToggleItems() {
	PrepareTest();
	SetupMenuWorld();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	for (const char* permission : {"@admin/god", "@admin/bury", "@admin/unbury", "@admin/blind"}) {
		Grant(10, permission);
	}
	g_messageType = 0;

	// --- god -------------------------------------------------------------------------
	Check(SelectAdminItem(10, "god"), "the god item exists");
	Update();
	Check(OpenMenuOf(10) != nullptr && OpenMenuOf(10)->items.size() == 3,
		  "the god player list is displayed");
	Check(OpenMenuOf(10) != nullptr && OpenMenuOf(10)->items.size() > 1 &&
			  OpenMenuOf(10)->items[1].display == "Alpha [OFF]",
		  "god OFF means the player takes damage");
	SelectByDisplay(10, "Alpha [OFF]");
	Update();
	Check(!FakeSlot(11).takesDamage, "the god selection enables god mode");
	Check(OpenMenuOf(10) != nullptr && OpenMenuOf(10)->items.size() > 1 &&
			  OpenMenuOf(10)->items[1].display == "Alpha [ON]",
		  "the god list re-opens with the new state");
	g_actions.clear();
	SelectByDisplay(10, "Alpha [ON]");
	Update();
	Check(FakeSlot(11).takesDamage, "the second god selection disables god mode");
	Check(ActionCount("god") == 1 && LastAction()->param == "11 1",
		  "the second god selection sends the original payload");

	// --- bury / unbury ---------------------------------------------------------------
	MutableSlot(11).origin = plg::vec3{1.0f, 2.0f, 70.0f};
	Check(SelectAdminItem(10, "bury"), "the bury item exists");
	Update();
	Check(OpenMenuOf(10) != nullptr && OpenMenuOf(10)->items.size() > 1 &&
			  OpenMenuOf(10)->items[1].display == "Alpha",
		  "the bury list shows the plain player name");
	g_teleports.clear();
	SelectByDisplay(10, "Alpha");
	Update();
	Check(g_teleports.size() == 1 && g_teleports[0].origin.z == 60.0f,
		  "the bury selection teleports the target down");
	Check(OpenMenuOf(10) != nullptr && OpenMenuOf(10)->items.size() > 1 &&
			  OpenMenuOf(10)->items[1].display == "Alpha",
		  "the bury list re-opens");

	Check(SelectAdminItem(10, "unbury"), "the unbury item exists");
	Update();
	g_teleports.clear();
	SelectByDisplay(10, "Alpha");
	Update();
	Check(g_teleports.size() == 1 && g_teleports[0].origin.z == 70.0f,
		  "the unbury selection teleports the target up");

	// --- blind ----------------------------------------------------------------------
	Check(SelectAdminItem(10, "blind"), "the blind item exists");
	Update();
	Check(OpenMenuOf(10) != nullptr && OpenMenuOf(10)->items.size() > 1 &&
			  OpenMenuOf(10)->items[1].display == "Alpha [OFF]",
		  "the blind label starts OFF");
	SelectByDisplay(10, "Alpha [OFF]");
	Update();
	Check(FakeSlot(11).flashDuration == 9999.0f, "the blind selection blinds the target");
	Check(OpenMenuOf(10) != nullptr && OpenMenuOf(10)->items.size() > 1 &&
			  OpenMenuOf(10)->items[1].display == "Alpha [ON]",
		  "the blind list re-opens with the new state");
	g_actions.clear();
	SelectByDisplay(10, "Alpha [ON]");
	Update();
	Check(FakeSlot(11).flashDuration == 0.0f, "the second blind selection unblinds the target");
	Check(ActionCount("blind") == 1 && LastAction()->param == "11 0",
		  "the unblind selection sends the original payload");

	FinishTest();
}

void TestMenuValueMenus() {
	PrepareTest();
	SetupMenuWorld();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	for (const char* permission :
		 {"@admin/gravity", "@admin/hp", "@admin/armor", "@admin/speed", "@admin/scale"}) {
		Grant(10, permission);
	}
	g_messageType = 0;

	// --- gravity ---------------------------------------------------------------------
	Check(SelectAdminItem(10, "gravity"), "the gravity item exists");
	Update();
	Check(OpenMenuOf(10) != nullptr && OpenMenuOf(10)->items.size() > 1 &&
			  OpenMenuOf(10)->items[1].display == "Alpha [1.0x]",
		  "the gravity player list shows the current value");
	SelectByDisplay(10, "Alpha [1.0x]");
	Update();
	const FakeMenu* menu = OpenMenuOf(10);
	Check(menu != nullptr, "the gravity value menu is displayed");
	if (menu != nullptr) {
		Check(menu->title == "Alpha [1.0x]", "the gravity value menu keeps the original title");
		Check(menu->items.size() == 12, "the gravity value menu has twelve options");
		Check(menu->items[5].info == "1.0" && menu->items[5].display == "1x",
			  "the gravity options keep their values and labels");
		Check(menu->items[11].info == "10.0" && menu->items[11].display == "10x",
			  "the last gravity option is 10x");
	}
	g_actions.clear();
	SelectByDisplay(10, "2x");
	Check(FakeSlot(11).gravity == 1.0f, "the value selection performs no action in the handler");
	Update();
	Check(FakeSlot(11).gravity == 2.0f, "the gravity value is applied on the next update");
	Check(ActionCount("gravity") == 1 && LastAction()->param == "11 2.000000",
		  "the gravity value sends the original payload");
	menu = OpenMenuOf(10);
	Check(menu != nullptr && menu->title == "Alpha [2.0x]",
		  "the gravity menu re-opens with the new value");
	BackByUser(10);
	Update();
	menu = OpenMenuOf(10);
	Check(menu != nullptr && menu->title == "Select player",
		  "back from a value menu re-opens the player list");
	Check(menu != nullptr && menu->items.size() > 1 &&
			  menu->items[1].display == "Alpha [2.0x]",
		  "the re-opened player list shows the new gravity");

	// --- hp --------------------------------------------------------------------------
	Check(SelectAdminItem(10, "hp"), "the hp item exists");
	Update();
	SelectByDisplay(10, "Alpha [100]");
	Update();
	menu = OpenMenuOf(10);
	Check(menu != nullptr && menu->title == "Alpha [100]", "the hp value menu keeps its title");
	Check(menu != nullptr && menu->items.size() == 9 && menu->items[0].display == "1" &&
			  menu->items[8].display == "1000",
		  "the hp value menu has the original nine values");
	g_actions.clear();
	SelectByDisplay(10, "50");
	Update();
	Check(FakeSlot(11).health == 50, "the hp value is applied");
	Check(ActionCount("hp") == 1 && LastAction()->param == "11 50",
		  "the hp value sends the original payload");
	Check(OpenMenuOf(10) != nullptr && OpenMenuOf(10)->title == "Alpha [50]",
		  "the hp menu re-opens with the new value");

	// --- armor -----------------------------------------------------------------------
	Check(SelectAdminItem(10, "armor"), "the armor item exists");
	Update();
	SelectByDisplay(10, "Alpha [0]");
	Update();
	menu = OpenMenuOf(10);
	Check(menu != nullptr && menu->items.size() == 9, "the armor value menu has nine values");
	g_actions.clear();
	SelectByDisplay(10, "20");
	Update();
	Check(FakeSlot(11).armor == 20, "the armor value is applied");
	Check(ActionCount("armor") == 1 && LastAction()->param == "11 20",
		  "the armor value sends the original payload");

	// --- speed -----------------------------------------------------------------------
	Check(SelectAdminItem(10, "speed"), "the speed item exists");
	Update();
	Check(OpenMenuOf(10) != nullptr && OpenMenuOf(10)->items.size() > 1 &&
			  OpenMenuOf(10)->items[1].display == "Alpha [1.0x]",
		  "the speed player list shows the current speed");
	SelectByDisplay(10, "Alpha [1.0x]");
	Update();
	menu = OpenMenuOf(10);
	Check(menu != nullptr && menu->items.size() == 10 && menu->items[3].display == "1x",
		  "the speed value menu has ten options");
	g_actions.clear();
	SelectByDisplay(10, "1.5x");
	Update();
	Check(FakeSlot(11).speed == 1.5f, "the speed value is applied");
	Check(ActionCount("speed") == 1 && LastAction()->param == "11 1.500000",
		  "the speed value sends the original payload");

	// --- scale -----------------------------------------------------------------------
	Check(SelectAdminItem(10, "scale"), "the scale item exists");
	Update();
	SelectByDisplay(10, "Alpha [1.0x]");
	Update();
	menu = OpenMenuOf(10);
	Check(menu != nullptr && menu->items.size() == 10, "the scale value menu has ten options");
	g_actions.clear();
	SelectByDisplay(10, "10x");
	Update();
	Check(FakeSlot(11).scale == 10.0f, "the scale value is applied");
	Check(ActionCount("scale") == 1 && LastAction()->param == "11 10.000000",
		  "the scale value sends the original payload");

	FinishTest();
}

void TestMenuIdentityAndPermissions() {
	PrepareTest();
	SetupMenuWorld();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/freeze");
	g_messageType = 0;

	// --- a revoked permission stops an already-open selection -------------------------
	Check(SelectAdminItem(10, "freeze"), "open the freeze list");
	Update();
	Revoke(10, "@admin/freeze");
	SelectByDisplay(10, "Alpha [OFF]");
	Update();
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "a revoked permission stops the selection");
	Check(OpenMenuOf(10) == nullptr, "the menu of a denied selection is closed");
	Grant(10, "@admin/freeze");

	// --- pending admin data stops an already-open selection --------------------------
	Check(SelectAdminItem(10, "freeze"), "open the freeze list again");
	Update();
	g_adminDataNotReady.insert(10);
	SelectByDisplay(10, "Alpha [OFF]");
	Update();
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "a pending admin data load stops the selection");
	g_adminDataNotReady.erase(10);

	// --- a reused target slot never receives the action ------------------------------
	Check(SelectAdminItem(10, "freeze"), "open the freeze list again");
	Update();
	SelectByDisplay(10, "Alpha [OFF]");
	MutableSlot(11).generation += 1; // the connection was re-established
	Update();
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "a reused target slot never receives the action");
	const FakeMenu* menu = OpenMenuOf(10);
	Check(menu != nullptr && menu->title == "Select player",
		  "the list is re-opened for the new connection instead");

	// --- a reused admin slot never runs the new player's selection -------------------
	Check(SelectAdminItem(10, "freeze"), "open the freeze list one more time");
	Update();
	SelectByDisplay(10, "Alpha [OFF]");
	MutableSlot(10).generation += 1; // the admin's slot was reused
	Update();
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "a reused admin slot never runs the selection");
	Check(OpenMenuOf(10) == nullptr, "the stale menu of the old admin is closed");

	// --- a selection outside the visible page is ignored -----------------------------
	Check(SelectAdminItem(10, "freeze"), "open the freeze list again");
	Update();
	g_clientOffset[10] = 5; // the client is on page 2
	SelectMenuItemFake(10, 0);
	Update();
	Check(static_cast<int32_t>(FakeSlot(11).moveType) == kMoveTypeWalk,
		  "a digit of another page is ignored");
	g_clientOffset[10] = 0;

	// --- a category other than "players" is ignored ----------------------------------
	Check(SelectAdminItem(10, "freeze"), "open the freeze list again");
	Update();
	const int createCallsBefore = g_createMenuCalls;
	const FakeItem* freeze = FindFakeItem("freeze");
	Check(freeze != nullptr && freeze->select != nullptr, "the freeze item has its callback");
	if (freeze != nullptr && freeze->select != nullptr) {
		freeze->select(10, PlgTest("other"), PlgTest("freeze"), PlgTest("Item_Freeze"));
	}
	Update();
	Check(g_createMenuCalls == createCallsBefore, "a foreign category opens no player list");

	// --- a client without a SteamID64 / a bot cannot open a menu ---------------------
	g_pending[10] = Pending{};
	Check(SelectAdminItem(15, "freeze"), "the no-id client's select callback runs");
	Update();
	Check(OpenMenuOf(15) == nullptr, "a client without a SteamID64 gets no menu");
	MakeBot(17, "BotAdmin");
	Check(SelectAdminItem(17, "freeze"), "the bot's select callback runs");
	Update();
	Check(OpenMenuOf(17) == nullptr, "a bot gets no menu");

	FinishTest();
}

void TestMenuTypeResolution() {
	PrepareTest();
	SetupMenuWorld();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/freeze");

	const auto openAndCheck = [](const std::string& expected, const std::string& what) {
		Check(SelectAdminItem(10, "freeze"), "the freeze item opens a list");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "a menu is open for the menu-type check");
		if (menu != nullptr) {
			Check(menu->type == expected, what + " (got '" + menu->type + "')");
		}
	};

	// No player choice: pisexcompat's HUD_LAYOUT default.
	g_clientMenuTypes[10].clear();
	g_menuTypes.insert("hud");
	openAndCheck("hud", "HUD_LAYOUT maps to hud when the backend is registered");
	g_menuTypes.erase("hud");
	openAndCheck("button", "HUD_LAYOUT maps to button when the hud backend is missing");

	// The player's own "!menu" choice wins over the default.
	g_menuTypes.insert("hud");
	for (const char* choice : {"chat", "centerhtml", "button"}) {
		g_clientMenuTypes[10] = choice;
		openAndCheck(choice, std::string("the player's '") + choice + "' choice is used");
	}
	g_clientMenuTypes[10] = "hud";
	openAndCheck("hud", "the player's hud choice is used while the backend exists");
	g_menuTypes.erase("hud");
	openAndCheck("button", "a hud choice falls back to button without the backend");
	g_menuTypes.insert("wasd");
	g_clientMenuTypes[10] = "wasd";
	openAndCheck("wasd", "the player's wasd choice is used while the backend exists");
	g_menuTypes.erase("wasd");
	openAndCheck("button", "a wasd choice falls back to button without the backend");
	g_menuTypes.insert("hud");
	g_clientMenuTypes[10] = "unknown-type";
	openAndCheck("hud", "an unknown player choice falls through to the mapped type");

	// Without the optional hudmenu plugin the default mapping applies.
	__hudmenu_GetClientMenuType = nullptr;
	g_clientMenuTypes[10] = "chat";
	openAndCheck("hud", "without hudmenu the HUD_LAYOUT default is used");
	RestoreFakes();

	FinishTest();
}

void TestMenuZeroAndFailures() {
	PrepareTest();
	SetupMenuWorld();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/freeze");

	// --- MenuId 0 is burnt and re-created -------------------------------------------
	g_burnFirstMenuId = true;
	Check(SelectAdminItem(10, "freeze"), "open the freeze list");
	Update();
	Check(g_menuZeroBurned, "a menu handed out as id 0 is destroyed and re-created");
	Check(OpenMenuOf(10) != nullptr && OpenMenuOf(10)->id != 0,
		  "the re-created menu has a real id and is displayed");
	Check(g_createMenuCalls == 2, "the id-0 burn costs exactly one extra CreateMenu");
	g_burnFirstMenuId = false;
	StopPlugin();
	ResetWorld();
	SetupMenuWorld();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "restart");
	StartMap();
	Grant(10, "@admin/freeze");

	// --- a refused CreateMenu creates no menu ---------------------------------------
	g_createMenuFails = true;
	Check(SelectAdminItem(10, "freeze"), "open the freeze list");
	Update();
	Check(OpenMenuOf(10) == nullptr, "a refused CreateMenu leaves no menu");
	Check(HasLog("refused to create a menu"), "the refused CreateMenu is logged");
	g_createMenuFails = false;

	// --- a refused AddMenuItem destroys the half-built menu --------------------------
	g_addMenuItemFails = true;
	Check(SelectAdminItem(10, "freeze"), "open the freeze list");
	Update();
	Check(OpenMenuOf(10) == nullptr, "a refused AddMenuItem leaves no menu");
	Check(HasLog("refused to add a menu item"), "the refused AddMenuItem is logged");
	g_addMenuItemFails = false;

	// --- a refused DisplayMenu destroys the created menu ----------------------------
	g_displayMenuFails = true;
	g_destroyedMenuIds.clear();
	Check(SelectAdminItem(10, "freeze"), "open the freeze list");
	Update();
	Check(OpenMenuOf(10) == nullptr, "a refused DisplayMenu leaves no display");
	Check(HasLog("could not display a menu"), "the refused DisplayMenu is logged");
	Check(!g_destroyedMenuIds.empty(), "the created menu of a refused display is destroyed");
	g_displayMenuFails = false;

	// --- a throwing DisplayMenu is caught -------------------------------------------
	g_throwOnDisplayMenu = true;
	Check(SelectAdminItem(10, "freeze"), "open the freeze list");
	Update();
	Check(OpenMenuOf(10) == nullptr, "a throwing DisplayMenu leaves no display");
	g_throwOnDisplayMenu = false;

	FinishTest();
}

// --- H. the map gate (PITFALLS B1/B2) -------------------------------------------------------

void TestMapGateBeforeFirstMap() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	Check(!g_mapReady, "the gate is shut before the first map");
	Check(g_clientCalls == 0 && g_schemaCalls == 0, "the start makes no client/schema call");

	g_actions.clear();
	ResetPrints();
	Check(DispatchCommand("mm_god", 10, s2sdk::ConCommandContext::Console, {"mm_god", "Target"}) ==
			  s2sdk::ResultType::Handled,
		  "a client command before the first map is still owned");
	Check(g_clientCalls == 0, "an unready client command makes no client call");
	Check(g_actions.empty(), "an unready client command performs no action");

	Check(DispatchCommand("mm_god", -1, s2sdk::ConCommandContext::Console,
						  {"mm_god", "Target"}) == s2sdk::ResultType::Handled,
		  "a console command before the first map is handled");
	Check(g_clientCalls == 0 && g_schemaCalls == 0,
		  "an unready console command makes no client/schema call");
	Check(FakeSlot(11).takesDamage, "an unready console command touches no target");

	// A menu request before the first map cannot open anything either.
	Check(SelectAdminItem(10, "freeze"), "the select callback runs");
	Update();
	Check(OpenMenuOf(10) == nullptr, "no menu is displayed before the first map");
	Check(g_clientCalls == 0 && g_schemaCalls == 0, "the unready update makes no client call");

	FinishTest();
}

void TestMidMapLatch() {
	// A real map name latches the gate on the first update.
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	g_currentMap = "de_mirage";
	LatchMap();
	Check(g_mapReady, "the first update of a mid-map load latches the gate");
	StopPlugin();
	ResetWorld();

	// "<empty>" and "" are not a map.
	MakeHuman(10, "Admin", 76561198000000010ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	g_currentMap = "<empty>";
	LatchMap();
	Check(!g_mapReady, "an \"<empty>\" map name does not latch the gate");
	StopPlugin();
	ResetWorld();

	MakeHuman(10, "Admin", 76561198000000010ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	g_currentMap.clear();
	LatchMap();
	Check(!g_mapReady, "an empty map name does not latch the gate");
	StopPlugin();
	ResetWorld();

	// A missing GetCurrentMap binding cannot prove a map exists.
	MakeHuman(10, "Admin", 76561198000000010ULL);
	__s2sdk_GetCurrentMap = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	LatchMap();
	Check(!g_mapReady, "a missing GetCurrentMap binding does not latch the gate");
	StopPlugin();
	RestoreFakes();
	ResetWorld();

	// An OnMapEnd seen before the first update means the server is between maps.
	MakeHuman(10, "Admin", 76561198000000010ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	g_currentMap = "de_dust2";
	FakeMapEnd();
	Check(g_clientCalls == 0, "OnMapEnd makes no client call");
	LatchMap();
	Check(!g_mapReady, "an OnMapEnd before the first update keeps the gate shut");
	FakeMapStart();
	Update();
	Check(g_mapReady, "the next OnMapStart opens the gate again");

	FinishTest();
}

void TestMapEndWindow() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/freeze");
	Check(SelectAdminItem(10, "freeze"), "open the freeze list");
	Update();
	Check(OpenMenuOf(10) != nullptr, "a menu is open before the map end");

	ResetClientCounters();
	ResetSchemaCounters();
	FakeMapEnd();
	Check(!g_mapReady, "OnMapEnd shuts the gate");
	Check(g_clientCalls == 0, "OnMapEnd makes no client call");
	Check(g_schemaCalls == 0, "OnMapEnd makes no schema call");
	Check(OpenMenuOf(10) == nullptr, "OnMapEnd drops the per-slot menu bookkeeping");

	Update();
	Check(g_clientCalls == 0, "the update in the map-change window makes no client call");

	// A client command in the window is inert.
	DispatchCommand("mm_god", 10, s2sdk::ConCommandContext::Console, {"mm_god", "Target"});
	Check(g_clientCalls == 0, "a client command in the map-change window makes no client call");
	Check(FakeSlot(11).takesDamage, "a command in the map-change window touches no target");

	FakeMapStart();
	Update();
	Check(g_mapReady, "the next OnMapStart opens the gate again");
	Check(FakeSlot(11).takesDamage, "the target is still untouched after the map change");

	FinishTest();
}

void TestMapListenerPairs() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	__s2sdk_OnMapEnd_Unregister = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	Check(g_mapStartRegisters == 0 && g_mapEndRegisters == 0,
		  "an incomplete listener pair registers nothing");
	Check(HasLog("map listener bindings are incomplete"), "the incomplete pair is logged");
	g_currentMap = "de_dust2";
	LatchMap();
	Check(!g_mapReady, "without the listeners the gate can never open");
	ResetClientCounters();
	DispatchCommand("mm_god", -1, s2sdk::ConCommandContext::Console, {"mm_god", "Target"});
	Check(g_clientCalls == 0, "no client call while the gate cannot open");
	StopPlugin();
	RestoreFakes();
	FinishTest();
}

// --- I. the raw schema offsets (PITFALLS C6/C8) --------------------------------------------

void TestSchemaOffsets() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	Grant(10, "@admin/god");
	Grant(10, "@admin/blind");

	// The gate is still shut: not a single schema lookup.
	DispatchCommand("mm_god", -1, s2sdk::ConCommandContext::Console, {"mm_god", "Target"});
	Check(g_schemaCalls == 0 && g_offsetLookups == 0,
		  "no schema lookup happens while the gate is closed");

	// A field the schema does not have (yet) is not cached: the lookup is retried on the next
	// use, and only the resolvable fields are written. The chain offset is probed at the same
	// time; 0 means "the object itself is the entity".
	const int32_t savedFlashOffset = g_fieldOffsets["CCSPlayerPawnBase::m_flFlashDuration"];
	g_fieldOffsets.erase("CCSPlayerPawnBase::m_flFlashDuration");
	g_chainOffsets["CCSPlayerPawnBase"] = 0;
	StartMap();
	ResetSchemaCounters();
	g_schemaWrites.clear();
	DispatchCommand("mm_blind", -1, s2sdk::ConCommandContext::Console, {"mm_blind", "Target"});
	// 5 fields + 2 chains, then the failed field is retried by IsBlinded and the "<empty>" chain
	// offset (normalised to -1, therefore never cached) is probed again.
	Check(g_offsetLookups == 9,
		  "the five fields and two chain offsets are probed, the failed ones retried");
	Check(g_schemaWrites.size() == 3, "the three resolvable blind fields are still written");
	Check(HasLog("schema field(s) not found"), "the missing schema field is logged");
	if (g_schemaWrites.size() == 3) {
		Check(g_schemaWrites[0].changeState && g_schemaWrites[0].chainOffset == -1,
			  "a non-positive chain offset is normalised to -1");
	}

	// Once the field resolves it is written and cached; the retried lookup happens exactly once.
	g_fieldOffsets["CCSPlayerPawnBase::m_flFlashDuration"] = savedFlashOffset;
	g_chainOffsets["CCSPlayerPawnBase"] = 8;
	ResetSchemaCounters();
	g_schemaWrites.clear();
	DispatchCommand("mm_blind", -1, s2sdk::ConCommandContext::Console,
					{"mm_blind", "Target", "0"});
	Check(g_offsetLookups == 2, "only the previously failed field and chain are looked up again");
	Check(g_schemaWrites.size() == 4, "all four blind fields are written once it resolves");
	Check(g_schemaWrites.size() == 4 && g_schemaWrites[0].changeState &&
			  g_schemaWrites[0].chainOffset == 8,
		  "the resolved chain offset is used for the networked flash field");

	ResetSchemaCounters();
	DispatchCommand("mm_god", -1, s2sdk::ConCommandContext::Console, {"mm_god", "Target"});
	Check(g_offsetLookups == 0, "cached offsets are not resolved again");

	// Unload clears the cache, so a reload resolves against the live map again.
	StopPlugin();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "reload start");
	StartMap();
	ResetSchemaCounters();
	DispatchCommand("mm_god", -1, s2sdk::ConCommandContext::Console, {"mm_god", "Target"});
	Check(g_offsetLookups == 7, "the reload resolves the schema offsets again");

	FinishTest();
}

// --- J. admin item registration and retirement (PITFALLS A7) -------------------------------

void TestAdminItemRetirement() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	Check(g_items.size() == 10, "the ten items are registered");
	StopPlugin();
	for (const ItemDef& def : kItems) {
		const FakeItem* item = FindFakeItem(def.szIdentity);
		Check(item != nullptr, std::string("the item '") + def.szIdentity + "' is still listed");
		if (item != nullptr) {
			Check(item->display == nullptr && item->select == nullptr,
				  std::string("the item '") + def.szIdentity +
					  "' has null callbacks after the unload");
			Check(item->flags == def.szPermission,
				  std::string("the item '") + def.szIdentity + "' keeps its permission");
		}
	}
	const FakeCategory* category = FindFakeCategory("players");
	Check(category != nullptr && category->display == nullptr,
		  "the category keeps its null display callback");

	// A missing RegisterItem binding at unload time fails soft.
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "second start");
	__admin_system_RegisterItem = nullptr;
	StopPlugin();
	Check(HasLog("cannot be retired"), "the missing retirement binding is logged");
	RestoreFakes();

	// A missing menu API keeps the items (and the category) unregistered.
	ResetWorld();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	__s2sdk_CreateMenu = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without the menu API");
	Check(g_items.empty(), "no items are registered without the menu API");
	Check(!HasCategory("players"), "the category is not registered without the menu API");
	Check(g_coreLoadedRegisters == 1, "the admin core listener is still registered");
	Check(HasLog("menu bindings are unavailable"), "the missing menu API is logged");
	StopPlugin();
	RestoreFakes();

	// A missing admin API registers neither the listener nor the items.
	ResetWorld();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	__admin_system_RegisterCategory = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without the admin API");
	Check(g_coreLoadedRegisters == 0, "no admin core listener without the admin API");
	Check(g_items.empty(), "no items without the admin API");
	Check(g_addedCommands.size() == 12, "the commands are still registered");
	StopPlugin();
	RestoreFakes();

	FinishTest();
}

// --- K. paired registrations (PITFALLS A7) -------------------------------------------------

void TestPairedBindings() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	__s2sdk_RemoveCommandListener = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without RemoveCommandListener");
	Check(g_addedCommands.empty(), "no command is registered without the removal pair");
	Check(HasLog("no mm_* command is registered"), "the missing removal binding is logged");
	StopPlugin();
	RestoreFakes();

	ResetWorld();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	__s2sdk_RemoveCommand = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without RemoveCommand");
	Check(g_addedCommands.empty(), "no command is registered without RemoveCommand");
	StopPlugin();
	RestoreFakes();

	ResetWorld();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	__s2sdk_AddConsoleCommand = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without AddConsoleCommand");
	Check(g_addedCommands.empty(), "no command is registered without AddConsoleCommand");
	StopPlugin();
	RestoreFakes();

	ResetWorld();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	__admin_system_OnCoreLoaded_Unregister = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without the core unregister");
	Check(g_coreLoadedRegisters == 0, "no admin core listener without its removal half");
	Check(g_items.empty(), "no admin items without the core listener");
	Check(g_addedCommands.size() == 12, "the commands do not depend on the admin core");
	StopPlugin();
	RestoreFakes();

	FinishTest();
}

// --- L. safe menu destruction (PITFALLS B2/A7) ---------------------------------------------

void TestSafeMenuDestruction() {
	// --- a map-up unload closes the display with Disconnect --------------------------
	PrepareTest();
	SetupMenuWorld();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/freeze");
	Check(SelectAdminItem(10, "freeze"), "open the freeze list");
	Update();
	const FakeMenu* menu = OpenMenuOf(10);
	Check(menu != nullptr, "a menu is open before the unload");
	const s2sdk::MenuId menuId = menu != nullptr ? menu->id : 0;
	StopPlugin();
	Check(g_destroyedMenuIds.end() !=
			  std::find(g_destroyedMenuIds.begin(), g_destroyedMenuIds.end(), menuId),
		  "the open menu is destroyed on a map-up unload");
	Check(!g_cancelledMenus.empty() &&
			  g_cancelledMenus.back().second == s2sdk::MenuCancelReason::Disconnect,
		  "the display is ended with Disconnect before the destroy");
	Check(g_liveBackendCloses == 0, "no backend close reaches the client layer on unload");

	// --- a map-down unload whose cancel is refused switches to a no-op type ----------
	PrepareTest();
	SetupMenuWorld();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/freeze");
	Check(SelectAdminItem(10, "freeze"), "open the freeze list");
	Update();
	g_cancelFails = true;
	ResetClientCounters();
	ResetSchemaCounters();
	FakeMapEnd();
	Update();
	Check(g_clientCalls == 0, "the refused cancel in the map-change window makes no client call");
	Check(g_liveBackendCloses == 0,
		  "a display that survives the cancel is switched to a no-op-close type");
	Check(g_setMenuTypeCalls > 0, "the menu type was switched before the destroy");
	Check(g_destroyMenuCalls > 0, "the menu is destroyed after the switch");
	g_cancelFails = false;

	// --- a cancel that lies is re-queried and handled the same way -------------------
	PrepareTest();
	SetupMenuWorld();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/freeze");
	Check(SelectAdminItem(10, "freeze"), "open the freeze list");
	Update();
	g_cancelLies = true;
	ResetClientCounters();
	FakeMapEnd();
	Update();
	Check(g_clientCalls == 0, "the lying cancel is re-queried without a client call");
	Check(g_liveBackendCloses == 0, "the still-displayed menu is switched to a no-op type");
	Check(g_destroyMenuCalls > 0, "the still-displayed menu is destroyed");
	g_cancelLies = false;

	// --- when even the switch is impossible the request is parked and reported -------
	PrepareTest();
	SetupMenuWorld();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/freeze");
	Check(SelectAdminItem(10, "freeze"), "open the freeze list");
	Update();
	g_cancelFails = true;
	g_setMenuTypeFails = true;
	ResetClientCounters();
	FakeMapEnd();
	Update();
	Check(g_clientCalls == 0, "the unresolvable destroy makes no client call");
	Check(HasLog("A7_UNRESOLVED"), "the unresolvable menu is reported as A7_UNRESOLVED");
	Check(g_destroyMenuCalls == 0, "the unresolvable menu is not destroyed blindly");
	g_setMenuTypeFails = false;
	g_cancelFails = false;
	FakeMapStart();
	Update();
	Check(g_destroyMenuCalls > 0, "the parked menu is destroyed once a map is back");

	FinishTest();
}

// --- M. unload and reload (PITFALLS A7) ----------------------------------------------------

void TestUnloadAndReload() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/god");

	// Leave a pending selection behind, then unload.
	Check(SelectAdminItem(10, "freeze"), "queue a menu selection");
	const s2sdk::ConCommandCallback godCallback =
		FindAddedCommand("mm_god") != nullptr ? FindAddedCommand("mm_god")->callback : nullptr;
	StopPlugin();
	for (const CommandDef& def : g_commands) {
		Check(RemovedCommand(def.szName), std::string(def.szName) + " is removed on unload");
	}
	Check(g_mapStartUnregisters == 1, "the map start listener is unregistered");
	Check(g_mapEndUnregisters == 1, "the map end listener is unregistered");
	Check(g_coreLoadedUnregisters == 1, "the admin core listener is unregistered");
	Check(g_pAdminApi == nullptr, "the admin API pointer is cleared");
	for (const CommandDef& def : g_commands) {
		const FakeCommand* command = FindAddedCommand(def.szName);
		const s2sdk::ConCommandCallback callback = command != nullptr ? command->callback : nullptr;
		Check(callback != nullptr && RemovedCommandWith(def.szName, callback),
			  std::string(def.szName) + " is removed with its exact callback");
	}

	// A stale command callback cannot reach a client after the unload.
	ResetClientCounters();
	ResetSchemaCounters();
	g_actions.clear();
	if (godCallback != nullptr) {
		Check(godCallback(-1, s2sdk::ConCommandContext::Console, ToArgs({"mm_god", "Target"})) ==
				  s2sdk::ResultType::Handled,
			  "the stale callback is still handled");
	}
	Check(g_clientCalls == 0, "a stale callback makes no client call after the unload");
	Check(g_actions.empty(), "a stale callback performs no action after the unload");

	// The update loop after the unload is inert.
	Update();
	Check(g_clientCalls == 0, "the update after the unload makes no client call");

	// A second unload is idempotent.
	StopPlugin();
	Check(g_mapStartUnregisters == 1, "a second unload does not unregister the map listener twice");

	// A reload starts cleanly and re-registers everything.
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	g_addedCommands.clear();
	g_removedCommands.clear();
	g_removedListeners.clear();
	g_mapStartRegisters = 0;
	g_mapEndRegisters = 0;
	g_coreLoadedRegisters = 0;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "reload start");
	Check(g_addedCommands.size() == 12, "the reload registers the commands again");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1,
		  "the reload registers the map listeners");
	Check(g_items.size() == 10, "the reload registers the admin items");
	StartMap();
	Grant(10, "@admin/god");
	g_actions.clear();
	DispatchCommand("mm_god", 10, s2sdk::ConCommandContext::Console, {"mm_god", "Target"});
	Check(ActionCount("god") == 1, "the reloaded plugin works");

	FinishTest();
}

// --- N. missing bindings and exceptions ----------------------------------------------------

void TestMissingBindings() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);

	// A completely null s2sdk surface: the plugin starts, updates and unloads without a crash.
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapStart_Unregister = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_OnMapEnd_Unregister = nullptr;
	__s2sdk_GetCurrentMap = nullptr;
	__s2sdk_IsClientConnected = nullptr;
	__s2sdk_IsClientInGame = nullptr;
	__s2sdk_IsClientSourceTV = nullptr;
	__s2sdk_IsFakeClient = nullptr;
	__s2sdk_IsClientAlive = nullptr;
	__s2sdk_GetClientName = nullptr;
	__s2sdk_GetClientSteamID64 = nullptr;
	__s2sdk_PlayerSlotToEntHandle = nullptr;
	__s2sdk_IsValidEntHandle = nullptr;
	__s2sdk_GetClientPawn = nullptr;
	__s2sdk_EntPointerToEntHandle = nullptr;
	__s2sdk_GetSchemaOffset = nullptr;
	__s2sdk_GetSchemaChainOffset = nullptr;
	__s2sdk_SetEntData = nullptr;
	__s2sdk_SetEntDataFloat = nullptr;
	__s2sdk_PrintToChatAll = nullptr;
	__s2sdk_PrintToConsole = nullptr;
	__s2sdk_PrintToServer = nullptr;
	__s2sdk_AddConsoleCommand = nullptr;
	__s2sdk_RemoveCommand = nullptr;
	__s2sdk_RemoveCommandListener = nullptr;
	__s2sdk_CreateMenu = nullptr;
	__s2sdk_DisplayMenu = nullptr;
	__s2sdk_AddMenuItem = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "a null s2sdk surface still starts");
	Update();
	Check(g_addedCommands.empty(), "no command is registered without the command bindings");
	StopPlugin();
	RestoreFakes();
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "the restored surface starts");
	StopPlugin();
	ResetWorld();

	// No admin_system at all: the server console still works, the client paths deny.
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	__admin_system_GetPluginVersion = nullptr;
	__admin_system_GetTranslation = nullptr;
	__admin_system_GetMessageType = nullptr;
	__admin_system_IsCoreLoaded = nullptr;
	__admin_system_HasPermission = nullptr;
	__admin_system_IsClientAdminDataReady = nullptr;
	__admin_system_SendAction = nullptr;
	__admin_system_RegisterCategory = nullptr;
	__admin_system_RegisterItem = nullptr;
	__admin_system_OnCoreLoaded_Register = nullptr;
	__admin_system_OnCoreLoaded_Unregister = nullptr;
	__admin_system_ShowAdminLastCategoryMenu = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "a null admin_system surface still starts");
	StartMap();
	Check(g_addedCommands.size() == 12, "the commands are still registered");
	DispatchCommand("mm_hp", -1, s2sdk::ConCommandContext::Console, {"mm_hp", "Target", "55"});
	Check(FakeSlot(11).health == 55, "the server console works without the admin core");
	DispatchCommand("mm_hp", 10, s2sdk::ConCommandContext::Console, {"mm_hp", "Target", "60"});
	Check(FakeSlot(11).health == 55, "a client is denied without the admin core");
	StopPlugin();
	RestoreFakes();
	ResetWorld();

	// No plg::Log at all: the plugin must not crash while logging.
	MakeHuman(10, "Admin", 76561198000000010ULL);
	plg::Log = nullptr;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "a null plg::Log still starts");
	StartMap();
	StopPlugin();
	RestoreFakes();

	FinishTest();
}

void TestExceptions() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	StartMap();
	Grant(10, "@admin/gravity");
	Grant(10, "@admin/hp");

	// A throwing name lookup is caught by the command dispatcher.
	g_throwOnClientName = true;
	g_actions.clear();
	Check(DispatchCommand("mm_hp", 10, s2sdk::ConCommandContext::Console,
						  {"mm_hp", "Target", "50"}) == s2sdk::ResultType::Handled,
		  "a throwing client query is caught by the command hook");
	Check(g_actions.empty(), "the failed command performs no action");
	g_throwOnClientName = false;

	// A throwing schema lookup resolves to -1 and writes nothing.
	g_throwOnSchemaOffset = true;
	g_schemaWrites.clear();
	(void)DispatchCommand("mm_hp", 10, s2sdk::ConCommandContext::Console,
						  {"mm_hp", "Target", "50"});
	g_throwOnSchemaOffset = false;
	Check(g_schemaWrites.empty(), "a throwing schema lookup writes nothing");

	// A throwing client setter is caught; the announce/action still go out (like the original).
	g_throwOnSetGravity = true;
	g_actions.clear();
	(void)DispatchCommand("mm_gravity", 10, s2sdk::ConCommandContext::Console,
						  {"mm_gravity", "Target", "0.5"});
	g_throwOnSetGravity = false;
	Check(ActionCount("gravity") == 1, "a throwing setter does not stop the action");

	FinishTest();
}


} // namespace

int main() {
	InstallFakes();
	ResetWorld();


	TestStartCommandsAndContexts();
	TestPreexistingCommandNames();
	TestAdminReadiness();
	TestMissingAdminBindings();
	TestSignedHandles();
	TestUnknownNameAndBots();
	TestValueCommands();
	TestGodAndFreeze();
	TestBlindAndBury();
	TestDelayedActions();
	TestDelayedActionsDroppedOnMapEnd();
	TestMenuPlayerList();
	TestMenuToggleItems();
	TestMenuValueMenus();
	TestMenuIdentityAndPermissions();
	TestMenuTypeResolution();
	TestMenuZeroAndFailures();
	TestMapGateBeforeFirstMap();
	TestMidMapLatch();
	TestMapEndWindow();
	TestMapListenerPairs();
	TestSchemaOffsets();
	TestAdminItemRetirement();
	TestPairedBindings();
	TestSafeMenuDestruction();
	TestUnloadAndReload();
	TestMissingBindings();
	TestExceptions();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
