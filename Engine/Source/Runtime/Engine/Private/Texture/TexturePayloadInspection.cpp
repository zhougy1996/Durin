#include "Texture/TexturePayloadInspection.h"

#include "Asset/CookedAsset.h"
#include "Asset/EditorBulkDataStorage.h"
#include "Asset/Load.h"
#include "Misc/FileHelper.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DCompilation.h"
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
		auto ReadScalar(const FAssetPackageInspection& Package,
			std::string_view Name, T& OutValue) -> bool
		{
			const FAssetPackageField* Field = Package.FindField(Name);
			return Field && Field->TryReadScalar(OutValue);
		}

		auto FindField(const std::vector<FAssetPackageField>& Fields,
			std::string_view Name) -> const FAssetPackageField*
		{
			const auto It = std::ranges::find(Fields, Name,
				&FAssetPackageField::Name);
			return It == Fields.end() ? nullptr : &*It;
		}

		template<typename T>
		auto ReadScalar(const std::vector<FAssetPackageField>& Fields,
			std::string_view Name, T& OutValue) -> bool
		{
			const FAssetPackageField* Field = FindField(Fields, Name);
			return Field && Field->TryReadScalar(OutValue);
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

		auto MakeInspectedCookedFieldEntry(
			std::string Domain, const FAssetPackageField* Field)
			-> FTexturePayloadInspectionEntry
		{
			FEditorBulkDataStorageDescriptor Descriptor;
			const bool bPresent = Field
				&& Field->TryReadBulkDataStorageDescriptor(Descriptor);
			return {
				.Domain = std::move(Domain),
				.Stage = ETexturePayloadStage::Cooked,
				.State = bPresent ? ETexturePayloadState::Available
					: ETexturePayloadState::NotPresent,
				.Repair = bPresent ? ETexturePayloadRepairAction::None
					: ETexturePayloadRepairAction::Recook,
				.DomainSchemaVersion = TexturePayloadSchemaVersion,
				.LogicalByteCount = bPresent ? Descriptor.LogicalByteCount : 0,
				.StoredByteCount = bPresent ? Descriptor.StoredByteCount : 0,
				.PayloadId = bPresent ? Descriptor.PayloadId : FGuid{},
				.Placement = bPresent
					? (Descriptor.StorageKind == EEditorBulkDataStorageKind::External
						? "PackageBulkRange" : "PackageInlineBulk")
					: "PackageBulkField",
				.Diagnostic = bPresent
					? "Cooked TXPL field metadata is present in DAST v9."
					: "No valid cooked TXPL field is present."};
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

	auto InspectTexturePayloadPackage(const FAssetPackageInspection& Package,
		FTexturePayloadInspection& OutInspection, std::string* OutError) -> bool
	{
		OutInspection = {.bConstructFree = true};
		const std::string Texture2DClass = DTexture2D::StaticClass()->GetQualifiedName().ToString();
		const std::string VolumeClass = DVolumeTexture::StaticClass()->GetQualifiedName().ToString();
		if (Package.Header.AssetClassName == Texture2DClass)
		{
			uint32 Width = 0, Height = 0;
			uint64 SourceBytes = 0;
			std::vector<FAssetPackageField> SourceFields;
			if (const FAssetPackageField* SourceField = Package.FindField("Source");
				SourceField && SourceField->TryInspectStructFields(SourceFields))
			{
				ReadScalar(SourceFields, "Width", Width);
				ReadScalar(SourceFields, "Height", Height);
			}
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
			OutInspection.Entries.push_back(MakeInspectedCookedFieldEntry(
				"Texture2D", Package.FindField("PlatformData")));
		}
		else if (Package.Header.AssetClassName == VolumeClass)
		{
			const FAssetPackageField* SourceField = Package.FindField("Source");
			std::vector<FAssetPackageField> SourceFields;
			const bool bReadable = SourceField
				&& SourceField->TryInspectStructFields(SourceFields);
			uint32 Width = 0, Height = 0, Depth = 0, SchemaVersion = 0;
			if (bReadable)
			{
				ReadScalar(SourceFields, "Width", Width);
				ReadScalar(SourceFields, "Height", Height);
				ReadScalar(SourceFields, "Depth", Depth);
				ReadScalar(SourceFields, "SchemaVersion", SchemaVersion);
			}
			FEditorBulkDataStorageDescriptor Descriptor;
			const FAssetPackageField* VoxelsField = FindField(SourceFields, "Payload");
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
				&& Descriptor.StorageKind == EEditorBulkDataStorageKind::External)
			{
				Placement = "EditorPackageCompanion";
				std::filesystem::path CompanionPath;
				FByteArray CompanionBytes;
				std::string StorageError;
				std::vector<std::filesystem::path> CompanionPaths;
				if (Package.PhysicalPath.empty()
					|| !InspectEditorBulkDataCompanionPaths(
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
				else if (CompanionBytes.size() != Package.Header.BulkSegmentExtent
					|| FXxHash128::HashBuffer(CompanionBytes) != Package.Header.BulkSegmentDigest)
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
				&& InspectOrphanedEditorBulkDataCompanionPaths(
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
			OutInspection.Entries.push_back(MakeInspectedCookedFieldEntry(
				"VolumeTexture", Package.FindField("PlatformData")));
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
			.LogicalElementCount = Multiply(Texture.GetSourceWidth(), Texture.GetSourceHeight()),
			.LogicalByteCount = ImportedSource ? ImportedSource->ByteCount : 0,
			.StoredByteCount = ImportedSource ? ImportedSource->ByteCount : 0,
			.Placement = "EditorBulkData",
			.Provenance = ImportedSource ? ImportedSource->Hint : std::string{},
			.Diagnostic = bImportedDataValid
				? "Canonical imported pixels are authored; the optional source hint was not probed."
				: "Canonical imported pixels are missing or invalid."});
		const FTexture2DCompilationDiagnostic Compilation =
			GetTexture2DCompilationDiagnostic(Texture);
		const FTexturePlatformData* PlatformData =
			Texture.GetInstalledPlatformData();
		const bool bPlatformReady = PlatformData && PlatformData->IsValid();
		const bool bCompilationFailed =
			Compilation.Phase == ETexture2DCompilationPhase::Failed;
		Result.Entries.push_back({
			.Domain = "Texture2D", .Stage = ETexturePayloadStage::DerivedData,
			.State = bPlatformReady ? ETexturePayloadState::Available
				: bCompilationFailed ? ETexturePayloadState::Failed
				: ETexturePayloadState::Missing,
			.Repair = bPlatformReady
				? ETexturePayloadRepairAction::None : ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(PlatformData),
			.Placement = "DerivedDataCache",
			.Provenance = Compilation.DerivedDataKey,
			.Diagnostic = Compilation.Message});
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
			.Repair = bPlatformReady ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(PlatformData),
			.Placement = "ResidentMemory"});
		Result.Entries.push_back({
			.Domain = "Texture2D", .Stage = ETexturePayloadStage::RuntimeResource,
			.State = MapResourceState(Texture.GetRenderResourceState()),
			.Repair = Texture.GetRenderResourceState() == ERenderResourceState::Failed
				? ETexturePayloadRepairAction::RetryRuntimeResource : ETexturePayloadRepairAction::None,
			.Placement = "GPU",
			.Diagnostic = Texture.GetRenderFailure() == ETextureRenderFailure::UnsupportedFormat
				? "The current RHI does not support this texture format and usage."
				: Texture.GetRenderFailure() == ETextureRenderFailure::CreateOrUpload
					? "GPU texture creation or upload failed." : std::string{}});
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
			.DomainSchemaVersion = Source.SchemaVersion,
			.LogicalElementCount = Multiply(Multiply(Source.Width, Source.Height), Source.Depth),
			.LogicalByteCount = Source.Payload.GetPayloadSize(),
			.StoredByteCount = Source.Payload.GetPayloadSize(),
			.PayloadId = Source.Payload.GetInstanceId(),
			.Placement = "EditorPackage",
			.Provenance = ImportedSource ? ImportedSource->Hint : std::string{}});
		const FVolumeTexturePlatformData* PlatformData =
			Texture.GetInstalledPlatformData();
		const bool bDerivedReady = PlatformData && PlatformData->IsValid();
		Result.Entries.push_back({
			.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::DerivedData,
			.State = bDerivedReady ? ETexturePayloadState::Available : ETexturePayloadState::Missing,
			.Repair = bDerivedReady ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(PlatformData),
			.Placement = "DerivedDataCache",
			.Diagnostic = bDerivedReady
				? "Platform data is installed; DDC provenance is operation-owned."
				: "No platform data is installed."});
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
			.Repair = bDerivedReady ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::RebuildDerivedData,
			.DomainSchemaVersion = TexturePayloadSchemaVersion,
			.LogicalByteCount = MipBytes(PlatformData),
			.Placement = "ResidentMemory"});
		Result.Entries.push_back({
			.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::RuntimeResource,
			.State = MapResourceState(Texture.GetRenderResourceState()),
			.Repair = Texture.GetRenderResourceState() == ERenderResourceState::Failed
				? ETexturePayloadRepairAction::RetryRuntimeResource : ETexturePayloadRepairAction::None,
			.Placement = "GPU",
			.Diagnostic = Texture.GetRenderFailure() == ETextureRenderFailure::UnsupportedFormat
				? "The current RHI does not support this volume texture format and usage."
				: Texture.GetRenderFailure() == ETextureRenderFailure::CreateOrUpload
					? "GPU volume texture creation or upload failed." : std::string{}});
		return Result;
	}
}
