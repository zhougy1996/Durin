#pragma once

#include "CoreMinimal.h"

#include "Asset/PackageSerialization.h"
#include "AssetForge/Builtins/TextureCubeFactory.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/DObjectGlobals.h"
#include "FactoryImportTestSupport.h"

namespace Durin::AssetForge::Builtins
{
	inline auto SaveTextureCubeFactoryResult(
		DObject* Imported, std::string_view Error) -> Durin::Testing::TFactoryImportResult<Durin::DTextureCube>
	{
		auto* Texture = Cast<DTextureCube>(Imported);
		if (!Imported || !Texture)
			return {false, std::string(Error), Texture};
		const FAssetWriteResult Saved = SavePackage(Imported->GetPackage());
		return Saved
			? Durin::Testing::TFactoryImportResult<Durin::DTextureCube>{true, {}, Texture}
			: Durin::Testing::TFactoryImportResult<Durin::DTextureCube>{false, Saved.Message, Texture};
	}

	inline auto ImportTextureCubeFacesForTest(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		std::string_view AssetPath,
		const FTextureCubeImportSettings& Settings = {}) -> Durin::Testing::TFactoryImportResult<Durin::DTextureCube>
	{
		FPackagePath ParsedPath;
		std::string Error;
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(AssetPath, ParsedPath); !PathValidation)
		{
			Error = Durin::ToString(PathValidation.error());
			return {false, std::move(Error), nullptr};
		}
		auto* Factory = NewObject<DTextureCubeFactory>(
			nullptr, "TextureCubeFacesTestFactory", EObjectFlags::Transient);
		Factory->ConfigureFaces(FaceFiles, Settings);
		FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) {
			if (Level >= ELogLevel::Error) Error += Message;
		});
		auto* Imported = IAssetTools::Get().ImportPackageLeafAssetForTesting(
			ParsedPath, DTextureCube::StaticClass(), FaceFiles[0], Factory, nullptr, EObjectFlags::Public | EObjectFlags::Transactional, &Feedback);
		return SaveTextureCubeFactoryResult(Imported, Error);
	}

	inline auto ImportTextureCubePanoramaForTest(
		std::string_view PanoramaFile,
		std::string_view AssetPath,
		const FTextureCubePanoramaImportSettings& Settings = {})
		-> Durin::Testing::TFactoryImportResult<Durin::DTextureCube>
	{
		FPackagePath ParsedPath;
		std::string Error;
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(AssetPath, ParsedPath); !PathValidation)
		{
			Error = Durin::ToString(PathValidation.error());
			return {false, std::move(Error), nullptr};
		}
		auto* Factory = NewObject<DTextureCubeFactory>(
			nullptr, "TextureCubePanoramaTestFactory", EObjectFlags::Transient);
		Factory->ConfigurePanorama(Settings);
		FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) {
			if (Level >= ELogLevel::Error) Error += Message;
		});
		auto* Imported = IAssetTools::Get().ImportPackageLeafAssetForTesting(
			ParsedPath, DTextureCube::StaticClass(), PanoramaFile, Factory, nullptr, EObjectFlags::Public | EObjectFlags::Transactional, &Feedback);
		return SaveTextureCubeFactoryResult(Imported, Error);
	}
}
