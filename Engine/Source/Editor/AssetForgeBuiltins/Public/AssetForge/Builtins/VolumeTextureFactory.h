#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "AssetForge/Builtins/VolumeTextureImport.h"
#include "EditorReimportHandler.h"
#include "Factories/Factory.h"

#include "VolumeTextureFactory.gen.h"

namespace Durin::AssetForge::Builtins
{
	DCLASS()
	class DVolumeTextureFactory final : public DFactory, public FReimportHandler
	{
		GENERATED_BODY()

	public:
		auto SetImportSettings(const FVolumeTextureImportSettings& InSettings) -> void
		{
			Settings = InSettings;
		}
		auto GetImportSettings() const -> const FVolumeTextureImportSettings&
		{
			return Settings;
		}

		ASSETFORGEBUILTINS_API auto FactoryCreateFromFile(
			DClass* InClass,
			DObject* InParent,
			FName InName,
			EObjectFlags Flags,
			std::string_view Filename,
			DObject* Context,
			FFactoryDiagnostics* Diagnostics) const -> DObject* override;
		ASSETFORGEBUILTINS_API auto GetReimportCapabilities(
			const DObject& Object) const -> FReimportCapabilities override;
		ASSETFORGEBUILTINS_API auto Reimport(
			DObject& Object, FReimportCompletion Completion) const -> void override;
		ASSETFORGEBUILTINS_API auto ReimportFromFiles(
			DObject& Object, std::span<const std::string> Filenames,
			FReimportCompletion Completion) const -> void override;

	private:
		ASSETFORGEBUILTINS_API explicit DVolumeTextureFactory(
			const FObjectInitializer& ObjectInitializer);

		FVolumeTextureImportSettings Settings;
	};
}
