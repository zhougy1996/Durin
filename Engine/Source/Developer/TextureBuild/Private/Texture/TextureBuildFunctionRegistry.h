#pragma once

#include "AssetBuild/BuildFunction.h"

namespace Durin::Asset::Build
{
	auto EnsureTextureBuildFunctions(
		std::string* OutError = nullptr, FModuleOwnedCallbackGate Gate = {}) -> bool;
	auto InitializeTextureBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError = nullptr) -> bool;
	auto ShutdownTextureBuildFunctions() -> void;
}
