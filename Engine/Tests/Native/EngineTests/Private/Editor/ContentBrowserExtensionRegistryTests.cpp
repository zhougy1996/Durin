#include <gtest/gtest.h>
#include "Editor/BrowserDefinitionTestSupport.h"

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Panels/ContentBrowserExtensionPresentation.h"
#include "Panels/ContentBrowserItemView.h"
#include "Panels/AssetCreationDialog.h"
#include "Asset/Asset.h"
#include "Asset/PackageSerialization.h"
#include "AssetTools/IAssetTools.h"
#include "Engine/Level.h"
#include "EngineTestSupport.h"
#include "NativeTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"

namespace Durin
{
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FBrowserTestDefinition)
}


namespace Durin::Editor::ContentBrowser
{
	TEST(FContentBrowserExtensionRegistryTests, StopOnlyContributionsWithdrawAndCreationCannotOutliveItsHost)
	{
		auto Owner = std::make_shared<uint8>(0);
		auto Other = std::make_shared<uint8>(0);
		const std::weak_ptr<void> Lifetime = Owner;
		int Stops = 0, Creations = 0;
		std::string Error;
		auto Registration = RegisterExtension({.Id = "test.stop-only", .Label = "Stop-only tool",
			.Mutation = EContentMutation::ReadOnly, .IsApplicable = [](const auto&) { return true; }, .Invoke = [](const auto&) {},
			.StopHostPresentation = [&](const auto& Stopped) { if (!Lifetime.owner_before(Stopped) && !Stopped.owner_before(Lifetime)) ++Stops; }}, Error);
		ASSERT_TRUE(Registration.IsValid()) << Error;
		StopHostPresentations(Other);
		EXPECT_EQ(0, Stops);
		StopHostPresentations(Lifetime);
		EXPECT_EQ(1, Stops);
		Registration.Reset();
		StopHostPresentations(Lifetime);
		EXPECT_EQ(1, Stops);
		Private::FAssetCreationDialog Dialog({.Id = "test.host-creation", .Label = "Host asset", .DefaultName = "Asset",
			.Create = [&](const auto&, auto&) { ++Creations; return true; }});
		Dialog.Open({.HostLifetime = Lifetime, .bHasHostLifetime = true});
		Dialog.StopForHost(Other);
		EXPECT_TRUE(Dialog.IsPending());
		Owner.reset();
		EXPECT_FALSE(Dialog.Confirm(true));
		EXPECT_FALSE(Dialog.IsPending());
		EXPECT_EQ(0, Creations);
	}

	TEST(FContentBrowserExtensionRegistryTests, ImportCallbacksAndQueuedActionsStopWithTheirRequestingHost)
	{
		auto Lifetime = std::make_shared<uint8>(0);
		int Calls = 0, FilesystemChanges = 0;
		FExtensionInvocation Invocation{
			.RevealAsset = [&](std::string_view) { ++Calls; return true; },
			.NotifyMountedContentChanged = [&] { ++FilesystemChanges; },
			.ReportError = [&](std::string) { ++Calls; },
			.HostLifetime = Lifetime, .bHasHostLifetime = true};
		const auto Callbacks = MakeImportDialogCallbacks(Invocation, {});
		Callbacks.NotifyAssetCreated("/Project/Asset.Asset");
		EXPECT_EQ(1, Calls);
		EXPECT_EQ(0, FilesystemChanges);
		std::string Error;
		auto Registration = RegisterExtension({.Id = "test.host-lifetime", .Label = "Host command",
			.Mutation = EContentMutation::ReadOnly, .IsApplicable = [](const auto&) { return true; },
			.Invoke = [&](const auto&) { ++Calls; }}, Error);
		ASSERT_TRUE(Registration.IsValid()) << Error;
		const auto Entries = CaptureExtensions(EExtensionCategory::ContextMenu);
		const auto Entry = std::ranges::find(Entries, "test.host-lifetime", &FExtensionDescriptor::Id);
		ASSERT_NE(Entry, Entries.end());
		EXPECT_TRUE(InvokeExtension(*Entry, Invocation));
		Lifetime.reset();
		Callbacks.NotifyAssetCreated("/Project/Asset.Asset");
		Callbacks.Report("Retired host");
		EXPECT_FALSE(InvokeExtension(*Entry, Invocation));
		EXPECT_EQ(2, Calls);
		EXPECT_EQ(0, FilesystemChanges);
	}

