#include "Source/MountedSourceRelocation.h"
#include "Source/SourceReferenceIndex.h"
#include "Texture/TextureTestSupport.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture2DSourceTranslation.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

TEST(FSourceReferenceIndexTests, TracksSharedMountedSourcesAcrossRegistryRevisions)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SourceReferenceIndexCache");
	const std::filesystem::path Input =
		Durin::Testing::GetTestWorkDirectory() / "SourceReferenceIndex.png";
	WriteTextureFixture(Input);

	const Durin::FTexture2DImportResult First = Durin::Asset::Import::ImportTexture2DAsset(
		Input.generic_string(), "/TextureImportTests/SourceIndex/First");
	ASSERT_TRUE(First) << First.Message;
	ASSERT_NE(First.Asset, nullptr);
	const std::string SourceVirtualPath = First.Asset->GetSourceFile();
	const Durin::FTextureSourceDiagnostic Source = First.Asset->InspectSource();
	ASSERT_EQ(Source.Status, Durin::ETextureSourceStatus::Available);

	const Durin::FTexture2DImportResult Second = Durin::Asset::Import::ImportTexture2DAsset(
		Source.PhysicalPath, "/TextureImportTests/SourceIndex/Second");
	ASSERT_TRUE(Second) << Second.Message;
	ASSERT_EQ(Second.Asset->GetSourceFile(), SourceVirtualPath);

	Durin::Editor::FSourceReferenceIndex Index;
	Index.Refresh();
	EXPECT_EQ(Index.FindReferences(SourceVirtualPath).size(), 2u);
	const Durin::uint64 FirstRevision = Index.GetRegistryRevision();

	const Durin::FTexture2DImportResult Third = Durin::Asset::Import::ImportTexture2DAsset(
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
		Durin::Asset::GetAssetRegistry().GetAssets(),
		[](const auto& Entry) {
			const std::string& ClassName = Entry.second.AssetClassName;
			return ClassName.find("Texture2D") != std::string::npos
				|| ClassName.find("TextureCube") != std::string::npos
				|| ClassName.find("StaticMesh") != std::string::npos;
		});
	if (SourceBearingAssets > 1)
		EXPECT_FALSE(Index.GetWarning().empty());
}

TEST(FSourceReferenceIndexTests, RelocatesSharedSourceAndAllReferencingPackages)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SharedSourceRelocationCache");
	const std::filesystem::path Input =
		Durin::Testing::GetTestWorkDirectory() / "SharedSourceRelocation.png";
	WriteTextureFixture(Input);
	Durin::FTexture2DImportSettings ImportSettings;
	ImportSettings.SourceDestination =
		"/TextureImportTests/Textures/RelocationTransactionShared.png";
	const Durin::FTexture2DImportResult First = Durin::Asset::Import::ImportTexture2DAsset(
		Input.generic_string(), "/TextureImportTests/Relocation/First",
		ImportSettings);
	ASSERT_TRUE(First) << First.Message;
	const Durin::FTextureSourceDiagnostic OriginalSource =
		First.Asset->InspectSource();
	ASSERT_EQ(OriginalSource.Status, Durin::ETextureSourceStatus::Available);
	const Durin::FTexture2DImportResult Second = Durin::Asset::Import::ImportTexture2DAsset(
		OriginalSource.PhysicalPath, "/TextureImportTests/Relocation/Second");
	ASSERT_TRUE(Second) << Second.Message;

	Durin::Editor::FSourceReferenceIndex Index;
	Index.Refresh();
	const std::span<const Durin::Editor::FSourceReference> References =
		Index.FindReferences(First.Asset->GetSourceFile());
	ASSERT_EQ(References.size(), 2u);
	const std::string OriginalVirtualPath = First.Asset->GetSourceFile();
	const std::string DestinationVirtualPath =
		"/TextureImportTests/Textures/RelocatedSharedSource.png";
	std::string Error;
	Second.Asset->MarkPackageDirty();
	EXPECT_FALSE(Durin::Editor::RelocateMountedSourceAcrossPackages({
		.AuthoringAssetPath = "/TextureImportTests/Relocation/First",
		.OriginalSourceVirtualPath = OriginalVirtualPath,
		.DestinationSourceVirtualPath = DestinationVirtualPath,
		.AffectedAssets = {References.begin(), References.end()}}, Error));
	EXPECT_NE(Error.find("Save or discard"), std::string::npos);
	EXPECT_TRUE(std::filesystem::is_regular_file(OriginalSource.PhysicalPath));
	const Durin::PathUtilities::FSourcePathResult UnpublishedDestination =
		Durin::PathUtilities::ResolveSourcePath(
			DestinationVirtualPath,
			Durin::PathUtilities::EPathExistence::AllowMissing);
	ASSERT_TRUE(UnpublishedDestination) << UnpublishedDestination.Message;
	EXPECT_FALSE(std::filesystem::exists(UnpublishedDestination.PhysicalPath));
	Second.Asset->GetPackage()->ClearDirty();

	ASSERT_TRUE(Durin::Editor::RelocateMountedSourceAcrossPackages({
		.AuthoringAssetPath = "/TextureImportTests/Relocation/First",
		.OriginalSourceVirtualPath = OriginalVirtualPath,
		.DestinationSourceVirtualPath = DestinationVirtualPath,
		.AffectedAssets = {References.begin(), References.end()}}, Error)) << Error;

	EXPECT_FALSE(std::filesystem::exists(OriginalSource.PhysicalPath));
	const Durin::PathUtilities::FSourcePathResult Destination =
		Durin::PathUtilities::ResolveSourcePath(DestinationVirtualPath);
	ASSERT_TRUE(Destination) << Destination.Message;
	EXPECT_TRUE(std::filesystem::is_regular_file(Destination.PhysicalPath));
	EXPECT_EQ(First.Asset->GetSourceFile(), DestinationVirtualPath);
	EXPECT_EQ(Second.Asset->GetSourceFile(), DestinationVirtualPath);

	Index.Refresh();
	EXPECT_TRUE(Index.FindReferences(OriginalVirtualPath).empty());
	EXPECT_EQ(Index.FindReferences(DestinationVirtualPath).size(), 2u);
}
#include "TextureAuthoringTestEnvironment.h"
