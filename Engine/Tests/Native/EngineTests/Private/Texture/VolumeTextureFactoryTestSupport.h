#pragma once

#include "Asset/AssetOperations.h"
#include "AssetForge/Builtins/VolumeTextureFactory.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/DObjectGlobals.h"
#include "FactoryImportTestSupport.h"

namespace Durin::AssetForge::Builtins
{
	// Test-only result adapter that exercises the production Factory/AssetTools
	// path and preserves existing result-shaped assertions.
	inline auto ImportVolumeTextureForTest(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FVolumeTextureImportSettings& Settings = {})
		-> Durin::Testing::TFactoryImportResult<Durin::DVolumeTexture>
	{
		FPackagePath ParsedPath;
		std::string Error;
		if (!FPackagePath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		auto* Factory = NewObject<DVolumeTextureFactory>(
			nullptr, "VolumeTextureTestFactory", EObjectFlags::Transient);
		Factory->SetImportSettings(Settings);
		const FAssetToolsResult Imported = IAssetTools::Get().ImportPackageLeafAssetForTesting(
			ParsedPath, DVolumeTexture::StaticClass(), FilePath, Factory);
		auto* Texture = Cast<DVolumeTexture>(Imported.Asset);
		if (!Imported || !Texture)
			return {false, Imported.Message, Texture};
		const Asset::FAssetResult Saved = Asset::SavePackage(Imported.Package);
		return Saved
			? Durin::Testing::TFactoryImportResult<Durin::DVolumeTexture>{true, {}, Texture}
			: Durin::Testing::TFactoryImportResult<Durin::DVolumeTexture>{false, Saved.Message, Texture};
	}
}
