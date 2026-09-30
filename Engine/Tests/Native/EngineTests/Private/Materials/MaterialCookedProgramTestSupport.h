#pragma once

#include "Hash/XxHash.h"
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
