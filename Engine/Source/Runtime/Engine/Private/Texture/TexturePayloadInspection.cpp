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

		auto MapSourceState(ETextureSourceStatus Status) -> ETexturePayloadState
		{
			switch (Status)
			{
			case ETextureSourceStatus::NoSource: return ETexturePayloadState::NotPresent;
			case ETextureSourceStatus::Available: return ETexturePayloadState::Available;
			case ETextureSourceStatus::Changed: return ETexturePayloadState::Stale;
			case ETextureSourceStatus::Missing: return ETexturePayloadState::Missing;
			case ETextureSourceStatus::Invalid: return ETexturePayloadState::Corrupt;
			}
			return ETexturePayloadState::Unknown;
		}

		auto MapDerivedState(ETextureDerivedDataStatus Status) -> ETexturePayloadState
		{
			switch (Status)
			{
			case ETextureDerivedDataStatus::Hit:
			case ETextureDerivedDataStatus::Rebuilt:
			case ETextureDerivedDataStatus::SourceUnavailableCached:
			case ETextureDerivedDataStatus::CookedLoaded:
				return ETexturePayloadState::Available;
			case ETextureDerivedDataStatus::Missing:
			case ETextureDerivedDataStatus::SourceUnavailable:
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
			AssetImport::FAssetImportInfo CommonInfo;
			std::string CommonError;
			bool bHasSource = false;
			if (AssetImport::InspectAssetImportInfo(Package, CommonInfo, CommonError))
			{
				if (const AssetImport::FSourceFile* Source = CommonInfo.FindByRole("source"))
				{
					bHasSource = true;
					SourceBytes = Source->ByteCount;
					SourcePath = Source->SourcePath.Path;
				}
			}
			else
			{
				ReadScalar(Package, "SourceFileSize", SourceBytes);
				FTexture2DSourceImportData ImportData;
				const Asset::FAssetPackageField* ImportField = Package.FindField("SourceImportData");
				bHasSource = ImportField && ImportField->TryReadStruct(
					FTexture2DSourceImportData::StaticStruct(), &ImportData)
					&& ImportData.HasSource();
				if (bHasSource) SourcePath = ImportData.Source.SourcePath.Path;
			}
			OutInspection.Entries.push_back({
				.Domain = "Texture2D", .Stage = ETexturePayloadStage::Source,
				.State = bHasSource ? ETexturePayloadState::Unknown : ETexturePayloadState::NotPresent,
				.Repair = bHasSource ? ETexturePayloadRepairAction::ReimportSource
					: ETexturePayloadRepairAction::None,
				.LogicalElementCount = Multiply(Width, Height),
				.LogicalByteCount = SourceBytes,
				.StoredByteCount = SourceBytes,
				.Placement = "MountedSource",
				.Provenance = std::move(SourcePath),
				.Diagnostic = bHasSource
					? "Source identity is present; availability requires a live source mount."
					: "No mounted source identity is present."});
			OutInspection.Entries.push_back({
				.Domain = "Texture2D", .Stage = ETexturePayloadStage::DerivedData,
				.State = ETexturePayloadState::Unknown,
				.Repair = bHasSource ? ETexturePayloadRepairAction::RebuildDerivedData
					: ETexturePayloadRepairAction::ReimportSource,
				.DomainSchemaVersion = TexturePayloadSchemaVersion,
				.Placement = "DerivedDataCache",
				.Diagnostic = "DDC keys and records are intentionally not serialized in the package."});
			Asset::FCookedPayloadDescriptor Cooked;
			if (ReadCooked(Package, Cooked))
				OutInspection.Entries.push_back(MakeCookedEntry(
					"Texture2D", Cooked, Texture2DPrimaryCookedPayloadId));
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
				if (Package.PhysicalPath.empty()
					|| !Asset::ResolveEditorBulkDataCompanionPath(
						Package.PhysicalPath, CompanionPath, &StorageError)
					|| !FFileHelper::LoadFileToArray(CompanionBytes, CompanionPath))
				{
					SourceState = ETexturePayloadState::Missing;
					SourceRepair = ETexturePayloadRepairAction::RestoreEditorCompanion;
					SourceDiagnostic = StorageError.empty()
						? "Editor source companion is missing or unreadable." : StorageError;
				}
				else if (!Asset::ValidateEditorBulkDataCompanion(
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
			Asset::FCookedPayloadDescriptor Cooked;
			if (ReadCooked(Package, Cooked))
				OutInspection.Entries.push_back(MakeCookedEntry(
					"VolumeTexture", Cooked, VolumeTexturePrimaryCookedPayloadId));
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
		const FTextureSourceDiagnostic SourceDiagnostic = Texture.InspectSource();
		Result.Entries.push_back({
			.Domain = "Texture2D", .Stage = ETexturePayloadStage::Source,
			.State = MapSourceState(SourceDiagnostic.Status),
			.Repair = SourceDiagnostic.Status == ETextureSourceStatus::Available
				? ETexturePayloadRepairAction::None : ETexturePayloadRepairAction::ReimportSource,
			.LogicalElementCount = Multiply(Texture.GetSourceWidth(), Texture.GetSourceHeight()),
			.LogicalByteCount = Texture.GetSourceFileSize(),
			.StoredByteCount = Texture.GetSourceFileSize(),
			.Placement = "MountedSource",
			.Provenance = Texture.GetSourceFile(),
			.Diagnostic = SourceDiagnostic.Message});
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
		FTexturePayloadInspectionEntry Cooked = MakeCookedEntry(
			"Texture2D", Texture.GetCookedPayloadDescriptor(), Texture2DPrimaryCookedPayloadId);
		if (Cooked.State == ETexturePayloadState::NotPresent
			&& Asset::GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			Cooked.State = ETexturePayloadState::Missing;
			Cooked.Repair = ETexturePayloadRepairAction::Recook;
			Cooked.Diagnostic = "The cooked runtime requires a Texture2D payload descriptor.";
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
		const Asset::FBulkDataDescriptor& SourceDescriptor =
			Source.Voxels.GetBulkData().GetDescriptor();
		Result.Entries.push_back({
			.Domain = "VolumeTexture", .Stage = ETexturePayloadStage::Source,
			.State = Source.IsValid() ? ETexturePayloadState::Available : ETexturePayloadState::Corrupt,
			.Repair = Source.IsValid() ? ETexturePayloadRepairAction::None
				: ETexturePayloadRepairAction::ReimportSource,
			.DomainSchemaVersion = Source.PayloadSchemaVersion,
			.LogicalElementCount = Multiply(Multiply(Source.Width, Source.Height), Source.Depth),
			.LogicalByteCount = SourceDescriptor.LogicalByteCount,
			.StoredByteCount = SourceDescriptor.LogicalByteCount,
			.PayloadId = SourceDescriptor.PayloadId,
			.Placement = "EditorPackage",
			.Provenance = Texture.GetSourceImportData().SourceFile});
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
		FTexturePayloadInspectionEntry Cooked = MakeCookedEntry(
			"VolumeTexture", Texture.GetCookedPayloadDescriptor(), VolumeTexturePrimaryCookedPayloadId);
		if (Cooked.State == ETexturePayloadState::NotPresent
			&& Asset::GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			Cooked.State = ETexturePayloadState::Missing;
			Cooked.Repair = ETexturePayloadRepairAction::Recook;
			Cooked.Diagnostic = "The cooked runtime requires a VolumeTexture payload descriptor.";
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
