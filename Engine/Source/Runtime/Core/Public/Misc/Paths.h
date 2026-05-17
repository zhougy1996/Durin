#pragma once

#include "CoreAPI.h"

namespace Durin
{
	namespace PathUtilities
	{
		struct FMountPoint
		{
			std::string VirtualRoot;
			std::string PhysicalPath;
		};

		CORE_API auto GetRegisteredMountPoints() -> const std::vector<FMountPoint>&;

		CORE_API auto RegisterMountPoint(std::string_view VirtualRoot, std::string_view PhysicalPath) -> void;

		CORE_API auto InitDefaultMountPoints() -> void;
	}

	class FPaths
	{
	public:
		static CORE_API auto LaunchDir() ->  std::string;

		static CORE_API auto RootDir() -> std::string;

		static CORE_API auto EngineDir() -> std::string;

		static CORE_API auto ProjectDir() -> std::string;

		static CORE_API auto EngineContentDir() -> std::string;

		static CORE_API auto EngineBinariesDir() -> std::string;

		static CORE_API auto EngineThirdPartyRuntimeBinariesDir() -> std::string;

		// Resolve virtual path to physical path.
		// For example,  "/Engine/" to "DOGE_ROOT/Engine/", or "/MyProject/" to "MyProject_Dir/".
		static CORE_API auto Resolve(std::string_view VirtualPath) -> std::string;
	};
}