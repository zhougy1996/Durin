#include "PropertyEditor/DetailLayoutBuilder.h"
#include "PropertyEditor/DetailPanel.h"
#include "TextureBuildSettingsDetails.h"
#include "MaterialParameterCollectionDetails.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureSource.h"
#include "Image/Image.h"
#include "Materials/MaterialParameterCollection.h"
#include "DObject/Class.h"
#include "DObject/Property.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "EngineTestSupport.h"
#include "MonaImGui.h"
#include "Widgets/MonaImGuiPropertyTable.h"

#include <gtest/gtest.h>

TEST(FAssetDetailCustomizationTests, PreservesTextureControlsAndCollectionDeclarations)
{
	InitializeDObjectSystem();
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "BuildDetailsTexture");
	auto* Collection = Durin::NewObject<Durin::DMaterialParameterCollection>(nullptr, "DetailsCollection");
	Durin::Editor::FDetailLayoutBuilder TextureLayout;
	TextureLayout.ApplyCustomization(Texture, *Durin::Editor::Texture::CreateTextureBuildSettingsDetails());
	EXPECT_EQ(TextureLayout.GetVisibleRowCount(), 7u);
	EXPECT_TRUE(TextureLayout.IsReplacingDefaultProperties());
	for (const char* Name : {"Usage", "bSRGB", "MaxResolution", "CompressionQuality", "AlphaMipMode", "AlphaCoverageThreshold"})
	{
		auto* Property = Texture->GetClass()->FindPropertyByName(Name);
		ASSERT_NE(Property, nullptr);
		EXPECT_FALSE(TextureLayout.IsPropertyHidden(*Property));
		EXPECT_TRUE(TextureLayout.IsDefaultPropertyPlaced(Texture, *Property, 0));
	}
	Durin::Editor::FDetailLayoutBuilder Search("Alpha Mip Mode");
	Search.ApplyCustomization(Texture, *Durin::Editor::Texture::CreateTextureBuildSettingsDetails());
	EXPECT_EQ(Search.GetVisibleRowCount(), 1u);
	Durin::Editor::FDetailLayoutBuilder CollectionLayout;
	CollectionLayout.ApplyCustomization(Collection, *Durin::Editor::Material::CreateMaterialParameterCollectionDetails());
	EXPECT_EQ(CollectionLayout.GetVisibleRowCount(), 1u);
	EXPECT_FALSE(CollectionLayout.IsReplacingDefaultProperties());
	EXPECT_FALSE(CollectionLayout.IsPropertyHidden(*Collection->GetClass()->FindPropertyByName("Declarations")));
	EXPECT_TRUE(CollectionLayout.IsDefaultPropertyPlaced(Collection, *Collection->GetClass()->FindPropertyByName("Declarations"), 0));
	Durin::MarkObjectHierarchyAsGarbage(Texture);
	Durin::MarkObjectHierarchyAsGarbage(Collection);
	Durin::CollectGarbage();
}

