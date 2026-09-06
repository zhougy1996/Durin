#include <gtest/gtest.h>

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Panels/ContentBrowserExtensionPresentation.h"
#include "Panels/ContentBrowserItemView.h"

namespace Durin::Editor::ContentBrowser
{
	TEST(FContentBrowserExtensionRegistryTests, OrdersByOrderThenStableId)
	{
		std::string Error;
		auto B = RegisterExtension({
			.Id = "test.order.b", .Label = "B",
			.Category = EExtensionCategory::Import, .Order = 20,
			.Mutation = ::Durin::Editor::ContentBrowser::EContentMutation::MutatesContent,
			.IsApplicable = [](const auto&) { return true; },
			.Invoke = [](const auto&) {}}, Error);
		auto A = RegisterExtension({
			.Id = "test.order.a", .Label = "A",
			.Category = EExtensionCategory::Import, .Order = 20,
			.Mutation = ::Durin::Editor::ContentBrowser::EContentMutation::MutatesContent,
			.IsApplicable = [](const auto&) { return true; },
			.Invoke = [](const auto&) {}}, Error);
		auto First = RegisterExtension({
			.Id = "test.order.first", .Label = "First",
			.Category = EExtensionCategory::Import, .Order = 10,
			.Mutation = ::Durin::Editor::ContentBrowser::EContentMutation::MutatesContent,
			.IsApplicable = [](const auto&) { return true; },
			.Invoke = [](const auto&) {}}, Error);
		ASSERT_TRUE(A.IsValid());
		ASSERT_TRUE(B.IsValid());
		ASSERT_TRUE(First.IsValid());

		const auto Snapshot = CaptureExtensions(EExtensionCategory::Import);
		const auto FindIndex = [&Snapshot](std::string_view Id) {
			return std::distance(Snapshot.begin(), std::ranges::find(
				Snapshot, Id, &FExtensionDescriptor::Id));
		};
		EXPECT_LT(FindIndex("test.order.first"), FindIndex("test.order.a"));
		EXPECT_LT(FindIndex("test.order.a"), FindIndex("test.order.b"));
	}

	TEST(FContentBrowserExtensionRegistryTests, RejectsDuplicatesAndRemovesRegistration)
	{
		int Invocations = 0;
		std::string Error;
		auto Registration = RegisterExtension({
			.Id = "test.lifetime", .Label = "Lifetime",
			.Category = EExtensionCategory::Import,
			.Mutation = ::Durin::Editor::ContentBrowser::EContentMutation::MutatesContent,
			.IsApplicable = [](const auto& Context) {
				return !Context.VirtualDirectory.empty();
			},
			.Invoke = [&Invocations](const auto&) { ++Invocations; },
			}, Error);
		ASSERT_TRUE(Registration.IsValid());
		auto Duplicate = RegisterExtension({
			.Id = "test.lifetime", .Label = "Duplicate",
			.Category = EExtensionCategory::Import,
			.Mutation = ::Durin::Editor::ContentBrowser::EContentMutation::MutatesContent,
			.IsApplicable = [](const auto&) { return true; },
			.Invoke = [](const auto&) {}}, Error);
		EXPECT_FALSE(Duplicate.IsValid());
		EXPECT_FALSE(Error.empty());

		const auto Snapshot = CaptureExtensions(EExtensionCategory::Import);
		const auto Entry = std::ranges::find(
			Snapshot, "test.lifetime", &FExtensionDescriptor::Id);
		ASSERT_NE(Entry, Snapshot.end());
		EXPECT_TRUE(InvokeExtension(*Entry,
			{.Context = {.VirtualDirectory = "/Project"}}));
		EXPECT_EQ(Invocations, 1);
		Registration.Reset();
		const auto Remaining = CaptureExtensions(EExtensionCategory::Import);
		EXPECT_EQ(std::ranges::find(Remaining, "test.lifetime", &FExtensionDescriptor::Id),
			Remaining.end());
	}

