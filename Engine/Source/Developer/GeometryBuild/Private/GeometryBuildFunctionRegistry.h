#pragma once

#include "DerivedDataCache/DerivedDataBuildFunction.h"

namespace Durin::Asset::Build
{
	using namespace ::Durin::DerivedData;

	auto EnsureGeometryBuildFunctions(
		std::string* OutError = nullptr, FModuleOwnedCallbackGate Gate = {}) -> bool;
	auto InitializeGeometryBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError = nullptr) -> bool;
	auto ShutdownGeometryBuildFunctions() -> void;
}
