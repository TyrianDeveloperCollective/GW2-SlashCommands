#include "Addon.h"

#include <format>
#include <map>
#include <mutex>
#include <queue>
#include <string>

#include "GW2RE/Game/_Patterns.h"
#include "GW2RE/Game/Game/EventApi.h"
#include "GW2RE/Util/Hook.h"
#include "GW2RE/Util/Validation.h"
#include "Util/src/Strings.h"

#include "Remote.h"
#include "Version.h"

/* Helper functions for Hook. */
FUNC_HOOKCREATE  HookCreate{};
FUNC_HOOKREMOVE  HookRemove{};
FUNC_HOOKENABLE  HookEnable{};
FUNC_HOOKDISABLE HookDisable{};

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
	static AddonDefinition_t s_AddonDef;

	s_AddonDef.Signature        = ADDON_SIG;
	s_AddonDef.APIVersion       = NEXUS_API_VERSION;
	s_AddonDef.Name             = ADDON_NAME;
	s_AddonDef.Version.Major    = V_MAJOR;
	s_AddonDef.Version.Minor    = V_MINOR;
	s_AddonDef.Version.Build    = V_BUILD;
	s_AddonDef.Version.Revision = V_REVISION;
	s_AddonDef.Author           = "Tyrian Developer Collective";
	s_AddonDef.Description      = "Adds InputBinds for commands as well as an API for other addons.";
	s_AddonDef.Load             = Addon::Load;
	s_AddonDef.Unload           = Addon::Unload;
	s_AddonDef.Flags            = static_cast<EAddonFlags>(1 << 4);

	s_AddonDef.Provider         = UP_GitHub;
	s_AddonDef.UpdateLink       = REMOTE_URL;

	return &s_AddonDef;
}

namespace Addon
{
	static AddonAPI_t* s_APIDefs{};
	static std::string s_Error{};

	typedef uint64_t (__fastcall*FUNC_ONCOMMAND)(const wchar_t* aCommand);
	static GW2RE::Hook<FUNC_ONCOMMAND>* s_CmdHandlerHook{};

	///----------------------------------------------------------------------------------------------------
	/// s_Commands:
	/// 	List of commands for which InputBinds are registered.
	///----------------------------------------------------------------------------------------------------
	static const std::vector<std::string> s_Commands{
		"/gg",

		/* Emotes */
		"/barbecue",
		"/beckon",
		"/bless",
		"/bloodstoneboogie",
		"/blowkiss",
		"/bow",
		"/breakdance",
		"/channel",
		"/cheer",
		"/cower",
		"/crabdance",
		"/crossarms",
		"/cry",
		"/dance",
		"/drink",
		"/facepalm",
		"/geargrind",
		"/heroic",
		"/hiss",
		"/kneel",
		"/laugh",
		"/magicjuggle",
		"/magictrick",
		"/no",
		"/paper",
		"/petalthrow",
		"/playdead",
		"/point",
		"/ponder",
		"/posecover",
		"/posehigh",
		"/poselow",
		"/posetwist",
		"/possessed",
		"/readbook",
		"/rock",
		"/rockout",
		"/sad",
		"/salute",
		"/scissors",
		"/serve",
		"/shiver",
		"/shiverplus",
		"/shocked",
		"/shrug",
		"/shuffle",
		"/sipcoffee",
		"/sit",
		"/sleep",
		"/stretch",
		"/step",
		"/surprised",
		"/talk",
		"/thank",
		"/threaten",
		"/thumbsdown",
		"/thumbsup",
		"/unleash",
		"/wave",
		"/yes"
	};

	///----------------------------------------------------------------------------------------------------
	/// s_QueuedCommands:
	/// 	List of queued commands to execute.
	///----------------------------------------------------------------------------------------------------
	static std::queue<std::wstring> s_QueuedCommands{};
	static std::mutex               s_QueuedCommandsMutex{};

