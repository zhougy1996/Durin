#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "EditorReimportHandler.h"
#include "Factories/Factory.h"
#include "StaticMesh/StaticMesh.h"

#include "StaticMeshFactory.gen.h"

namespace Durin::AssetForge::Builtins
{
	DCLASS()
	class DStaticMeshFactory final : public DFactory, public FReimportHandler
	{
		GENERATED_BODY()

	public:
		auto SetImportSettings(const FStaticMeshImportSettings& InSettings) -> void
		{
			Settings = InSettings;
		}
		auto GetImportSettings() const -> const FStaticMeshImportSettings&
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
		ASSETFORGEBUILTINS_API auto QueryReimportActions(std::string_view AssetClassName) const
			-> FReimportActions override;
		ASSETFORGEBUILTINS_API auto GetSourceFileDialogs(const DObject& Object) const
			-> std::vector<FReimportSourceFileDialog> override;
		ASSETFORGEBUILTINS_API auto GetReimportCapabilities(
			const DObject& Object) const -> FReimportCapabilities override;
		ASSETFORGEBUILTINS_API auto Reimport(
			DObject& Object, FReimportCompletion Completion) const -> void override;
		ASSETFORGEBUILTINS_API auto ReimportFromFiles(
			DObject& Object, std::span<const std::string> Filenames,
			FReimportCompletion Completion) const -> void override;

	private:
		ASSETFORGEBUILTINS_API explicit DStaticMeshFactory(
			const FObjectInitializer& ObjectInitializer);

		FStaticMeshImportSettings Settings;
	};
}
