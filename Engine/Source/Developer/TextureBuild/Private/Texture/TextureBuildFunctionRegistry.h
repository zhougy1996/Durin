#pragma once

#include "DerivedDataCache/DerivedDataBuildFunction.h"

namespace Durin
{
	using namespace ::Durin::DerivedData;

	auto EnsureTextureBuildFunctions(
		std::string* OutError = nullptr, FModuleOwnedCallbackGate Gate = {}) -> bool;
	auto InitializeTextureBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError = nullptr) -> bool;
	auto ShutdownTextureBuildFunctions() -> void;
}
