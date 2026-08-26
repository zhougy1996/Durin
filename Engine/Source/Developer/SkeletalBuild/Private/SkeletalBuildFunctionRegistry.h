#pragma once

#include "DerivedDataCache/DerivedDataBuildFunction.h"

namespace Durin::Asset
{
	using namespace ::Durin::DerivedData;

	auto EnsureSkeletalBuildFunctions(
		std::string* OutError = nullptr, FModuleOwnedCallbackGate Gate = {}) -> bool;
	auto InitializeSkeletalBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError = nullptr) -> bool;
	auto ShutdownSkeletalBuildFunctions() -> void;
}
