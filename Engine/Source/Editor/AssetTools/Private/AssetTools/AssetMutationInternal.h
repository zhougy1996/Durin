#pragma once
#include "AssetTools/MutationTypes.h"
#include "Asset/MutationExtensions.h"
namespace Durin
{
	enum class EPackageAliasFixupMode : uint8 { RewriteOnly, RewriteAndDelete };
	auto FixupPackageAliases(std::span<const FPackagePath> Paths, EPackageAliasFixupMode Mode) -> FAssetMutationResultDetails;
}
