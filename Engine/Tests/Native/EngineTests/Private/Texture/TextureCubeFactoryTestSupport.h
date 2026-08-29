#pragma once

#include "Asset/AssetOperations.h"
#include "AssetForge/Builtins/TextureCubeFactory.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/DObjectGlobals.h"
#include "FactoryImportTestSupport.h"

namespace Durin::AssetForge::Builtins
{
	inline auto SaveTextureCubeFactoryResult(
		FAssetToolsResult Imported) -> Durin::Testing::TFactoryImportResult<Durin::DTextureCube>
	{
		auto* Texture = Cast<DTextureCube>(Imported.Asset);
		if (!Imported || !Texture)
			return {false, Imported.Message, Texture};
		const Asset::FAssetResult Saved = Asset::SavePackage(Imported.Package);
		return Saved
			? Durin::Testing::TFactoryImportResult<Durin::DTextureCube>{true, {}, Texture}
			: Durin::Testing::TFactoryImportResult<Durin::DTextureCube>{false, Saved.Message, Texture};
	}

	inline auto ImportTextureCubeFacesForTest(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		std::string_view AssetPath,
		const FTextureCubeImportSettings& Settings = {}) -> Durin::Testing::TFactoryImportResult<Durin::DTextureCube>
	{
		FAssetPath ParsedPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		auto* Factory = NewObject<DTextureCubeFactory>(
			nullptr, "TextureCubeFacesTestFactory", EObjectFlags::Transient);
		Factory->ConfigureFaces(FaceFiles, Settings);
		return SaveTextureCubeFactoryResult(IAssetTools::Get().ImportAsset(
			ParsedPath, DTextureCube::StaticClass(), FaceFiles[0], Factory));
	}

	inline auto ImportTextureCubePanoramaForTest(
		std::string_view PanoramaFile,
		std::string_view AssetPath,
		const FTextureCubePanoramaImportSettings& Settings = {})
		-> Durin::Testing::TFactoryImportResult<Durin::DTextureCube>
	{
		FAssetPath ParsedPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		auto* Factory = NewObject<DTextureCubeFactory>(
			nullptr, "TextureCubePanoramaTestFactory", EObjectFlags::Transient);
		Factory->ConfigurePanorama(Settings);
		return SaveTextureCubeFactoryResult(IAssetTools::Get().ImportAsset(
			ParsedPath, DTextureCube::StaticClass(), PanoramaFile, Factory));
	}
}
