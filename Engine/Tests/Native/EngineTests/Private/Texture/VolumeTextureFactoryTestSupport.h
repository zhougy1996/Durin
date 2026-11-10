#pragma once

#include "CoreMinimal.h"

#include "Asset/PackageSerialization.h"
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
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(AssetPath, ParsedPath); !PathValidation)
		{
			Error = Durin::ToString(PathValidation.error());
			return {false, std::move(Error), nullptr};
		}
		auto* Factory = NewObject<DVolumeTextureFactory>(
			nullptr, "VolumeTextureTestFactory", EObjectFlags::Transient);
		Factory->SetImportSettings(Settings);
		std::string ImportedError;
		Durin::FFeedbackContext ImportedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
			if (Level >= Durin::ELogLevel::Error) { if (!ImportedError.empty()) ImportedError += "\n"; ImportedError += Message; }
		});
		const auto Imported = IAssetTools::Get().ImportPackageLeafAssetForTesting(
			ParsedPath, DVolumeTexture::StaticClass(), FilePath, Factory, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &ImportedFeedback);
		auto* Texture = Cast<DVolumeTexture>(Imported);
		if (!Imported || !Texture)
			return {false, ImportedError, Texture};
		const FAssetWriteResult Saved = SavePackage(Imported->GetPackage());
		return Saved
			? Durin::Testing::TFactoryImportResult<Durin::DVolumeTexture>{true, {}, Texture}
			: Durin::Testing::TFactoryImportResult<Durin::DVolumeTexture>{false, Saved.Message, Texture};
	}
}