	TEST(FContentBrowserExtensionRegistryTests, BuiltInFilterShortcutsUseDefinitionCategoryPaths)
	{
		InitializeDObjectSystem();
		using namespace Private;
		const std::vector<std::pair<EContentBrowserTypeFilter, std::string>> Cases{
			{EContentBrowserTypeFilter::Levels, "Level"}, {EContentBrowserTypeFilter::StaticMeshes, "Meshes"},
			{EContentBrowserTypeFilter::Materials, "Material"}, {EContentBrowserTypeFilter::Textures, "Texture"}};
		for (const auto& [Filter, Category] : Cases)
		{
			auto* Definition = NewObject<FBrowserTestDefinition>(nullptr, {});
			Definition->Categories = {{Category}};
			auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
			ASSERT_TRUE(Registration);
			FContentBrowserDataSource ModelSource;
			FContentBrowserModel Model(ModelSource);
			Model.SetSnapshotForTesting("/content", {{.Kind = EContentBrowserItemKind::Asset,
				.Name = "Typed", .PhysicalPath = "/content/Typed.dasset", .AssetClassName = Definition->ClassPath}});
			Model.SetTypeFilter(Filter); EXPECT_EQ(1u, Model.GetItems().size()) << Category;
			Model.SetCategoryPath({Category}); EXPECT_EQ(1u, Model.GetItems().size()) << Category;
			Registration.Reset();
			Model.SetTypeFilter(Filter); EXPECT_TRUE(Model.GetItems().empty()) << Category;
		}
	}

	TEST(FContentBrowserExtensionRegistryTests, DefinitionsAndExtrasWithdrawIndependently)
	{
		InitializeDObjectSystem();
		auto* Definition = NewObject<FBrowserTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		ASSERT_TRUE(Registration);
		std::string Error;
		auto Extras = RegisterAssetTypeExtras({.AssetClassName = Definition->ClassPath, .ThumbnailBadgeIcon = "badge"}, Error);
		ASSERT_TRUE(Extras.IsValid());
		const auto Both = FindAssetTypePresentation(Definition->ClassPath);
		ASSERT_TRUE(Both); EXPECT_EQ("Project fixture", Both->DisplayName); EXPECT_EQ("badge", Both->ThumbnailBadgeIcon);
		Extras.Reset();
		const auto Metadata = FindAssetTypePresentation(Definition->ClassPath);
		ASSERT_TRUE(Metadata); EXPECT_EQ("Project fixture", Metadata->DisplayName); EXPECT_TRUE(Metadata->ThumbnailBadgeIcon.empty());
		Extras = RegisterAssetTypeExtras({.AssetClassName = Definition->ClassPath, .ThumbnailBadgeIcon = "badge"}, Error);
		Registration.Reset();
		const auto ExtrasOnly = FindAssetTypePresentation(Definition->ClassPath);
		ASSERT_TRUE(ExtrasOnly); EXPECT_TRUE(ExtrasOnly->DisplayName.empty()); EXPECT_TRUE(ExtrasOnly->CategoryPaths.empty());
		EXPECT_EQ("badge", ExtrasOnly->ThumbnailBadgeIcon);
		Extras.Reset(); EXPECT_FALSE(FindAssetTypePresentation(Definition->ClassPath));
	}

