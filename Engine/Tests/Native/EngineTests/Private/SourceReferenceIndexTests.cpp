#include "Source/SourceReferenceIndex.h"
#include "Texture/TextureTestSupport.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture2DSourceTranslation.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "AssetForgeAuthoringTestSupport.h"
#include "AssetForgeProviderTestFixture.h"

namespace
{
	class FScopedTextureBuildHost final
	{
	public:
		~FScopedTextureBuildHost()
		{
			Durin::Asset::Build::ShutdownBuildHost();
			Durin::Asset::Build::ShutdownTextureBuildService();
		}
	};
}

TEST(FSourceReferenceIndexTests, TracksSharedMountedSourcesAcrossRegistryRevisions)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::Tests::InstallAssetForgeAuthoringFeatures());
	Durin::Tests::FScopedAssetForgeProviders Providers;
	std::string ProviderError;
	ASSERT_TRUE(Providers.Register(ProviderError)) << ProviderError;
	FScopedTextureBuildHost BuildHost;
	ASSERT_TRUE(EnsureTextureBuildHost());
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SourceReferenceIndexCache");
	const std::filesystem::path Input =
		Durin::Testing::GetTestWorkDirectory() / "SourceReferenceIndex.png";
	WriteTextureFixture(Input);

	const Durin::FTexture2DImportResult First = Durin::Asset::Forge::ImportTexture2DAsset(
		Input.generic_string(), "/TextureImportTests/SourceIndex/First");
	ASSERT_TRUE(First) << First.Message;
	ASSERT_NE(First.Asset, nullptr);
	const std::string SourceVirtualPath = First.Asset->GetSourceFile();
	const Durin::FTextureSourceDiagnostic Source = First.Asset->InspectSource();
	ASSERT_EQ(Source.Status, Durin::ETextureSourceStatus::Available);

	const Durin::FTexture2DImportResult Second = Durin::Asset::Forge::ImportTexture2DAsset(
		Source.PhysicalPath, "/TextureImportTests/SourceIndex/Second");
	ASSERT_TRUE(Second) << Second.Message;
	ASSERT_EQ(Second.Asset->GetSourceFile(), SourceVirtualPath);

	Durin::Editor::FSourceReferenceIndex Index;
	Index.Refresh();
	EXPECT_EQ(Index.FindReferences(SourceVirtualPath).size(), 2u);
	const uint64 FirstRevision = Index.GetRegistryRevision();

	const Durin::FTexture2DImportResult Third = Durin::Asset::Forge::ImportTexture2DAsset(
		Source.PhysicalPath, "/TextureImportTests/SourceIndex/Third");
	ASSERT_TRUE(Third) << Third.Message;
	ASSERT_EQ(Third.Asset->GetSourceFile(), SourceVirtualPath);
	Index.Refresh();
	EXPECT_GT(Index.GetRegistryRevision(), FirstRevision);
	EXPECT_EQ(Index.FindReferences(SourceVirtualPath).size(), 3u);
}

TEST(FSourceReferenceIndexTests, BoundsPackageInspectionWork)
{
	InitializeDObjectSystem();
	Durin::Editor::FSourceReferenceIndex Index;
	Index.Refresh(1);
	EXPECT_LE(Index.GetInspectedPackageCount(), 1u);
	const size_t SourceBearingAssets = std::ranges::count_if(
		Durin::Asset::CaptureAssetCatalogSnapshot().Assets,
		[](const auto& Entry) {
			const std::string& ClassName = Entry.second.AssetClassName;
			return ClassName.find("Texture2D") != std::string::npos
				|| ClassName.find("TextureCube") != std::string::npos
				|| ClassName.find("StaticMesh") != std::string::npos;
		});
	if (SourceBearingAssets > 1)
		EXPECT_FALSE(Index.GetWarning().empty());
}

TEST(FSourceReferenceIndexTests, PublishesOneSharedAsynchronousSnapshot)
{
	InitializeDObjectSystem();
	Durin::Editor::FSourceReferenceIndex First;
	First.Invalidate();
	First.RequestRefresh();
	EXPECT_FALSE(First.IsCurrent());

	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!First.IsCurrent() && std::chrono::steady_clock::now() < Deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		First.RequestRefresh();
	}
	ASSERT_TRUE(First.IsCurrent());

	Durin::Editor::FSourceReferenceIndex Second;
	Second.RequestRefresh();
	EXPECT_TRUE(Second.IsCurrent());
	EXPECT_EQ(Second.GetRegistryRevision(), First.GetRegistryRevision());
	EXPECT_EQ(Second.GetInspectedPackageCount(), First.GetInspectedPackageCount());
}

#include "TextureAuthoringTestEnvironment.h"
