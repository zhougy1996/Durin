#include "Misc/Version.h"

namespace Durin
{
	namespace
	{
		constexpr FEngineVersion GEngineVersion{
			DURIN_ENGINE_VERSION_MAJOR,
			DURIN_ENGINE_VERSION_MINOR,
			DURIN_ENGINE_VERSION_PATCH,
			DURIN_ENGINE_VERSION_CHANNEL
		};
	}

	auto GetEngineVersion() -> const FEngineVersion& { return GEngineVersion; }
	auto GetEngineVersionString() -> std::string_view { return DURIN_ENGINE_VERSION_STRING; }
}
