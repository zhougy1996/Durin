#pragma once
#include "CoreMinimal.h"
#include "AssetMutationResultInternal.h"
#include "Asset/MutationExtensions.h"
namespace Durin
{
	namespace AssetToolsPrivate { class FAssetReferenceFixup; }
	enum class EPackageAliasFixupMode : uint8 { RewriteOnly, RewriteAndDelete };
	auto FixupPackageAliases(std::span<const FPackagePath> Paths, EPackageAliasFixupMode Mode,
		AssetToolsPrivate::FAssetReferenceFixup* References = nullptr) -> FAssetMutationResultDetails;
}
