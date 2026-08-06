#include "EnvironmentLighting/EnvironmentLighting.h"

#include "AssetSystem.h"
#include "Hash/XxHash.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 EnvironmentLightingStablePixelFormatRgba16Float = 1;

		auto Fail(std::string& OutError, std::string Message) -> bool
		{
			OutError = std::move(Message);
			return false;
		}

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

		auto AppendHalfBytes(std::vector<uint8>& Bytes, const std::vector<uint16>& Values) -> void
		{
			const size_t Offset = Bytes.size();
			Bytes.resize(Offset + Values.size() * sizeof(uint16));
			std::memcpy(Bytes.data() + Offset, Values.data(), Values.size() * sizeof(uint16));
		}

		auto ReadHalfValues(
			std::span<const uint8> Bytes,
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

		auto LoadAuthoringPayload(
			std::string_view VirtualPackagePath,
			std::vector<uint8>& OutBytes,
			std::string& OutError) -> bool
		{
			const std::filesystem::path PayloadPath =
				DEnvironmentLighting::GetAuthoringPayloadPath(VirtualPackagePath);
			if (PayloadPath.empty()
				|| !FFileHelper::LoadFileToArray(OutBytes, PayloadPath.generic_string()))
			{
				return Fail(OutError, std::format(
					"Environment-lighting authoring payload is missing for '{}'.",
					VirtualPackagePath));
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

	auto EncodeEnvironmentLightingPayload(
		const FEnvironmentLightingData& Data,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		OutError.clear();
		if (!Data.IsValid())
			return Fail(OutError, "Environment-lighting data is incomplete or malformed.");

		std::vector<uint8> Body;
		Body.reserve(static_cast<size_t>(ExpectedElementCount() * sizeof(uint16)));
		for (const std::vector<uint16>& Face : Data.Irradiance) AppendHalfBytes(Body, Face);
		for (const auto& Mip : Data.Prefiltered)
			for (const std::vector<uint16>& Face : Mip) AppendHalfBytes(Body, Face);
		AppendHalfBytes(Body, Data.BrdfLut);

		DerivedDataCache::FWriter Writer;
		Writer.WriteHeader({
			.Magic = EnvironmentLightingPayloadMagic,
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

	auto DecodeEnvironmentLightingPayload(
		std::span<const uint8> Bytes,
		std::shared_ptr<const FEnvironmentLightingData>& OutData,
		std::string& OutError) -> bool
	{
		OutData.reset();
		OutError.clear();
		DerivedDataCache::FReader Reader(Bytes);
		uint32 PixelFormat = 0;
		uint32 IrradianceDimension = 0;
		uint32 PrefilterDimension = 0;
		uint32 PrefilterMipCount = 0;
		uint32 BrdfLutDimension = 0;
		uint64 ElementCount = 0;
		uint64 StoredHash = 0;
		if (!Reader.ReadAndValidateHeader(
				EnvironmentLightingPayloadMagic,
				EnvironmentLightingPayloadSchemaVersion,
				DefaultStudioEnvironmentBuilderVersion)
			|| !Reader.ReadU32(PixelFormat)
			|| !Reader.ReadU32(IrradianceDimension)
			|| !Reader.ReadU32(PrefilterDimension)
			|| !Reader.ReadU32(PrefilterMipCount)
			|| !Reader.ReadU32(BrdfLutDimension)
			|| !Reader.ReadU64(ElementCount)
			|| !Reader.ReadU64(StoredHash))
		{
			return Fail(OutError, "Environment-lighting payload header is invalid.");
		}
		if (PixelFormat != EnvironmentLightingStablePixelFormatRgba16Float
			|| IrradianceDimension != EnvironmentIrradianceDimension
			|| PrefilterDimension != EnvironmentPrefilterDimension
			|| PrefilterMipCount != EnvironmentPrefilterMipCount
			|| BrdfLutDimension != EnvironmentBrdfLutDimension
			|| ElementCount != ExpectedElementCount())
		{
			return Fail(OutError, "Environment-lighting payload layout is incompatible.");
		}
		const uint64 ExpectedBodyBytes = ElementCount * sizeof(uint16);
		if (ExpectedBodyBytes != Reader.GetRemainingBytes())
			return Fail(OutError, "Environment-lighting payload size is invalid.");
		std::vector<uint8> Body;
		if (!Reader.ReadBytes(Body, ExpectedBodyBytes, ExpectedBodyBytes)
			|| !Reader.IsAtEnd()
			|| FXxHash64::HashBuffer(Body).HashValue != StoredHash)
		{
			return Fail(OutError, "Environment-lighting payload checksum does not match.");
		}

		auto Candidate = std::make_shared<FEnvironmentLightingData>();
		size_t Offset = 0;
		const size_t IrradianceElements = static_cast<size_t>(EnvironmentIrradianceDimension)
			* EnvironmentIrradianceDimension * 4;
		for (std::vector<uint16>& Face : Candidate->Irradiance)
			if (!ReadHalfValues(Body, Offset, IrradianceElements, Face))
				return Fail(OutError, "Environment-lighting irradiance data is truncated.");
		for (uint32 Mip = 0; Mip < EnvironmentPrefilterMipCount; ++Mip)
		{
			const size_t Dimension = EnvironmentPrefilterDimension >> Mip;
			for (std::vector<uint16>& Face : Candidate->Prefiltered[Mip])
				if (!ReadHalfValues(Body, Offset, Dimension * Dimension * 4, Face))
					return Fail(OutError, "Environment-lighting prefilter data is truncated.");
		}
		if (!ReadHalfValues(
				Body, Offset,
				static_cast<size_t>(EnvironmentBrdfLutDimension)
					* EnvironmentBrdfLutDimension * 4,
				Candidate->BrdfLut)
			|| Offset != Body.size() || !Candidate->IsValid())
		{
			return Fail(OutError, "Environment-lighting BRDF LUT data is invalid.");
		}
		OutData = std::move(Candidate);
		return true;
	}

	DEnvironmentLighting::DEnvironmentLighting(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DEnvironmentLighting::GetAuthoringPayloadPath(std::string_view VirtualPackagePath)
		-> std::filesystem::path
	{
		const PathUtilities::FAssetPathResult Resolved = PathUtilities::ResolveAssetPath(
			VirtualPackagePath, PathUtilities::EPathExistence::AllowMissing);
		if (!Resolved) return {};
		std::filesystem::path Result = Resolved.PhysicalPath;
		Result += ".iblbulk";
		return Result;
	}

	auto DEnvironmentLighting::PostLoad(std::string& OutError) -> bool
	{
		if (Asset::IsAssetMigrationLoad())
		{
			OutError.clear();
			return true;
		}
		std::vector<uint8> PayloadBytes;
		if (Asset::GetPackageLoadContext().Mode == Asset::EPackageLoadMode::CookedRuntime)
		{
			if (CookedPayload.PayloadId != EnvironmentLightingPrimaryCookedPayloadId
				|| CookedPayload.LocationKind
					!= static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
				|| CookedPayload.PayloadSchemaVersion != EnvironmentLightingPayloadSchemaVersion
				|| CookedPayload.TargetPlatform
					!= static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
				|| CookedPayload.TargetProfile
					!= static_cast<uint32>(Asset::ECookTargetProfile::Game))
			{
				return Fail(OutError, "Cooked environment-lighting descriptor is missing or incompatible.");
			}
			const Asset::FPackageLoadContext& LoadContext = Asset::GetPackageLoadContext();
			std::filesystem::path PackagePath;
			std::filesystem::path CompanionPath;
			if (!GetPackage()
				|| !Asset::ResolveCookedPackagePath(
					LoadContext.CookRoot, GetPackage()->GetPackagePath(), PackagePath, &OutError)
				|| !Asset::ResolveCookedCompanionPath(
					LoadContext.CookRoot, PackagePath, CompanionPath, &OutError)) return false;
			Asset::FCookedBulkContainer Container;
			if (!Asset::LoadCookedBulkFile(
					CompanionPath,
					Asset::ECookTargetPlatform::Win64,
					Asset::ECookTargetProfile::Game,
					Container,
					&OutError)) return false;
			std::span<const uint8> Payload;
			if (!Asset::ResolveCookedPayload(Container, CookedPayload, Payload, &OutError))
				return false;
			PayloadBytes.assign(Payload.begin(), Payload.end());
		}
		else
		{
			if (!GetPackage()
				|| !LoadAuthoringPayload(GetPackage()->GetPackagePath(), PayloadBytes, OutError))
				return false;
		}
		return DecodeEnvironmentLightingPayload(PayloadBytes, Data, OutError);
	}

	auto DEnvironmentLighting::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
		{
			return Fail(OutError, "Environment lighting supports only the Win64 game cook target.");
		}
		if (!GetPackage()) return Fail(OutError, "Environment-lighting asset has no package.");
		std::vector<uint8> PayloadBytes;
		if (!LoadAuthoringPayload(GetPackage()->GetPackagePath(), PayloadBytes, OutError)) return false;
		std::shared_ptr<const FEnvironmentLightingData> Validated;
		if (!DecodeEnvironmentLightingPayload(PayloadBytes, Validated, OutError)) return false;

		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = EnvironmentLightingPrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = EnvironmentLightingPayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = EnvironmentLightingPayloadAlignment,
			.Bytes = std::move(PayloadBytes)};
		return Context.AddPackage(
			std::string(VirtualPackagePath), {std::move(BulkPayload)},
			[this](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<uint8>& OutPackageBytes,
				std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId != EnvironmentLightingPrimaryCookedPayloadId)
				{
					if (Error) *Error = "Environment-lighting cook produced an invalid descriptor.";
					return false;
				}
				const Asset::FCookedPayloadDescriptor Saved = CookedPayload;
				CookedPayload = Descriptors.front();
				const Asset::FAssetResult Result =
					Asset::SerializeAssetPackageBytes(GetPackage(), OutPackageBytes);
				CookedPayload = Saved;
				if (!Result)
				{
					if (Error) *Error = Result.Message;
					return false;
				}
				return true;
			}, &OutError);
	}
}