	///----------------------------------------------------------------------------------------------------
	/// s_CustomCommands:
	/// 	Lookup table for custom registered commands.
	///----------------------------------------------------------------------------------------------------
	static std::map<std::string, std::string> s_CustomCommands{};
	static std::mutex                         s_CustomCommandsMutex{};

	void Load(AddonAPI_t* aApi)
	{
		s_APIDefs = aApi;

		FUNC_ONCOMMAND cmdHandler = GW2RE::Pointers::SendCommand.Scan<FUNC_ONCOMMAND>();

		GW2RE::Validate(cmdHandler, s_Error, "Command handler not registered.\n");

		s_Error += GW2RE::RunDiag();

		if (!s_Error.empty())
		{
			s_APIDefs->Log(LOGL_CRITICAL, ADDON_NAME, std::format("Cancelled load:\n{}", s_Error.c_str()).c_str());
			return;
		}

		/* Set these for the Hook implementation. */
		HookCreate = reinterpret_cast<FUNC_HOOKCREATE>(s_APIDefs->MinHook_Create);
		HookRemove = reinterpret_cast<FUNC_HOOKREMOVE>(s_APIDefs->MinHook_Remove);
		HookEnable = reinterpret_cast<FUNC_HOOKENABLE>(s_APIDefs->MinHook_Enable);
		HookDisable = reinterpret_cast<FUNC_HOOKDISABLE>(s_APIDefs->MinHook_Disable);

		/* Create hook. */
		s_CmdHandlerHook = new GW2RE::Hook<FUNC_ONCOMMAND>(cmdHandler, OnCommand);
		s_CmdHandlerHook->Enable();

		GW2RE::CEventApi::Get().Register(GW2RE::EEngineEvent::EngineTick, OnEngineTick);

		for (const std::string& command : s_Commands)
		{
			s_APIDefs->InputBinds_RegisterWithString(command.c_str(), OnInputBind, "(null)");
		}
	}

	void Unload()
	{
		/* If never initalized. */
		if (!s_Error.empty())
		{
			return;
		}

		for (const std::string& command : s_Commands)
		{
			s_APIDefs->InputBinds_Deregister(command.c_str());
		}

		GW2RE::CEventApi::Get().Deregister(GW2RE::EEngineEvent::EngineTick, OnEngineTick);

		if (s_CmdHandlerHook)
		{
			GW2RE::DestroyHook(s_CmdHandlerHook);
		}
	}

	void OnInputBind(const char* aIdentifier, bool aIsRelease)
	{
		/* Only handle presses. */
		if (aIsRelease) { return; }

		if (std::find(s_Commands.begin(), s_Commands.end(), aIdentifier) != s_Commands.end())
		{
			const std::lock_guard<std::mutex> lock(s_QueuedCommandsMutex);
			s_QueuedCommands.push(String::ToWString(aIdentifier));
		}
	}

	uint64_t __fastcall OnCommand(const wchar_t* aCommand)
	{
		const std::lock_guard<std::mutex> lock(s_CmdHandlerHook->Mutex);

		uint64_t result = s_CmdHandlerHook->OriginalFunction(aCommand);

#ifdef _DEBUG
		std::string cmdText = String::ToString(aCommand);

		s_APIDefs->Log(
			LOGL_DEBUG,
			ADDON_NAME,
			std::format(
				"OnCommand\n"
				"\tCommand text: {}\n"
				"\tCommand result: {}",
				cmdText.c_str(),
				result
			).c_str()
		);
#endif

		return result;
	}

	uint64_t __fastcall OnEngineTick(void*, void*)
	{
		const std::lock_guard<std::mutex> lock(s_QueuedCommandsMutex);

		if (!s_QueuedCommands.empty())
		{
			const std::wstring& cmd = s_QueuedCommands.front();
			OnCommand(cmd.c_str());
			s_QueuedCommands.pop();
		}

		return 0;
	}
}
