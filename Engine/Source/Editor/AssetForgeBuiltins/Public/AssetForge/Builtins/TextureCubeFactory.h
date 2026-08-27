#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Factories/Factory.h"

#include "TextureCubeFactory.gen.h"

namespace Durin::AssetForge::Builtins
{
	DCLASS()
	class DTextureCubeFactory final : public DFactory
	{
		GENERATED_BODY()

	public:
		auto ConfigurePanorama(
			const FTextureCubePanoramaImportSettings& InSettings = {}) -> void
		{
			SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
			PanoramaSettings = InSettings;
		}
		auto ConfigureFaces(
			const std::array<std::string, TextureCubeFaceCount>& InFaceFiles,
			const FTextureCubeImportSettings& InSettings = {}) -> void
		{
			SourceLayout = ETextureCubeSourceLayout::SixFaces;
			FaceFiles = InFaceFiles;
			FaceSettings = InSettings;
		}

		auto GetSourceLayout() const -> ETextureCubeSourceLayout
		{
			return SourceLayout;
		}

		ASSETFORGEBUILTINS_API auto FactoryCreateFromFile(
			DClass* InClass,
			DObject* InParent,
			FName InName,
			EObjectFlags Flags,
			std::string_view Filename,
			DObject* Context,
			FFactoryDiagnostics* Diagnostics = nullptr) const -> DObject* override;
		ASSETFORGEBUILTINS_API auto GetReimportCapabilities(
			const DObject& Object) const -> FReimportCapabilities override;
		ASSETFORGEBUILTINS_API auto FactoryReimport(
			DObject& Object, FReimportCompletion Completion) const -> void override;
		ASSETFORGEBUILTINS_API auto FactoryReimportFromFiles(
			DObject& Object, std::span<const std::string> Filenames,
			FReimportCompletion Completion) const -> void override;

	private:
		ASSETFORGEBUILTINS_API explicit DTextureCubeFactory(
			const FObjectInitializer& ObjectInitializer);

		ETextureCubeSourceLayout SourceLayout =
			ETextureCubeSourceLayout::EquirectangularPanorama;
		std::array<std::string, TextureCubeFaceCount> FaceFiles;
		FTextureCubeImportSettings FaceSettings;
		FTextureCubePanoramaImportSettings PanoramaSettings;
	};
}
