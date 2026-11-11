#pragma once

#include "CoreMinimal.h"

#include "DObject/AssetPath.h"
#include "DObject/Object.h"

namespace Durin
{
	class DClass;
	class DFactory;

	inline constexpr EObjectFlags DefaultAssetCreationFlags =
		EObjectFlags::Public | EObjectFlags::Standalone | EObjectFlags::Transactional;

	// One standalone output. Factories own typed settings and multi-file layouts.
	struct FAssetImportRequest
	{
		FTopLevelAssetPath AssetPath;
		DClass* AssetClass = nullptr;
		std::string Filename;
		const DFactory* Factory = nullptr;
		DObject* Context = nullptr;
		EObjectFlags Flags = DefaultAssetCreationFlags;
	};

	struct FAssetImportBatchRequest
	{
		std::vector<FAssetImportRequest> Items;
		bool bStopOnFailure = false;
		// Called on the game thread before each item; cannot interrupt a factory.
		std::function<bool()> ShouldCancel;
	};

}
