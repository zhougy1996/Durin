#pragma once

#include "CoreMinimal.h"
#include "AssetTools/IAssetTools.h"
#include "Asset/Load.h"

namespace Durin::Testing
{
	// Select one exact export; this does not move its package siblings or reinterpret save diagnostics.
	inline auto RenamePackageLeafAssetForTesting(const FPackagePath& Source, const FPackagePath& Destination) -> bool
	{
		FTopLevelAssetPath Path;
		if (!FTopLevelAssetPath::TryCreate(Source, Source.GetPackageName(), Path)) return false;
		auto* Object = LoadObject(Path, nullptr);
		if (!Object) return false;
		const std::string Package = Destination.ToString();
		const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object),
			Package.substr(0, Package.find_last_of('/')), Destination.GetPackageName()};
		return IAssetTools::Get().RenameAssets(std::span{&Rename, 1});
	}
}
