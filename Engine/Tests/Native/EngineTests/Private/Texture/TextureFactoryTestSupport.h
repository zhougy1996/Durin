#pragma once

#include "Asset/PackageSerialization.h"
#include "AssetForge/Builtins/Texture2DFactory.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/DObjectGlobals.h"
#include "FactoryImportTestSupport.h"

namespace Durin::AssetForge::Builtins
{
	// Test-only convenience that preserves existing result-shaped assertions
	// while exercising the production Factory/AssetTools creation path.
	inline auto ImportTexture2DForTest(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTexture2DImportSettings& Settings = {}) -> Durin::Testing::TFactoryImportResult<Durin::DTexture2D>
	{
		FPackagePath ParsedPath;
		std::string Error;
		if (!FPackagePath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		auto* Factory = NewObject<DTexture2DFactory>(
			nullptr, "Texture2DTestFactory", EObjectFlags::Transient);
		Factory->SetImportSettings(Settings);
		const FAssetToolsResult Imported = IAssetTools::Get().ImportPackageLeafAssetForTesting(
			ParsedPath, DTexture2D::StaticClass(), FilePath, Factory);
		auto* Texture = Cast<DTexture2D>(Imported.Asset);
		if (!Imported || !Texture)
			return {false, Imported.Message, Texture};
		const FAssetResult Saved = SavePackage(Imported.Package);
		return Saved
			? Durin::Testing::TFactoryImportResult<Durin::DTexture2D>{true, {}, Texture}
			: Durin::Testing::TFactoryImportResult<Durin::DTexture2D>{false, Saved.Message, Texture};
	}
}
