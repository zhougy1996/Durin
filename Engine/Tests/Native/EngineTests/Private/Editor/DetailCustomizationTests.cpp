#include "PropertyEditor/DetailLayoutBuilder.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Texture/Texture2D.h"
#include "EngineTestSupport.h"
#include "DObject/DurinPropertyTypes.h"
#include "Components/SceneComponent.h"
#include "PropertyEditor/DetailPanel.h"
#include "MonaImGui.h"
#include "Widgets/MonaImGuiPropertyTable.h"

#include <gtest/gtest.h>

namespace
{
	// Records asset dispatch through the host-independent customization interface.
	class FAssetDetails final : public Durin::Editor::IDetailCustomization
	{
	public:
		explicit FAssetDetails(bool bAddProperty = false) : bAddProperty(bAddProperty) {}
		auto CustomizeDetails(Durin::Editor::IDetailLayoutBuilder& Layout) -> void override
		{
			Object = Layout.GetObject();
			Host = Layout.GetHostContext();
			if (bAddProperty)
				Layout.EditCategory("Texture", "Texture Settings").AddProperty(Object,
					Object->GetClass()->FindPropertyByName("bSRGB"));
			else Layout.AddCustomRow("Asset", [](auto&, const auto&) { return false; });
		}
		bool bAddProperty;
		Durin::DObject* Object = nullptr;
		Durin::Editor::IDetailHostContext* Host = nullptr;
	};

	// Unregisters the shared entry even when a test assertion exits early.
	struct FRegistration
	{
		Durin::Editor::FDetailCustomizationHandle Handle;
		~FRegistration() { if (Handle) Durin::Editor::FDetailCustomizationRegistry::Get().Unregister(Handle); }
	};
}

TEST(FSharedDetailCustomizationTests, CustomizesTextureAssetWithoutLevelEditorHost)
{
	InitializeDObjectSystem();
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "SharedDetailsTexture");
	ASSERT_NE(Texture, nullptr);
	auto* Property = Texture->GetClass()->FindPropertyByName("bSRGB");
	ASSERT_NE(Property, nullptr);
	auto Details = std::make_shared<FAssetDetails>(true);
	FRegistration Registration{Durin::Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(Texture->GetClass(), Details)};
	ASSERT_TRUE(Registration.Handle);
	Durin::Editor::FDetailLayoutBuilder Layout("Texture Settings");
	Layout.ApplyRegisteredCustomizations(Texture);
	EXPECT_EQ(Details->Object, Texture);
	EXPECT_EQ(Details->Host, nullptr);
	EXPECT_EQ(Layout.GetVisibleRowCount(), 1u);
	EXPECT_FALSE(Layout.IsPropertyHidden(*Property));
	EXPECT_TRUE(Layout.IsDefaultPropertyPlaced(Texture, *Property, 0));
	Durin::MarkObjectHierarchyAsGarbage(Texture);
	Durin::CollectGarbage();
}

TEST(FSharedDetailCustomizationTests, ResolvesAssetInheritanceAndRejectsStaleHandles)
{
	InitializeDObjectSystem();
	auto& Registry = Durin::Editor::FDetailCustomizationRegistry::Get();
	auto Base = std::make_shared<FAssetDetails>();
	auto Derived = std::make_shared<FAssetDetails>();
	FRegistration BaseRegistration{Registry.RegisterDetailCustomization(Durin::DObject::StaticClass(), Base)};
	FRegistration DerivedRegistration{Registry.RegisterDetailCustomization(Durin::DTexture2D::StaticClass(), Derived)};
	ASSERT_TRUE(BaseRegistration.Handle);
	ASSERT_TRUE(DerivedRegistration.Handle);
	EXPECT_FALSE(Registry.RegisterDetailCustomization(Durin::DTexture2D::StaticClass(), Derived));
	const auto Chain = Registry.FindDetailCustomizations(Durin::DTexture2D::StaticClass());
	ASSERT_EQ(Chain.size(), 2u);
	EXPECT_EQ(Chain[0], Base);
	EXPECT_EQ(Chain[1], Derived);
	EXPECT_EQ(Registry.FindDetailCustomization(Durin::DTexture2D::StaticClass()), Derived);
	ASSERT_TRUE(Registry.Unregister(DerivedRegistration.Handle));
	EXPECT_FALSE(Registry.Unregister(DerivedRegistration.Handle));
	DerivedRegistration.Handle = {};
	EXPECT_EQ(Registry.FindDetailCustomization(Durin::DTexture2D::StaticClass()), Base);
}

