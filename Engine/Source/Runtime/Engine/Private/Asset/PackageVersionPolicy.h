#pragma once

#include "EngineAPI.h"
#include "AssetRegistry/PackageFormat.h"

namespace Durin::Asset
{
	inline constexpr uint32 OrdinaryAssetPackageWriterVersion = AssetPackageV9FormatVersion;

	ENGINE_API auto ValidateAssetPackageVersionPolicy(std::string& OutError) -> bool;
	ENGINE_API auto GetAssetPackageReaderPolicyIdentity() -> uint32;
}
