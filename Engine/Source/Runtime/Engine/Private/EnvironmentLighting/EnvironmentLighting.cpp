#include "EnvironmentLighting/EnvironmentLighting.h"
#include "Logging/LogMacros.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "Hash/XxHash.h"
#include "Serialization/BinaryFormat.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Serialization/Archive.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 EnvironmentLightingStablePixelFormatRgba16Float = 1;

		auto ExpectedElementCount() -> uint64
		{
			uint64 Count = static_cast<uint64>(TextureCubeFaceCount)
				* EnvironmentIrradianceDimension * EnvironmentIrradianceDimension * 4;
			for (uint32 Mip = 0; Mip < EnvironmentPrefilterMipCount; ++Mip)
			{
				const uint64 Dimension = EnvironmentPrefilterDimension >> Mip;
				Count += static_cast<uint64>(TextureCubeFaceCount) * Dimension * Dimension * 4;
			}
			Count += static_cast<uint64>(EnvironmentBrdfLutDimension)
				* EnvironmentBrdfLutDimension * 4;
			return Count;
		}

		auto SerializeEnvironmentBody(FArchive& Ar, FEnvironmentLightingData& Data) -> void
		{
			auto Transfer = [&](std::vector<uint16>& Values, size_t Count) {
				if (Ar.IsLoading()) Values.resize(Count);
				for (uint16& Value : Values) Ar << Value;
			};
			for (auto& Face : Data.Irradiance)
				Transfer(Face, EnvironmentIrradianceDimension * EnvironmentIrradianceDimension * 4);
			for (uint32 Mip = 0; Mip < EnvironmentPrefilterMipCount; ++Mip)
			{
				const size_t Dimension = EnvironmentPrefilterDimension >> Mip;
				for (auto& Face : Data.Prefiltered[Mip]) Transfer(Face, Dimension * Dimension * 4);
			}
			Transfer(Data.BrdfLut, EnvironmentBrdfLutDimension * EnvironmentBrdfLutDimension * 4);
		}

		auto LoadAuthoredPayload(
			std::string_view VirtualPackagePath,
			FByteBuffer& OutBytes,
			std::string& OutError) -> bool
		{
			const std::filesystem::path PayloadPath =
				DEnvironmentLighting::GetAuthoredPayloadPath(VirtualPackagePath);
			const auto File = FFileHelper::OpenRead(PayloadPath);
			if (!File || File->GetSize() != ExpectedElementCount() * sizeof(uint16) + 52)
				return Fail("Environment-lighting authored payload is missing or has an invalid size.", &OutError);
			OutBytes.resize(static_cast<size_t>(File->GetSize()));
			if (!File->ReadAt(0, OutBytes))
				return Fail("Environment-lighting authored payload read failed.", &OutError);
			return true;
		}
	}

	auto FEnvironmentLightingData::IsValid() const -> bool
	{
		const size_t IrradianceElements = static_cast<size_t>(EnvironmentIrradianceDimension)
			* EnvironmentIrradianceDimension * 4;
		for (const std::vector<uint16>& Face : Irradiance)
			if (Face.size() != IrradianceElements) return false;
		for (uint32 Mip = 0; Mip < EnvironmentPrefilterMipCount; ++Mip)
		{
			const size_t Dimension = EnvironmentPrefilterDimension >> Mip;
			const size_t Elements = Dimension * Dimension * 4;
			for (const std::vector<uint16>& Face : Prefiltered[Mip])
				if (Face.size() != Elements) return false;
		}
		return BrdfLut.size() == static_cast<size_t>(EnvironmentBrdfLutDimension)
			* EnvironmentBrdfLutDimension * 4;
	}

	auto FEnvironmentLightingData::Serialize(FArchive& Ar) -> void
	{
		if (Ar.HasError()) return;
		if (Ar.IsSaving() && !IsValid())
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, "Environment-lighting data is incomplete or malformed.");
			return;
		}
		uint32 Reserved = 0, Schema = EnvironmentLightingPayloadSchemaVersion;
		uint32 Producer = DefaultStudioEnvironmentBuilderVersion, Marker = BinaryFormatMarker;
		uint32 Format = EnvironmentLightingStablePixelFormatRgba16Float;
		uint32 IrradianceSize = EnvironmentIrradianceDimension, PrefilterSize = EnvironmentPrefilterDimension;
		uint32 MipCount = EnvironmentPrefilterMipCount, BrdfSize = EnvironmentBrdfLutDimension;
		uint64 ElementCount = ExpectedElementCount(), Hash = 0;
		FByteBuffer Body;
		if (Ar.IsSaving())
		{
			// Fixed dimensions bound staging before growth; the historical header
			// precedes the checksum's body, so one canonical body is retained.
			Body.reserve(static_cast<size_t>(ExpectedElementCount() * sizeof(uint16)));
			FCanonicalMemoryWriter BodyAr(Body);
			SerializeEnvironmentBody(BodyAr, *this);
			if (BodyAr.HasError())
			{
				Ar.Fail(BodyAr.GetFailure()->Code, BodyAr.GetFailure()->Message);
				return;
			}
			Hash = FXxHash64::HashBuffer(Body).HashValue;
		}
		Ar << Reserved << Schema << Producer << Marker << Format << IrradianceSize
			<< PrefilterSize << MipCount << BrdfSize << ElementCount << Hash;
		if (Ar.HasError()) return;
		if (Reserved != 0)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, "Environment-lighting reserved header field is nonzero.");
			return;
		}
		if (Schema != EnvironmentLightingPayloadSchemaVersion || Marker != BinaryFormatMarker
			|| Format != EnvironmentLightingStablePixelFormatRgba16Float
			|| IrradianceSize != EnvironmentIrradianceDimension || PrefilterSize != EnvironmentPrefilterDimension
			|| MipCount != EnvironmentPrefilterMipCount || BrdfSize != EnvironmentBrdfLutDimension)
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedVersion, "Environment-lighting payload layout is incompatible.");
			return;
		}
		if (ElementCount != ExpectedElementCount())
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded, "Environment-lighting element count is invalid.");
			return;
		}
		if (Ar.IsSaving())
		{
			Ar.WriteBytes(Body);
			return;
		}
		FByteView Region;
		if (!Ar.ReadRegion(ElementCount * sizeof(uint16), Region)) return;
		if (FXxHash64::HashBuffer(Region).HashValue != Hash)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, "Environment-lighting payload checksum does not match.");
			return;
		}
		FCanonicalMemoryReader BodyAr(Region);
		SerializeEnvironmentBody(BodyAr, *this);
		if (!RequireArchiveEnd(BodyAr))
			Ar.Fail(BodyAr.GetFailure()->Code, BodyAr.GetFailure()->Message);
	}

	DEnvironmentLighting::DEnvironmentLighting(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DEnvironmentLighting::GetAuthoredPayloadPath(std::string_view VirtualPackagePath)
		-> std::filesystem::path
	{
		const FAssetPathResult Resolved = FMountPaths::ResolveAssetPath(
			VirtualPackagePath, EMountPathExistence::AllowMissing);
		if (!Resolved) return {};
		std::filesystem::path Result = Resolved.PhysicalPath;
		Result += ".iblbulk";
		return Result;
	}

	auto DEnvironmentLighting::PostLoad() -> void
	{
		Super::PostLoad();
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			Data.reset();
			if (PayloadSchemaVersion != EnvironmentLightingPayloadSchemaVersion
				|| CookedPlatformData.GetMetadata().LogicalSize == 0)
				DURIN_ERROR("PostLoad '{}': cooked environment-lighting PlatformData field is missing.", GetObjectPath());
			return;
		}
		if (!GetPackage())
		{
			DURIN_ERROR("PostLoad '{}': environment-lighting asset has no package.", GetObjectPath());
			return;
		}
		FByteBuffer PayloadBytes;
		std::string Error;
		if (!LoadAuthoredPayload(GetPackage()->GetPackagePath(), PayloadBytes, Error))
		{
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		auto Candidate = std::make_shared<FEnvironmentLightingData>();
		FCanonicalMemoryReader PayloadAr(PayloadBytes, EArchivePurpose::DerivedDataPayload);
		Candidate->Serialize(PayloadAr);
		if (PayloadAr.HasError() || !RequireArchiveEnd(PayloadAr))
		{
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), PayloadAr.GetFailure()->Message);
			return;
		}
		Data = std::move(Candidate);
	}

	auto DEnvironmentLighting::GetData() const
		-> const std::shared_ptr<const FEnvironmentLightingData>&
	{
		if (!Data && GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedPlatformData.GetMetadata().LogicalSize != 0)
		{
			DEnvironmentLighting* Mutable = const_cast<DEnvironmentLighting*>(this);
			if (auto Read = Mutable->CookedPlatformData.AcquireRead())
			{
				auto Candidate = std::make_shared<FEnvironmentLightingData>();
				FCanonicalMemoryReader Ar(Read.Lock.GetBytes(), EArchivePurpose::CookedPayload);
				Candidate->Serialize(Ar);
				const bool bValid = !Ar.HasError() && RequireArchiveEnd(Ar);
				Read.Lock.Reset();
				if (bValid) Mutable->Data = std::move(Candidate);
			}
		}
		return Data;
	}

	auto DEnvironmentLighting::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		if (Ar.GetTarget().Platform != "Win64" || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"EnvironmentLighting cooked platform data requires the Win64 Game target.");
			return;
		}
		FBulkData Projection;
		FBulkData* FieldValue = &CookedPlatformData;
		if (Ar.IsSaving())
		{
			if (!Data || !Data->IsValid())
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"EnvironmentLighting cooked platform data is unavailable.");
				return;
			}
			FByteBuffer Bytes;
			FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::CookedPayload);
			const_cast<FEnvironmentLightingData&>(*Data).Serialize(Writer);
			std::string Error;
			if (Writer.HasError()
				|| !FBulkData::TryCreateDetached(Bytes, Projection, &Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					Error.empty() ? std::string(Writer.GetError()) : std::move(Error));
				return;
			}
			FieldValue = &Projection;
		}
		auto Field = EnterArchiveField(Ar, {FName("Durin::DEnvironmentLighting"),
			FName("PlatformData"), FArchiveLogicalTypeDescriptor::BulkData()});
		FieldValue->Serialize(Ar, {.Alignment = EditorBulkDataExternalAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

	auto DEnvironmentLighting::ContributeToCook(
		FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
		{
			return Fail("Environment lighting supports only the Win64 game cook target.", &OutError);
		}
		if (!GetPackage()) return Fail("Environment-lighting asset has no package.", &OutError);
		FByteBuffer PayloadBytes;
		if (!LoadAuthoredPayload(GetPackage()->GetPackagePath(), PayloadBytes, OutError)) return false;
		auto Validated = std::make_shared<FEnvironmentLightingData>();
		FCanonicalMemoryReader PayloadAr(PayloadBytes, EArchivePurpose::CookedPayload);
		Validated->Serialize(PayloadAr);
		if (PayloadAr.HasError() || !RequireArchiveEnd(PayloadAr)) return Fail(PayloadAr.GetFailure()->Message, &OutError);
		Data = std::move(Validated);
		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}
}
