#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "AssetForge/Builtins/TerrainHeightmapImportData.h"
#include "AssetForge/Builtins/TextureCubeImportData.h"
#include "AssetForge/Builtins/VolumeTextureImportData.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		auto IsEmpty(const AssetImport::FAssetImportDataState& State) -> bool
		{
			return State.SourceData.Sources.empty();
		}

		auto ValidateSingleSource(
			const AssetImport::FAssetImportDataState& State,
			std::string_view Family,
			std::string& OutError) -> bool
		{
			if (State.SchemaVersion != AssetImport::AssetImportDataSchemaVersion
				|| !State.SourceData.Validate(OutError)) return false;
			const AssetImport::FSourceFile* Source =
				State.SourceData.FindByRole("source");
			if (State.SourceData.Sources.size() != 1 || !Source
				|| Source->StableIdentity != "root")
			{
				OutError = std::format(
					"{} import data requires one root source.", Family);
				return false;
			}
			return true;
		}

		auto ValidateState(
			const FStaticMeshImportDataState& State,
			std::string& OutError) -> bool
		{
			if (IsEmpty(State) && State.ImporterId.empty()
				&& State.ImporterVersion == 0)
			{
				OutError.clear();
				return true;
			}
			if (!ValidateSingleSource(State, "StaticMesh", OutError)) return false;
			if (State.ImporterId.empty() || State.ImporterVersion == 0
				|| !State.ImportSettings.IsValid(&OutError))
			{
				if (OutError.empty())
					OutError = "StaticMesh import data requires a supported importer and axis settings.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto ValidateState(
			const FTerrainHeightmapImportDataState& State,
			std::string& OutError) -> bool
		{
			if (IsEmpty(State) && State.DecoderId.empty()
				&& State.DecoderVersion == 0
				&& State.SourceFormat == ETerrainHeightmapSourceFormat::Unknown
				&& State.SourceProfileVersion == 0)
			{
				OutError.clear();
				return true;
			}
			if (!ValidateSingleSource(State, "TerrainHeightmap", OutError)) return false;
			if (State.DecoderId.empty() || State.DecoderVersion == 0
				|| State.SourceFormat == ETerrainHeightmapSourceFormat::Unknown
				|| State.SourceProfileVersion == 0)
			{
				OutError = "TerrainHeightmap import data requires a supported source decoder.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto ValidateState(
			const FTextureCubeImportDataState& State,
			std::string& OutError) -> bool
		{
			if (IsEmpty(State) && State.DecoderId.empty()
				&& State.DecoderVersion == 0 && State.ProjectionVersion == 0)
			{
				OutError.clear();
				return true;
			}
			if (State.SchemaVersion != AssetImport::AssetImportDataSchemaVersion
				|| !State.SourceData.Validate(OutError)) return false;
			if (State.DecoderId.empty() || State.DecoderVersion == 0
				|| State.ProjectionVersion == 0)
			{
				OutError = "TextureCube import data requires a supported decoder.";
				return false;
			}
			if (State.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
			{
				const AssetImport::FSourceFile* Panorama =
					State.SourceData.FindByRole("panorama");
				if (State.SourceData.Sources.size() != 1 || !Panorama
					|| Panorama->StableIdentity != "root")
				{
					OutError = "TextureCube panorama import data requires one root panorama and projection version.";
					return false;
				}
			}
			else
			{
				static constexpr std::array Roles{
					"positive-x", "negative-x", "positive-y",
					"negative-y", "positive-z", "negative-z"};
				if (State.SourceData.Sources.size() != Roles.size())
				{
					OutError = "TextureCube face import data requires exactly six face sources.";
					return false;
				}
				for (size_t Index = 0; Index < Roles.size(); ++Index)
				{
					const AssetImport::FSourceFile* Source =
						State.SourceData.FindByRole(Roles[Index]);
					if (!Source || Source->StableIdentity
						!= (Index == 0 ? "root" : std::format("face:{}", Index)))
					{
						OutError = "TextureCube face import-data identities are incomplete.";
						return false;
					}
				}
			}
			OutError.clear();
			return true;
		}

		auto ValidateState(
			const FVolumeTextureImportDataState& State,
			std::string& OutError) -> bool
		{
			if (IsEmpty(State) && State.DecoderId.empty()
				&& State.DecoderVersion == 0 && State.SliceWidth == 0
				&& State.SliceHeight == 0 && State.Depth == 0
				&& State.TilesX == 0 && State.TilesY == 0)
			{
				OutError.clear();
				return true;
			}
			if (!ValidateSingleSource(State, "VolumeTexture", OutError)) return false;
			const uint64 Capacity = static_cast<uint64>(State.TilesX) * State.TilesY;
			if (State.DecoderId.empty() || State.DecoderVersion == 0
				|| State.ImportFormat != EVolumeTextureImportFormat::PngRowMajorAtlas
				|| State.SliceWidth == 0 || State.SliceHeight == 0
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
		AssetImport::FAssetImportDataState BaseState = State;
		if (!ApplyState(std::move(BaseState), OutError)) return false;
		ImporterId = std::move(State.ImporterId);
		ImporterVersion = State.ImporterVersion;
		ImportSettings = State.ImportSettings;
		OutError.clear();
		return true;
	}

	auto DStaticMeshImportData::GetStaticMeshState() const
		-> FStaticMeshImportDataState
	{
		FStaticMeshImportDataState State;
		static_cast<AssetImport::FAssetImportDataState&>(State) = {
			.SchemaVersion = GetSchemaVersion(), .SourceData = GetSourceData()};
		State.ImporterId = ImporterId;
		State.ImporterVersion = ImporterVersion;
		State.ImportSettings = ImportSettings;
		return State;
	}

	auto DStaticMeshImportData::Validate(std::string& OutError) const -> bool
	{
		return ValidateState(GetStaticMeshState(), OutError);
	}

	auto DStaticMeshImportData::CloneToOwner(
		DObject* Owner, FName Name, std::string& OutError) const
		-> AssetImport::DAssetImportData*
	{
		if (!Owner || Name.IsNone() || !Validate(OutError))
		{
			if (OutError.empty()) OutError = "StaticMesh import-data clone requires a valid owner and name.";
			return nullptr;
		}
		auto* Clone = NewObject<DStaticMeshImportData>(
			Owner, Name, EObjectConstructionPurpose::Duplication);
		return Clone && Clone->SetState(GetStaticMeshState(), OutError) ? Clone : nullptr;
	}

	DTerrainHeightmapImportData::DTerrainHeightmapImportData(
		const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {}

	auto DTerrainHeightmapImportData::SetState(
		FTerrainHeightmapImportDataState State, std::string& OutError) -> bool
	{
		State.SourceData.Normalize();
		if (!ValidateState(State, OutError)) return false;
		AssetImport::FAssetImportDataState BaseState = State;
		if (!ApplyState(std::move(BaseState), OutError)) return false;
		DecoderId = std::move(State.DecoderId);
		DecoderVersion = State.DecoderVersion;
		SourceFormat = State.SourceFormat;
		SourceProfileVersion = State.SourceProfileVersion;
		OutError.clear();
		return true;
	}

	auto DTerrainHeightmapImportData::GetTerrainHeightmapState() const
		-> FTerrainHeightmapImportDataState
	{
		FTerrainHeightmapImportDataState State;
		static_cast<AssetImport::FAssetImportDataState&>(State) = {
			.SchemaVersion = GetSchemaVersion(), .SourceData = GetSourceData()};
		State.DecoderId = DecoderId;
		State.DecoderVersion = DecoderVersion;
		State.SourceFormat = SourceFormat;
		State.SourceProfileVersion = SourceProfileVersion;
		return State;
	}

	auto DTerrainHeightmapImportData::Validate(std::string& OutError) const -> bool
	{
		return ValidateState(GetTerrainHeightmapState(), OutError);
	}

	auto DTerrainHeightmapImportData::CloneToOwner(
		DObject* Owner, FName Name, std::string& OutError) const
		-> AssetImport::DAssetImportData*
	{
		if (!Owner || Name.IsNone() || !Validate(OutError))
		{
			if (OutError.empty()) OutError = "TerrainHeightmap import-data clone requires a valid owner and name.";
			return nullptr;
		}
		auto* Clone = NewObject<DTerrainHeightmapImportData>(
			Owner, Name, EObjectConstructionPurpose::Duplication);
		return Clone && Clone->SetState(GetTerrainHeightmapState(), OutError) ? Clone : nullptr;
	}

	DTextureCubeImportData::DTextureCubeImportData(
		const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {}

	auto DTextureCubeImportData::SetState(
		FTextureCubeImportDataState State, std::string& OutError) -> bool
	{
		State.SourceData.Normalize();
		if (!ValidateState(State, OutError)) return false;
		AssetImport::FAssetImportDataState BaseState = State;
		if (!ApplyState(std::move(BaseState), OutError)) return false;
		SourceLayout = State.SourceLayout;
		DecoderId = std::move(State.DecoderId);
		DecoderVersion = State.DecoderVersion;
		ProjectionVersion = State.ProjectionVersion;
		OutError.clear();
		return true;
	}

	auto DTextureCubeImportData::GetTextureCubeState() const
		-> FTextureCubeImportDataState
	{
		FTextureCubeImportDataState State;
		static_cast<AssetImport::FAssetImportDataState&>(State) = {
			.SchemaVersion = GetSchemaVersion(), .SourceData = GetSourceData()};
		State.SourceLayout = SourceLayout;
		State.DecoderId = DecoderId;
		State.DecoderVersion = DecoderVersion;
		State.ProjectionVersion = ProjectionVersion;
		return State;
	}

	auto DTextureCubeImportData::Validate(std::string& OutError) const -> bool
	{
		return ValidateState(GetTextureCubeState(), OutError);
	}

	auto DTextureCubeImportData::CloneToOwner(
		DObject* Owner, FName Name, std::string& OutError) const
		-> AssetImport::DAssetImportData*
	{
		if (!Owner || Name.IsNone() || !Validate(OutError))
		{
			if (OutError.empty()) OutError = "TextureCube import-data clone requires a valid owner and name.";
			return nullptr;
		}
		auto* Clone = NewObject<DTextureCubeImportData>(
			Owner, Name, EObjectConstructionPurpose::Duplication);
		return Clone && Clone->SetState(GetTextureCubeState(), OutError) ? Clone : nullptr;
	}

	DVolumeTextureImportData::DVolumeTextureImportData(
		const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {}

	auto DVolumeTextureImportData::SetState(
		FVolumeTextureImportDataState State, std::string& OutError) -> bool
	{
		State.SourceData.Normalize();
		if (!ValidateState(State, OutError)) return false;
		AssetImport::FAssetImportDataState BaseState = State;
		if (!ApplyState(std::move(BaseState), OutError)) return false;
		ImportFormat = State.ImportFormat;
		Channels = State.Channels;
		SliceWidth = State.SliceWidth;
		SliceHeight = State.SliceHeight;
		Depth = State.Depth;
		TilesX = State.TilesX;
		TilesY = State.TilesY;
		DecoderId = std::move(State.DecoderId);
		DecoderVersion = State.DecoderVersion;
		OutError.clear();
		return true;
	}

	auto DVolumeTextureImportData::GetVolumeTextureState() const
		-> FVolumeTextureImportDataState
	{
		FVolumeTextureImportDataState State;
		static_cast<AssetImport::FAssetImportDataState&>(State) = {
			.SchemaVersion = GetSchemaVersion(), .SourceData = GetSourceData()};
		State.ImportFormat = ImportFormat;
		State.Channels = Channels;
		State.SliceWidth = SliceWidth;
		State.SliceHeight = SliceHeight;
		State.Depth = Depth;
		State.TilesX = TilesX;
		State.TilesY = TilesY;
		State.DecoderId = DecoderId;
		State.DecoderVersion = DecoderVersion;
		return State;
	}

	auto DVolumeTextureImportData::Validate(std::string& OutError) const -> bool
	{
		return ValidateState(GetVolumeTextureState(), OutError);
	}

	auto DVolumeTextureImportData::CloneToOwner(
		DObject* Owner, FName Name, std::string& OutError) const
		-> AssetImport::DAssetImportData*
	{
		if (!Owner || Name.IsNone() || !Validate(OutError))
		{
			if (OutError.empty()) OutError = "VolumeTexture import-data clone requires a valid owner and name.";
			return nullptr;
		}
		auto* Clone = NewObject<DVolumeTextureImportData>(
			Owner, Name, EObjectConstructionPurpose::Duplication);
		return Clone && Clone->SetState(GetVolumeTextureState(), OutError) ? Clone : nullptr;
	}
}
