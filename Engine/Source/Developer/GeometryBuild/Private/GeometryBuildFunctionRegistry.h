#pragma once

#include "AssetBuild/BuildFunction.h"

namespace Durin::Asset::Build
{
	auto EnsureGeometryBuildFunctions(
		std::string* OutError = nullptr, FModuleOwnedCallbackGate Gate = {}) -> bool;
	auto InitializeGeometryBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError = nullptr) -> bool;
	auto ShutdownGeometryBuildFunctions() -> void;
}
