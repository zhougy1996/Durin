#pragma once

#include "CoreAPI.h"

namespace Durin
{
	namespace PathUtilities
	{
		inline constexpr std::string_view ProjectContentMountRoot = "/Game/";

		// Maps one normalized virtual root to its physical directory.
		struct FMountPoint
		{
			std::string VirtualRoot;
			std::string PhysicalPath;
		};

		CORE_API auto GetRegisteredMountPoints() -> const std::vector<FMountPoint>&;

		CORE_API auto RegisterMountPoint(std::string_view VirtualRoot, std::string_view PhysicalPath) -> void;

		CORE_API auto InitDefaultMountPoints() -> void;
	}

	// Resolves engine and active-project locations from process-wide path state.
	class FPaths
	{
	public:
		static CORE_API auto SetProjectFile(std::string_view ProjectFile, std::string* OutError = nullptr) -> bool;
		static CORE_API auto ProjectFile() -> std::string;

		static CORE_API auto LaunchDir() ->  std::string;

		static CORE_API auto RootDir() -> std::string;

		static CORE_API auto EngineDir() -> std::string;

		static CORE_API auto ProjectDir() -> std::string;
		static CORE_API auto DerivedDataCacheDir() -> std::string;
		static CORE_API auto SetDerivedDataCacheDirForTests(std::string_view Directory) -> void;

		static CORE_API auto EngineContentDir() -> std::string;

		static CORE_API auto EngineBinariesDir() -> std::string;

		static CORE_API auto EngineThirdPartyRuntimeBinariesDir() -> std::string;

		// Resolve virtual path to physical path.
		// For example, "/Engine/" to "DURIN_ROOT/Engine/", or "/Game/" to the active project's Content directory.
		static CORE_API auto Resolve(std::string_view VirtualPath) -> std::string;
	};
}