TEST(FAssetDetailCustomizationTests, PreservesPreviewCancellationAndAssetUndoRedo)
{
	InitializeDObjectSystem();
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "UndoBuildDetailsTexture");
	// Undo deserializes the authored texture, including its bulk-data identity.
	auto Image = Durin::Image::FImage::TryCreate({.Width = 2, .Height = 2,
		.Format = Durin::Image::ERawImageFormat::RGBA8}, Durin::FByteBuffer(16, std::byte{42}));
	ASSERT_TRUE(Image);
	Durin::FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Image->GetView(), 4, 0, Durin::ETextureSourceCompression::Raw));
	Texture->SetSource(std::move(Source));
	auto* Collection = Durin::NewObject<Durin::DMaterialParameterCollection>(nullptr, "UndoDetailsCollection");
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	std::string Error;
	Durin::Editor::FPropertyViewContext Context{.ReportError = [&](std::string Message) { Error = Message; }};
	Durin::Editor::FPropertyView View;
	ASSERT_TRUE(View.HandleOwnerContext(Context, Texture));
	Durin::Editor::FDetailLayoutBuilder TextureLayout;
	TextureLayout.ApplyCustomization(Texture, *Durin::Editor::Texture::CreateTextureBuildSettingsDetails());
	auto* Resolution = Texture->GetClass()->FindPropertyByName("MaxResolution");
	ASSERT_NE(Resolution, nullptr);
	const auto Target = Durin::Editor::FPropertyEditTarget::ForMember(Texture, Resolution);
	const uint32 Original = Texture->GetMaxResolution();
	const auto AssignResolution = [](Durin::FProperty* Property, void* Container, uint32 Index) {
		*Property->ContainerPtrToValuePtr<uint32>(Container, Index) = 512;
	};
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, Target, AssignResolution, true)) << Error;
	EXPECT_EQ(Texture->GetMaxResolution(), 512u);
	ASSERT_TRUE(View.FinishActiveEdit(&Context, true));
	EXPECT_EQ(Texture->GetMaxResolution(), Original);
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, Target, AssignResolution, false)) << Error;
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Texture->GetMaxResolution(), Original);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Texture->GetMaxResolution(), 512u);
	ASSERT_TRUE(View.HandleOwnerContext(Context, Collection));
	Durin::Editor::FDetailLayoutBuilder CollectionLayout;
	CollectionLayout.ApplyCustomization(Collection, *Durin::Editor::Material::CreateMaterialParameterCollectionDetails());
	auto* Declarations = Collection->GetClass()->FindPropertyByName("Declarations");
	ASSERT_NE(Declarations, nullptr);
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, Durin::Editor::FPropertyEditTarget::ForMember(Collection, Declarations),
		[](Durin::FProperty* Property, void* Container, uint32 Index) {
			*Property->ContainerPtrToValuePtr<std::vector<Durin::FMaterialParameterCollectionDeclaration>>(Container, Index) = {
				{.Id = Durin::FGuid::NewGuid(), .Name = Durin::FName("Amount"), .Type = Durin::EMaterialParameterType::Scalar}};
		}, false)) << Error;
	EXPECT_EQ(Collection->GetDeclarations().size(), 1u);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Collection->GetDeclarations().empty());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Collection->GetDeclarations().size(), 1u);
	EXPECT_TRUE(Collection->Validate());
	ASSERT_TRUE(Transactions->TryReset());
	Durin::MarkObjectHierarchyAsGarbage(Texture);
	Durin::MarkObjectHierarchyAsGarbage(Collection);
	Durin::CollectGarbage();
}

TEST(FAssetDetailCustomizationTests, DrawsBothLayoutsThroughTheSharedPropertyTable)
{
	InitializeDObjectSystem();
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "DrawBuildDetailsTexture");
	auto* Collection = Durin::NewObject<Durin::DMaterialParameterCollection>(nullptr, "DrawDetailsCollection");
	ImGuiContext* ImContext = ImGui::CreateContext();
	ASSERT_NE(ImContext, nullptr);
	ImGui::GetIO().DisplaySize = {1000.0f, 800.0f};
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
	ImGui::GetIO().Fonts->Build();
	ImGui::NewFrame();
	ImGui::SetNextWindowSize({800.0f, 700.0f});
	ImGui::Begin("Asset Layout Rendering Test");
	Durin::Editor::FPropertyView View;
	const auto TextureResult = Durin::Editor::DrawDetailPanel(View, {}, Texture, {
		.PropertyTableId = "TextureAssetDetailsRows",
		.ExtendLayout = [&](auto& Layout) { Layout.ApplyCustomization(Texture, *Durin::Editor::Texture::CreateTextureBuildSettingsDetails()); }});
	EXPECT_TRUE(TextureResult.bOwnerReady);
	EXPECT_TRUE(TextureResult.bTableDrawn);
	EXPECT_EQ(TextureResult.VisibleRowCount, 7u);
	const auto CollectionResult = Durin::Editor::DrawDetailPanel(View, {}, Collection, {
		.PropertyTableId = "CollectionAssetDetailsRows",
		.ExtendLayout = [&](auto& Layout) { Layout.ApplyCustomization(Collection, *Durin::Editor::Material::CreateMaterialParameterCollectionDetails()); }});
	EXPECT_TRUE(CollectionResult.bOwnerReady);
	EXPECT_TRUE(CollectionResult.bTableDrawn);
	EXPECT_EQ(CollectionResult.VisibleRowCount, 1u);
	ImGui::End();
	ImGui::Render();
	ImGui::DestroyContext(ImContext);
	Durin::MarkObjectHierarchyAsGarbage(Texture);
	Durin::MarkObjectHierarchyAsGarbage(Collection);
	Durin::CollectGarbage();
}
