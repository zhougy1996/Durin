#include <gtest/gtest.h>

#include "StaticMesh/StaticMeshBuildDerivedData.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshResources.h"

namespace
{
	auto MakeKeyInput() -> Durin::Asset::Build::FStaticMeshBuildKeyInput
	{
		Durin::Asset::Build::FStaticMeshBuildKeyInput Input;
		Input.SourceContentHash = Durin::FXxHash128{
			0x0123456789abcdefull,
			0xfedcba9876543210ull};
		Input.ImporterId = "Assimp";
		Input.ImporterVersion = 602;
		Input.ImportSettings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
		Input.TargetPlatform = Durin::EStaticMeshTargetPlatform::Win64;
		return Input;
	}
}

TEST(FStaticMeshDerivedDataContractTests, KeyEncodingIsCanonicalAndDeterministic)
{
	const Durin::Asset::Build::FStaticMeshBuildKeyInput Input = MakeKeyInput();
	std::string Error;
	const std::vector<std::byte> First =
		Durin::Asset::Build::BuildStaticMeshDerivedDataKeyBytes(Input, Error);
	ASSERT_TRUE(Error.empty()) << Error;
	const std::vector<std::byte> Second =
		Durin::Asset::Build::BuildStaticMeshDerivedDataKeyBytes(Input, Error);
	const std::vector<std::byte> Expected = [] {
		const Durin::uint8 Values[]{
		0x01, 0x00, 0x00, 0x00,
		0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
		0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
		0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		'A', 's', 's', 'i', 'm', 'p',
		0x5a, 0x02, 0x00, 0x00,
		0x05, 0x00, 0x02,
		0x03, 0x00, 0x00, 0x00,
		0x04, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00};
		const std::span<const std::byte> Bytes = std::as_bytes(std::span{Values});
		return std::vector<std::byte>(Bytes.begin(), Bytes.end());
	}();

	EXPECT_EQ(First, Second);
	EXPECT_EQ(First, Expected);
	EXPECT_EQ(Durin::Asset::Build::BuildStaticMeshDerivedDataKey(Input, Error),
		"423fd576f6529b0df5c564c4f093ae11");
	EXPECT_EQ(Durin::Asset::Build::BuildStaticMeshDerivedDataKey(Input, Error).size(), 32u);
}

TEST(FStaticMeshDerivedDataContractTests, EverySemanticInputChangesTheKey)
{
	const Durin::Asset::Build::FStaticMeshBuildKeyInput Baseline = MakeKeyInput();
	std::string Error;
	const std::string BaselineKey =
		Durin::Asset::Build::BuildStaticMeshDerivedDataKey(Baseline, Error);

	auto ExpectChanged = [&](auto Mutate)
	{
		Durin::Asset::Build::FStaticMeshBuildKeyInput Changed = Baseline;
		Mutate(Changed);
		EXPECT_NE(Durin::Asset::Build::BuildStaticMeshDerivedDataKey(Changed, Error), BaselineKey);
	};

	ExpectChanged([](auto& Value) { ++Value.SourceContentHash.HashLow; });
	ExpectChanged([](auto& Value) { Value.ImporterId = "DurinFixtureImporter"; });
	ExpectChanged([](auto& Value) { ++Value.ImporterVersion; });
	ExpectChanged([](auto& Value) { Value.ImportSettings.ForwardAxis = Durin::EStaticMeshImportAxis::PositiveX; });
	ExpectChanged([](auto& Value) { Value.ImportSettings.RightAxis = Durin::EStaticMeshImportAxis::NegativeX; });
	ExpectChanged([](auto& Value) { Value.ImportSettings.UpAxis = Durin::EStaticMeshImportAxis::PositiveZ; });
	ExpectChanged([](auto& Value) { ++Value.BuilderVersion; });
	ExpectChanged([](auto& Value) { ++Value.PayloadSchemaVersion; });
	ExpectChanged([](auto& Value) { Value.TargetPlatform = Durin::EStaticMeshTargetPlatform::Unknown; });
}

TEST(FStaticMeshDerivedDataContractTests, FormatConstantsRemainWithinReaderLimits)
{
	EXPECT_EQ(Durin::StaticMeshPayloadHeaderSize, 64u);
	EXPECT_EQ(Durin::StaticMeshPayloadChunkEntrySize, 32u);
	EXPECT_EQ(Durin::StaticMeshPayloadAlignment, 16u);
	EXPECT_GE(Durin::MaximumStaticMeshPayloadChunks, 6u);
	EXPECT_GE(Durin::MaxStaticMeshUVChannels, 4u);
}
