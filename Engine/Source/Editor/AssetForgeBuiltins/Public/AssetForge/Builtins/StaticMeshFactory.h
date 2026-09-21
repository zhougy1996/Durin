#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Import/EditorReimportHandler.h"
#include "Factories/Factory.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include <variant>

#include "StaticMeshFactory.gen.h"

namespace Durin::AssetForge::Builtins
{
	class ASSETFORGEBUILTINS_API FStaticMeshFactoryError final : public IFactoryErrorDetail
	{
	public:
		explicit FStaticMeshFactoryError(FStaticMeshCompilationDiagnostic Diagnostic) : Cause(std::move(Diagnostic)) {}
		auto Format() const -> std::string override;
		explicit FStaticMeshFactoryError(FStaticMeshRebuildError Error) : Cause(std::move(Error)) {}
		std::variant<FStaticMeshRebuildError, FStaticMeshCompilationDiagnostic> Cause;
	};

	DCLASS()
	class DStaticMeshFactory final : public DFactory, public FReimportHandler
	{
		GENERATED_BODY()

	public:
		// Explicit editor adapter; ordinary object-returning factory calls remain synchronous.
		auto SetAsyncImportCompletion(FStaticMeshCompilationCompletion Completion) -> void
		{
			AsyncImportCompletion = std::move(Completion);
			bAsyncImport = true;
		}
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
		FStaticMeshCompilationCompletion AsyncImportCompletion;
		bool bAsyncImport = false;
	};
}
