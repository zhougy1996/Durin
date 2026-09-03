#include "Terrain/TerrainHeightmapBuild.h"
#include "Terrain/TerrainHeightmapBuildKey.h"

#include "Asset/AssetDerivedDataCache.h"
#include "Serialization/Archive.h"
#include "Terrain/TerrainHeightmapDerivedData.h"

namespace Durin
{
#if DURIN_WITH_EDITOR
	namespace
	{
		constexpr std::string_view TerrainHeightmapBucket =
			"TerrainHeightmap/Objects";

		auto EncodePayload(const FTerrainHeightmapPayload& Payload,
			FByteArray& OutBytes, std::string& OutError) -> bool
		{
			OutBytes.clear();
			FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::DerivedDataPayload);
			const_cast<FTerrainHeightmapPayload&>(Payload).Serialize(
				Ar, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
			if (!Ar.HasError())
			{
				OutError.clear();
				return true;
			}
			OutError = Ar.GetFailure()->Message;
			OutBytes.clear();
			return false;
		}

		auto DecodePayload(std::span<const std::byte> Bytes,
			std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
			std::string& OutError) -> bool
		{
			auto Candidate = std::make_shared<FTerrainHeightmapPayload>();
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			Candidate->Serialize(Ar, ECookTargetPlatform::Win64,
				ECookTargetProfile::Game);
			if (Ar.HasError() || !RequireArchiveEnd(Ar) || !Candidate->IsValid())
			{
				OutError = Ar.GetFailure() ? Ar.GetFailure()->Message
					: "Terrain heightmap payload is invalid or has trailing bytes.";
				return false;
			}
			OutPayload = std::move(Candidate);
			OutError.clear();
			return true;
		}

