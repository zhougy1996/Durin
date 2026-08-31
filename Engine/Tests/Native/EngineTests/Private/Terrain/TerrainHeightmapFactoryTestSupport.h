#pragma once

#include "Asset/AssetOperations.h"
#include "AssetForge/Builtins/TerrainHeightmapFactory.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/DObjectGlobals.h"
#include "FactoryImportTestSupport.h"

namespace Durin::AssetForge::Builtins
{
	// Test-only result adapter for assertions that also exercises the production
	// Factory/AssetTools creation path and explicit package save.
	inline auto ImportTerrainHeightmapForTest(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTerrainHeightmapImportSettings& Settings = {})
		-> Durin::Testing::TFactoryImportResult<Durin::DTerrainHeightmap>
	{
		FPackagePath ParsedPath;
		std::string Error;
		if (!FPackagePath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		auto* Factory = NewObject<DTerrainHeightmapFactory>(
			nullptr, "TerrainHeightmapTestFactory", EObjectFlags::Transient);
		Factory->SetImportSettings(Settings);
		const FAssetToolsResult Imported = IAssetTools::Get().ImportPackageLeafAssetForTesting(
			ParsedPath, DTerrainHeightmap::StaticClass(), FilePath, Factory);
		auto* Heightmap = Cast<DTerrainHeightmap>(Imported.Asset);
		if (!Imported || !Heightmap)
			return {false, Imported.Message, Heightmap};
		const Asset::FAssetResult Saved = Asset::SavePackage(Imported.Package);
		return Saved
			? Durin::Testing::TFactoryImportResult<Durin::DTerrainHeightmap>{true, {}, Heightmap}
			: Durin::Testing::TFactoryImportResult<Durin::DTerrainHeightmap>{false, Saved.Message, Heightmap};
	}
}
