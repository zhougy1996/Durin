#include "Source/SourceReferenceIndex.h"
#include "Texture/TextureTestSupport.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"

namespace
{
	auto FindImportedSource(const Durin::DTexture2D& Texture)
		-> const Durin::FSourceFile*
	{
		const Durin::DAssetImportData* ImportData =
			Texture.GetAssetImportData();
		return ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
	}

	class FScopedTextureCompilingManager final
	{
	public:
		~FScopedTextureCompilingManager()
		{
			Durin::ShutdownAssetCompilingManager();
		}
	};
}

TEST(FSourceReferenceIndexTests, TracksSharedSourceFilesAcrossRegistryRevisions)
{
	InitializeDObjectSystem();
	FScopedTextureCompilingManager CompilingManager;
	ASSERT_TRUE(EnsureTextureCompilingManager());
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SourceReferenceIndexCache");
	const std::filesystem::path Input =
		Durin::Testing::GetTestWorkDirectory() / "SourceReferenceIndex.png";
	WriteTextureFixture(Input);

	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> First = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Input.generic_string(), "/TextureImportTests/SourceIndex/First");
	ASSERT_TRUE(First) << First.Message;
	ASSERT_NE(First.Asset, nullptr);
	const Durin::FSourceFile* FirstSource = FindImportedSource(*First.Asset);
	ASSERT_NE(FirstSource, nullptr);
	const std::string SourceVirtualPath = FirstSource->Hint;

	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Second = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Input.generic_string(), "/TextureImportTests/SourceIndex/Second");
	ASSERT_TRUE(Second) << Second.Message;
	const Durin::FSourceFile* SecondSource = FindImportedSource(*Second.Asset);
	ASSERT_NE(SecondSource, nullptr);
	ASSERT_EQ(SecondSource->Hint, SourceVirtualPath);

	Durin::Editor::FSourceReferenceIndex Index;
	Index.Refresh();
	EXPECT_EQ(Index.FindReferences(SourceVirtualPath).size(), 2u);
	const uint64 FirstRevision = Index.GetRegistryRevision();

	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Third = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Input.generic_string(), "/TextureImportTests/SourceIndex/Third");
	ASSERT_TRUE(Third) << Third.Message;
	const Durin::FSourceFile* ThirdSource = FindImportedSource(*Third.Asset);
	ASSERT_NE(ThirdSource, nullptr);
	ASSERT_EQ(ThirdSource->Hint, SourceVirtualPath);
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
		Durin::CaptureAssetCatalogSnapshot().Assets,
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

#include "TextureAssetTestEnvironment.h"
