#pragma once

namespace Doge
{
	class FPaths
	{
	public:
		static CORE_API auto LaunchDir() ->  std::string;

		static CORE_API auto RootDir() -> std::string;

		static CORE_API auto EngineDir() -> std::string;

		static CORE_API auto EngineContentDir() -> std::string;

		static CORE_API auto EngineBinariesDir() -> std::string;

		static CORE_API auto EngineThirdPartyRuntimeBinariesDir() -> std::string;
	};
}