#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Factories/Factory.h"
#include "Texture/Texture2D.h"

#include "Texture2DFactory.gen.h"

namespace Durin::AssetForge::Builtins
{
	DCLASS()
	class DTexture2DFactory final : public DFactory
	{
		GENERATED_BODY()

	public:
		auto SetImportSettings(const FTexture2DImportSettings& InSettings) -> void
		{
			Settings = InSettings;
		}
		auto GetImportSettings() const -> const FTexture2DImportSettings&
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
			FFactoryDiagnostics* Diagnostics = nullptr) const -> DObject* override;
		ASSETFORGEBUILTINS_API auto GetReimportCapabilities(
			const DObject& Object) const -> FReimportCapabilities override;
		ASSETFORGEBUILTINS_API auto FactoryReimport(
			DObject& Object,
			FReimportCompletion Completion) const -> void override;
		ASSETFORGEBUILTINS_API auto FactoryReimportFromFiles(
			DObject& Object,
			std::span<const std::string> Filenames,
			FReimportCompletion Completion) const -> void override;

	private:
		ASSETFORGEBUILTINS_API explicit DTexture2DFactory(
			const FObjectInitializer& ObjectInitializer);

		FTexture2DImportSettings Settings;
	};
}
