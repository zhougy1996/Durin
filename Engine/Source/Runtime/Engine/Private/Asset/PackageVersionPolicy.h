#pragma once

#include "EngineAPI.h"
#include "DObject/PackageFormat.h"

namespace Durin::Asset
{
	inline constexpr uint32 OrdinaryAssetPackageWriterVersion = ObjectPackage::DastV9FormatVersion;

	ENGINE_API auto ValidateAssetPackageVersionPolicy(std::string& OutError) -> bool;
	ENGINE_API auto GetAssetPackageReaderPolicyIdentity() -> uint32;
}
