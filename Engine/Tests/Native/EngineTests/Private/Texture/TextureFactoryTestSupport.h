#pragma once

#include "CoreMinimal.h"

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
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(AssetPath, ParsedPath); !PathValidation)
		{
			Error = Durin::ToString(PathValidation.error());
			return {false, std::move(Error), nullptr};
		}
		auto* Factory = NewObject<DTexture2DFactory>(
			nullptr, "Texture2DTestFactory", EObjectFlags::Transient);
		Factory->SetImportSettings(Settings);
		std::string ImportedError;
		Durin::FFeedbackContext ImportedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
			if (Level >= Durin::ELogLevel::Error) { if (!ImportedError.empty()) ImportedError += "\n"; ImportedError += Message; }
		});
		const auto Imported = IAssetTools::Get().ImportPackageLeafAssetForTesting(
			ParsedPath, DTexture2D::StaticClass(), FilePath, Factory, nullptr, Durin::DefaultAssetCreationFlags, &ImportedFeedback);
		auto* Texture = Cast<DTexture2D>(Imported);
		if (!Imported || !Texture)
			return {false, ImportedError, Texture};
		if (!Texture->FinishCachePlatformData())
			return {false, "Texture compilation failed.", Texture};
		const FAssetWriteResult Saved = SavePackage(Imported->GetPackage());
		return Saved
			? Durin::Testing::TFactoryImportResult<Durin::DTexture2D>{true, {}, Texture}
			: Durin::Testing::TFactoryImportResult<Durin::DTexture2D>{false, Saved.Message, Texture};
	}
}
