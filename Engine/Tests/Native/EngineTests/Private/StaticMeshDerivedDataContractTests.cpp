#include <gtest/gtest.h>

#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshResources.h"

namespace
{
	auto MakeKeyInput() -> Durin::FStaticMeshDerivedDataKeyInput
	{
		Durin::FStaticMeshDerivedDataKeyInput Input;
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
	const Durin::FStaticMeshDerivedDataKeyInput Input = MakeKeyInput();
	const std::vector<Durin::uint8> First = Durin::BuildStaticMeshDerivedDataKeyBytes(Input);
	const std::vector<Durin::uint8> Second = Durin::BuildStaticMeshDerivedDataKeyBytes(Input);
	const std::vector<Durin::uint8> Expected{
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

	EXPECT_EQ(First, Second);
	EXPECT_EQ(First, Expected);
	EXPECT_EQ(Durin::BuildStaticMeshDerivedDataKey(Input), Durin::BuildStaticMeshDerivedDataKey(Input));
	EXPECT_EQ(Durin::BuildStaticMeshDerivedDataKey(Input).size(), 32u);
}

TEST(FStaticMeshDerivedDataContractTests, EverySemanticInputChangesTheKey)
{
	const Durin::FStaticMeshDerivedDataKeyInput Baseline = MakeKeyInput();
	const std::string BaselineKey = Durin::BuildStaticMeshDerivedDataKey(Baseline);

	auto ExpectChanged = [&](auto Mutate)
	{
		Durin::FStaticMeshDerivedDataKeyInput Changed = Baseline;
		Mutate(Changed);
		EXPECT_NE(Durin::BuildStaticMeshDerivedDataKey(Changed), BaselineKey);
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
