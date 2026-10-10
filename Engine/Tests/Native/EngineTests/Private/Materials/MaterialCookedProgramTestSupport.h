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

	inline auto SetMaterialCookedWordForTest(FByteBuffer& Bytes, size_t Offset, uint32 Value) -> bool
	{
		if (Offset > Bytes.size() || Bytes.size() - Offset < sizeof(uint32)) return false;
		FByteBuffer Encoded;
		FCanonicalMemoryWriter Writer(Encoded, EArchivePurpose::CookedPayload);
		Writer << Value;
		if (Writer.IsError()) return false;
		std::ranges::copy(Encoded, Bytes.begin() + Offset);
		return true;
	}

	inline auto RehashMaterialCookedPayloadForTest(FByteBuffer& Bytes) -> bool
	{
		if (Bytes.size() < 16) return false;
		auto Checksum = FXxHash128::HashBuffer(FByteView(Bytes).first(Bytes.size() - 16));
		FByteBuffer Encoded;
		FCanonicalMemoryWriter Writer(Encoded, EArchivePurpose::CookedPayload);
		Writer << Checksum.HashLow << Checksum.HashHigh;
		if (Writer.IsError()) return false;
		std::ranges::copy(Encoded, Bytes.end() - 16);
		return true;
	}

	// Mutate actual artifact headers and repair both checksums, so admission is tested
	// independently of corruption detection. Word 1 is schema, word 2 is pass ABI.
	inline auto SetMaterialCookedProgramArtifactWordForTest(
		FByteBuffer& Bytes, uint32 Word, uint32 Value) -> bool
	{
		if (Bytes.size() < 24 || Word < 1 || Word > 2) return false;
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
				if (Ar.IsError() || Artifact.size() < 36) { Valid = false; return; }
				if (!SetMaterialCookedWordForTest(Artifact, Word * sizeof(uint32), Value)
					|| !RehashMaterialCookedPayloadForTest(Artifact)) { Valid = false; return; }
				std::ranges::copy(Artifact, Bytes.begin() + Offset);
			});
		return Valid && !Reader.IsError() && !Artifacts.empty()
			&& RehashMaterialCookedPayloadForTest(Bytes);
	}

	inline auto SetMaterialCookedProgramFamilyVersionForTest(
		FByteBuffer& Bytes, uint32 Version) -> bool
	{
		return Bytes.size() >= 24 && SetMaterialCookedWordForTest(Bytes, sizeof(uint32), Version)
			&& RehashMaterialCookedPayloadForTest(Bytes);
	}
}