	TEST(FContentBrowserExtensionRegistryTests, DefinitionsDriveMetadataAndNestedFiltersWithoutLoadingAssets)
	{
		InitializeDObjectSystem();
		auto* Definition = NewObject<FBrowserTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		ASSERT_TRUE(Registration);
		using namespace Private;
		FContentBrowserDataSource ModelSource;
		FContentBrowserModel Model(ModelSource);
		Model.SetSnapshotForTesting("/content", {
			{.Kind = EContentBrowserItemKind::Asset, .Name = "Project", .PhysicalPath = "/content/Project.dasset", .AssetClassName = "Project::DCategoryFixture"},
			{.Kind = EContentBrowserItemKind::Asset, .Name = "Other", .PhysicalPath = "/content/Other.dasset", .AssetClassName = "Project::Other"}});
		const auto ObjectRevision = GDObjectArray.GetRevision();
		const auto Type = FindAssetTypePresentation("Project::DCategoryFixture");
		ASSERT_TRUE(Type);
		EXPECT_EQ("Project fixture", Type->DisplayName);
		EXPECT_EQ("project-icon", Type->Icon);
		EXPECT_FLOAT_EQ(0.2f, Type->Color[1]);
		const auto Paths = CaptureAssetCategoryPaths();
		EXPECT_NE(Paths.end(), std::ranges::find(Paths, std::vector<std::string>{"Project", "Simulation"}));
		Model.SetCategoryPath({"Project", "Simulation"});
		ASSERT_EQ(1u, Model.GetItems().size());
		EXPECT_EQ("Project", Model.GetItems().front().Name);
		Model.SetCategoryPath({"Project", "Missing"});
		EXPECT_TRUE(Model.GetItems().empty());
		const auto Revision = GetPresentationRevision();
		EXPECT_EQ(ObjectRevision, GDObjectArray.GetRevision());
		Registration.Reset();
		EXPECT_GT(GetPresentationRevision(), Revision);
		Model.SetCategoryPath({"Tools"});
		EXPECT_TRUE(Model.GetItems().empty());
		// The selected package paths do not exist: metadata and filtering never execute a load.
		EXPECT_FALSE(std::filesystem::exists("/content/Project.dasset"));
	}

	TEST(FContentBrowserExtensionRegistryTests, ReusesHostSnapshotAndRejectsRetiredRegistrations)
	{
		int Presentations = 0;
		std::string Error;
		auto Register = [&](std::string Id, int Order, bool bPresenter = true) {
			return RegisterExtension({
				.Id = std::move(Id), .Label = "Cached presenter", .Order = Order,
				.Mutation = EContentMutation::ReadOnly,
				.IsApplicable = [](const auto&) { return true; },
				.Invoke = [](const auto&) {},
				.DrawHostPresentation = bPresenter
					? std::function<void(const FExtensionHost&, bool)>{[&](const FExtensionHost&, bool) { ++Presentations; }}
					: std::function<void(const FExtensionHost&, bool)>{}}, Error);
		};
		auto Later = Register("test.cached.later", 20);
		auto Earlier = Register("test.cached.earlier", 10);
		ASSERT_TRUE(Later.IsValid()) << Error;
		ASSERT_TRUE(Earlier.IsValid()) << Error;
		const auto Snapshot = CaptureHostPresenters();
		EXPECT_EQ(CaptureHostPresenters(), Snapshot);
		const auto First = std::ranges::find(*Snapshot, "test.cached.earlier", &FExtensionDescriptor::Id);
		const auto Last = std::ranges::find(*Snapshot, "test.cached.later", &FExtensionDescriptor::Id);
		ASSERT_NE(First, Snapshot->end());
		ASSERT_NE(Last, Snapshot->end());
		EXPECT_LT(First, Last);
		EXPECT_TRUE(DrawHostPresentation(*First, false));
		EXPECT_TRUE(DrawHostPresentation(*First, true));
		EXPECT_EQ(Presentations, 2);

		// Unrelated commands do not invalidate the host presentation list.
		auto Command = Register("test.cached.command", 0, false);
		ASSERT_TRUE(Command.IsValid()) << Error;
		EXPECT_EQ(CaptureHostPresenters(), Snapshot);
		Command.Reset();
		EXPECT_EQ(CaptureHostPresenters(), Snapshot);

		Earlier.Reset();
		const auto Removed = CaptureHostPresenters();
		EXPECT_NE(Removed, Snapshot);
		EXPECT_EQ(std::ranges::find(*Removed, "test.cached.earlier", &FExtensionDescriptor::Id), Removed->end());
		EXPECT_FALSE(DrawHostPresentation(*First, true));
		auto Replacement = Register("test.cached.earlier", 10);
		ASSERT_TRUE(Replacement.IsValid()) << Error;
		const auto Replaced = CaptureHostPresenters();
		EXPECT_NE(Replaced, Removed);
		EXPECT_FALSE(DrawHostPresentation(*First, true));
		const auto Current = std::ranges::find(*Replaced, "test.cached.earlier", &FExtensionDescriptor::Id);
		ASSERT_NE(Current, Replaced->end());
		EXPECT_TRUE(DrawHostPresentation(*Current, true));
		EXPECT_EQ(Presentations, 3);
	}