TEST(FSharedDetailCustomizationTests, DeduplicatesOnlyThePlacedObjectPropertyAndArrayIndex)
{
	InitializeDObjectSystem();
	auto* First = Durin::NewObject<Durin::DTexture2D>(nullptr, "FirstLayoutObject");
	auto* Second = Durin::NewObject<Durin::DTexture2D>(nullptr, "SecondLayoutObject");
	// A synthetic descriptor exercises fixed-array placement without touching object storage.
	Durin::FNumericProperty Values(Durin::FFieldVariant(First->GetClass()), Durin::FName("Values"),
		Durin::EObjectFlags::Transient, Durin::EPropertyFlags::Edit, 3, 0, sizeof(int32),
		Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
	Durin::Editor::FDetailLayoutBuilder Layout("unmatched-search");
	Layout.EditCategory("Curated").AddProperty(First, &Values, 1);
	EXPECT_FALSE(Layout.IsPropertyHidden(Values));
	EXPECT_TRUE(Layout.ShouldDrawDefaultProperty(First, Values, 0));
	EXPECT_FALSE(Layout.ShouldDrawDefaultProperty(First, Values, 1));
	EXPECT_TRUE(Layout.ShouldDrawDefaultProperty(First, Values, 2));
	EXPECT_TRUE(Layout.ShouldDrawDefaultProperty(Second, Values, 1));
	EXPECT_EQ(Layout.GetVisibleRowCount(), 0u);
	Layout.HideCategory("Curated");
	EXPECT_FALSE(Layout.ShouldDrawDefaultProperty(First, Values, 1));
	Layout.HideProperty(&Values);
	EXPECT_FALSE(Layout.ShouldDrawDefaultProperty(First, Values, 0));
	EXPECT_FALSE(Layout.ShouldDrawDefaultProperty(Second, Values, 1));
	Durin::MarkObjectHierarchyAsGarbage(First);
	Durin::MarkObjectHierarchyAsGarbage(Second);
	Durin::CollectGarbage();
}

TEST(FSharedDetailCustomizationTests, SharedPanelKeepsSearchAndCrossObjectDefaultRows)
{
	InitializeDObjectSystem();
	auto* First = Durin::NewObject<Durin::DSceneComponent>(nullptr, "SharedPanelFirst");
	auto* Second = Durin::NewObject<Durin::DSceneComponent>(nullptr, "SharedPanelSecond");
	auto* Transform = First->GetClass()->FindPropertyByName("RelativeTransform");
	ASSERT_NE(Transform, nullptr);
	ImGuiContext* ImContext = ImGui::CreateContext();
	ASSERT_NE(ImContext, nullptr);
	ImGui::GetIO().DisplaySize = {1000.0f, 800.0f};
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
	ImGui::GetIO().Fonts->Build();
	ImGui::NewFrame();
	ImGui::SetNextWindowSize({800.0f, 700.0f});
	ImGui::Begin("Shared Details Panel Test");
	Durin::Editor::FPropertyView View;
	const auto Baseline = Durin::Editor::DrawDetailPanel(View, {}, First,
		{.SearchText = "RelativeTransform", .PropertyTableId = "Baseline"});
	EXPECT_TRUE(Baseline.bOwnerReady);
	EXPECT_TRUE(Baseline.bTableDrawn);
	EXPECT_EQ(Baseline.VisibleRowCount, 1u);
	const auto Placed = Durin::Editor::DrawDetailPanel(View, {}, First, {
		.SearchText = "RelativeTransform", .PropertyTableId = "Placed",
		.ExtendLayout = [&](auto& Layout) { Layout.EditCategory("Curated").AddProperty(First, Transform); }});
	EXPECT_EQ(Placed.VisibleRowCount, 1u);
	const auto External = Durin::Editor::DrawDetailPanel(View, {}, First, {
		.SearchText = "RelativeTransform", .PropertyTableId = "External",
		.ExtendLayout = [&](auto& Layout) { Layout.AddProperty(Second, Transform); }});
	EXPECT_EQ(External.VisibleRowCount, 2u);
	const auto Hidden = Durin::Editor::DrawDetailPanel(View, {}, First, {
		.SearchText = "RelativeTransform", .PropertyTableId = "Hidden",
		.ExtendLayout = [&](auto& Layout) { Layout.HideProperty(Transform); }});
	EXPECT_EQ(Hidden.VisibleRowCount, 0u);
	ImGui::End();
	ImGui::Render();
	ImGui::DestroyContext(ImContext);
	Durin::MarkObjectHierarchyAsGarbage(First);
	Durin::MarkObjectHierarchyAsGarbage(Second);
	Durin::CollectGarbage();
}
