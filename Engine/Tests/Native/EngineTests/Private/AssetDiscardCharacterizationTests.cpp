#include "Asset/Asset.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/PackageSerialization.h"
#include "Components/VolumetricCloudComponent.h"
#include "DObject/StrongObjectPtr.h"
#include "Editor/WorkspaceRootWindow.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Texture/Texture2D.h"
#include "Texture/VolumeTexture.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

namespace
{
	// Captures the pre-reload content bug without admitting it as feature behavior.
	// Stage 4 must remove EXPECT_NONFATAL_FAILURE and retain the inner assertions.
	class FAssetDiscardCharacterizationTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
			Durin::Testing::InitializeDObjectSystemForTests();
			Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
			ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
			const auto Root = Durin::Testing::CreateTestFixtureDirectory("AssetDiscard");
			Durin::Testing::RegisterMountPointForTests(
				"/AssetDiscardTests/", Root.generic_string() + "/");
			Cloud = Durin::TStrongObjectPtr<Durin::DVolumetricCloudComponent>(
				Durin::NewObject<Durin::DVolumetricCloudComponent>(nullptr, "DiscardConsumer"));
		}

		auto TearDown() -> void override
		{
			Cloud.Reset();
			Durin::ShutdownAssetCompilingManager();
			Durin::CollectGarbage();
			Durin::ShutdownTaskScheduler();
		}

		template<typename T>
		auto VerifyDiscard(T* Texture) -> void
		{
			const auto Path = Texture->GetPackage()->GetPackagePathIdentity();
			const auto SavedIdentity = Texture->GetSource().GetIdentity();
			ASSERT_TRUE(Durin::SavePackage(Texture->GetPackage()));
			// Establish that the expected baseline really survives a fresh load.
			ASSERT_TRUE(Durin::UnloadPackage(Path));
			Texture = nullptr;
			ASSERT_TRUE(Durin::LoadObject(
				Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Texture));
			ASSERT_EQ(Texture->GetSource().GetIdentity(), SavedIdentity);
			if constexpr (std::is_same_v<T, Durin::DTexture2D>) Cloud->SetWeatherTexture(Texture);
			else Cloud->SetBaseDensityTexture(Texture);

			Durin::Editor::FEditableAssetDocumentModel Documents;
			ASSERT_TRUE(Documents.Activate({.Id = {1}, .ResourceId = Path.ToString()}, Texture));
			std::string Error;
			ASSERT_TRUE(SetSource(*Texture, std::byte{91}, Error)) << Error;
			Texture->GetPackage()->MarkDirty();
			ASSERT_NE(Texture->GetSource().GetIdentity(), SavedIdentity);
			if constexpr (std::is_same_v<T, Durin::DTexture2D>)
			{
				ASSERT_TRUE(Documents.Discard(Texture, [Texture] {
					Durin::FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Texture);
				}));
			}
			else ASSERT_TRUE(Documents.Discard(Texture));

			T* Referenced = nullptr;
			if constexpr (std::is_same_v<T, Durin::DTexture2D>) Referenced = Cloud->GetWeatherTexture();
			else Referenced = Cloud->GetBaseDensityTexture();
			ASSERT_NE(Referenced, nullptr);
			EXPECT_FALSE(Referenced->GetPackage()->IsDirty());
			EXPECT_NONFATAL_FAILURE(
				EXPECT_EQ(Referenced->GetSource().GetIdentity(), SavedIdentity), "SavedIdentity");

			ASSERT_TRUE(Durin::SavePackage(Referenced->GetPackage()));
			Cloud->SetWeatherTexture(nullptr);
			Cloud->SetBaseDensityTexture(nullptr);
			ASSERT_TRUE(Durin::UnloadPackage(Path));
			Texture = nullptr;
			ASSERT_TRUE(Durin::LoadObject(
				Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Texture));
			EXPECT_NONFATAL_FAILURE(
				EXPECT_EQ(Texture->GetSource().GetIdentity(), SavedIdentity), "SavedIdentity");
			ASSERT_TRUE(Durin::UnloadPackage(Path));
		}

		static auto SetSource(Durin::DTexture2D& Texture, std::byte Value,
			std::string& Error) -> bool
		{
			return Texture.SetSourceData(Durin::FTexture2DImportedData(Durin::FTextureSourceData{
				.Pixels = Durin::FByteBuffer(16, Value), .Width = 2, .Height = 2,
				.SourceChannelCount = 4, .Format = Durin::ETextureSourceFormat::RGBA8}), Error);
		}

		static auto SetSource(Durin::DVolumeTexture& Texture, std::byte Value,
			std::string& Error) -> bool
		{
			Durin::FVolumeTextureSourceData Source{
				.Width = 2, .Height = 2, .Depth = 2,
				.Format = Durin::EVolumeTextureFormat::R8_UNORM};
			return Source.SetVoxelBytes(Durin::FByteBuffer(8, Value))
				&& Texture.SetSourceData(Source, Error);
		}

		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::TStrongObjectPtr<Durin::DVolumetricCloudComponent> Cloud;
	};
}

TEST_F(FAssetDiscardCharacterizationTests, Texture2DDiscardLeavesSceneSourceAndPollutesNextSave)
{
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/AssetDiscardTests/Texture", Path));
	Durin::DTexture2D* Texture = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Texture));
	std::string Error;
	ASSERT_TRUE(SetSource(*Texture, std::byte{17}, Error)) << Error;
	VerifyDiscard(Texture);
}

TEST_F(FAssetDiscardCharacterizationTests, VolumeDiscardLeavesSceneSourceAndPollutesNextSave)
{
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/AssetDiscardTests/Volume", Path));
	Durin::DVolumeTexture* Texture = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Texture));
	std::string Error;
	ASSERT_TRUE(SetSource(*Texture, std::byte{17}, Error)) << Error;
	VerifyDiscard(Texture);
}
