#include "EngineTestSupport.h"
#include "AssetSystem.h"
#include "EnvironmentLighting/EnvironmentLighting.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeEnvironmentLightingFixture() -> Durin::FEnvironmentLightingData
	{
		Durin::FEnvironmentLightingData Data;
		const size_t IrradianceElements =
			static_cast<size_t>(Durin::EnvironmentIrradianceDimension)
			* Durin::EnvironmentIrradianceDimension * 4;
		for (size_t Face = 0; Face < Data.Irradiance.size(); ++Face)
			Data.Irradiance[Face].assign(IrradianceElements, static_cast<Durin::uint16>(Face + 1));
		for (Durin::uint32 Mip = 0; Mip < Durin::EnvironmentPrefilterMipCount; ++Mip)
		{
			const size_t Dimension = Durin::EnvironmentPrefilterDimension >> Mip;
			for (size_t Face = 0; Face < Data.Prefiltered[Mip].size(); ++Face)
			{
				Data.Prefiltered[Mip][Face].assign(
					Dimension * Dimension * 4,
					static_cast<Durin::uint16>(100 + Mip * 10 + Face));
			}
		}
		Data.BrdfLut.assign(
			static_cast<size_t>(Durin::EnvironmentBrdfLutDimension)
				* Durin::EnvironmentBrdfLutDimension * 4,
			555);
		return Data;
	}
}

TEST(FEnvironmentLightingTests, PayloadRoundTripsDeterministicallyAndRejectsCorruption)
{
	const Durin::FEnvironmentLightingData Expected = MakeEnvironmentLightingFixture();
	ASSERT_TRUE(Expected.IsValid());
	std::vector<Durin::uint8> First;
	std::vector<Durin::uint8> Second;
	std::string Error;
	ASSERT_TRUE(Durin::EncodeEnvironmentLightingPayload(Expected, First, Error)) << Error;
	ASSERT_TRUE(Durin::EncodeEnvironmentLightingPayload(Expected, Second, Error)) << Error;
	EXPECT_EQ(First, Second);

	std::shared_ptr<const Durin::FEnvironmentLightingData> Decoded;
	ASSERT_TRUE(Durin::DecodeEnvironmentLightingPayload(First, Decoded, Error)) << Error;
	ASSERT_NE(Decoded, nullptr);
	EXPECT_EQ(*Decoded, Expected);

	First.back() ^= 0x80;
	EXPECT_FALSE(Durin::DecodeEnvironmentLightingPayload(First, Decoded, Error));
	EXPECT_EQ(Decoded, nullptr);
}

TEST(FEnvironmentLightingTests, CheckedInStudioPayloadIsValid)
{
	const std::filesystem::path PayloadPath =
		std::filesystem::path(Durin::FPaths::EngineContentDir())
		/ "Renderer/DefaultStudioEnvironment.iblbulk";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, PayloadPath.generic_string()));
	std::shared_ptr<const Durin::FEnvironmentLightingData> Data;
	std::string Error;
	ASSERT_TRUE(Durin::DecodeEnvironmentLightingPayload(Bytes, Data, Error)) << Error;
	ASSERT_NE(Data, nullptr);
	EXPECT_TRUE(Data->IsValid());
}

TEST(FEnvironmentLightingTests, AssetCooksAuthoringPayloadDirectlyWithoutDdc)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "EnvironmentLightingCook";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(Root / "Content");
	Durin::PathUtilities::RegisterMountPointForTests(
		"/EnvironmentLightingCook/", (Root / "Content").generic_string() + "/");

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/EnvironmentLightingCook/StudioEnvironment", AssetPath));
	Durin::DEnvironmentLighting* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(AssetPath, Asset));
	ASSERT_NE(Asset, nullptr);

	std::vector<Durin::uint8> SourceBytes;
	std::string Error;
	ASSERT_TRUE(Durin::EncodeEnvironmentLightingPayload(
		MakeEnvironmentLightingFixture(), SourceBytes, Error)) << Error;
	ASSERT_TRUE(Durin::DerivedDataCache::WriteFileAtomically(
		Durin::DEnvironmentLighting::GetAuthoringPayloadPath(AssetPath.ToString()),
		SourceBytes,
		&Error)) << Error;

	const std::filesystem::path CookRoot = std::filesystem::absolute(Root / "Cook");
	Durin::Asset::FCookContext Context(
		CookRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Asset->AddToCook(Context, "/Game/StudioEnvironment", Error)) << Error;
	ASSERT_TRUE(Context.Publish(&Error)) << Error;

	Durin::Asset::FCookedBulkContainer Container;
	ASSERT_TRUE(Durin::Asset::LoadCookedBulkFile(
		CookRoot / "Game/StudioEnvironment.dbulk",
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game,
		Container,
		&Error)) << Error;
	ASSERT_EQ(Container.Entries.size(), 1);
	std::span<const Durin::uint8> CookedBytes;
	ASSERT_TRUE(Durin::Asset::ResolveCookedPayload(
		Container, Container.Entries.front(), CookedBytes, &Error)) << Error;
	EXPECT_EQ(std::vector<Durin::uint8>(CookedBytes.begin(), CookedBytes.end()), SourceBytes);
}
