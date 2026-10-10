#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialCookedProgram.h"
#include "Serialization/Archive.h"

namespace Durin::Testing
{
	inline auto EncodeMaterialCookedProgramFamilyForTest(
		const FMaterialCompilerResult& Program,
		const FMaterialStaticProperties& StaticProperties,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		FByteBuffer& OutBytes) -> FMaterialOperationResult
	{
		const std::array<const FMaterialCompilerResult*, 1> Programs{&Program};
		return EncodeMaterialCookedProgramFamily(Programs, StaticProperties,
			TargetPlatform, TargetProfile, OutBytes);
	}

	inline auto DecodeMaterialCookedProgramFamilyForTest(
		FByteView Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		const FMaterialCompilerResult& ExpectedConfiguration,
		FMaterialStaticProperties& OutStaticProperties,
		std::shared_ptr<const FMaterialCompilerResult>& OutProgram)
		-> FMaterialOperationResult
	{
		return DecodeMaterialCookedProgramFamily(Bytes, ExpectedPlatform,
			ExpectedProfile, ExpectedConfiguration.Quality,
			ExpectedConfiguration.FeatureLevel, ExpectedConfiguration.StaticBools,
			OutStaticProperties, OutProgram);
	}

	// Mutate actual artifact headers and repair both checksums, so admission is tested
	// independently of corruption detection. Word 1 is schema, 2-4 producer, 5 pass ABI.
	inline auto SetMaterialCookedProgramArtifactWordForTest(
		FByteBuffer& Bytes, uint32 Word, uint32 Value, uint32* Original = nullptr) -> bool
	{
		if (Bytes.size() < 24 || Word < 1 || Word > 5) return false;
		FCanonicalMemoryReader Reader(FByteView(Bytes).first(Bytes.size() - 16),
			EArchivePurpose::CookedPayload);
		uint32 Magic = 0, Version = 0;
		ECookTargetPlatform Platform{};
		ECookTargetProfile Profile{};
		Reader << Magic << Version << Platform << Profile;
		std::vector<uint32> Configurations;
		SerializeBoundedSequence(Reader, Configurations, MaterialCookedProgramMaxConfigurations,
			[](FArchive& Ar, uint32& ArtifactIndex) {
				EMaterialQualityLevel Quality{};
				ERHIFeatureLevel Feature{};
				std::vector<FMaterialCompilerEnvironment::FStaticBoolValue> StaticBools;
				Ar << Quality << Feature;
				SerializeBoundedSequence(Ar, StaticBools, MaterialMaxStaticBoolDeclarations,
					[](FArchive& Inner, auto& Bool) { Inner << Bool.DeclarationId << Bool.Value; });
				Ar << ArtifactIndex;
			});
		std::vector<FByteBuffer> Artifacts;
		bool Valid = true;
		SerializeBoundedSequence(Reader, Artifacts, MaterialCookedProgramMaxConfigurations,
			[&](FArchive& Ar, FByteBuffer& Artifact) {
				const auto Offset = Ar.Tell() + sizeof(uint64);
				SerializeByteBuffer(Ar, Artifact, MaterialCookedProgramMaxPayloadBytes);
				if (Ar.IsError() || Artifact.size() < 48) { Valid = false; return; }
				const auto WordOffset = Word * sizeof(uint32);
				if (Original)
				{
					FCanonicalMemoryReader WordReader(FByteView(Artifact).subspan(WordOffset, 4),
						EArchivePurpose::CookedPayload);
					WordReader << *Original;
				}
				FByteBuffer Encoded;
				FCanonicalMemoryWriter Writer(Encoded, EArchivePurpose::CookedPayload);
				Writer << Value;
				std::ranges::copy(Encoded, Artifact.begin() + WordOffset);
				auto Checksum = FXxHash128::HashBuffer(FByteView(Artifact).first(Artifact.size() - 16));
				Encoded.clear();
				Writer << Checksum.HashLow << Checksum.HashHigh;
				std::ranges::copy(Encoded, Artifact.end() - 16);
				std::ranges::copy(Artifact, Bytes.begin() + Offset);
			});
		if (!Valid || Reader.IsError() || Artifacts.empty()) return false;
		auto Checksum = FXxHash128::HashBuffer(FByteView(Bytes).first(Bytes.size() - 16));
		FByteBuffer Encoded;
		FCanonicalMemoryWriter Writer(Encoded, EArchivePurpose::CookedPayload);
		Writer << Checksum.HashLow << Checksum.HashHigh;
		std::ranges::copy(Encoded, Bytes.end() - 16);
		return true;
	}

	inline auto SetMaterialCookedProgramFamilyVersionForTest(
		FByteBuffer& Bytes, uint32 Version) -> bool
	{
		if (Bytes.size() < 24) return false;
		FByteBuffer EncodedVersion;
		FCanonicalMemoryWriter VersionWriter(
			EncodedVersion, EArchivePurpose::CookedPayload);
		VersionWriter << Version;
		if (VersionWriter.IsError() || EncodedVersion.size() != sizeof(uint32))
			return false;
		std::ranges::copy(EncodedVersion, Bytes.begin() + sizeof(uint32));
		const FXxHash128 Checksum = FXxHash128::HashBuffer(
			FByteView(Bytes.data(), Bytes.size() - 16));
		FByteBuffer EncodedChecksum;
		FCanonicalMemoryWriter ChecksumWriter(
			EncodedChecksum, EArchivePurpose::CookedPayload);
		auto MutableChecksum = Checksum;
		ChecksumWriter << MutableChecksum.HashLow << MutableChecksum.HashHigh;
		if (ChecksumWriter.IsError() || EncodedChecksum.size() != 16) return false;
		std::ranges::copy(EncodedChecksum, Bytes.end() - 16);
		return true;
	}
}