	TEST(FContentBrowserExtensionRegistryTests, RegisteredAssetCreationOnlyRequestsNamingOnInvoke)
	{
		int Creations = 0;
		std::string Error;
		auto Registration = RegisterAssetCreation({
			.Id = "test.registered-creation", .Label = "Asset", .DefaultName = "NewAsset",
			.Create = [&](const auto&, auto&) { ++Creations; return true; }}, Error);
		ASSERT_TRUE(Registration.IsValid()) << Error;
		const auto Entries = CaptureExtensions(EExtensionCategory::Create);
		const auto Entry = std::ranges::find(Entries, "test.registered-creation", &FExtensionDescriptor::Id);
		ASSERT_NE(Entry, Entries.end());
		ASSERT_TRUE(Entry->DrawHostPresentation);
		EXPECT_FALSE(InvokeExtension(*Entry, {
			.Context = {.VirtualDirectory = "/Project"}, .bAllowAssetMutation = false}));
		EXPECT_TRUE(InvokeExtension(*Entry, {.Context = {.VirtualDirectory = "/Project"}}));
		EXPECT_EQ(Creations, 0);
		Registration.Reset();
		const auto Presenters = CaptureHostPresenters();
		EXPECT_EQ(std::ranges::find(*Presenters, "test.registered-creation", &FExtensionDescriptor::Id),
			Presenters->end());
		EXPECT_EQ(Creations, 0);
	}

	TEST(FContentBrowserExtensionRegistryTests, AssetCreationDefersPersistenceUntilFinalNameConfirmed)
	{
		InitializeDObjectSystem();
		const auto Root = Testing::CreateTestFixtureDirectory("DeferredAssetCreation");
		const std::array Definitions{FMountPoint{
			.VirtualRoot = "/DeferredCreation/", .Owner = EMountOwner::Test,
			.Root = Root, .bAutoScan = true, .bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Definitions);
		ASSERT_TRUE(Registry.IsValid());
		int Creations = 0;
		int Notifications = 0;
		std::string Revealed;
		std::string Opened;
		DLevel* OpenedLevel = nullptr;
		std::string OpenError;
		DPackage* CreatedPackage = nullptr;
		struct FPackageCleanup
		{
			DPackage*& Package;
			~FPackageCleanup() { if (Package) IAssetTools::Get().DiscardPackage(Package); }
		} Cleanup{CreatedPackage};
		Private::FAssetCreationDialog Dialog({
			.Id = "test.deferred-creation", .Label = "Level", .DefaultName = "NewLevel",
			.Create = [&](const FTopLevelAssetPath& Path, std::string& Error) {
				++Creations;
				std::string CreatedError;
				Durin::FFeedbackContext CreatedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
					if (Level >= Durin::ELogLevel::Error) { if (!CreatedError.empty()) CreatedError += "\n"; CreatedError += Message; }
				});
				const auto Created = IAssetTools::Get().CreateAsset(Path, DLevel::StaticClass(), nullptr, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &CreatedFeedback);
				if (!Created) { Error = CreatedError; return false; }
				CreatedPackage = Created->GetPackage();
				const auto Saved = SavePackage(Created->GetPackage());
				Error = Saved.Message;
				return Saved.Succeeded();
			},
			.AssetClassNameToOpen = DLevel::StaticClass()->GetQualifiedName().ToString()});
		const FExtensionInvocation Invocation{
			.Context = {.VirtualDirectory = "/DeferredCreation"},
			.RevealAsset = [&](std::string_view Path) { Revealed = Path; return true; },
			.OpenAsset = [&](std::string_view Path, std::string_view ClassName) {
				Opened = Path;
				EXPECT_EQ(ClassName, DLevel::StaticClass()->GetQualifiedName().ToString());
				FSoftObjectPath ObjectPath;
				if (const auto PathValidation = FSoftObjectPath::TryCreateWithDiagnostic(Path, ObjectPath); !PathValidation) { OpenError = Durin::ToString(PathValidation.error()); return false; }
				const auto Result = LoadObjectDetailed<DLevel>(ObjectPath);
				OpenedLevel = Result.value_or(nullptr);
				OpenError = (Result ? std::string{} : Result.error().Message);
				return Result.has_value();
			},
			.NotifyMountedContentChanged = [&] { ++Notifications; },
			.ReportError = [&](std::string Message) { ADD_FAILURE() << Message; }};
		Dialog.Open(Invocation);
		EXPECT_EQ(Creations, 0);
		EXPECT_TRUE(std::filesystem::is_empty(Root));
		Dialog.Cancel();
		EXPECT_FALSE(Dialog.Confirm(true));
		EXPECT_EQ(Creations, 0);
		EXPECT_TRUE(std::filesystem::is_empty(Root));

