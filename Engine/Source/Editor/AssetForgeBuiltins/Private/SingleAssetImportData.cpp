#include "StaticMesh/StaticMeshCompilation.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "AssetForge/Builtins/VolumeTextureImportData.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		auto ValidateSingleSource(
			const FAssetImportDataState& State,
			std::string_view Family,
			std::string& OutError) -> bool
		{
			if (State.SchemaVersion != AssetImportDataSchemaVersion
				|| !State.SourceData.Validate(OutError)) return false;
			const FSourceFile* Source = State.SourceData.FindByRole("source");
			if (State.SourceData.Sources.size() != 1 || !Source)
			{
				OutError = std::format(
					"{} import data requires exactly one source role.", Family);
				return false;
			}
			return true;
		}

		auto ValidateState(
			const FStaticMeshImportDataState& State,
			std::string& OutError) -> bool
		{
			if (State.SourceData.Sources.empty())
			{
				OutError.clear();
				return true;
			}
			if (!ValidateSingleSource(State, "StaticMesh", OutError)) return false;
			if (!State.ImportSettings.IsValid(&OutError))
			{
				if (OutError.empty())
					OutError = "StaticMesh import data requires valid axis settings.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto ValidateState(
			const FVolumeTextureImportDataState& State,
			std::string& OutError) -> bool
		{
			if (State.SourceData.Sources.empty() && State.SliceWidth == 0
				&& State.SliceHeight == 0 && State.Depth == 0
				&& State.TilesX == 0 && State.TilesY == 0)
			{
				OutError.clear();
				return true;
			}
			if (!ValidateSingleSource(State, "VolumeTexture", OutError)) return false;
			const uint64 Capacity = static_cast<uint64>(State.TilesX) * State.TilesY;
			if (State.SliceWidth == 0 || State.SliceHeight == 0
				|| State.Depth == 0 || State.TilesX == 0 || State.TilesY == 0
				|| Capacity < State.Depth)
			{
				OutError = "VolumeTexture import data requires a valid row-major atlas interpretation.";
				return false;
			}
			OutError.clear();
			return true;
		}
	}

	DStaticMeshImportData::DStaticMeshImportData(
		const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {}

	auto DStaticMeshImportData::SetState(
		FStaticMeshImportDataState State, std::string& OutError) -> bool
	{
		State.SourceData.Normalize();
		if (!ValidateState(State, OutError)) return false;
		FAssetImportDataState BaseState = State;
		if (!DAssetImportData::SetState(std::move(BaseState), OutError)) return false;
		const bool bChanged = ImportSettings != State.ImportSettings;
		ImportSettings = State.ImportSettings;
		if (auto* Mesh = Cast<DStaticMesh>(GetOuter()); bChanged && Mesh && Mesh->GetAssetImportData() == this)
			NotifyStaticMeshCompilationMutation(*Mesh);
		OutError.clear();
		return true;
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

	auto DStaticMeshImportData::Validate(std::string& OutError) const -> bool
	{
		return ValidateState(GetStaticMeshState(), OutError);
	}

	DVolumeTextureImportData::DVolumeTextureImportData(
		const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {}

	auto DVolumeTextureImportData::SetState(
		FVolumeTextureImportDataState State, std::string& OutError) -> bool
	{
		State.SourceData.Normalize();
		if (!ValidateState(State, OutError)) return false;
		FAssetImportDataState BaseState = State;
		if (!DAssetImportData::SetState(std::move(BaseState), OutError)) return false;
		Channels = State.Channels;
		SliceWidth = State.SliceWidth;
		SliceHeight = State.SliceHeight;
		Depth = State.Depth;
		TilesX = State.TilesX;
		TilesY = State.TilesY;
		OutError.clear();
		return true;
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

	auto DVolumeTextureImportData::Validate(std::string& OutError) const -> bool
	{
		return ValidateState(GetVolumeTextureState(), OutError);
	}
}