		auto SetInvocationError(EFeatureInvokeStatus Status,
			std::string& OutError) -> void
		{
			if (Status == EFeatureInvokeStatus::Unavailable)
				OutError = "The Terrain heightmap build provider is unavailable.";
			else if (Status == EFeatureInvokeStatus::Ambiguous)
				OutError = "Multiple Terrain heightmap build providers are registered.";
			else if (Status == EFeatureInvokeStatus::VisitorFailed)
				OutError = "The Terrain heightmap build provider invocation failed.";
			else if (OutError.empty())
				OutError = "The Terrain heightmap build provider failed without a diagnostic.";
		}
	}

#endif
	auto BuildTerrainHeightmapDerivedData(
		FTerrainHeightmapDerivedDataRequest Request,
		FTerrainHeightmapDerivedDataResult& OutResult,
		std::string& OutError) -> bool
	{
		OutResult = {};
#if !DURIN_WITH_EDITOR
		OutError = "Terrain heightmap authored build orchestration is unavailable outside editor builds.";
		return false;
#else
		if (Request.ShouldCancel && Request.ShouldCancel())
		{
			OutError = "Terrain heightmap build was canceled.";
			return false;
		}
		FTerrainHeightmapImportedData ImportedData = Request.ImportedData.value_or(
			FTerrainHeightmapImportedData{});
		if ((!Request.ImportedData
			&& !ImportedData.SetSamples(Request.Width, Request.Height, Request.Samples))
			|| !ImportedData.IsValid())
		{
			OutError = "Terrain heightmap build requires valid canonical uint16 samples.";
			return false;
		}
		Request.Width = ImportedData.Width;
		Request.Height = ImportedData.Height;
		const FXxHash128 ImportedIdentity = ImportedData.GetIdentity();
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ITerrainHeightmapBuildProvider>([&](ITerrainHeightmapBuildProvider& Provider) {
			const FTerrainHeightmapBuildProviderDescriptor Descriptor =
				Provider.GetHeightmapDescriptor();
			if (!Descriptor.IsValid())
			{
				OutError = "The Terrain heightmap provider descriptor is invalid.";
				return false;
			}
			const FTerrainHeightmapBuildKeyInput KeyInput{
				.SourceContentHash = ImportedIdentity,
				.DecoderId = Descriptor.ProducerIdentity,
				.DecoderVersion = Descriptor.ProducerVersion,
				.SourceFormat = ETerrainHeightmapSourceFormat::Raw16,
				.SourceProfileVersion = TerrainHeightmapImportedDataSchemaVersion,
				.TargetPlatform = ECookTargetPlatform::Win64,
				.TargetProfile = ECookTargetProfile::Game};
			const std::string Key = BuildTerrainHeightmapDerivedDataKey(KeyInput, OutError);
			if (Key.empty()) return false;
			AssetDerivedDataCache::FOperationDiagnostic ReadDiagnostic;
			FByteArray Bytes;
			std::shared_ptr<const FTerrainHeightmapPayload> Payload;
			bool bHit = Request.bPersistDerivedData && Request.bQueryDerivedData
				&& AssetDerivedDataCache::Load(TerrainHeightmapBucket, Key,
					MaximumTerrainHeightmapPayloadBytes, Bytes, ReadDiagnostic)
					== AssetDerivedDataCache::ELoadResult::Hit;
			if (bHit)
			{
				bHit = DecodePayload(Bytes, Payload, ReadDiagnostic.Message)
					&& Payload->Width == Request.Width && Payload->Height == Request.Height;
				if (!bHit && ReadDiagnostic.Message.empty())
					ReadDiagnostic.Message = "Cached Terrain heightmap dimensions do not match the canonical input.";
			}
			AssetDerivedDataCache::FOperationDiagnostic WriteDiagnostic;
			if (!bHit)
			{
				OutError.clear();
				if (Request.ShouldCancel && Request.ShouldCancel())
				{
					OutError = "Terrain heightmap build was canceled.";
					return false;
				}
				if (Request.ImportedData) Request.Samples = ImportedData.GetSamples();
				if (Request.Samples.size() != static_cast<uint64>(Request.Width) * Request.Height)
				{
					OutError = "Terrain heightmap canonical samples are unavailable.";
					return false;
				}
				FTerrainHeightmapRecipeProduct Product;
				if (!Provider.Build({
					.Samples = std::move(Request.Samples),
					.Width = Request.Width,
					.Height = Request.Height,
					.ShouldCancel = Request.ShouldCancel}, Product, OutError)
					|| !Product.Payload) return false;
				Payload = std::move(Product.Payload);
				if (Payload->Width != Request.Width || Payload->Height != Request.Height
					|| !Payload->IsValid())
				{
					OutError = "Terrain heightmap provider returned an incompatible payload.";
					return false;
				}
				if (!EncodePayload(*Payload, Bytes, OutError)) return false;
				if (Request.ShouldCancel && Request.ShouldCancel())
				{
					OutError = "Terrain heightmap build was canceled.";
					return false;
				}
				if (Request.bPersistDerivedData)
					AssetDerivedDataCache::Store(TerrainHeightmapBucket, Key, Bytes,
						MaximumTerrainHeightmapPayloadBytes, WriteDiagnostic);
			}
			if (Request.ShouldCancel && Request.ShouldCancel())
			{
				OutError = "Terrain heightmap build was canceled.";
				return false;
			}
			OutResult = {
				.Payload = std::move(Payload),
				.ImportedData = std::move(ImportedData),
				.ImportedDataIdentity = ImportedIdentity,
				.Key = Key,
				.Origin = bHit ? ETerrainHeightmapDerivedDataOrigin::CacheHit
					: ETerrainHeightmapDerivedDataOrigin::Rebuilt,
				.Descriptor = Descriptor,
				.CacheReadNanoseconds = ReadDiagnostic.DurationNanoseconds,
				.CacheWriteNanoseconds = WriteDiagnostic.DurationNanoseconds,
				.PayloadBytes = Bytes.size(),
				.Diagnostic = AssetDerivedDataCache::CombineDiagnostics(
					ReadDiagnostic, WriteDiagnostic)};
			return true;
		});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			OutError.clear();
			return true;
		}
		OutResult = {};
		SetInvocationError(Invocation.Status, OutError);
		return false;
#endif
	}
}