		Dialog.Open(Invocation);
		Dialog.SetName("FinalLevel");
		EXPECT_FALSE(Dialog.Confirm(false));
		EXPECT_EQ(Creations, 0);
		ASSERT_TRUE(Dialog.Confirm(true)) << Dialog.GetError();
		EXPECT_FALSE(Dialog.IsPending());
		EXPECT_FALSE(Dialog.Confirm(true));
		EXPECT_EQ(Creations, 1);
		EXPECT_EQ(Notifications, 0);
		EXPECT_EQ(Revealed, "/DeferredCreation/FinalLevel.FinalLevel");
		EXPECT_EQ(Opened, Revealed);
		ASSERT_NE(OpenedLevel, nullptr) << OpenError;
		EXPECT_EQ(OpenedLevel->GetObjectPath(), Opened);
		EXPECT_TRUE(std::filesystem::is_regular_file(Root / "FinalLevel.dasset"));
		EXPECT_FALSE(std::filesystem::exists(Root / "NewLevel.dasset"));
		FPackagePath DefaultPath;
		ASSERT_TRUE(FPackagePath::TryCreate("/DeferredCreation/NewLevel", DefaultPath));
		EXPECT_FALSE(FindPackageExact(DefaultPath));
		EXPECT_EQ(FindResidentPackage(DefaultPath), nullptr);

