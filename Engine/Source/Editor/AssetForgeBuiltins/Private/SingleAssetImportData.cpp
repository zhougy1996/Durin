#include "StaticMesh/StaticMeshCompilation.h"
#include "AssetForge/Builtins/ImportDataValidation.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "AssetForge/Builtins/VolumeTextureImportData.h"

namespace Durin::AssetForge::Builtins
{
	auto FImportDataValidationCause::Format() const -> std::string
	{
		if (SettingsCause) return FormatStaticMeshImportSettingsError(*SettingsCause);
		if (Code == EImportDataValidationError::InvalidAtlas)
			return "VolumeTexture import data requires a valid row-major atlas interpretation.";
		return std::format("{} import data requires exactly one source role.",
			Family == EImportDataFamily::StaticMesh ? "StaticMesh" : "VolumeTexture");
	}

	namespace
	{
		auto RejectImportData(FImportDataValidationCause Cause) -> FAssetImportDataResult
		{
			return {.Error = {.Code = EAssetImportDataError::ModuleRejected,
				.Cause = std::make_shared<FImportDataValidationCause>(std::move(Cause))}};
		}
		auto ValidateSingleSource(const FAssetImportDataState& State, EImportDataFamily Family)
			-> FAssetImportDataResult
		{
			const FSourceFile* Source = State.SourceData.FindByRole("source");
			if (State.SourceData.Sources.size() == 1 && Source) return {};
			FImportDataValidationCause Cause;
			Cause.Family = Family;
			Cause.SourceCount = State.SourceData.Sources.size();
			for (const auto& Item : State.SourceData.Sources) Cause.SourceRoles.push_back(Item.Role.ToString());
			return RejectImportData(std::move(Cause));
		}
	}

	auto FStaticMeshImportDataState::Validate() const -> FAssetImportDataResult
	{
		if (auto Validation = FAssetImportDataState::Validate(); !Validation) return Validation;
		if (SourceData.Sources.empty()) return {};
		if (auto Validation = ValidateSingleSource(*this, EImportDataFamily::StaticMesh); !Validation) return Validation;
		if (const auto Validation = ImportSettings.Validate(); !Validation)
		{
			FImportDataValidationCause Cause;
			Cause.Code = EImportDataValidationError::InvalidAxisSettings;
			Cause.SettingsCause = Validation.Error;
			return RejectImportData(std::move(Cause));
		}
		return {};
	}

	auto FVolumeTextureImportDataState::Validate() const -> FAssetImportDataResult
	{
		if (auto Validation = FAssetImportDataState::Validate(); !Validation) return Validation;
		if (SourceData.Sources.empty() && SliceWidth == 0 && SliceHeight == 0
			&& Depth == 0 && TilesX == 0 && TilesY == 0) return {};
		if (auto Validation = ValidateSingleSource(*this, EImportDataFamily::VolumeTexture); !Validation) return Validation;
		const uint64 Capacity = static_cast<uint64>(TilesX) * TilesY;
		if (SliceWidth == 0 || SliceHeight == 0 || Depth == 0 || TilesX == 0 || TilesY == 0 || Capacity < Depth)
		{
			FImportDataValidationCause Cause;
			Cause.Code = EImportDataValidationError::InvalidAtlas;
			Cause.Family = EImportDataFamily::VolumeTexture;
			Cause.SliceWidth = SliceWidth;
			Cause.SliceHeight = SliceHeight;
			Cause.Depth = Depth;
			Cause.TilesX = TilesX;
			Cause.TilesY = TilesY;
			return RejectImportData(std::move(Cause));
		}
		return {};
	}

	DStaticMeshImportData::DStaticMeshImportData(
		const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {}

	auto DStaticMeshImportData::SetState(
		FStaticMeshImportDataState State) -> void
	{
		FAssetImportDataState BaseState = State;
		DAssetImportData::SetState(std::move(BaseState));
		const bool bChanged = ImportSettings != State.ImportSettings;
		ImportSettings = State.ImportSettings;
		if (auto* Mesh = Cast<DStaticMesh>(GetOuter()); bChanged && Mesh && Mesh->GetAssetImportData() == this)
			NotifyStaticMeshCompilationMutation(*Mesh);
	}

	auto DStaticMeshImportData::GetStaticMeshState() const
		-> FStaticMeshImportDataState
	{
		FStaticMeshImportDataState State;
		static_cast<FAssetImportDataState&>(State) = DAssetImportData::GetState();
		State.ImportSettings = ImportSettings;
		return State;
	}

	auto DStaticMeshImportData::GetCompilationIdentity() const -> FXxHash128
	{
		FXxHash128Builder Builder;
		Builder.UpdateValue(Super::GetCompilationIdentity());
		Builder.UpdateValue(ImportSettings.ForwardAxis);
		Builder.UpdateValue(ImportSettings.RightAxis);
		Builder.UpdateValue(ImportSettings.UpAxis);
		return Builder.Finalize();
	}

	auto DStaticMeshImportData::Validate() const -> FAssetImportDataResult
	{
		return GetStaticMeshState().Validate();
	}

	DVolumeTextureImportData::DVolumeTextureImportData(
		const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {}

	auto DVolumeTextureImportData::SetState(
		FVolumeTextureImportDataState State) -> void
	{
		FAssetImportDataState BaseState = State;
		DAssetImportData::SetState(std::move(BaseState));
		Channels = State.Channels;
		SliceWidth = State.SliceWidth;
		SliceHeight = State.SliceHeight;
		Depth = State.Depth;
		TilesX = State.TilesX;
		TilesY = State.TilesY;
	}

	auto DVolumeTextureImportData::GetVolumeTextureState() const
		-> FVolumeTextureImportDataState
	{
		FVolumeTextureImportDataState State;
		static_cast<FAssetImportDataState&>(State) = DAssetImportData::GetState();
		State.Channels = Channels;
		State.SliceWidth = SliceWidth;
		State.SliceHeight = SliceHeight;
		State.Depth = Depth;
		State.TilesX = TilesX;
		State.TilesY = TilesY;
		return State;
	}

	auto DVolumeTextureImportData::Validate() const -> FAssetImportDataResult
	{
		return GetVolumeTextureState().Validate();
	}
}
