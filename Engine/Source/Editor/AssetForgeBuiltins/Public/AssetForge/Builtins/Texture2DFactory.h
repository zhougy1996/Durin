#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Import/EditorReimportHandler.h"
#include "Factories/Factory.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include <variant>

#include "Texture2DFactory.gen.h"

namespace Durin::AssetForge::Builtins
{
	class ASSETFORGEBUILTINS_API FTexture2DFactoryError final : public IFactoryErrorDetail
	{
	public:
		explicit FTexture2DFactoryError(FTexture2DPreparationError Error) : Cause(std::move(Error)) {}
		explicit FTexture2DFactoryError(FTexture2DSubmissionError Error) : Cause(std::move(Error)) {}
		explicit FTexture2DFactoryError(FTexture2DCompilationError Error) : Cause(std::move(Error)) {}
		auto Format() const -> std::string override;
		std::variant<FTexture2DPreparationError, FTexture2DSubmissionError, FTexture2DCompilationError> Cause;
	};

	DCLASS()
	class DTexture2DFactory final : public DFactory, public FReimportHandler
	{
		GENERATED_BODY()

	public:
		// Prepared imports return an accepted object before compilation completes.
		// Completion is game-thread-only; save/publish must wait for success.
		auto SetPreparedImport(std::shared_ptr<const FPreparedTexture2DImport> InPrepared,
			FTexture2DCompilationCompletion InCompletion) -> void
		{
			Prepared = std::move(InPrepared);
			Completion = std::move(InCompletion);
		}
		// Opt-in first-import inference after capture/decode; explicit callers and
		// reimport retain their configured settings.
		auto SetAutoDetectSettings(bool bEnabled) -> void { bAutoDetectSettings = bEnabled; }
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
			FFactoryDiagnostics* Diagnostics) const -> DObject* override;
		ASSETFORGEBUILTINS_API auto QueryReimportActions(std::string_view AssetClassName) const
			-> FReimportActions override;
		ASSETFORGEBUILTINS_API auto GetSourceFileDialogs(const DObject& Object) const
			-> std::vector<FReimportSourceFileDialog> override;
		ASSETFORGEBUILTINS_API auto GetReimportCapabilities(
			const DObject& Object) const -> FReimportCapabilities override;
		ASSETFORGEBUILTINS_API auto Reimport(
			DObject& Object,
			FReimportCompletion Completion) const -> void override;
		ASSETFORGEBUILTINS_API auto ReimportFromFiles(
			DObject& Object,
			std::span<const std::string> Filenames,
			FReimportCompletion Completion) const -> void override;

	private:
		ASSETFORGEBUILTINS_API explicit DTexture2DFactory(
			const FObjectInitializer& ObjectInitializer);

		FTexture2DImportSettings Settings;
		bool bAutoDetectSettings = false;
		std::shared_ptr<const FPreparedTexture2DImport> Prepared;
		FTexture2DCompilationCompletion Completion;
	};
}