		Dialog.Open(Invocation);
		Dialog.SetName("FinalLevel");
		EXPECT_FALSE(Dialog.Confirm(true));
		EXPECT_TRUE(Dialog.IsPending());
		EXPECT_FALSE(Dialog.GetError().empty());
		EXPECT_EQ(Creations, 1);
		Dialog.Cancel();
	}

	TEST(FContentBrowserExtensionRegistryTests, AssetCreationRejectsInvalidNamesAndRetainsFailedRequest)
	{
		InitializeDObjectSystem();
		const auto Root = Testing::CreateTestFixtureDirectory("AssetCreationRetry");
		const std::array Definitions{FMountPoint{
			.VirtualRoot = "/CreationRetry/", .Owner = EMountOwner::Test,
			.Root = Root, .bAutoScan = true, .bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Definitions);
		ASSERT_TRUE(Registry.IsValid());
		int Attempts = 0;
		int Notifications = 0;
		std::string LastPath;
		Private::FAssetCreationDialog Dialog({
			.Id = "test.creation-retry", .Label = "Asset", .DefaultName = "NewAsset",
			.Create = [&](const FTopLevelAssetPath& Path, std::string& Error) {
				++Attempts;
				LastPath = Path.ToString();
				Error = "Injected save failure";
				return false;
			}});
		Dialog.Open({.Context = {.VirtualDirectory = "/CreationRetry"},
			.NotifyMountedContentChanged = [&] { ++Notifications; }});
		for (const auto* Name : {"", "../Escape", "Folder/Asset", "Asset.dasset"})
		{
			Dialog.SetName(Name);
			EXPECT_FALSE(Dialog.Confirm(true));
			EXPECT_FALSE(Dialog.GetError().empty());
		}
		EXPECT_EQ(Attempts, 0);
		Dialog.SetName("ChosenName");
		EXPECT_FALSE(Dialog.Confirm(true));
		EXPECT_EQ(LastPath, "/CreationRetry/ChosenName.ChosenName");
		EXPECT_EQ(Dialog.GetError(), "Injected save failure");
		EXPECT_TRUE(Dialog.IsPending());
		EXPECT_FALSE(Dialog.Confirm(true));
		EXPECT_EQ(Attempts, 2);
		EXPECT_EQ(Notifications, 0);
		Dialog.Cancel();
		EXPECT_TRUE(std::filesystem::is_empty(Root));
	}

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
					? std::function<void(const FExtensionHost&, bool)>{[
						&Presentations,
						&bLastPresentationAllowedMutation](const FExtensionHost&, bool bAllowAssetMutation) {
						++Presentations;
						bLastPresentationAllowedMutation = bAllowAssetMutation;
					}}
					: std::function<void(const FExtensionHost&, bool)>{},
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
			*Presenters, "test.mutation-policy.import", &FExtensionDescriptor::Id);
		ASSERT_NE(Presenter, Presenters->end());
		EXPECT_TRUE(DrawHostPresentation(*Presenter, false));
		EXPECT_EQ(Presentations, 1);
		EXPECT_FALSE(bLastPresentationAllowedMutation);

		Registrations.clear();
		const auto Remaining = CaptureHostPresenters();
		EXPECT_EQ(std::ranges::find(*Remaining, "test.mutation-policy.import",
			&FExtensionDescriptor::Id), Remaining->end());
		EXPECT_EQ(Presentations, 1);
	}
	TEST(FContentBrowserExtensionRegistryTests, PresentsUnknownTypeThroughProductionDetailsAndMenu)
	{
		using namespace Private;
		std::string Error;
		int Invocations = 0;
		InitializeDObjectSystem();
		auto* Definition = NewObject<FBrowserTestDefinition>(nullptr, {});
		Definition->ClassPath = "Plugin::DNovelAsset"; Definition->DisplayName = "Novel Asset";
		Definition->Icon = "custom icon"; Definition->Categories = {{"Texture"}};
		auto DefinitionRegistration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		ASSERT_TRUE(DefinitionRegistration);
		auto Type = RegisterAssetTypeExtras({.AssetClassName = Definition->ClassPath,
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
		DefinitionRegistration.Reset();
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
		InitializeDObjectSystem();
		auto* Definition = NewObject<FBrowserTestDefinition>(nullptr, {});
		Definition->ClassPath = "FilterTest::DVolumeTexture"; Definition->DisplayName = "Localized Material label";
		Definition->Icon = "texture"; Definition->Categories = {{"Texture"}};
		auto Type = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		ASSERT_TRUE(Type);
		auto Duplicate = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition, &Error);
		EXPECT_FALSE(Duplicate);
		FContentBrowserDataSource ModelSource;
		FContentBrowserModel Model(ModelSource);
		Model.SetSnapshotForTesting("/content", {
			{.Kind = EContentBrowserItemKind::Asset, .Name = "Volume", .VirtualPath = "/Project/Volume",
				.PhysicalPath = "/content/Volume.dasset", .AssetClassName = "FilterTest::DVolumeTexture"},
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