	TEST(FContentBrowserExtensionRegistryTests, RegistersInvokesAndRemovesEveryCommandCategory)
	{
		const std::array Categories{
			EExtensionCategory::Create,
			EExtensionCategory::ContextMenu,
			EExtensionCategory::Import};
		std::vector<FScopedExtensionRegistration> Registrations;
		int Invocations = 0;
		for (size_t Index = 0; Index < Categories.size(); ++Index)
		{
			std::string Error;
			const std::string Id = std::format("test.category.{}", Index);
			auto Registration = RegisterExtension({
				.Id = Id,
				.Label = "Category",
				.Category = Categories[Index],
				.Mutation = ::Durin::Editor::ContentBrowser::EContentMutation::MutatesContent,
				.IsApplicable = [](const auto&) { return true; },
				.Invoke = [&Invocations](const auto&) { ++Invocations; },
				}, Error);
			ASSERT_TRUE(Registration.IsValid()) << Error;
			const auto Snapshot = CaptureExtensions(Categories[Index]);
			const auto Entry = std::ranges::find(
				Snapshot, Id, &FExtensionDescriptor::Id);
			ASSERT_NE(Entry, Snapshot.end());
			EXPECT_TRUE(InvokeExtension(*Entry, {}));
			Registrations.push_back(std::move(Registration));
		}
		EXPECT_EQ(Invocations, static_cast<int>(Categories.size()));

		for (size_t Index = Registrations.size(); Index > 0; --Index)
			Registrations[Index - 1].Reset();
		for (size_t Index = 0; Index < Categories.size(); ++Index)
		{
			const std::string Id = std::format("test.category.{}", Index);
			const auto Snapshot = CaptureExtensions(Categories[Index]);
			EXPECT_EQ(std::ranges::find(Snapshot, Id, &FExtensionDescriptor::Id),
				Snapshot.end());
		}
	}

