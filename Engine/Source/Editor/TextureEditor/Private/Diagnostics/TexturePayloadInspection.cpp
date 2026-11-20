#include "Diagnostics/TexturePayloadInspection.h"
#include "RenderResource.h"

#include "Asset/CookedAsset.h"
#include "Asset/AssetImportData.h"
#include "Asset/Load.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DCompilation.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
	namespace
	{
		auto Multiply(uint64 A, uint64 B) -> uint64
		{
			return A != 0 && B > std::numeric_limits<uint64>::max() / A
				? std::numeric_limits<uint64>::max() : A * B;
		}

		auto Add(uint64 A, uint64 B) -> uint64
		{
			return B > std::numeric_limits<uint64>::max() - A
				? std::numeric_limits<uint64>::max() : A + B;
		}

		auto FindImportedSource(const DAssetImportData* ImportData)
			-> const FSourceFile*
		{
			return ImportData
				? ImportData->GetSourceData().FindByRole("source") : nullptr;
		}

		auto MipBytes(const FTexturePlatformData* Data) -> uint64
		{
			uint64 Bytes = 0;
			if (Data)
				for (const FTexture2DMipData& Mip : Data->Mips)
					Bytes = Add(Bytes, static_cast<uint64>(Mip.Pixels.size()));
			return Bytes;
		}

		auto MipBytes(const FVolumeTexturePlatformData* Data) -> uint64
		{
			uint64 Bytes = 0;
			if (Data)
				for (const FVolumeTextureMipData& Mip : Data->Mips)
					Bytes = Add(Bytes, static_cast<uint64>(Mip.Voxels.size()));
			return Bytes;
		}

		auto MipBytes(const FTextureCubePlatformData* Data) -> uint64
		{
			uint64 Bytes = 0;
			if (Data)
				for (const FTexturePlatformData& Face : Data->Faces)
					Bytes = Add(Bytes, MipBytes(&Face));
			return Bytes;
		}

		auto MakeCookedFieldEntry(std::string Domain, const FBulkData& Field)
			-> FTexturePayloadInspectionEntry
		{
			const FBulkDataMetadata Metadata = Field.GetMetadata();
			const bool bPresent = Metadata.LogicalSize != 0;
			return {
				.Domain = std::move(Domain),
				.Stage = ETexturePayloadStage::Cooked,
				.State = bPresent ? ETexturePayloadState::Available
					: ETexturePayloadState::NotPresent,
				.Repair = bPresent ? ETexturePayloadRepairAction::None
					: ETexturePayloadRepairAction::Recook,
				.DomainSchemaVersion = TexturePayloadSchemaVersion,
				.LogicalByteCount = Metadata.LogicalSize,
				.StoredByteCount = Metadata.Range.StoredSize,
				.Placement = Field.GetState() == EBulkDataState::Attached
					? "PackageBulkRange" : "CookProjection",
				.Diagnostic = bPresent
					? "Cooked TXPL is stored as a package BulkData field."
					: "No cooked TXPL field is present."};
		}

		auto MapResourceState(const DTexture& Texture) -> ETexturePayloadState
		{
			if (Texture.IsPendingKill()) return ETexturePayloadState::NotPresent;
			if (Texture.HasPendingRenderResourceInitialization()) return ETexturePayloadState::Unknown;
			const auto* Resource = Texture.GetResource();
			if (Resource) return ETexturePayloadState::Available;
			if (!Texture.IsAsyncCacheComplete())
				return ETexturePayloadState::Unknown;
			return ETexturePayloadState::NotPresent;
		}
	}

	auto InspectTexturePayloads(const DTexture2D& Texture) -> FTexturePayloadInspection
	{
		FTexturePayloadInspection Result;
		const bool bImportedDataValid = Texture.GetSource().IsValid();
		const FSourceFile* ImportedSource =
			FindImportedSource(Texture.GetAssetImportData());
		Result.Entries.push_back({
			.Domain = "Texture2D", .Stage = ETexturePayloadStage::Source,
			.State = bImportedDataValid
				? ETexturePayloadState::Available : ETexturePayloadState::Missing,
			.Repair = bImportedDataValid
				? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RestoreEditorCompanion,
			.DomainSchemaVersion = Texture.GetSource().GetSchemaVersion(),
			.LogicalElementCount = Multiply(Texture.GetSource().GetWidth(), Texture.GetSource().GetHeight()),
			.LogicalByteCount = Texture.GetSource().GetDecodedPayloadSize(),
			.StoredByteCount = Texture.GetSource().GetBulkData().GetPayloadSize(),
			.PayloadId = Texture.GetSource().GetBulkData().GetInstanceId(),
			.Placement = "EditorPackage",
			.Provenance = ImportedSource ? ImportedSource->Hint : std::string{},
			.Diagnostic = bImportedDataValid
				? "Canonical imported pixels are authored; the optional source hint was not probed."
				: "Canonical imported pixels are missing or invalid."});
		const FTexturePlatformData* PlatformData =
			Texture.GetPlatformData();
		const bool bPlatformReady = PlatformData && PlatformData->IsValid();
		const bool bCachePending = !Texture.IsAsyncCacheComplete();
		Result.Entries.push_back({
			.Domain = "Texture2D", .Stage = ETexturePayloadStage::DerivedData,
			.State = bPlatformReady ? ETexturePayloadState::Available
				: ETexturePayloadState::Missing,
			.Repair = bPlatformReady || bCachePending
				? ETexturePayloadRepairAction::None : ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(PlatformData),
			.Placement = "DerivedDataCache",
			.Diagnostic = bCachePending ? "Platform data is being prepared."
				: bPlatformReady ? "" : "Platform data is unavailable; rebuild the texture and check the log."});
		FTexturePayloadInspectionEntry Cooked = MakeCookedFieldEntry(
			"Texture2D", Texture.GetCookedPlatformData());
		if (Cooked.State == ETexturePayloadState::NotPresent
			&& GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			Cooked.State = ETexturePayloadState::Missing;
			Cooked.Repair = ETexturePayloadRepairAction::Recook;
			Cooked.Diagnostic = "The cooked runtime requires a Texture2D PlatformData field.";
		}
		Result.Entries.push_back(std::move(Cooked));
		Result.Entries.push_back({
			.Domain = "Texture2D", .Stage = ETexturePayloadStage::Decoded,
			.State = bPlatformReady
				? ETexturePayloadState::Available : ETexturePayloadState::NotPresent,
			.Repair = bPlatformReady || bCachePending ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(PlatformData),
			.Placement = "ResidentMemory"});
		Result.Entries.push_back({.Domain = "Texture2D", .Stage = ETexturePayloadStage::RuntimeResource, .State = MapResourceState(Texture), .Repair = ETexturePayloadRepairAction::None, .Placement = "GPU", .Diagnostic = {}});
		return Result;
	}

	auto InspectTexturePayloads(const DVolumeTexture& Texture) -> FTexturePayloadInspection
	{
		FTexturePayloadInspection Result;
		const FTextureSource& Source = Texture.GetSource();
		const FSourceFile* ImportedSource =
			FindImportedSource(Texture.GetAssetImportData());
		Result.Entries.push_back({
			.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::Source,
			.State = Source.IsValid() ? ETexturePayloadState::Available : ETexturePayloadState::Corrupt,
			.Repair = Source.IsValid() ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::ReimportSource,
			.DomainSchemaVersion = Source.GetSchemaVersion(),
			.LogicalElementCount = Multiply(Multiply(Source.GetWidth(), Source.GetHeight()), Source.GetDepth()),
			.LogicalByteCount = Source.GetDecodedPayloadSize(),
			.StoredByteCount = Source.GetBulkData().GetPayloadSize(),
			.PayloadId = Source.GetBulkData().GetInstanceId(),
			.Placement = "EditorPackage",
			.Provenance = ImportedSource ? ImportedSource->Hint : std::string{}});
		const FVolumeTexturePlatformData* PlatformData =
			Texture.GetPlatformData();
		const bool bDerivedReady = PlatformData && PlatformData->IsValid();
		const bool bCachePending = !Texture.IsAsyncCacheComplete();
		Result.Entries.push_back({
			.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::DerivedData,
			.State = bDerivedReady ? ETexturePayloadState::Available : ETexturePayloadState::Missing,
			.Repair = bDerivedReady || bCachePending ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(PlatformData),
			.Placement = "DerivedDataCache",
			.Diagnostic = bDerivedReady
				? "Platform data is installed; DDC provenance is operation-owned."
				: bCachePending ? "Platform data is being prepared." : "No platform data is installed."});
		FTexturePayloadInspectionEntry Cooked = MakeCookedFieldEntry(
			"VolumeTexture", Texture.GetCookedPlatformData());
		if (Cooked.State == ETexturePayloadState::NotPresent
			&& GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			Cooked.State = ETexturePayloadState::Missing;
			Cooked.Repair = ETexturePayloadRepairAction::Recook;
			Cooked.Diagnostic = "The cooked runtime requires a VolumeTexture PlatformData field.";
		}
		Result.Entries.push_back(std::move(Cooked));
		Result.Entries.push_back({
			.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::Decoded,
			.State = bDerivedReady
				? ETexturePayloadState::Available : ETexturePayloadState::NotPresent,
			.Repair = bDerivedReady || bCachePending ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(PlatformData),
			.Placement = "ResidentMemory"});
		Result.Entries.push_back({.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::RuntimeResource, .State = MapResourceState(Texture), .Repair = ETexturePayloadRepairAction::None, .Placement = "GPU", .Diagnostic = {}});
		return Result;
	}

	auto InspectTexturePayloads(const DTextureCube& Texture) -> FTexturePayloadInspection
	{
		FTexturePayloadInspection Result;
		const FTextureSource& Source = Texture.GetSource();
		const bool bSourceValid = Source.IsValid();
		Result.Entries.push_back({
			.Domain = "TextureCube", .Stage = ETexturePayloadStage::Source,
			.State = bSourceValid ? ETexturePayloadState::Available : ETexturePayloadState::Corrupt,
			.Repair = bSourceValid ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::ReimportSource,
			.DomainSchemaVersion = Source.GetSchemaVersion(),
			.LogicalElementCount = Multiply(Multiply(Source.GetWidth(), Source.GetHeight()),
				Source.GetNumSlices()),
			.LogicalByteCount = Source.GetDecodedPayloadSize(),
			.StoredByteCount = Source.GetBulkData().GetPayloadSize(),
			.PayloadId = Source.GetBulkData().GetInstanceId(),
			.Placement = "EditorPackage"});
		const FTextureCubePlatformData* PlatformData = Texture.GetPlatformData();
		const bool bReady = PlatformData && PlatformData->IsValid();
		const bool bCachePending = !Texture.IsAsyncCacheComplete();
		Result.Entries.push_back({
			.Domain = "TextureCube", .Stage = ETexturePayloadStage::DerivedData,
			.State = bReady ? ETexturePayloadState::Available : ETexturePayloadState::Missing,
			.Repair = bReady || bCachePending ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(PlatformData),
			.Placement = "DerivedDataCache",
			.Diagnostic = bCachePending ? "Platform data is being prepared." : std::string{}});
		Result.Entries.push_back(MakeCookedFieldEntry(
			"TextureCube", Texture.GetCookedPlatformData()));
		Result.Entries.push_back({
			.Domain = "TextureCube", .Stage = ETexturePayloadStage::Decoded,
			.State = bReady ? ETexturePayloadState::Available : ETexturePayloadState::NotPresent,
			.Repair = bReady || bCachePending ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(PlatformData), .Placement = "ResidentMemory"});
		Result.Entries.push_back({.Domain = "TextureCube", .Stage = ETexturePayloadStage::RuntimeResource, .State = MapResourceState(Texture), .Repair = ETexturePayloadRepairAction::None, .Placement = "GPU"});
		return Result;
	}
}
