#pragma once

#include "DerivedDataCache/DerivedDataBuildFunction.h"

namespace Durin
{
	using namespace ::Durin::DerivedData;

	auto EnsureTerrainBuildFunctions(
		std::string* OutError = nullptr, FModuleOwnedCallbackGate Gate = {}) -> bool;
	auto InitializeTerrainBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError = nullptr) -> bool;
	auto ShutdownTerrainBuildFunctions() -> void;
}