	TEST(FContentBrowserExtensionRegistryTests,
		RejectsMutatingInvocationsAndRemovesHostPresenters)
	{
		int Invocations = 0;
		int Presentations = 0;
		bool bLastPresentationAllowedMutation = true;
		std::string Error;
		std::vector<FScopedExtensionRegistration> Registrations;
		for (const EExtensionCategory Category : {
			EExtensionCategory::Create, EExtensionCategory::Import})
		{
			const std::string Id = Category == EExtensionCategory::Create
				? "test.mutation-policy.create"
				: "test.mutation-policy.import";
			auto Registration = RegisterExtension({
				.Id = Id,
				.Label = "Mutation Policy",
				.Category = Category,
				.Mutation = ::Durin::Editor::ContentBrowser::EContentMutation::MutatesContent,
				.IsApplicable = [](const auto&) { return true; },
				.Invoke = [&Invocations](const auto&) { ++Invocations; },
				.DrawHostPresentation = Category == EExtensionCategory::Import
					? std::function<void(bool)>{[
						&Presentations,
						&bLastPresentationAllowedMutation](bool bAllowAssetMutation) {
						++Presentations;
						bLastPresentationAllowedMutation = bAllowAssetMutation;
					}}
					: std::function<void(bool)>{},
				}, Error);
			ASSERT_TRUE(Registration.IsValid()) << Error;

			const auto Extensions = CaptureExtensions(Category);
			const auto Entry = std::ranges::find(
				Extensions, Id, &FExtensionDescriptor::Id);
			ASSERT_NE(Entry, Extensions.end());
			EXPECT_FALSE(InvokeExtension(*Entry, {.bAllowAssetMutation = false}));
			EXPECT_TRUE(InvokeExtension(*Entry, {.bAllowAssetMutation = true}));
			Registrations.push_back(std::move(Registration));
		}
		EXPECT_EQ(Invocations, 2);

		const auto Presenters = CaptureHostPresenters();
		const auto Presenter = std::ranges::find(
			Presenters, "test.mutation-policy.import", &FExtensionDescriptor::Id);
		ASSERT_NE(Presenter, Presenters.end());
		EXPECT_TRUE(DrawHostPresentation(*Presenter, false));
		EXPECT_EQ(Presentations, 1);
		EXPECT_FALSE(bLastPresentationAllowedMutation);

		Registrations.clear();
		const auto Remaining = CaptureHostPresenters();
		EXPECT_EQ(std::ranges::find(Remaining, "test.mutation-policy.import",
			&FExtensionDescriptor::Id), Remaining.end());
		EXPECT_EQ(Presentations, 1);
	}
	TEST(FContentBrowserExtensionRegistryTests, PresentsUnknownTypeThroughProductionDetailsAndMenu)
	{
		using namespace Private;
		std::string Error;
		int Invocations = 0;
		auto Type = RegisterAssetTypePresentation({
			.AssetClassName = "Plugin::DNovelAsset", .DisplayName = "Novel Asset",
			.Category = EAssetCategory::Texture, .Icon = "custom icon",
			.Details = [](const FExtensionContext& Context) {
				return std::vector<FDetailRow>{{"Type detail", Context.Selection.front().PhysicalPath}};
			}}, Error);
		ASSERT_TRUE(Type.IsValid()) << Error;
		auto Details = RegisterExtension({
			.Id = "test.details", .Label = "Details", .Category = EExtensionCategory::Details,
			.Mutation = EContentMutation::ReadOnly,
			.IsApplicable = [](const auto& Context) { return Context.Selection.size() == 1; },
			.Details = [](const auto&) { return std::vector<FDetailRow>{{"Extra", "Visible"}}; }}, Error);
		ASSERT_TRUE(Details.IsValid()) << Error;
		auto Command = RegisterExtension({
			.Id = "test.context", .Label = "Inspect", .Category = EExtensionCategory::ContextMenu,
			.Mutation = EContentMutation::ReadOnly,
			.IsApplicable = [](const auto& Context) {
				return Context.PrimaryItem && Context.PrimaryItem->AssetClassName == "Plugin::DNovelAsset";
			},
			.Invoke = [&](const auto& Invocation) {
				EXPECT_EQ(Invocation.Context.Selection.size(), 1);
				++Invocations;
			}}, Error);
		ASSERT_TRUE(Command.IsValid()) << Error;
		const std::vector<FContentBrowserItem> Items{{.Kind = EContentBrowserItemKind::Asset,
			.Name = "Example", .VirtualPath = "/Project/Example", .PhysicalPath = "/content/Example.dasset",
			.AssetClassName = "Plugin::DNovelAsset"}};
		const auto Context = BuildExtensionContext(Items, {Items.front().StableId()}, "/content", "/Project");
		const auto Rows = CaptureSelectionDetails(Context);
		ASSERT_EQ(Rows.size(), 2);
		EXPECT_EQ(Rows[0].Value, "/content/Example.dasset");
		EXPECT_EQ(Rows[1].Value, "Visible");
		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Items.front()), "Novel Asset");
		EXPECT_EQ(ContentBrowserItemView::Icon(Items.front()), "custom icon");
		int VisibleCommands = 0;
		PresentExtensionMenu(EExtensionCategory::ContextMenu, Context, false,
			[&](const FExtensionDescriptor& Entry, bool bEnabled) {
				EXPECT_EQ(Entry.Id, "test.context");
				EXPECT_TRUE(bEnabled);
				++VisibleCommands;
				return true;
			}, [&](const auto& Entry, const auto& SelectedContext) {
				EXPECT_TRUE(InvokeExtension(Entry, {.Context = SelectedContext, .bAllowAssetMutation = false}));
			});
		EXPECT_EQ(VisibleCommands, 1);
		EXPECT_EQ(Invocations, 1);
		Details.Reset();
		Type.Reset();
		Command.Reset();
		EXPECT_TRUE(CaptureSelectionDetails(Context).empty());
		PresentExtensionMenu(EExtensionCategory::ContextMenu, Context, true,
			[](const auto&, bool) { ADD_FAILURE() << "Unregistered menu entry remained visible"; return false; },
			[](const auto&, const auto&) { ADD_FAILURE(); });
	}

	TEST(FContentBrowserExtensionRegistryTests, BuildsMixedSelectionAndDirectoryDestinations)
	{
		using namespace Private;
		const std::vector<FContentBrowserItem> Items{
			{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = "/Project/Asset",
				.PhysicalPath = "/content/Asset.dasset", .AssetClassName = "Plugin::DAsset"},
			{.Kind = EContentBrowserItemKind::File, .PhysicalPath = "/content/source.png"},
			{.Kind = EContentBrowserItemKind::Folder, .VirtualPath = "/Project/Sub", .PhysicalPath = "/content/Sub"}};
		const auto Mixed = BuildExtensionContext(Items,
			{Items[0].StableId(), Items[1].StableId(), Items[2].StableId()}, "/content", "/Project", &Items[1]);
		ASSERT_EQ(Mixed.Selection.size(), 3);
		EXPECT_EQ(Mixed.Selection[0].Kind, EExtensionItemKind::Asset);
		EXPECT_EQ(Mixed.Selection[1].Kind, EExtensionItemKind::File);
		EXPECT_EQ(Mixed.Selection[2].Kind, EExtensionItemKind::Directory);
		EXPECT_EQ(Mixed.PrimaryItem->PhysicalPath, "/content/source.png");
		EXPECT_EQ(Mixed.PhysicalDirectory, "/content");
		const auto Folder = BuildExtensionContext(Items, {}, "/content", "/Project", &Items[2]);
		EXPECT_EQ(Folder.PhysicalDirectory, "/content/Sub");
		EXPECT_EQ(Folder.VirtualDirectory, "/Project/Sub");
		const auto Tree = BuildExtensionContext(Items, {Items[0].StableId()}, "/content", "/Project",
			nullptr, "/external/directory", "");
		ASSERT_EQ(Tree.Selection.size(), 1);
		EXPECT_EQ(Tree.PrimaryItem->Kind, EExtensionItemKind::Directory);
		EXPECT_EQ(Tree.PhysicalDirectory, "/external/directory");
		EXPECT_TRUE(Tree.VirtualDirectory.empty());
		EXPECT_EQ(Tree.CurrentVirtualDirectory, "/Project");
		const auto Background = BuildExtensionContext(Items, {}, "/content", "/Project");
		EXPECT_TRUE(Background.Selection.empty());
		EXPECT_FALSE(Background.PrimaryItem);
		EXPECT_EQ(Background.PhysicalDirectory, "/content");
	}

	TEST(FContentBrowserExtensionRegistryTests, RejectsContextMutationsAtPresentationAndExecution)
	{
		std::string Error;
		int Invocations = 0;
		auto Registration = RegisterExtension({
			.Id = "test.context-write", .Label = "Modify", .Category = EExtensionCategory::ContextMenu,
			.Mutation = EContentMutation::MutatesContent,
			.IsApplicable = [](const auto&) { return true; },
			.Invoke = [&](const auto&) { ++Invocations; }}, Error);
		ASSERT_TRUE(Registration.IsValid());
		Private::PresentExtensionMenu(EExtensionCategory::ContextMenu, {}, false,
			[](const auto&, bool bEnabled) { EXPECT_FALSE(bEnabled); return true; },
			[](const auto&, const auto&) { ADD_FAILURE() << "Disabled command was queued"; });
		Private::PresentExtensionMenu(EExtensionCategory::ContextMenu, {}, true,
			[](const auto&, bool bEnabled) { EXPECT_TRUE(bEnabled); return true; },
			[](const auto& Entry, const auto& Context) {
				EXPECT_FALSE(InvokeExtension(Entry, {.Context = Context, .bAllowAssetMutation = false}));
			});
		EXPECT_EQ(Invocations, 0);
		auto Invalid = RegisterExtension({.Id = "test.unspecified", .Label = "Invalid",
			.IsApplicable = [](const auto&) { return true; }, .Invoke = [](const auto&) {}}, Error);
		EXPECT_FALSE(Invalid.IsValid());
	}

	TEST(FContentBrowserExtensionRegistryTests, RejectsQueuedSnapshotAfterUnregisterAndReplacement)
	{
		std::string Error;
		int Invocations = 0;
		auto Register = [&] {
			return RegisterExtension({.Id = "test.replacement", .Label = "Replaceable",
				.Category = EExtensionCategory::ContextMenu, .Mutation = EContentMutation::ReadOnly,
				.IsApplicable = [](const auto&) { return true; },
				.Invoke = [&](const auto&) { ++Invocations; }}, Error);
		};
		auto Registration = Register();
		ASSERT_TRUE(Registration.IsValid());
		const auto Snapshot = CaptureExtensions(EExtensionCategory::ContextMenu);
		const auto It = std::ranges::find(Snapshot, "test.replacement", &FExtensionDescriptor::Id);
		ASSERT_NE(It, Snapshot.end());
		Registration.Reset();
		EXPECT_FALSE(InvokeExtension(*It, {}));
		Registration = Register();
		ASSERT_TRUE(Registration.IsValid());
		EXPECT_FALSE(InvokeExtension(*It, {}));
		EXPECT_EQ(Invocations, 0);
	}

	TEST(FContentBrowserExtensionRegistryTests, FiltersByFullIdentityAndCategoryInsteadOfDisplayText)
	{
		using namespace Private;
		std::string Error;
		auto Type = RegisterAssetTypePresentation({
			.AssetClassName = "Durin::DVolumeTexture", .DisplayName = "Localized Material label",
			.Category = EAssetCategory::Texture, .Icon = "texture"}, Error);
		ASSERT_TRUE(Type.IsValid());
		auto Duplicate = RegisterAssetTypePresentation({
			.AssetClassName = "Durin::DVolumeTexture", .DisplayName = "Duplicate", .Icon = "other"}, Error);
		EXPECT_FALSE(Duplicate.IsValid());
		FContentBrowserModel Model;
		Model.SetSnapshotForTesting("/content", {
			{.Kind = EContentBrowserItemKind::Asset, .Name = "Volume", .VirtualPath = "/Project/Volume",
				.PhysicalPath = "/content/Volume.dasset", .AssetClassName = "Durin::DVolumeTexture"},
			{.Kind = EContentBrowserItemKind::Asset, .Name = "Unrelated", .VirtualPath = "/Project/Other",
				.PhysicalPath = "/content/Other.dasset", .AssetClassName = "Plugin::DVolumeTexture"},
			{.Kind = EContentBrowserItemKind::Asset, .Name = "MaterialNamed", .VirtualPath = "/Project/Named",
				.PhysicalPath = "/content/Named.dasset", .AssetClassName = "Plugin::DMaterialNamed"}});
		Model.SetTypeFilter(EContentBrowserTypeFilter::Textures);
		ASSERT_EQ(Model.GetItems().size(), 1);
		EXPECT_EQ(Model.GetItems().front().Name, "Volume");
		Model.SetTypeFilter(EContentBrowserTypeFilter::Materials);
		EXPECT_TRUE(Model.GetItems().empty());
		Model.SetTypeFilter(EContentBrowserTypeFilter::OtherAssets);
		EXPECT_EQ(Model.GetItems().size(), 2);
		const uint64 Revision = GetPresentationRevision();
		Type.Reset();
		EXPECT_GT(GetPresentationRevision(), Revision);
		Model.SetTypeFilter(EContentBrowserTypeFilter::Textures);
		EXPECT_TRUE(Model.GetItems().empty());
	}

} // namespace Durin::Editor::ContentBrowser
