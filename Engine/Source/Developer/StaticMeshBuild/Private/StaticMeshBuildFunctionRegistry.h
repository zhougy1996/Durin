#pragma once

#include "DerivedDataCache/DerivedDataBuildFunction.h"

namespace Durin::Asset::Build
{
	using namespace ::Durin::DerivedData;

	auto EnsureStaticMeshBuildFunctions(
		std::string* OutError = nullptr, FModuleOwnedCallbackGate Gate = {}) -> bool;
	auto InitializeStaticMeshBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError = nullptr) -> bool;
	auto ShutdownStaticMeshBuildFunctions() -> void;
}
