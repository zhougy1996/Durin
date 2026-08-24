#include "Source/MountedSourceRelocation.h"
#include "Source/SourceReferenceIndex.h"
#include "Texture/TextureTestSupport.h"
#include "AssetForge/Builtins/Texture2DImport.h"

TEST(FTextureSourceRelocationTests, RelocatesSharedSourceAndAllReferencingPackages)
{
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SharedSourceRelocationCache");
	const std::filesystem::path Input =
		Durin::Testing::GetTestWorkDirectory() / "SharedSourceRelocation.png";
	WriteTextureFixture(Input);
	Durin::FTexture2DImportSettings ImportSettings;
	ImportSettings.SourceDestination =
		"/TextureImportTests/Textures/RelocationTransactionShared.png";
	const Durin::FTexture2DImportResult First = Durin::AssetForge::Builtins::ImportTexture2DAsset(
		Input.generic_string(), "/TextureImportTests/Relocation/First",
		ImportSettings);
	ASSERT_TRUE(First) << First.Message;
	const Durin::FTextureSourceDiagnostic OriginalSource = First.Asset->InspectSource();
	ASSERT_EQ(OriginalSource.Status, Durin::ETextureSourceStatus::Available);
	const Durin::FTexture2DImportResult Second = Durin::AssetForge::Builtins::ImportTexture2DAsset(
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
