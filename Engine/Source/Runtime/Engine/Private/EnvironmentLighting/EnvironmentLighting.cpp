#include "EnvironmentLighting/EnvironmentLighting.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "Hash/XxHash.h"
#include "Serialization/BinaryFormat.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Serialization/Archive.h"
#include "Serialization/BoundedPayloadSerialization.h"

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

		auto AppendHalfBytes(FByteArray& Bytes, const std::vector<uint16>& Values) -> void
		{
			const size_t Offset = Bytes.size();
			Bytes.resize(Offset + Values.size() * sizeof(uint16));
			std::memcpy(Bytes.data() + Offset, Values.data(), Values.size() * sizeof(uint16));
		}

		auto ReadHalfValues(
			std::span<const std::byte> Bytes,
			size_t& Offset,
			size_t Count,
			std::vector<uint16>& OutValues) -> bool
		{
			const size_t ByteCount = Count * sizeof(uint16);
			if (Offset > Bytes.size() || ByteCount > Bytes.size() - Offset) return false;
			OutValues.resize(Count);
			std::memcpy(OutValues.data(), Bytes.data() + Offset, ByteCount);
			Offset += ByteCount;
			return true;
		}

		auto LoadAuthoredPayload(
			std::string_view VirtualPackagePath,
			FByteArray& OutBytes,
			std::string& OutError) -> bool
		{
			const std::filesystem::path PayloadPath =
				DEnvironmentLighting::GetAuthoredPayloadPath(VirtualPackagePath);
			if (PayloadPath.empty()
				|| !FFileHelper::LoadFileToArray(OutBytes, PayloadPath))
			{
				return Fail(std::format(
					"Environment-lighting authored payload is missing for '{}'.",
					VirtualPackagePath), &OutError);
			}
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

	static auto BuildEnvironmentLightingSerializedValue(
		const FEnvironmentLightingData& Data,
		FByteArray& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		OutError.clear();
		if (!Data.IsValid())
			return Fail("Environment-lighting data is incomplete or malformed.", &OutError);

		FByteArray Body;
		Body.reserve(static_cast<size_t>(ExpectedElementCount() * sizeof(uint16)));
		for (const std::vector<uint16>& Face : Data.Irradiance) AppendHalfBytes(Body, Face);
		for (const auto& Mip : Data.Prefiltered)
			for (const std::vector<uint16>& Face : Mip) AppendHalfBytes(Body, Face);
		AppendHalfBytes(Body, Data.BrdfLut);

		FBinaryWriter Writer;
		Writer.WriteHeader({
			.Magic = 0,
			.SchemaVersion = EnvironmentLightingPayloadSchemaVersion,
			.FormatVersion = DefaultStudioEnvironmentBuilderVersion});
		Writer.WriteU32(EnvironmentLightingStablePixelFormatRgba16Float);
		Writer.WriteU32(EnvironmentIrradianceDimension);
		Writer.WriteU32(EnvironmentPrefilterDimension);
		Writer.WriteU32(EnvironmentPrefilterMipCount);
		Writer.WriteU32(EnvironmentBrdfLutDimension);
		Writer.WriteU64(ExpectedElementCount());
		Writer.WriteU64(FXxHash64::HashBuffer(Body).HashValue);
		Writer.WriteBytes(Body);
		OutBytes = Writer.TakeBytes();
		return true;
	}

	static auto ParseEnvironmentLightingSerializedValue(
		std::span<const std::byte> Bytes,
		FEnvironmentLightingData& OutData) -> FDecodeResult
	{
		auto Reject = [](EDecodeError Code, std::string Message) {
			return FDecodeResult{Code, std::move(Message)};
		};
		FBinaryReader Reader(Bytes);
		uint32 PixelFormat = 0;
		uint32 IrradianceDimension = 0;
		uint32 PrefilterDimension = 0;
		uint32 PrefilterMipCount = 0;
		uint32 BrdfLutDimension = 0;
		uint64 ElementCount = 0;
		uint64 StoredHash = 0;
		uint32 Reserved0 = 0;
		uint32 SchemaVersion = 0;
		uint32 ProducerVersion = 0;
		uint32 SerializationMarker = 0;
		if (!Reader.ReadU32(Reserved0)
			|| !Reader.ReadU32(SchemaVersion)
			|| !Reader.ReadU32(ProducerVersion)
			|| !Reader.ReadU32(SerializationMarker)
			|| !Reader.ReadU32(PixelFormat)
			|| !Reader.ReadU32(IrradianceDimension)
			|| !Reader.ReadU32(PrefilterDimension)
			|| !Reader.ReadU32(PrefilterMipCount)
			|| !Reader.ReadU32(BrdfLutDimension)
			|| !Reader.ReadU64(ElementCount)
			|| !Reader.ReadU64(StoredHash))
		{
			return Reject(EDecodeError::Corrupt,
				"Environment-lighting payload header is invalid.");
		}
		if (Reserved0 != 0)
			return Reject(EDecodeError::Corrupt,
				"Environment-lighting payload reserved header field is nonzero.");
		if (SchemaVersion != EnvironmentLightingPayloadSchemaVersion
			|| SerializationMarker != BinaryFormatMarker
			|| PixelFormat != EnvironmentLightingStablePixelFormatRgba16Float
			|| IrradianceDimension != EnvironmentIrradianceDimension
			|| PrefilterDimension != EnvironmentPrefilterDimension
			|| PrefilterMipCount != EnvironmentPrefilterMipCount
			|| BrdfLutDimension != EnvironmentBrdfLutDimension
			|| ElementCount != ExpectedElementCount())
		{
			return Reject(EDecodeError::Incompatible,
				"Environment-lighting payload layout is incompatible.");
		}
		// Producer identity is diagnostic metadata. Runtime compatibility is owned
		// by the schema and stable value identifiers.
		(void)ProducerVersion;
		const uint64 ExpectedBodyBytes = ElementCount * sizeof(uint16);
		if (ExpectedBodyBytes != Reader.GetRemainingBytes())
			return Reject(EDecodeError::Corrupt,
				"Environment-lighting payload size is invalid.");
		FByteArray Body;
		if (!Reader.ReadBytes(Body, ExpectedBodyBytes, ExpectedBodyBytes)
			|| !Reader.IsAtEnd()
			|| FXxHash64::HashBuffer(Body).HashValue != StoredHash)
		{
			return Reject(EDecodeError::Corrupt,
				"Environment-lighting payload checksum does not match.");
		}

		FEnvironmentLightingData Candidate;
		size_t Offset = 0;
		const size_t IrradianceElements = static_cast<size_t>(EnvironmentIrradianceDimension)
			* EnvironmentIrradianceDimension * 4;
		for (std::vector<uint16>& Face : Candidate.Irradiance)
			if (!ReadHalfValues(Body, Offset, IrradianceElements, Face))
				return Reject(EDecodeError::Corrupt,
					"Environment-lighting irradiance data is truncated.");
		for (uint32 Mip = 0; Mip < EnvironmentPrefilterMipCount; ++Mip)
		{
			const size_t Dimension = EnvironmentPrefilterDimension >> Mip;
			for (std::vector<uint16>& Face : Candidate.Prefiltered[Mip])
				if (!ReadHalfValues(Body, Offset, Dimension * Dimension * 4, Face))
					return Reject(EDecodeError::Corrupt,
						"Environment-lighting prefilter data is truncated.");
		}
		if (!ReadHalfValues(
				Body, Offset,
				static_cast<size_t>(EnvironmentBrdfLutDimension)
					* EnvironmentBrdfLutDimension * 4,
				Candidate.BrdfLut)
			|| Offset != Body.size() || !Candidate.IsValid())
		{
			return Reject(EDecodeError::Corrupt,
				"Environment-lighting BRDF LUT data is invalid.");
		}
		OutData = std::move(Candidate);
		return {};
	}

	auto FEnvironmentLightingData::Serialize(FArchive& Ar) -> void
	{
		SerializeBoundedArchivePayload(
			Ar,
			*this,
			{ExpectedElementCount() * sizeof(uint16) + 64,
				"Environment-lighting payload"},
			[](const FEnvironmentLightingData& Value,
				FByteArray& Bytes, std::string& Error) {
				return BuildEnvironmentLightingSerializedValue(Value, Bytes, Error);
			},
			[](std::span<const std::byte> Bytes, FEnvironmentLightingData& Candidate) {
				return ParseEnvironmentLightingSerializedValue(Bytes, Candidate);
			});
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

	auto DEnvironmentLighting::PostLoad(std::string& OutError) -> bool
	{
		FByteArray PayloadBytes;
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (PayloadSchemaVersion != EnvironmentLightingPayloadSchemaVersion
				|| CookedPlatformData.GetMetadata().LogicalSize == 0)
				return Fail(
					"Cooked environment-lighting PlatformData field is missing.",
					&OutError);
			Data.reset();
			OutError.clear();
			return true;
		}
		else
		{
			if (!GetPackage()
				|| !LoadAuthoredPayload(GetPackage()->GetPackagePath(), PayloadBytes, OutError))
				return false;
		}
		auto Candidate = std::make_shared<FEnvironmentLightingData>();
		FCanonicalMemoryReader PayloadAr(PayloadBytes,
			GetAssetRuntimeConfiguration().RequiresCookedPayload()
				? EArchivePurpose::CookedPayload : EArchivePurpose::DerivedDataPayload);
		Candidate->Serialize(PayloadAr);
		if (PayloadAr.HasError())
			return Fail(PayloadAr.GetFailure()->Message, &OutError);
		Data = std::move(Candidate);
		OutError.clear();
		return true;
	}

	auto DEnvironmentLighting::GetData() const
		-> const std::shared_ptr<const FEnvironmentLightingData>&
	{
		if (!Data && GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedPlatformData.GetMetadata().LogicalSize != 0)
		{
			std::span<const std::byte> Bytes;
			std::string Error;
			DEnvironmentLighting* Mutable = const_cast<DEnvironmentLighting*>(this);
			if (Mutable->CookedPlatformData.LockReadOnly(Bytes, &Error))
			{
				auto Candidate = std::make_shared<FEnvironmentLightingData>();
				FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::CookedPayload);
				Candidate->Serialize(Ar);
				const bool bValid = !Ar.HasError() && RequireArchiveEnd(Ar);
				Mutable->CookedPlatformData.UnlockReadOnly();
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
			FByteArray Bytes;
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
		FByteArray PayloadBytes;
		if (!LoadAuthoredPayload(GetPackage()->GetPackagePath(), PayloadBytes, OutError)) return false;
		auto Validated = std::make_shared<FEnvironmentLightingData>();
		FCanonicalMemoryReader PayloadAr(PayloadBytes, EArchivePurpose::CookedPayload);
		Validated->Serialize(PayloadAr);
		if (PayloadAr.HasError()) return Fail(PayloadAr.GetFailure()->Message, &OutError);
		Data = std::move(Validated);
		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}
}
