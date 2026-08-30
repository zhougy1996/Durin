#include "Texture/TexturePayloadInspection.h"

#include "Asset/CookedAsset.h"
#include "Asset/EditorBulkDataStorage.h"
#include "Asset/Load.h"
#include "Misc/FileHelper.h"
#include "Texture/Texture2D.h"
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

		template<typename T>
		auto ReadScalar(const Asset::FAssetPackageInspection& Package,
			std::string_view Name, T& OutValue) -> bool
		{
			const Asset::FAssetPackageField* Field = Package.FindField(Name);
			return Field && Field->TryReadScalar(OutValue);
		}

		auto FindField(const std::vector<Asset::FAssetPackageField>& Fields,
			std::string_view Name) -> const Asset::FAssetPackageField*
		{
			const auto It = std::ranges::find(Fields, Name,
				&Asset::FAssetPackageField::Name);
			return It == Fields.end() ? nullptr : &*It;
		}

		template<typename T>
		auto ReadScalar(const std::vector<Asset::FAssetPackageField>& Fields,
			std::string_view Name, T& OutValue) -> bool
		{
			const Asset::FAssetPackageField* Field = FindField(Fields, Name);
			return Field && Field->TryReadScalar(OutValue);
		}

		auto ReadCooked(const Asset::FAssetPackageInspection& Package,
			Asset::FCookedPayloadDescriptor& OutValue) -> bool
		{
			const Asset::FAssetPackageField* Field = Package.FindField("CookedPayload");
			return Field && Field->TryReadStruct(
				Asset::FCookedPayloadDescriptor::StaticStruct(), &OutValue);
		}

		auto MakeCookedEntry(std::string Domain,
			const Asset::FCookedPayloadDescriptor& Descriptor,
			FGuid ExpectedId) -> FTexturePayloadInspectionEntry
		{
			FTexturePayloadInspectionEntry Entry{
				.Domain = std::move(Domain),
				.Stage = ETexturePayloadStage::Cooked,
				.State = ETexturePayloadState::NotPresent,
				.Repair = ETexturePayloadRepairAction::Recook,
				.DomainSchemaVersion = Descriptor.PayloadSchemaVersion,
				.LogicalByteCount = Descriptor.UncompressedSize,
				.StoredByteCount = Descriptor.StoredSize,
				.PayloadId = Descriptor.PayloadId,
				.Placement = "CookedPackageCompanion"};
			if (!Descriptor.PayloadId.IsValid())
			{
				Entry.Repair = ETexturePayloadRepairAction::None;
				Entry.Diagnostic = "No cooked payload is referenced by this package.";
				return Entry;
			}
			if (Descriptor.PayloadId != ExpectedId
				|| Descriptor.LocationKind != static_cast<uint32>(
					Asset::ECookedPayloadLocationKind::PackageCompanion)
				|| Descriptor.PayloadSchemaVersion != TexturePayloadSchemaVersion)
			{
				Entry.State = ETexturePayloadState::Unsupported;
				Entry.Repair = ETexturePayloadRepairAction::UpgradeOrResave;
				Entry.Diagnostic = "Cooked payload identity, placement, or TXPL schema is unsupported.";
				return Entry;
			}
			Entry.State = ETexturePayloadState::Available;
			Entry.Diagnostic = "Descriptor is domain-qualified; companion integrity requires storage validation.";
			return Entry;
		}

		auto MakeCookedFieldEntry(std::string Domain, const Asset::FBulkData& Field)
			-> FTexturePayloadInspectionEntry
		{
			const Asset::FBulkDataMetadata Metadata = Field.GetMetadata();
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
				.Placement = Field.GetState() == Asset::EBulkDataState::Attached
					? "PackageBulkRange" : "CookProjection",
				.Diagnostic = bPresent
					? "Cooked TXPL is stored as a package BulkData field."
					: "No cooked TXPL field is present."};
		}

		auto MapDerivedState(ETextureDerivedDataStatus Status) -> ETexturePayloadState
		{
			switch (Status)
			{
			case ETextureDerivedDataStatus::Hit:
			case ETextureDerivedDataStatus::Rebuilt:
			case ETextureDerivedDataStatus::CookedLoaded:
				return ETexturePayloadState::Available;
			case ETextureDerivedDataStatus::Missing:
				return ETexturePayloadState::Missing;
			case ETextureDerivedDataStatus::Corrupt: return ETexturePayloadState::Corrupt;
			case ETextureDerivedDataStatus::Incompatible: return ETexturePayloadState::Unsupported;
			case ETextureDerivedDataStatus::WriteFailure:
			case ETextureDerivedDataStatus::CookedFailure:
				return ETexturePayloadState::Failed;
			case ETextureDerivedDataStatus::None: return ETexturePayloadState::Unknown;
			}
			return ETexturePayloadState::Unknown;
		}

		auto MapResourceState(ERenderResourceState State) -> ETexturePayloadState
		{
			switch (State)
			{
			case ERenderResourceState::Ready: return ETexturePayloadState::Available;
			case ERenderResourceState::Failed: return ETexturePayloadState::Failed;
			case ERenderResourceState::Released: return ETexturePayloadState::NotPresent;
			case ERenderResourceState::Idle:
			case ERenderResourceState::Pending:
			case ERenderResourceState::Building:
				return ETexturePayloadState::Unknown;
			}
			return ETexturePayloadState::Unknown;
		}
	}

	auto InspectTexturePayloadPackage(const Asset::FAssetPackageInspection& Package,
		FTexturePayloadInspection& OutInspection, std::string* OutError) -> bool
	{
		OutInspection = {.bConstructFree = true};
		const std::string Texture2DClass = DTexture2D::StaticClass()->GetQualifiedName().ToString();
		const std::string VolumeClass = DVolumeTexture::StaticClass()->GetQualifiedName().ToString();
		if (Package.Header.AssetClassName == Texture2DClass)
		{
			uint32 Width = 0, Height = 0;
			uint64 SourceBytes = 0;
			ReadScalar(Package, "SourceWidth", Width);
			ReadScalar(Package, "SourceHeight", Height);
			std::string SourcePath;
			FAssetImportInfo CommonInfo;
			std::string CommonError;
			bool bHasSource = false;
			if (InspectAssetImportInfo(Package, CommonInfo, CommonError))
			{
				if (const FSourceFile* Source = CommonInfo.FindByRole("source"))
				{
					bHasSource = true;
					SourceBytes = Source->ByteCount;
					SourcePath = Source->Hint;
				}
			}
			OutInspection.Entries.push_back({
				.Domain = "Texture2D", .Stage = ETexturePayloadStage::Source,
				.State = bHasSource ? ETexturePayloadState::Unknown : ETexturePayloadState::NotPresent,
				.Repair = bHasSource ? ETexturePayloadRepairAction::ReimportSource
					: ETexturePayloadRepairAction::None,
				.LogicalElementCount = Multiply(Width, Height),
				.LogicalByteCount = SourceBytes,
				.StoredByteCount = SourceBytes,
				.Placement = "SourceFile",
				.Provenance = std::move(SourcePath),
				.Diagnostic = bHasSource
					? "Source identity is present; availability requires resolving its filename."
					: "No source-file identity is present."});
			OutInspection.Entries.push_back({
				.Domain = "Texture2D", .Stage = ETexturePayloadStage::DerivedData,
				.State = ETexturePayloadState::Unknown,
				.Repair = bHasSource ? ETexturePayloadRepairAction::RebuildDerivedData
					: ETexturePayloadRepairAction::ReimportSource,
				.DomainSchemaVersion = TexturePayloadSchemaVersion,
				.Placement = "DerivedDataCache",
				.Diagnostic = "DDC keys and records are intentionally not serialized in the package."});
			if (const Asset::FAssetPackageField* PlatformField =
				Package.FindField("PlatformData"))
				OutInspection.Entries.push_back({
					.Domain = "Texture2D", .Stage = ETexturePayloadStage::Cooked,
					.State = ETexturePayloadState::Available,
					.Repair = ETexturePayloadRepairAction::None,
					.DomainSchemaVersion = TexturePayloadSchemaVersion,
					.StoredByteCount = PlatformField->Payload.size(),
					.Placement = "PackageBulkField",
					.Diagnostic = "Cooked TXPL field metadata is present in DAST."});
			else
				OutInspection.Entries.push_back({
					.Domain = "Texture2D", .Stage = ETexturePayloadStage::Cooked,
					.State = ETexturePayloadState::NotPresent,
					.Repair = ETexturePayloadRepairAction::None,
					.DomainSchemaVersion = TexturePayloadSchemaVersion,
					.Placement = "PackageBulkField",
					.Diagnostic = "No cooked TXPL field is present."});
		}
		else if (Package.Header.AssetClassName == VolumeClass)
		{
			const Asset::FAssetPackageField* SourceField = Package.FindField("SourceData");
			std::vector<Asset::FAssetPackageField> SourceFields;
			const bool bReadable = SourceField
				&& SourceField->TryInspectStructFields(SourceFields);
			uint32 Width = 0, Height = 0, Depth = 0, SchemaVersion = 0;
			if (bReadable)
			{
				ReadScalar(SourceFields, "Width", Width);
				ReadScalar(SourceFields, "Height", Height);
				ReadScalar(SourceFields, "Depth", Depth);
				ReadScalar(SourceFields, "PayloadSchemaVersion", SchemaVersion);
			}
			Asset::FEditorBulkDataStorageDescriptor Descriptor;
			const Asset::FAssetPackageField* VoxelsField = FindField(SourceFields, "Voxels");
			const bool bHasDescriptor = VoxelsField
				&& VoxelsField->TryReadEditorBulkDataStorageDescriptor(Descriptor);
			ETexturePayloadState SourceState = bHasDescriptor
				? ETexturePayloadState::Available : ETexturePayloadState::Corrupt;
			ETexturePayloadRepairAction SourceRepair = bHasDescriptor
				? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::UpgradeOrResave;
			std::string SourceDiagnostic = bHasDescriptor
				? "Editor source descriptor is structurally valid."
				: "Editor source metadata or storage descriptor is malformed.";
			std::string Placement = "EditorPackageInline";
			if (bHasDescriptor
				&& Descriptor.StorageKind == Asset::EEditorBulkDataStorageKind::External)
			{
				Placement = "EditorPackageCompanion";
				std::filesystem::path CompanionPath;
				std::vector<std::byte> CompanionBytes;
				std::string StorageError;
				std::vector<std::filesystem::path> CompanionPaths;
				if (Package.PhysicalPath.empty()
					|| !Asset::InspectEditorBulkDataCompanionPaths(
						Package.PhysicalPath, Package, CompanionPaths, &StorageError)
					|| CompanionPaths.empty()
					|| (CompanionPath = CompanionPaths.front()).empty()
					|| !FFileHelper::LoadFileToArray(CompanionBytes, CompanionPath))
				{
					SourceState = ETexturePayloadState::Missing;
					SourceRepair = ETexturePayloadRepairAction::RestoreEditorCompanion;
					SourceDiagnostic = StorageError.empty()
						? "Editor source companion is missing or unreadable." : StorageError;
				}
				else if (Package.Header.FormatVersion >= Asset::AssetPackageV7FormatVersion
					? (CompanionBytes.size() != Package.Header.BulkSegmentExtent
						|| FXxHash128::HashBuffer(CompanionBytes) != Package.Header.BulkSegmentDigest)
					: !Asset::ValidateEditorBulkDataCompanion(
						CompanionBytes, Descriptor.ContainerHash, &StorageError))
				{
					SourceState = ETexturePayloadState::Corrupt;
					SourceRepair = ETexturePayloadRepairAction::RestoreEditorCompanion;
					SourceDiagnostic = StorageError;
				}
				else
					SourceDiagnostic = "Editor source companion and integrity are valid.";
			}
			OutInspection.Entries.push_back({
				.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::Source,
				.State = SourceState, .Repair = SourceRepair,
				.DomainSchemaVersion = SchemaVersion,
				.LogicalElementCount = Multiply(Multiply(Width, Height), Depth),
				.LogicalByteCount = bHasDescriptor ? Descriptor.LogicalByteCount : 0,
				.StoredByteCount = bHasDescriptor ? Descriptor.StoredByteCount : 0,
				.PayloadId = bHasDescriptor ? Descriptor.PayloadId : FGuid{},
				.Placement = std::move(Placement),
				.Diagnostic = std::move(SourceDiagnostic)});
			std::vector<std::filesystem::path> Orphans;
			std::string OrphanError;
			if (!Package.PhysicalPath.empty()
				&& Asset::InspectOrphanedEditorBulkDataCompanionPaths(
					Package.PhysicalPath, Package, Orphans, &OrphanError)
				&& !Orphans.empty())
				OutInspection.Entries.push_back({
					.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::Source,
					.State = ETexturePayloadState::Stale,
					.Repair = ETexturePayloadRepairAction::RemoveOrphan,
					.Placement = "EditorPackageCompanion",
					.Diagnostic = std::format(
						"{} unreferenced editor companion(s) require explicit cleanup.",
						Orphans.size())});
			OutInspection.Entries.push_back({
				.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::DerivedData,
				.State = ETexturePayloadState::Unknown,
				.Repair = ETexturePayloadRepairAction::RebuildDerivedData,
				.DomainSchemaVersion = TexturePayloadSchemaVersion,
				.Placement = "DerivedDataCache",
				.Diagnostic = "DDC keys and records are intentionally not serialized in the package."});
			if (const Asset::FAssetPackageField* PlatformField =
				Package.FindField("PlatformData"))
				OutInspection.Entries.push_back({
					.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::Cooked,
					.State = ETexturePayloadState::Available,
					.Repair = ETexturePayloadRepairAction::None,
					.DomainSchemaVersion = TexturePayloadSchemaVersion,
					.StoredByteCount = PlatformField->Payload.size(),
					.Placement = "PackageBulkField",
					.Diagnostic = "Cooked TXPL field metadata is present in DAST."});
			else
				OutInspection.Entries.push_back({
					.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::Cooked,
					.State = ETexturePayloadState::NotPresent,
					.Repair = ETexturePayloadRepairAction::None,
					.DomainSchemaVersion = TexturePayloadSchemaVersion,
					.Placement = "PackageBulkField",
					.Diagnostic = "No cooked TXPL field is present."});
		}
		else
		{
			if (OutError) *OutError = "Package is not a Texture2D or VolumeTexture asset.";
			return false;
		}
		if (OutError) OutError->clear();
		return true;
	}

	auto InspectTexturePayloads(const DTexture2D& Texture) -> FTexturePayloadInspection
	{
		FTexturePayloadInspection Result;
		const bool bImportedDataValid = Texture.GetImportedData().IsValid();
		const FSourceFile* ImportedSource =
			FindImportedSource(Texture.GetAssetImportData());
		Result.Entries.push_back({
			.Domain = "Texture2D", .Stage = ETexturePayloadStage::Source,
			.State = bImportedDataValid
				? ETexturePayloadState::Available : ETexturePayloadState::Missing,
			.Repair = bImportedDataValid
				? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RestoreEditorCompanion,
			.LogicalElementCount = Multiply(Texture.GetSourceWidth(), Texture.GetSourceHeight()),
			.LogicalByteCount = ImportedSource ? ImportedSource->ByteCount : 0,
			.StoredByteCount = ImportedSource ? ImportedSource->ByteCount : 0,
			.Placement = "EditorBulkData",
			.Provenance = ImportedSource ? ImportedSource->Hint : std::string{},
			.Diagnostic = bImportedDataValid
				? "Canonical imported pixels are authored; the optional source hint was not probed."
				: "Canonical imported pixels are missing or invalid."});
		const FTextureDerivedDataDiagnostic& Derived = Texture.GetDerivedDataDiagnostic();
		Result.Entries.push_back({
			.Domain = "Texture2D", .Stage = ETexturePayloadStage::DerivedData,
			.State = MapDerivedState(Derived.Status),
			.Repair = MapDerivedState(Derived.Status) == ETexturePayloadState::Available
				? ETexturePayloadRepairAction::None : ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(Texture.GetPlatformData()),
			.Placement = "DerivedDataCache",
			.Provenance = Derived.Key,
			.Diagnostic = Derived.Message});
		FTexturePayloadInspectionEntry Cooked = MakeCookedFieldEntry(
			"Texture2D", Texture.GetCookedPlatformData());
		if (Cooked.State == ETexturePayloadState::NotPresent
			&& Asset::GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			Cooked.State = ETexturePayloadState::Missing;
			Cooked.Repair = ETexturePayloadRepairAction::Recook;
			Cooked.Diagnostic = "The cooked runtime requires a Texture2D PlatformData field.";
		}
		else if (Derived.Status == ETextureDerivedDataStatus::CookedFailure)
		{
			Cooked.State = ETexturePayloadState::Failed;
			Cooked.Repair = ETexturePayloadRepairAction::Recook;
			Cooked.Diagnostic = Derived.Message;
		}
		Result.Entries.push_back(std::move(Cooked));
		Result.Entries.push_back({
			.Domain = "Texture2D", .Stage = ETexturePayloadStage::Decoded,
			.State = Texture.GetPlatformData() && Texture.GetPlatformData()->IsValid()
				? ETexturePayloadState::Available : ETexturePayloadState::NotPresent,
			.Repair = Texture.GetPlatformData() ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(Texture.GetPlatformData()),
			.Placement = "ResidentMemory"});
		Result.Entries.push_back({
			.Domain = "Texture2D", .Stage = ETexturePayloadStage::RuntimeResource,
			.State = MapResourceState(Texture.GetRenderResourceState()),
			.Repair = Texture.GetRenderResourceState() == ERenderResourceState::Failed
				? ETexturePayloadRepairAction::RetryRuntimeResource : ETexturePayloadRepairAction::None,
			.Placement = "GPU",
			.Diagnostic = Texture.GetLastBuildError()});
		return Result;
	}

	auto InspectTexturePayloads(const DVolumeTexture& Texture) -> FTexturePayloadInspection
	{
		FTexturePayloadInspection Result;
		const FVolumeTextureSourceData& Source = Texture.GetSourceData();
		const FSourceFile* ImportedSource =
			FindImportedSource(Texture.GetAssetImportData());
		Result.Entries.push_back({
			.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::Source,
			.State = Source.IsValid() ? ETexturePayloadState::Available : ETexturePayloadState::Corrupt,
			.Repair = Source.IsValid() ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::ReimportSource,
			.DomainSchemaVersion = Source.PayloadSchemaVersion,
			.LogicalElementCount = Multiply(Multiply(Source.Width, Source.Height), Source.Depth),
			.LogicalByteCount = Source.Voxels.GetPayloadSize(),
			.StoredByteCount = Source.Voxels.GetPayloadSize(),
			.PayloadId = Source.Voxels.GetInstanceId(),
			.Placement = "EditorPackage",
			.Provenance = ImportedSource ? ImportedSource->Hint : std::string{}});
		const bool bDerivedReady = Texture.GetPlatformData() && Texture.GetPlatformData()->IsValid()
			&& !Texture.GetDerivedDataKey().empty();
		Result.Entries.push_back({
			.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::DerivedData,
			.State = bDerivedReady ? ETexturePayloadState::Available : ETexturePayloadState::Missing,
			.Repair = bDerivedReady ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(Texture.GetPlatformData()),
			.Placement = "DerivedDataCache",
			.Provenance = Texture.GetDerivedDataKey(),
			.Diagnostic = Texture.GetLastBuildError()});
		FTexturePayloadInspectionEntry Cooked = MakeCookedFieldEntry(
			"VolumeTexture", Texture.GetCookedPlatformData());
		if (Cooked.State == ETexturePayloadState::NotPresent
			&& Asset::GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			Cooked.State = ETexturePayloadState::Missing;
			Cooked.Repair = ETexturePayloadRepairAction::Recook;
			Cooked.Diagnostic = "The cooked runtime requires a VolumeTexture PlatformData field.";
		}
		else if (Cooked.State == ETexturePayloadState::Available
			&& !Texture.GetPlatformData() && !Texture.GetLastBuildError().empty())
		{
			Cooked.State = ETexturePayloadState::Failed;
			Cooked.Repair = ETexturePayloadRepairAction::Recook;
			Cooked.Diagnostic = Texture.GetLastBuildError();
		}
		Result.Entries.push_back(std::move(Cooked));
		Result.Entries.push_back({
			.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::Decoded,
			.State = Texture.GetPlatformData() && Texture.GetPlatformData()->IsValid()
				? ETexturePayloadState::Available : ETexturePayloadState::NotPresent,
			.Repair = Texture.GetPlatformData() ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(Texture.GetPlatformData()),
			.Placement = "ResidentMemory"});
		Result.Entries.push_back({
			.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::RuntimeResource,
			.State = MapResourceState(Texture.GetRenderResourceState()),
			.Repair = Texture.GetRenderResourceState() == ERenderResourceState::Failed
				? ETexturePayloadRepairAction::RetryRuntimeResource : ETexturePayloadRepairAction::None,
			.Placement = "GPU",
			.Diagnostic = Texture.GetLastBuildError()});
		return Result;
	}
}
