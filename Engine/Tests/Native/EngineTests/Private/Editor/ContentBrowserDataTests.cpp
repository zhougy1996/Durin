#include "Threading/TaskRuntime.h"
#include "ContentBrowserAssetProviderSubsystem.h"
#include "ContentBrowserFileProviderSubsystem.h"
#include "Editor/ContentBrowserItemTestSupport.h"
#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserAssetDataSource.h"
#include "ContentBrowserFileDataSource.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorEngineTestSupport.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/FileHelper.h"
#include "AssetTools/IAssetTools.h"
#include "AssetTools/ContentMutation.h"
#include "Asset/Testing/PackageSaveTesting.h"
#include <gtest/gtest.h>

using Durin::ContentBrowser::Private::TestKind;
using Durin::ContentBrowser::Private::TestName;
using Durin::ContentBrowser::Private::TestVirtualPath;
using Durin::ContentBrowser::Private::TestPhysicalPath;
using Durin::ContentBrowser::Private::TestPackagePath;
using Durin::ContentBrowser::Private::TestAssetClassName;
using Durin::ContentBrowser::Private::TestExtension;
using Durin::ContentBrowser::Private::TestRedirectDestination;
using Durin::ContentBrowser::Private::TestFileSize;
using Durin::ContentBrowser::Private::TestPackageFileSize;
using Durin::ContentBrowser::Private::TestBulkFileSize;
using Durin::ContentBrowser::Private::TestLastWriteTime;

namespace Durin
{
	class FProbeContentSource : public DContentBrowserDataSource
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FProbeContentSource, DContentBrowserDataSource, FProbeContentSource::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FProbeContentSource)
	public:
		explicit FProbeContentSource(const FObjectInitializer& I = FObjectInitializer::Get())
			: Super(I)
		{
		}
		int Copies = 0;
		int CreatedFolders = 0;
		auto CanCreateFolder(std::string_view Path, std::string*) const -> bool override
		{
			return IsActive() && Path.starts_with("/ProbeFolders/");
		}
		auto CreateFolder(std::string_view Path, ContentBrowser::FContentBrowserItemDataTemporaryContext& Out, const ContentBrowser::FContentBrowserOperationContext&) -> bool override
		{
			using namespace ContentBrowser;
			if (!CanCreateFolder(Path, nullptr)) return false;
			FContentBrowserItemData Item(*this, EContentBrowserItemFlags::Type_Folder | EContentBrowserItemFlags::Temporary_Creation, std::string(Path), "", "", {});
			auto Validate = [Item](std::string_view Name, std::string*) {
				return Item.GetOwnerDataSource() && !Name.empty() && Name != "." && Name != ".." && Name.find_first_of("/\\") == std::string_view::npos;
			};
			Out = {Item, Validate, [Item, Validate](std::string_view Name, FContentBrowserItemData& Result) {
				if (!Validate(Name, nullptr)) return false;
				auto* Source = static_cast<FProbeContentSource*>(Item.GetOwnerDataSource());
				Result = FContentBrowserItemData(*Source, EContentBrowserItemFlags::Type_Folder, "/ProbeFolders/" + std::string(Name), "", "", {});
				++Source->CreatedFolders;
				return true;
			}};
			return true;
		}
		std::function<void()> BeforeAttribute;
		auto GetItemAttribute(const ContentBrowser::FContentBrowserItemData& Item, bool IncludeMetaData, std::string_view Key, ContentBrowser::FContentBrowserItemDataAttributeValue& Out) const -> bool override
		{
			Out={};
			if (Item.GetOwnerDataSource()!=this) return false;
			if (BeforeAttribute) BeforeAttribute();
			if (Key=="ProbeCounter") Out=ContentBrowser::FContentBrowserItemDataAttributeValue(std::numeric_limits<uint64>::max()-1);
			else if (Key==ContentBrowser::ContentBrowserItemAttributes::ItemTypeDisplayName) Out=ContentBrowser::FContentBrowserItemDataAttributeValue("Memory object");
			else return false;
			if (IncludeMetaData) Out.SetMetaData({.DisplayName="Counter"});
			return true;
		}
		auto GetItemAttributes(const ContentBrowser::FContentBrowserItemData& Item, bool IncludeMetaData, ContentBrowser::FContentBrowserItemDataAttributeValues& Out) const -> bool override
		{
			Out.clear();
			ContentBrowser::FContentBrowserItemDataAttributeValue Value;
			if (!GetItemAttribute(Item, IncludeMetaData, "ProbeCounter", Value)) return false;
			Out.emplace("ProbeCounter", std::move(Value)); return true;
		}
		bool bEditable = false;
		int Edits = 0;
		int Duplications = 0;
		auto CanEditItem(const ContentBrowser::FContentBrowserItemData& Item, std::string*) const -> bool override { return bEditable && Item.GetOwnerDataSource() == this; }
		auto CanDuplicateItem(const ContentBrowser::FContentBrowserItemData& Item, std::string*) const -> bool override { return bEditable && Item.GetOwnerDataSource() == this; }
		auto EditItem(const ContentBrowser::FContentBrowserItemData& Item) -> bool override { ++Edits; return bEditable && Item.GetOwnerDataSource() == this; }
		auto DuplicateItem(const ContentBrowser::FContentBrowserItemData& Item, ContentBrowser::FContentBrowserItemData& Out) -> bool override
		{
			++Duplications;
			if (!bEditable || Item.GetOwnerDataSource() != this) return false;
			Out = ContentBrowser::FContentBrowserItemData(*this, ContentBrowser::EContentBrowserItemFlags::Type_File, Item.GetVirtualPath() + "_Copy", "", "", {});
			return true;
		}
		int Saves = 0;
		bool bFailSave = false;
		std::function<void()> BeforeSave;
		auto CanSaveItem(const ContentBrowser::FContentBrowserItemData& Item, ContentBrowser::EContentBrowserItemSaveFlags, std::string*) const -> bool override { return Item.GetOwnerDataSource() == this; }
		auto BulkSaveItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, ContentBrowser::EContentBrowserItemSaveFlags) -> bool override
		{
			++Saves;
			if (BeforeSave) BeforeSave();
			return !bFailSave && !Items.empty();
		}
		std::string Name = "Probe";
		std::string ItemPath = "/Probe/Item";
		ContentBrowser::EContentBrowserItemKind Kind = ContentBrowser::EContentBrowserItemKind::File;
		int Ticks = 0;
		auto Tick(float) -> void override { ++Ticks; }
		auto GetDataSourceName() const -> FName override { return FName(Name); }
		auto CanCopyItem(const ContentBrowser::FContentBrowserItemData& Item, std::string_view Destination, std::string*) const -> bool override
		{
			CapabilityDestinations.emplace_back(Destination);
			if (BeforeCopyCheck) BeforeCopyCheck();
			return Item.GetOwnerDataSource() == this;
		}
		mutable std::vector<std::string> CapabilityDestinations;
		std::function<void()> BeforeCopyCheck;
		std::string LastCopyDestination;
		bool UseSingleItemCopy = false;
		int SingleCopies = 0;
		auto CopyItem(const ContentBrowser::FContentBrowserItemData& Item, std::string_view Destination) -> bool override
		{
			++SingleCopies; LastCopyDestination = Destination;
			return Item.GetOwnerDataSource() == this;
		}
		auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, std::string_view Destination) -> bool override
		{
			LastCopyDestination = Destination;
			if (UseSingleItemCopy) return Super::BulkCopyItems(Items, Destination);
			++Copies;
			return !Items.empty();
		}


		auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter&, const std::function<bool(ContentBrowser::FContentBrowserItemData&&)>& Callback) -> void override
		{
			Callback(ContentBrowser::FContentBrowserItemData(*this, Kind == ContentBrowser::EContentBrowserItemKind::Folder ? ContentBrowser::EContentBrowserItemFlags::Type_Folder : ContentBrowser::EContentBrowserItemFlags::Type_File, ItemPath, "", "", nullptr));
		}
	};
	auto FProbeContentSource::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class) GetPrivateStaticClassBody("", "FProbeContentSource", Class, nullptr, sizeof(FProbeContentSource), alignof(FProbeContentSource), EClassFlags::None, InternalConstructor<FProbeContentSource>, &DContentBrowserDataSource::StaticClass);
		return Class;
	}
} // namespace Durin
namespace
{
	using namespace Durin;
	using namespace Durin::ContentBrowser;
	class FNamedAssetSource final : public DContentBrowserAssetDataSource
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FNamedAssetSource, DContentBrowserAssetDataSource, FNamedAssetSource::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FNamedAssetSource)
	public:
		explicit FNamedAssetSource(const FObjectInitializer& I) : Super(I) {}
		auto GetDataSourceName() const -> FName override { return FName("Packages"); }
	};
	auto FNamedAssetSource::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class) GetPrivateStaticClassBody("", "FNamedAssetSource", Class, nullptr, sizeof(FNamedAssetSource), alignof(FNamedAssetSource), EClassFlags::None, InternalConstructor<FNamedAssetSource>, &DContentBrowserAssetDataSource::StaticClass);
		return Class;
	}
	class FNamedFileSource final : public DContentBrowserFileDataSource
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FNamedFileSource, DContentBrowserFileDataSource, FNamedFileSource::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FNamedFileSource)
	public:
		explicit FNamedFileSource(const FObjectInitializer& I) : Super(I) {}
		auto GetDataSourceName() const -> FName override { return FName("LooseFiles"); }
		int BulkCalls = 0;
		auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, std::string_view Destination) -> bool override
		{ ++BulkCalls; return Super::BulkCopyItems(Items, Destination); }

	};
	auto FNamedFileSource::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class) GetPrivateStaticClassBody("", "FNamedFileSource", Class, nullptr, sizeof(FNamedFileSource), alignof(FNamedFileSource), EClassFlags::None, InternalConstructor<FNamedFileSource>, &DContentBrowserFileDataSource::StaticClass);
		return Class;
	}
	class FContentBrowserDataTests : public testing::Test
	{
	protected:
		TStrongObjectPtr<DEditorEngine> Editor;
		DContentBrowserDataSubsystem* Data = nullptr;
		std::vector<FContentBrowserItemDataUpdate> PublishedUpdates;
		std::unique_ptr<Testing::FScopedMountRegistryFixture> Mounts;
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> Extras;
		std::filesystem::path Root;
		bool OwnScheduler = false;
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			OwnScheduler = !IsTaskSchedulerRunning();
			if (OwnScheduler) ASSERT_TRUE(InitializeTaskScheduler(2));
			if (!GetGameThreadDeferredWorkQueueDiagnostics().bInstalled) ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
			for (auto Name : {"ContentBrowserData", "ContentBrowserAssetDataSource", "ContentBrowserFileDataSource"}) FModuleManager::Get().LoadModuleChecked(Name);
			Editor = TStrongObjectPtr<DEditorEngine>(NewObject<DEditorEngine>(nullptr, {}));
			FEditorEngineTestAccess::StartEditor(*Editor);
			Data = Editor->GetEditorSubsystem<DContentBrowserDataSubsystem>();
			ASSERT_NE(nullptr, Data);
			Data->OnItemDataUpdated().AddLambda([this](auto Updates) { PublishedUpdates.assign(Updates.begin(), Updates.end()); });
			EXPECT_FALSE(FModuleManager::Get().IsModuleLoaded(FName("ContentBrowser")));
			Root = Testing::GetTestWorkDirectory() / "BrowserProviders" / testing::UnitTest::GetInstance()->current_test_info()->name();
			std::error_code Error;
			Testing::RemoveTestWorkDirectory(Root, Error);
			std::filesystem::create_directories(Root / "Sub");
			std::filesystem::create_directories(Root / "Target");
			std::ofstream(Root / "source.txt") << "original";
			Mounts = std::make_unique<Testing::FScopedMountRegistryFixture>();
			Testing::RegisterMountPointForTests("/ProviderTests/", Root.generic_string() + "/");
		}
		auto TearDown() -> void override
		{
			for (const auto& Source : Extras)
				Source->Shutdown();
			if (Editor.Get()) Editor->PreExit();
			Data = nullptr;
			Extras.clear();
			Editor.Reset();
			Mounts.reset();
			if (OwnScheduler) ShutdownTaskScheduler(true);
		}
		auto Enumerate() -> std::vector<FContentBrowserItem>
		{
			std::vector<FContentBrowserItem> Items;
			Data->EnumerateItemsUnderPath("/ProviderTests/", {}, [&](FContentBrowserItem&& Item) { Items.push_back(std::move(Item)); return true; });
			return Items;
		}
		auto File() -> FContentBrowserItemData
		{
			auto Items = Enumerate();
			for (auto& Item : Items)
				if (TestPhysicalPath(Item).ends_with("source.txt")) return Item.InternalItems.front();
			return {};
		}
	};
	TEST_F(FContentBrowserDataTests, RenamedNativeProvidersPublishOnlyTheirOwnDirectoryContributions)
	{
		Editor->RemoveEditorSubsystem(DContentBrowserAssetProviderSubsystem::StaticClass());
		Editor->RemoveEditorSubsystem(DContentBrowserFileProviderSubsystem::StaticClass());
		auto* Assets = NewObject<FNamedAssetSource>(Data, {});
		auto* Files = NewObject<FNamedFileSource>(Data, {});
		Extras.emplace_back(Assets); Extras.emplace_back(Files);
		Assets->Initialize(); Files->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Packages")));
		ASSERT_TRUE(Data->ActivateDataSource(FName("LooseFiles")));
		struct FOtherServiceScope
		{
			TStrongObjectPtr<DEditorEngine> Editor{NewObject<DEditorEngine>(nullptr, {})};
			FSubsystemCollection Collection{*Editor, DEditorSubsystem::StaticClass()};
			TStrongObjectPtr<DContentBrowserDataSubsystem> Data;
			TStrongObjectPtr<FNamedFileSource> Source;
			FOtherServiceScope()
			{
				Collection.Initialize({{.Type = DContentBrowserDataSubsystem::StaticClass()}});
				Data = TStrongObjectPtr<DContentBrowserDataSubsystem>(static_cast<DContentBrowserDataSubsystem*>(Collection.Find(DContentBrowserDataSubsystem::StaticClass())));
				Source = TStrongObjectPtr<FNamedFileSource>(NewObject<FNamedFileSource>(Data.Get(), {}));
				Source->Initialize();
				require(Data->ActivateDataSource(FName("LooseFiles")));
			}
			~FOtherServiceScope() { Source->Shutdown(); Collection.Deinitialize(); }
		} Other;
		for (auto* Creator : std::array<DContentBrowserDataSource*, 2>{Assets, Files})
		{
			const auto Name = Creator == Assets ? "AssetCreated" : "FileCreated";
			const auto Virtual = std::string("/ProviderTests/") + Name;
			FContentBrowserItemDataTemporaryContext Pending;
			ASSERT_TRUE(Creator->CreateFolder(Virtual, Pending));
			FContentBrowserItemData Created;
			ASSERT_TRUE(Pending.FinalizeItem(Name, Created));
			// Native command notifications belong to their data owner. Other services
			// reconcile the shared disk through their own registry/discovery streams.
			EXPECT_FALSE(Other.Data->HasPendingItemDataUpdates());
			(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.0f));
			for (auto* Owner : std::array<DContentBrowserDataSource*, 2>{Assets, Files})
				EXPECT_EQ(1, std::ranges::count_if(PublishedUpdates, [&](const auto& Update) {
					return Update.GetUpdateType() == EContentBrowserItemUpdateType::Added
						&& Update.GetItemData().GetVirtualPath() == Virtual
						&& Update.GetItemData().GetOwnerDataSource() == Owner;
				}));
		}
		Files->Shutdown();
		FContentBrowserItemDataTemporaryContext Pending;
		ASSERT_TRUE(Assets->CreateFolder("/ProviderTests/AfterWithdrawal", Pending));
		FContentBrowserItemData Created;
		ASSERT_TRUE(Pending.FinalizeItem("AfterWithdrawal", Created));
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.0f));
		EXPECT_TRUE(std::ranges::all_of(PublishedUpdates, [&](const auto& Update) {
			return Update.GetItemData().GetOwnerDataSource() == Assets;
		}));
	}
	TEST_F(FContentBrowserDataTests, EditorOwnedNativeProviderUpdatesServiceOwnedContributions)
	{
		auto* Assets = NewObject<FNamedAssetSource>(Editor.Get(), {});
		Extras.emplace_back(Assets); Assets->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Packages")));
		FContentBrowserItemDataTemporaryContext Pending;
		ASSERT_TRUE(Assets->CreateFolder("/ProviderTests/EditorOwned", Pending));
		FContentBrowserItemData Created;
		ASSERT_TRUE(Pending.FinalizeItem("EditorOwned", Created));
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.0f));
		EXPECT_EQ(1, std::ranges::count_if(PublishedUpdates, [&](const auto& Update) {
			return Update.GetUpdateType() == EContentBrowserItemUpdateType::Added
				&& Update.GetItemData().GetVirtualPath() == "/ProviderTests/EditorOwned"
				&& Update.GetItemData().GetOwnerDataSource() == Data->GetActiveDataSource(FName("File"));
		}));
	}
	TEST_F(FContentBrowserDataTests, InterruptedNativeInitializationDoesNotLeakSubscriptions)
	{
		Editor->RemoveEditorSubsystem(DContentBrowserAssetProviderSubsystem::StaticClass());
		Editor->RemoveEditorSubsystem(DContentBrowserFileProviderSubsystem::StaticClass());
		for (const bool AssetReceiver : {true, false})
		{
			SCOPED_TRACE(AssetReceiver ? "Asset" : "File");
			auto* Receiver = AssetReceiver
				? static_cast<DContentBrowserDataSource*>(NewObject<FNamedAssetSource>(Data, {}))
				: static_cast<DContentBrowserDataSource*>(NewObject<FNamedFileSource>(Data, {}));
			auto* Creator = AssetReceiver
				? static_cast<DContentBrowserDataSource*>(NewObject<DContentBrowserFileDataSource>(Data, {}))
				: static_cast<DContentBrowserDataSource*>(NewObject<DContentBrowserAssetDataSource>(Data, {}));
			Extras.emplace_back(Receiver); Extras.emplace_back(Creator);
			Creator->Initialize();
			ASSERT_TRUE(Data->ActivateDataSource(Creator->GetDataSourceName()));
			bool Interrupted = false;
			const auto Handle = DContentBrowserDataSource::OnDataSourceMembershipChanged().AddLambda([&] {
				if (!Interrupted && Receiver->IsInitialized()) { Interrupted = true; Receiver->Shutdown(); }
			});
			Receiver->Initialize();
			DContentBrowserDataSource::OnDataSourceMembershipChanged().Remove(Handle);
			ASSERT_TRUE(Interrupted);
			EXPECT_FALSE(Receiver->IsInitialized());
			Receiver->Initialize();
			ASSERT_TRUE(Data->ActivateDataSource(Receiver->GetDataSourceName()));
			const auto Name = AssetReceiver ? "OnceAsset" : "OnceFile";
			const auto Path = std::string("/ProviderTests/") + Name;
			FContentBrowserItemDataTemporaryContext Pending;
			ASSERT_TRUE(Creator->CreateFolder(Path, Pending));
			FContentBrowserItemData Created;
			ASSERT_TRUE(Pending.FinalizeItem(Name, Created));
			(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.0f));
			EXPECT_EQ(1, std::ranges::count_if(PublishedUpdates, [&](const auto& Update) {
				return Update.GetUpdateType() == EContentBrowserItemUpdateType::Added
					&& Update.GetItemData().GetVirtualPath() == Path
					&& Update.GetItemData().GetOwnerDataSource() == Receiver;
			}));
			Receiver->Shutdown(); Creator->Shutdown();
		}
	}
	TEST_F(FContentBrowserDataTests, MultipleFileProvidersSettleAndContinueDiscoveringChanges)
	{
		auto* Other = NewObject<FNamedFileSource>(Data, {});
		Extras.emplace_back(Other); Other->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("LooseFiles")));
		auto Pump = [&] {
			for (int I = 0; I < 100; ++I) { (PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.5f)); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
		};
		Pump();
		int Events = 0;
		const auto Changed = Data->OnItemDataUpdated().AddLambda([&](auto) { ++Events; });
		Pump();
		EXPECT_EQ(0, Events);
		Data->OnItemDataUpdated().Remove(Changed);
		std::ofstream(Root / "after-settle.txt") << "external change";
		bool DefaultObserved = false, OtherObserved = false;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do
		{
			(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.5f));
			for (const auto& Update : PublishedUpdates)
				if (Update.GetUpdateType() == EContentBrowserItemUpdateType::Added
					&& Update.GetItemData().GetVirtualPath() == "/ProviderTests/after-settle.txt")
				{
					DefaultObserved |= Update.GetItemData().GetOwnerDataSource() == Data->GetActiveDataSource(FName("File"));
					OtherObserved |= Update.GetItemData().GetOwnerDataSource() == Other;
				}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		} while ((!DefaultObserved || !OtherObserved) && std::chrono::steady_clock::now() < Deadline);
		EXPECT_TRUE(DefaultObserved); EXPECT_TRUE(OtherObserved);
	}
	TEST_F(FContentBrowserDataTests, StructuredMutationNotificationPreservesFullAndScopedEffects)
	{
		auto* Source = NewObject<FProbeContentSource>(Data, {});
		Extras.emplace_back(Source); Source->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		std::vector<FContentChangeBatch> Published;
		int Projections = 0;
		auto Policy = ScopeOperationContext(*Source,
			{.NotifyContentMutation = [&](FContentChangeBatch Batch) { Published.push_back(std::move(Batch)); }},
			[&](DContentBrowserDataSource&, const FContentChangeBatch&) { ++Projections; });
		Policy.NotifyContentMutation({.bFullRefresh = true});
		Policy.NotifyContentMutation({.Changes = {{EContentChangeKind::Added, {}, (Root / "new.txt").generic_string()}}});
		ASSERT_EQ(2u, Published.size());
		EXPECT_TRUE(Published[0].bFullRefresh);
		EXPECT_FALSE(Published[1].bFullRefresh);
		ASSERT_EQ(1u, Published[1].Changes.size());
		EXPECT_EQ(EContentChangeKind::Added, Published[1].Changes.front().Kind);
		EXPECT_EQ(2, Projections);
		Source->Shutdown();
		Policy.NotifyContentMutation({.bFullRefresh = true});
		EXPECT_EQ(2, Projections);
		EXPECT_EQ(3u, Published.size());
	}
	TEST_F(FContentBrowserDataTests, PublicCopyOverridesAndSingleItemFallbackNeedNoPlanner)
	{
		auto* Source = NewObject<FProbeContentSource>(Data, {});
		Extras.emplace_back(Source); Source->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		FContentBrowserItemData A(*Source, EContentBrowserItemFlags::Type_File, "/Probe/A", "A", {}, {});
		FContentBrowserItemData B(*Source, EContentBrowserItemFlags::Type_File, "/Probe/B", "B", {}, {});
		FContentBrowserItem Item{.InternalItems = {A}};
		EXPECT_TRUE(Item.Copy("/Probe/Target"));
		EXPECT_TRUE(Data->BulkCopyItems(std::array{A, B}, "/Probe/Target"));
		EXPECT_EQ(1, Source->Copies);
		Source->UseSingleItemCopy = true;
		EXPECT_TRUE(Source->BulkCopyItems(std::array{A, B}, "/Probe/Target"));
		EXPECT_EQ(3, Source->SingleCopies);
		Source->UnregisterDataSource();
		EXPECT_FALSE(Data->BulkCopyItems(std::array{A, B}, "/Probe/Target"));
		EXPECT_EQ(3, Source->SingleCopies);
	}
	TEST_F(FContentBrowserDataTests, EditAndDuplicateDispatchToEveryContributionAndPermitAliasedOutput)
	{
		auto* First = NewObject<FProbeContentSource>(Data, {});
		auto* Second = NewObject<FProbeContentSource>(Data, {});
		Extras.emplace_back(First); Extras.emplace_back(Second);
		First->Name = "First"; Second->Name = "Second";
		First->Initialize(); Second->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("First")));
		ASSERT_TRUE(Data->ActivateDataSource(FName("Second")));
		Second->bEditable = true;
		FContentBrowserItem Item{.InternalItems = {
			FContentBrowserItemData(*First, EContentBrowserItemFlags::Type_File, "/Shared/A", "A", {}, {}),
			FContentBrowserItemData(*Second, EContentBrowserItemFlags::Type_File, "/Shared/A", "A", {}, {})}};
		EXPECT_TRUE(Item.CanEdit()); EXPECT_TRUE(Item.CanDuplicate());
		EXPECT_TRUE(Item.Edit());
		EXPECT_EQ(1, First->Edits); EXPECT_EQ(1, Second->Edits);
		EXPECT_TRUE(Item.Duplicate(Item));
		EXPECT_EQ(1, First->Duplications); EXPECT_EQ(1, Second->Duplications);
		ASSERT_EQ(1u, Item.InternalItems.size());
		EXPECT_EQ(Second, Item.InternalItems.front().GetOwnerDataSource());
		EXPECT_EQ("/Shared/A_Copy", Item.InternalItems.front().GetVirtualPath());
	}
	TEST_F(FContentBrowserDataTests, PublicOperationDoesNotInvokeCapabilityCallbacks)
	{
		auto* Source = NewObject<FProbeContentSource>(Data, {});
		Extras.emplace_back(Source); Source->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		FContentBrowserItem Item{.InternalItems = {FContentBrowserItemData(*Source, EContentBrowserItemFlags::Type_File, "/Probe/A", "A", {}, {})}};
		std::string Target = "/Probe/Original";
		Source->BeforeCopyCheck = [&] { Target = "/Probe/ChangedByCallback"; };
		ASSERT_TRUE(Item.Copy(Target));
		EXPECT_EQ("/Probe/Original", Target);
		EXPECT_EQ("/Probe/Original", Source->LastCopyDestination);
	}
	TEST_F(FContentBrowserDataTests, CapabilityQueryRetainsContributionsAndTargetAcrossReentry)
	{
		auto* Source = NewObject<FProbeContentSource>(Data, {});
		Extras.emplace_back(Source); Source->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		FContentBrowserItem Item{.InternalItems = {
			FContentBrowserItemData(*Source, EContentBrowserItemFlags::Type_File, "/Probe/A", "A", {}, {}),
			FContentBrowserItemData(*Source, EContentBrowserItemFlags::Type_File, "/Probe/B", "B", {}, {})}};
		std::string Target = "/Probe/Original";
		Source->BeforeCopyCheck = [&] { Item.InternalItems.clear(); Target = "/Probe/Changed"; };
		EXPECT_TRUE(Item.CanCopy(Target));
		EXPECT_TRUE(Item.InternalItems.empty());
		ASSERT_EQ(2u, Source->CapabilityDestinations.size());
		for (const auto& Destination : Source->CapabilityDestinations) EXPECT_EQ("/Probe/Original", Destination);
	}
	TEST_F(FContentBrowserDataTests, NativePublicOverrideReceivesOrdinaryBulkDispatch)
	{
		auto* Source = NewObject<FNamedFileSource>(Data, {});
		Extras.emplace_back(Source); Source->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("LooseFiles")));
		const auto Description = ContentBrowser::Private::MakeFixtureItem({.Kind = EContentBrowserItemKind::File,
			.VirtualPath = "/ProviderTests/source.txt", .PhysicalPath = (Root / "source.txt").generic_string()}, false);
		std::ofstream(Root / "second.txt") << "second";
		const auto Second = ContentBrowser::Private::MakeFixtureItem({.Kind = EContentBrowserItemKind::File,
			.VirtualPath = "/ProviderTests/second.txt", .PhysicalPath = (Root / "second.txt").generic_string()}, false);
		const std::array Items{Source->BindItemData(*Description.GetPrimaryInternalItem()), Source->BindItemData(*Second.GetPrimaryInternalItem())};
		ASSERT_TRUE(Data->BulkCopyItems(Items, "/ProviderTests/Target"));
		EXPECT_EQ(1, Source->BulkCalls);
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/source.txt"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/second.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "Unexpected"));
	}
	TEST_F(FContentBrowserDataTests, FileCapabilitiesRejectSelectedPackageAndBulkFiles)
	{
		auto* Source = Data->GetActiveDataSource(FName("File"));
		for (const auto Name : {"package.dasset", "orphan.DBULK"})
		{
			std::ofstream(Root / Name) << "not an ordinary file";
			const auto Description = ContentBrowser::Private::MakeFixtureItem({.Kind = EContentBrowserItemKind::File,
				.VirtualPath = "/ProviderTests/" + std::string(Name), .PhysicalPath = (Root / Name).generic_string()}, false);
			const auto Item = Source->BindItemData(*Description.GetPrimaryInternalItem());
			ASSERT_EQ(Source, Item.GetOwnerDataSource());
			std::string Error;
			EXPECT_FALSE(Source->CanCopyItem(Item, "/ProviderTests/Target", &Error)); EXPECT_FALSE(Error.empty());
			EXPECT_FALSE(Source->CanMoveItem(Item, "/ProviderTests/Target", &Error)); EXPECT_FALSE(Error.empty());
			EXPECT_FALSE(Source->CanRenameItem(Item, std::nullopt, &Error)); EXPECT_FALSE(Error.empty());
		}
		EXPECT_TRUE(Source->CanCopyItem(File(), "/ProviderTests/Target"));
	}
	TEST_F(FContentBrowserDataTests, TransferCapabilitiesRespectFinalTargetMountWithinWritableDirectory)
	{
		std::filesystem::create_directory(Root / "Tree");
		std::ofstream(Root / "Tree/plain.txt") << "source bytes";
		std::filesystem::create_directory(Root / "Target/Tree");
		Testing::RegisterMountPointForTests("/LockedTarget/", (Root / "Target/Tree").generic_string() + "/", true, false);
		for (const auto Name : {FName("Asset"), FName("File")})
		{
			auto* Source = Data->GetActiveDataSource(Name);
			const auto Description = ContentBrowser::Private::MakeFixtureItem({.Kind = EContentBrowserItemKind::Folder,
				.VirtualPath = "/ProviderTests/Tree", .PhysicalPath = (Root / "Tree").generic_string()}, Name == FName("Asset"));
			const auto Item = Source->BindItemData(*Description.GetPrimaryInternalItem());
			std::string Error;
			EXPECT_FALSE(Source->CanCopyItem(Item, "/ProviderTests/Target", &Error)); EXPECT_FALSE(Error.empty());
			EXPECT_FALSE(Source->CanMoveItem(Item, "/ProviderTests/Target", &Error)); EXPECT_FALSE(Error.empty());
		}
		EXPECT_TRUE(std::filesystem::is_empty(Root / "Target/Tree"));
		// A writable nested mount is a valid copy target; File still cannot move
		// across that boundary, even when the destination parent is in its mount.
		Testing::RegisterMountPointForTests("/LockedTarget/", (Root / "Target/Tree").generic_string() + "/", true, true);
		auto* FileSource = Data->GetActiveDataSource(FName("File"));
		const auto Description = ContentBrowser::Private::MakeFixtureItem({.Kind = EContentBrowserItemKind::Folder,
			.VirtualPath = "/ProviderTests/Tree", .PhysicalPath = (Root / "Tree").generic_string()}, false);
		const auto Item = FileSource->BindItemData(*Description.GetPrimaryInternalItem());
		EXPECT_TRUE(FileSource->CanCopyItem(Item, "/ProviderTests/Target"));
		EXPECT_FALSE(FileSource->CanMoveItem(Item, "/ProviderTests/Target"));
		const std::array Inputs{Item};
		EXPECT_FALSE(FileSource->BulkMoveItems(Inputs, "/ProviderTests/Target"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Tree/plain.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "Target/Tree/plain.txt"));
	}
	TEST_F(FContentBrowserDataTests, BindingSharesMetadataAndConjoinsRestrictions)
	{
		for (const auto Name : {FName("Asset"), FName("File")})
		{
			auto* Source = Data->GetActiveDataSource(Name);
			const auto Description = ContentBrowser::Private::MakeFixtureItem({.Kind = EContentBrowserItemKind::Folder,
				.VirtualPath = "/ProviderTests/Sub", .PhysicalPath = (Root / "Sub").generic_string()}, Name == FName("Asset"));
			const auto Item = Source->BindItemData(*Description.GetPrimaryInternalItem(), {.CanMutateContent = [] { return false; }});
			const auto Shared = Source->BindItemData(Item);
			EXPECT_EQ(Item.GetPayload(), Shared.GetPayload());
			EXPECT_EQ(Item.GetContributionKey(), Shared.GetContributionKey());
			EXPECT_FALSE(Source->CanRenameItem(Shared));
			int Reports = 0;
			const auto FeedbackOnly = Source->BindItemData(Item, {.ReportDiagnostic = [&](ELogLevel, std::string_view) { ++Reports; }});
			EXPECT_FALSE(Source->CanRenameItem(FeedbackOnly));
			EXPECT_FALSE(Source->CopyItem(FeedbackOnly, "/ProviderTests/Target"));
			EXPECT_GT(Reports, 0);
			const auto Replaced = Source->BindItemData(Item, {.CanMutateContent = [] { return true; }});
			EXPECT_NE(Item.GetPayload(), Replaced.GetPayload());
			EXPECT_FALSE(Source->CanRenameItem(Replaced));
			EXPECT_FALSE(Source->CanRenameItem(Item));
			FContentBrowserDataCompiledFilter Filter; Source->CompileFilter("/", {}, Filter);
			int Count = 0;
			Source->EnumerateItemsMatchingFilter(Filter, [&](FContentBrowserItemData&& Row) {
				EXPECT_EQ(Row.GetPayload(), Source->BindItemData(Row).GetPayload()); ++Count; return false;
			});
			EXPECT_EQ(1, Count);
			ASSERT_TRUE(Data->DeactivateDataSource(Name));
			ASSERT_TRUE(Data->ActivateDataSource(Name));
			EXPECT_EQ(Source, Source->BindItemData(Item).GetOwnerDataSource());
		}
	}
	TEST_F(FContentBrowserDataTests, RenameCapabilitySeparatesPermissionFromProposedName)
	{
		auto* Source = Data->GetActiveDataSource(FName("File"));
		const auto Writable = File();
		EXPECT_TRUE(Source->CanRenameItem(Writable));
		EXPECT_FALSE(Source->CanRenameItem(Writable, "../invalid"));
		EXPECT_TRUE(Source->CanRenameItem(Writable, "renamed.txt"));
		std::filesystem::create_directory(Root / "ReadOnly");
		std::ofstream(Root / "ReadOnly/same.txt") << "readonly";
		Testing::RegisterMountPointForTests("/ReadOnlyProviderTests/", (Root / "ReadOnly").generic_string() + "/", true, false);
		const auto Description = ContentBrowser::Private::MakeFixtureItem({.Kind = EContentBrowserItemKind::File,
			.VirtualPath = "/ReadOnlyProviderTests/same.txt", .PhysicalPath = (Root / "ReadOnly/same.txt").generic_string()}, false);
		const auto ReadOnly = Source->BindItemData(*Description.GetPrimaryInternalItem());
		ASSERT_EQ(Source, ReadOnly.GetOwnerDataSource());
		EXPECT_FALSE(Source->CanRenameItem(ReadOnly));
		EXPECT_FALSE(Source->CanRenameItem(ReadOnly, "same.txt"));
		FContentBrowserItemData Out;
		// A no-op performs no write, but is not a capability grant for entering rename.
		EXPECT_TRUE(Source->RenameItem(ReadOnly, "same.txt", Out));
		const auto RootDescription = ContentBrowser::Private::MakeFixtureItem({.Kind = EContentBrowserItemKind::Folder,
			.VirtualPath = "/ReadOnlyProviderTests", .PhysicalPath = (Root / "ReadOnly").generic_string()}, false);
		EXPECT_FALSE(Source->CanRenameItem(Source->BindItemData(*RootDescription.GetPrimaryInternalItem())));
	}
	TEST_F(FContentBrowserDataTests, TransferCapabilitiesDeferRecursiveValidationUntilExecution)
	{
		std::filesystem::create_directory(Root / "Tree");
		std::ofstream(Root / "Tree/unregistered.dasset") << "not a registered package";
		auto* Source = Data->GetActiveDataSource(FName("File"));
		const auto Description = ContentBrowser::Private::MakeFixtureItem({.Kind = EContentBrowserItemKind::Folder,
			.VirtualPath = "/ProviderTests/Tree", .PhysicalPath = (Root / "Tree").generic_string()}, false);
		const auto Folder = Source->BindItemData(*Description.GetPrimaryInternalItem());
		ASSERT_TRUE(Source->CanCopyItem(Folder, "/ProviderTests/Target"));
		ASSERT_TRUE(Source->CanMoveItem(Folder, "/ProviderTests/Target"));
		const std::array Inputs{Folder};
		EXPECT_FALSE(Data->BulkCopyItems(Inputs, "/ProviderTests/Target"));
		EXPECT_FALSE(Data->BulkMoveItems(Inputs, "/ProviderTests/Target"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Tree/unregistered.dasset"));
		EXPECT_FALSE(std::filesystem::exists(Root / "Target/Tree"));
	}
	TEST_F(FContentBrowserDataTests, ProviderEnumerationReturnsOwnedContributionsAndHonorsEarlyStop)
	{
		auto* Probe = NewObject<FProbeContentSource>(Data, {});
		Extras.emplace_back(Probe); Probe->Kind = EContentBrowserItemKind::Folder;
		Probe->ItemPath = "/ProviderTests/Sub"; Probe->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		EXPECT_EQ(nullptr, Probe->BindItemData(File()).GetOwnerDataSource());
		for (const auto Name : {FName("Asset"), FName("File")})
		{
			auto* Source = Data->GetActiveDataSource(Name);
			FContentBrowserDataCompiledFilter Filter;
			Source->CompileFilter("/", {}, Filter);
			int Count = 0;
			Source->EnumerateItemsMatchingFilter(Filter, [&](FContentBrowserItemData&& Item) {
				EXPECT_EQ(Source, Item.GetOwnerDataSource());
				EXPECT_FALSE(Item.GetVirtualPath().empty());
				++Count;
				return false;
			});
			EXPECT_EQ(1, Count);
		}
		const auto Items = Enumerate();
		const auto Folder = std::ranges::find_if(Items, [](const auto& Item) { return Item.GetVirtualPath() == "/ProviderTests/Sub"; });
		ASSERT_NE(Folder, Items.end());
		EXPECT_EQ(2u, Folder->GetInternalItems().size());
	}
	TEST_F(FContentBrowserDataTests, ProvidersEnumerateOpaqueItemsAndMergeFolders)
	{
		auto Items = Enumerate();
		auto Folder = std::ranges::find_if(Items, [](const auto& Item) { return TestPhysicalPath(Item).ends_with("Sub"); });
		ASSERT_NE(Items.end(), Folder);
		EXPECT_EQ(1u, Folder->InternalItems.size());
		auto Item = File();
		ASSERT_NE(nullptr, Item.GetOwnerDataSource());
		EXPECT_EQ(FName("File"), Item.GetOwnerDataSource()->GetDataSourceName());
		EXPECT_NE(nullptr, Item.GetPayload());
	}
	TEST_F(FContentBrowserDataTests, WithdrawalStopsDispatchUntilProviderIsActive)
	{
		auto Item = File();
		auto* Owner = Item.GetOwnerDataSource();
		ASSERT_NE(nullptr, Owner);
		ASSERT_TRUE(Data->DeactivateDataSource(FName("File")));
		EXPECT_EQ(Owner, Item.GetOwnerDataSource());
		EXPECT_FALSE(Data->BulkCopyItems(std::array{Item}, "/ProviderTests/Target/"));
		EXPECT_FALSE(std::filesystem::exists(Root / "Target/source.txt"));
		ASSERT_TRUE(Data->ActivateDataSource(FName("File")));
		EXPECT_EQ(Owner, Owner->BindItemData(Item).GetOwnerDataSource());
		Owner->UnregisterDataSource();
		EXPECT_FALSE(Owner->IsActive());
		EXPECT_FALSE(Data->BulkCopyItems(std::array{Item}, "/ProviderTests/Target/"));
	}
	TEST_F(FContentBrowserDataTests, RoutesCopyAndRenameToTheOwningProvider)
	{
		auto Item = File();
		ASSERT_NE(nullptr, Item.GetOwnerDataSource());
		const std::array Items{Item};
		EXPECT_TRUE(Data->BulkCopyItems(Items, "/ProviderTests/Target/"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/source.txt"));
		FContentBrowserItemData Renamed;
		EXPECT_TRUE(Item.GetOwnerDataSource()->RenameItem(Item, "renamed.txt", Renamed));
		EXPECT_TRUE(std::filesystem::exists(Root / "renamed.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "source.txt"));
		EXPECT_EQ(Item.GetOwnerDataSource(), Renamed.GetOwnerDataSource());
	}
	TEST_F(FContentBrowserDataTests, ConfirmedDeleteRejectsStaleScopeAndSupportsRetry)
	{
		auto Item = File();
		auto* Owner = Item.GetOwnerDataSource();
		ASSERT_NE(nullptr, Owner);
		const std::array Items{Item};
		auto Context = Owner->PrepareDeleteItems(Items);
		ASSERT_NE(nullptr, Context.Request);
		ASSERT_TRUE(Context.CanExecute());
		EXPECT_FALSE(Data->BulkDeleteItems({}, Context));
		std::ofstream(Root / "source.txt", std::ios::app) << "changed";
		EXPECT_FALSE(Data->BulkDeleteItems(Items, Context));
		EXPECT_TRUE(std::filesystem::exists(Root / "source.txt"));
		EXPECT_TRUE(Data->BulkDeleteItems(Items, Context));
		EXPECT_FALSE(std::filesystem::exists(Root / "source.txt"));
		EXPECT_EQ(nullptr, Context.Request);
	}
	TEST_F(FContentBrowserDataTests, ShutdownRejectsDirectProviderOperations)
	{
		auto Item = File();
		auto* Source = Item.GetOwnerDataSource();
		ASSERT_NE(nullptr, Source);
		Editor->RequestShutdown();
		EXPECT_EQ(Source, Item.GetOwnerDataSource());
		FContentBrowserItemData Out;
		EXPECT_FALSE(Source->RenameItem(Item, "other.txt", Out));
		EXPECT_TRUE(std::filesystem::exists(Root / "source.txt"));
	}
	TEST_F(FContentBrowserDataTests, AdditionalProviderWorksWithoutBuiltinDispatchChanges)
	{
		TStrongObjectPtr<FProbeContentSource> Probe(NewObject<FProbeContentSource>(Editor.Get(), {}));
		Extras.emplace_back(Probe.Get());
		Probe->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		std::vector<FContentBrowserItem> Items;
		Data->EnumerateItemsUnderPath("/Probe/", {}, [&](FContentBrowserItem&& Item) {
			if (TestVirtualPath(Item) == "/Probe/Item") Items.push_back(std::move(Item));
			return true;
		});
		ASSERT_EQ(1u, Items.size());
		EXPECT_TRUE(Data->BulkCopyItems(Items.front().InternalItems, "/unused"));
		EXPECT_EQ(1, Probe->Copies);
		auto FileItem = File();
		std::array Mixed{Items.front().InternalItems.front(), FileItem};
		EXPECT_TRUE(Data->BulkCopyItems(Mixed, "/unused"));
		EXPECT_EQ(2, Probe->Copies);
		Probe->Shutdown();
	}
	TEST_F(FContentBrowserDataTests, CompiledScopesFollowCurrentMountsAfterReplacementAndRestoration)
	{
		FContentBrowserDataCompiledFilter Filter;
		Data->CompileFilter("/ProviderTests/", {}, Filter);
		int Rows = 0;
		auto Collect = [&](FContentBrowserItem&&) { ++Rows; return true; };
		{
			Testing::FScopedMountRegistryFixture Temporary;
			Data->EnumerateItemsMatchingFilter(Filter, Collect);
			EXPECT_EQ(0, Rows);
		}
		Data->EnumerateItemsMatchingFilter(Filter, Collect);
		EXPECT_GT(Rows, 0);
	}
	TEST_F(FContentBrowserDataTests, InvalidatedPayloadReleasesWhenItsLastSnapshotIsReleased)
	{
		struct FPayload final : IContentBrowserItemDataPayload
		{
		};
		auto Payload = std::make_shared<FPayload>();
		std::weak_ptr<FPayload> Weak = Payload;
		auto* Source = Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(nullptr, Source);
		FContentBrowserItemData Item(*Source, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/opaque", {}, {}, Payload);
		Payload.reset();
		EXPECT_FALSE(Weak.expired());
		Data->DeactivateDataSource(FName("File"));
		EXPECT_EQ(Source, Item.GetOwnerDataSource());
		Item = {};
		EXPECT_TRUE(Weak.expired());
	}
	TEST_F(FContentBrowserDataTests, RetirementInMutationPolicyCannotAuthorizeFileWrites)
	{
		auto* Source = Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(nullptr, Source);
		FContentBrowserItem Item = Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::File, .VirtualPath = "/ProviderTests/source.txt", .PhysicalPath = (Root / "source.txt").generic_string()};
		FContentBrowserOperationContext Host;
		Host.CanMutateContent = [&] { Data->DeactivateDataSource(FName("File")); return true; };
		auto Handle = Source->BindItemData(*Item.GetPrimaryInternalItem(), Host);
		FContentBrowserItemData Out;
		EXPECT_FALSE(Source->RenameItem(Handle, "retired.txt", Out));
		EXPECT_TRUE(std::filesystem::exists(Root / "source.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "retired.txt"));
	}
	TEST_F(FContentBrowserDataTests, LogicalFoldersMergeContributorsWithDifferentPhysicalPayloads)
	{
		TStrongObjectPtr<FProbeContentSource> Probe(NewObject<FProbeContentSource>(Editor.Get(), {}));
		Extras.emplace_back(Probe.Get());
		Probe->Kind = EContentBrowserItemKind::Folder;
		Probe->ItemPath = "/ProviderTests/Sub";
		Probe->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		auto Items = Enumerate();
		const auto Folder = std::ranges::find_if(Items, [](const auto& Item) { return TestVirtualPath(Item) == "/ProviderTests/Sub"; });
		ASSERT_NE(Items.end(), Folder);
		EXPECT_EQ(2u, Folder->InternalItems.size());
	}
	TEST_F(FContentBrowserDataTests, ItemFacadeRejectsInPlaceCopyAndExactPathQueriesReturnOnlyTheirTarget)
	{
		std::vector<FContentBrowserItem> Exact;
		Data->EnumerateItemsAtPath("/ProviderTests/source.txt", [&](FContentBrowserItem&& Item) { Exact.push_back(std::move(Item)); return true; });
		ASSERT_EQ(1u, Exact.size());
		EXPECT_EQ("/ProviderTests/source.txt", Exact.front().InternalItems.front().GetVirtualPath());
		EXPECT_FALSE(Exact.front().CanCopy("/ProviderTests/"));
		EXPECT_FALSE(Exact.front().Copy("/ProviderTests/"));
		size_t Files = 0;
		for (const auto& Entry : std::filesystem::directory_iterator(Root))
			if (Entry.path().extension() == ".txt") ++Files;
		EXPECT_EQ(1u, Files);
		FContentBrowserItem Renamed;
		EXPECT_TRUE(Exact.front().Rename("facade.txt", Renamed));
		EXPECT_EQ("/ProviderTests/facade.txt", TestVirtualPath(Renamed));
		EXPECT_TRUE(Renamed.Move("/ProviderTests/Target/"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/facade.txt"));
	}
	TEST_F(FContentBrowserDataTests, HostMutationPolicyIsRevalidatedAfterCapabilityQueries)
	{
		auto* Source = Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(nullptr, Source);
		FContentBrowserItem Item = Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "source.txt").generic_string()};
		bool Allowed = true;
		FContentBrowserOperationContext Host;
		Host.CanMutateContent = [&] { return Allowed; };
		auto Handle = Source->BindItemData(*Item.GetPrimaryInternalItem(), Host);
		EXPECT_TRUE(Source->CanRenameItem(Handle, "changed.txt"));
		Allowed = false;
		FContentBrowserItemData Out;
		EXPECT_FALSE(Source->RenameItem(Handle, "changed.txt", Out));
		EXPECT_TRUE(std::filesystem::exists(Root / "source.txt"));
	}
	TEST_F(FContentBrowserDataTests, OneProviderBatchesDifferentCapturesAndRechecksEveryMutationPolicy)
	{
		std::ofstream(Root / "second.txt") << "second";
		auto* Source = Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(nullptr, Source);
		bool Allowed = true;
		FContentBrowserOperationContext Host;
		Host.CanMutateContent = [&] { return Allowed; };
		FContentBrowserItem First = Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "source.txt").generic_string()};
		FContentBrowserItem Second = Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "second.txt").generic_string()};
		const std::array Handles{Source->BindItemData(*First.GetPrimaryInternalItem()), Source->BindItemData(*Second.GetPrimaryInternalItem(), Host)};
		Allowed = false;
		EXPECT_FALSE(Data->BulkCopyItems(Handles, "/ProviderTests/Target/"));
		EXPECT_FALSE(std::filesystem::exists(Root / "Target/source.txt"));
		Allowed = true;
		EXPECT_TRUE(Data->BulkCopyItems(Handles, "/ProviderTests/Target/"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/source.txt"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/second.txt"));
	}
	TEST_F(FContentBrowserDataTests, DuplicateSourceNamesCannotReplaceAnActiveProvider)
	{
		auto* Original = Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(nullptr, Original);
		TStrongObjectPtr<FProbeContentSource> Duplicate(NewObject<FProbeContentSource>(Editor.Get(), {}));
		Extras.emplace_back(Duplicate.Get());
		Duplicate->Name = "File";
		Duplicate->Initialize();
		EXPECT_FALSE(Data->ActivateDataSource(FName("File")));
		EXPECT_EQ(Original, Data->GetActiveDataSource(FName("File")));
		Duplicate->Shutdown();
		EXPECT_TRUE(Data->ActivateDataSource(FName("File")));
	}
	TEST_F(FContentBrowserDataTests, CommittedOperationsPublishIncrementalUpdatesAndFailuresPublishNothing)
	{
		auto Handle = File();
		auto* Source = Handle.GetOwnerDataSource();
		ASSERT_NE(nullptr, Source);
		const std::array Items{Handle};
		EXPECT_TRUE(Data->BulkCopyItems(Items, "/ProviderTests/Target/"));
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.0f));
		EXPECT_TRUE(std::ranges::any_of(PublishedUpdates, [](const auto& Update) {
			return Update.GetUpdateType() == EContentBrowserItemUpdateType::Added
				   && Update.GetItemData().GetVirtualPath() == "/ProviderTests/Target/source.txt";
		}));
		FContentBrowserItemData Out;
		EXPECT_TRUE(Source->RenameItem(Handle, "delta.txt", Out));
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.0f));
		EXPECT_TRUE(std::ranges::any_of(PublishedUpdates, [](const auto& Update) {
			return Update.GetUpdateType() == EContentBrowserItemUpdateType::Moved
				   && Update.GetPreviousVirtualPath() == "/ProviderTests/source.txt"
				   && Update.GetItemData().GetVirtualPath() == "/ProviderTests/delta.txt";
		}));
		// Successful writes restart the independent filesystem baseline. Wait for
		// that publication before attributing later notifications to a rejected write.
		for (int I = 0; I < 100; ++I) { (PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.5f)); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
		int Events = 0;
		const auto Changed = Data->OnItemDataUpdated().AddLambda([&](auto) { ++Events; });
		FContentBrowserItemData Invalid;
		EXPECT_FALSE(Source->RenameItem(Out, "", Invalid));
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.0f));
		EXPECT_EQ(0, Events);
		Data->OnItemDataUpdated().Remove(Changed);
		EXPECT_TRUE(PublishedUpdates.empty());
	}
	TEST_F(FContentBrowserDataTests, RetiringAProviderDropsUndeliveredCapturedItems)
	{
		std::ofstream(Root / "second.txt") << "second";
		size_t Files = 0;
		Data->EnumerateItemsUnderPath("/ProviderTests/", {}, [&](FContentBrowserItem&& Item) {
			if (TestKind(Item) == EContentBrowserItemKind::File)
			{
				++Files;
				Data->DeactivateDataSource(FName("File"));
			}
			return true;
		});
		EXPECT_EQ(1u, Files);
	}
	TEST_F(FContentBrowserDataTests, BatchDispatchRechecksWithdrawalAfterAnotherProviderCallback)
	{
		TStrongObjectPtr<FProbeContentSource> A(NewObject<FProbeContentSource>(Editor.Get(), {}));
		TStrongObjectPtr<FProbeContentSource> B(NewObject<FProbeContentSource>(Editor.Get(), {}));
		Extras.emplace_back(A.Get());
		Extras.emplace_back(B.Get());
		A->Name = "SaveA";
		B->Name = "SaveB";
		A->Initialize();
		B->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("SaveA")));
		ASSERT_TRUE(Data->ActivateDataSource(FName("SaveB")));
		A->BeforeSave = [&] { Data->DeactivateDataSource(FName("SaveB")); };
		B->BeforeSave = [&] { Data->DeactivateDataSource(FName("SaveA")); };
		const std::array Handles{
			FContentBrowserItemData(*A, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/SaveA/Item", {}, {}, nullptr),
			FContentBrowserItemData(*B, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/SaveB/Item", {}, {}, nullptr)
		};
		const auto Result = Data->BulkSaveItems(Handles, EContentBrowserItemSaveFlags::None);
		EXPECT_TRUE(Result);
		EXPECT_EQ(1, A->Saves);
		EXPECT_EQ(0, B->Saves);
		A->BeforeSave = {};
		B->BeforeSave = {};
	}
	TEST_F(FContentBrowserDataTests, RegistrationRetainsAnInstanceUntilWithdrawalAndMembershipRelease)
	{
		auto* Source = NewObject<FProbeContentSource>(Editor.Get(), {});
		TWeakObjectPtr<FProbeContentSource> Weak(Source);
		Source->Initialize();
		CollectGarbage(EObjectFlags::Standalone);
		ASSERT_NE(nullptr, Weak.Get());
		EXPECT_TRUE(Data->ActivateDataSource(FName("Probe")));
		Source->Shutdown();
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.0f));
		CollectGarbage(EObjectFlags::Standalone);
		EXPECT_EQ(nullptr, Weak.Get());
	}
	TEST_F(FContentBrowserDataTests, AssetSaveFlagsUseTheExistingPackagePolicy)
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/SaveAsset", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(nullptr, Asset);
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		ASSERT_NE(nullptr, Source);
		FContentBrowserItem Item = Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		const auto Handle = Source->BindItemData(*Item.GetPrimaryInternalItem());
		const std::array Items{Handle};
		EXPECT_TRUE(Source->CanSaveItem(Handle, EContentBrowserItemSaveFlags::SaveOnlyIfDirty));
		EXPECT_TRUE(Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::SaveOnlyIfDirty));
		Asset->MarkPackageDirty();
		EXPECT_TRUE(Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::SaveOnlyIfDirty | EContentBrowserItemSaveFlags::SaveOnlyIfLoaded));
		EXPECT_FALSE(Asset->GetPackage()->IsDirty());
		EXPECT_TRUE(std::filesystem::exists(Root / "SaveAsset.dasset"));
		EXPECT_TRUE(Source->CanSaveItem(Handle, EContentBrowserItemSaveFlags::SaveOnlyIfLoaded));
		EXPECT_TRUE(Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::SaveOnlyIfLoaded));
		EXPECT_FALSE(Source->CanSaveItem(Handle, static_cast<EContentBrowserItemSaveFlags>(4)));
		EXPECT_TRUE(Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::None));
	}
	TEST_F(FContentBrowserDataTests, BulkSaveContinuesIndependentSourceFailures)
	{
		std::vector<FContentBrowserItemData> Items;
		std::vector<std::string> Order;
		for (const auto* Name : {"SaveC", "SaveB", "SaveA"})
		{
			auto* Source = NewObject<FProbeContentSource>(Editor.Get(), {});
			Extras.emplace_back(Source);
			Source->Name = Name;
			Source->bFailSave = Source->Name == "SaveB";
			Source->BeforeSave = [&, Name] { Order.emplace_back(Name); };
			Source->Initialize();
			ASSERT_TRUE(Data->ActivateDataSource(FName(Name)));
			Items.emplace_back(*Source, EContentBrowserItemFlags::Type_File, "/" + Source->Name + "/Item", "", "", nullptr);
		}
		const auto Result = Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::None);
		EXPECT_EQ((std::vector<std::string>{"SaveC", "SaveB", "SaveA"}), Order);
		EXPECT_TRUE(Result);
	}
	TEST_F(FContentBrowserDataTests, AssetBulkSaveKeepsCommittedPackagesAndContinuesMissingPackage)
	{
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		ASSERT_NE(nullptr, Source);
		std::vector<FContentBrowserItemData> Items;
		int Notifications = 0;
		FContentBrowserOperationContext Host;
		Host.NotifyContentMutation = [&](FContentChangeBatch) { ++Notifications; };
		for (const auto* Name : {"20Last", "10Missing", "00First"})
		{
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/" + std::string(Name), Path));
			std::string VirtualPath = Path.ToString() + "." + Name;
			if (std::string_view(Name) != "10Missing")
			{
				auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
				ASSERT_NE(nullptr, Asset);
				VirtualPath = Asset->GetObjectPath();
			}
			FContentBrowserItem Description = Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = VirtualPath, .PackagePath = Path};
			Items.push_back(Source->BindItemData(*Description.GetPrimaryInternalItem(), Host));
		}
		const auto Result = Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::SaveOnlyIfDirty);
		EXPECT_TRUE(Result);
		EXPECT_EQ(1, Notifications);
		EXPECT_TRUE(std::filesystem::exists(Root / "00First.dasset"));
		EXPECT_TRUE(std::filesystem::exists(Root / "20Last.dasset"));
		EXPECT_FALSE(std::filesystem::exists(Root / "10Missing.dasset"));
		const auto FileData = File();
		EXPECT_FALSE(FileData.GetOwnerDataSource()->CanSaveItem(FileData, EContentBrowserItemSaveFlags::None));
		EXPECT_FALSE(FileData.GetOwnerDataSource()->CanDuplicateItem(FileData));
	}
	TEST_F(FContentBrowserDataTests, BulkSaveStopsUnknownWriteRecoveryAndKeepsEarlierCommit)
	{
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		ASSERT_NE(nullptr, Source);
		std::vector<FContentBrowserItemData> Items;
		for (const auto* Name : {"00Committed", "10Fault", "20Unattempted"})
		{
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/" + std::string(Name), Path));
			auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
			ASSERT_NE(nullptr, Asset);
			FContentBrowserItem Description = Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
			Items.push_back(Source->BindItemData(*Description.GetPrimaryInternalItem()));
		}
		int Stages = 0;
		Testing::FScopedPackageSaveFault Fault([&](auto Point, size_t) {
			return Point == Testing::EPackageSaveFaultPoint::StagePackage && ++Stages == 2;
		});
		const auto Result = Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::SaveOnlyIfDirty);
		EXPECT_TRUE(Result);
		EXPECT_TRUE(std::filesystem::exists(Root / "00Committed.dasset"));
		EXPECT_FALSE(std::filesystem::exists(Root / "20Unattempted.dasset"));
		EXPECT_EQ(2, Stages);
	}
	TEST_F(FContentBrowserDataTests, CopyRetainsEarlierFileCommitAndRetriesOnlyFailedInput)
	{
		std::ofstream(Root / "second.txt") << "second";
		std::ofstream(Root / "Target/second.txt") << "occupied";
		std::vector<FContentBrowserItemData> Inputs;
		for (const auto& Row : Enumerate())
			if (Row.GetVirtualPath().ends_with("source.txt") || Row.GetVirtualPath().ends_with("second.txt")) Inputs.push_back(Row.InternalItems.front());
		ASSERT_EQ(2u, Inputs.size());
		const auto Result = Data->BulkCopyItems(Inputs, "/ProviderTests/Target/");
		EXPECT_TRUE(Result);
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/source.txt"));
		EXPECT_TRUE(std::filesystem::exists(Root / "source.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "Target/second_Copy.txt"));
		const auto Retry = std::ranges::find_if(Inputs, [&](const auto& Item) { return Item.GetVirtualPath().ends_with("second.txt"); });
		ASSERT_NE(Inputs.end(), Retry);
		std::filesystem::remove(Root / "Target/second.txt");
		EXPECT_TRUE(Data->BulkCopyItems(std::span{&*Retry, 1}, "/ProviderTests/Target/"));
	}
	TEST_F(FContentBrowserDataTests, MoveRetainsEarlierCommitAndLeavesConflictingSource)
	{
		std::ofstream(Root / "a.txt") << "a";
		std::ofstream(Root / "b.txt") << "b";
		std::ofstream(Root / "Target/b.txt") << "occupied";
		std::vector<FContentBrowserItemData> Inputs;
		for (const auto& Row : Enumerate()) if (Row.GetVirtualPath().ends_with("a.txt") || Row.GetVirtualPath().ends_with("b.txt")) Inputs.push_back(Row.InternalItems.front());
		const auto Result = Data->BulkMoveItems(Inputs, "/ProviderTests/Target/");
		EXPECT_TRUE(Result);
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/a.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "a.txt"));
		EXPECT_TRUE(std::filesystem::exists(Root / "b.txt"));
	}
	TEST_F(FContentBrowserDataTests, FileFolderDeletionRetainsPackagesForTheirAssetOwner)
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/Sub/Owned", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(nullptr, Asset);
		auto* AssetSource = Data->GetActiveDataSource(FName("Asset"));
		FContentBrowserItem Description = Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		const auto AssetData = AssetSource->BindItemData(*Description.GetPrimaryInternalItem());
		ASSERT_TRUE(Data->BulkSaveItems(std::span{&AssetData, 1}, EContentBrowserItemSaveFlags::SaveOnlyIfDirty));
		std::ofstream(Root / "Sub/note.txt") << "ordinary";
		const auto Rows = Enumerate();
		const auto Folder = std::ranges::find_if(Rows, [](const auto& Row) { return Row.GetVirtualPath() == "/ProviderTests/Sub"; });
		ASSERT_NE(Rows.end(), Folder);
		const auto FileOwner = std::ranges::find_if(Folder->InternalItems, [](const auto& Item) { return Item.GetOwnerDataSource()->GetDataSourceName() == FName("File"); });
		ASSERT_NE(Folder->InternalItems.end(), FileOwner);
		auto* FileSource = FileOwner->GetOwnerDataSource();
		auto Context = FileSource->PrepareDeleteItems(std::span{&*FileOwner, 1});
		ASSERT_TRUE(Context.CanExecute());
		const auto Result = FileSource->BulkDeleteItems(std::span{&*FileOwner, 1}, Context);
		EXPECT_TRUE(Result);
		EXPECT_FALSE(std::filesystem::exists(Root / "Sub/note.txt"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Sub/Owned.dasset"));
		EXPECT_FALSE(Context.CanExecute());
	}
	TEST_F(FContentBrowserDataTests, NativeAssetAndFileBatchSharesOneCopyAndConfirmedDelete)
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/MixedAsset", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(nullptr, Asset);
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		ASSERT_NE(nullptr, Source);
		FContentBrowserItem Item = Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		auto AssetData = Source->BindItemData(*Item.GetPrimaryInternalItem());
		ASSERT_TRUE(Data->BulkSaveItems(std::array{AssetData}, EContentBrowserItemSaveFlags::SaveOnlyIfDirty));
		const auto Rows = Enumerate();
		std::vector<FContentBrowserItemData> Batch;
		for (const auto& Row : Rows)
			if (TestPhysicalPath(Row).ends_with("source.txt") || TestPackagePath(Row) == Path) Batch.insert(Batch.end(), Row.InternalItems.begin(), Row.InternalItems.end());
		ASSERT_EQ(2u, Batch.size());
		EXPECT_NE(Batch[0].GetOwnerDataSource(), Batch[1].GetOwnerDataSource());
		std::ranges::reverse(Batch); // Generic dispatch partitions this mixed batch by source.
		ASSERT_TRUE(Data->BulkCopyItems(Batch, "/ProviderTests/Target/"));
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.0f));
		EXPECT_TRUE(std::ranges::any_of(PublishedUpdates, [](const auto& Update) {
			auto* Owner = Update.GetItemData().GetOwnerDataSource();
			return (static_cast<uint16>(Update.GetItemData().GetItemFlags()) & static_cast<uint16>(EContentBrowserItemFlags::Category_Asset)) != 0 && Owner && Owner->GetDataSourceName() == FName("Asset");
		}));
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/source.txt"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/MixedAsset.dasset"));
		auto Context = Data->PrepareDeleteItems(Batch);
		ASSERT_TRUE(Context.CanExecute());
		const auto Preview = Context.Request->GetPreview();
		ASSERT_NE(nullptr, Preview);
		EXPECT_EQ(1u, Preview->Summary.AssetCount);
		EXPECT_EQ(1u, Preview->Summary.FileCount);
		EXPECT_TRUE(Data->BulkDeleteItems(Batch, Context));
		EXPECT_FALSE(std::filesystem::exists(Root / "source.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "MixedAsset.dasset"));
	}
	TEST_F(FContentBrowserDataTests, NativeFolderTemporaryContextValidatesNamesAndStopsAfterWithdrawal)
	{
		FContentBrowserItemDataTemporaryContext Pending;
		ASSERT_TRUE(Data->CreateFolder("/ProviderTests/New Folder", Pending));
		EXPECT_EQ("/ProviderTests/New Folder", Pending.GetItemData().GetVirtualPath());
		EXPECT_NE(static_cast<uint16>(Pending.GetItemData().GetItemFlags()) & static_cast<uint16>(EContentBrowserItemFlags::Temporary_Creation), 0);
		EXPECT_FALSE(std::filesystem::exists(Root / "New Folder"));
		EXPECT_FALSE(Pending.ValidateItem("../escape"));
		FContentBrowserItemData Created;
		EXPECT_FALSE(Pending.FinalizeItem("../escape", Created));
		ASSERT_TRUE(Pending.FinalizeItem("Chosen", Created));
		EXPECT_EQ("/ProviderTests/Chosen", Created.GetVirtualPath());
		EXPECT_NE(nullptr, Created.GetOwnerDataSource());
		EXPECT_EQ(static_cast<uint16>(Created.GetItemFlags()) & static_cast<uint16>(EContentBrowserItemFlags::Temporary_MASK), 0);
		EXPECT_TRUE(std::filesystem::is_directory(Root / "Chosen"));
		EXPECT_FALSE(Pending.FinalizeItem("Again", Created));
		ASSERT_TRUE(Data->CreateFolder("/ProviderTests/Another", Pending));
		ASSERT_TRUE(Data->DeactivateDataSource(FName("Asset")));
		EXPECT_FALSE(Pending.ValidateItem("Late"));
		EXPECT_FALSE(Pending.FinalizeItem("Late", Created));
		EXPECT_FALSE(std::filesystem::exists(Root / "Late"));
	}
	TEST_F(FContentBrowserDataTests, NativeFolderTemporaryContextRetriesNameConflicts)
	{
		FContentBrowserItemDataTemporaryContext Pending;
		ASSERT_TRUE(Data->CreateFolder("/ProviderTests/New Folder", Pending));
		std::filesystem::create_directory(Root / "Taken");
		FContentBrowserItemData Created;
		EXPECT_FALSE(Pending.FinalizeItem("Taken", Created));
		ASSERT_TRUE(Pending.FinalizeItem("Available", Created));
		EXPECT_TRUE(std::filesystem::is_directory(Root / "Available"));

	}
	TEST_F(FContentBrowserDataTests, NonphysicalSourceCreatesItemDataThroughTemporaryContext)
	{
		auto* Source = NewObject<FProbeContentSource>(Editor.Get(), {});
		Extras.emplace_back(Source);
		Source->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		FContentBrowserItemDataTemporaryContext Pending;
		ASSERT_TRUE(Data->CanCreateFolder("/ProbeFolders/New"));
		ASSERT_TRUE(Data->CreateFolder("/ProbeFolders/New", Pending));
		EXPECT_EQ(Source, Pending.GetItemData().GetOwnerDataSource());
		EXPECT_EQ(0, Source->CreatedFolders);
		FContentBrowserItemData Created;
		EXPECT_FALSE(Pending.FinalizeItem("../escape", Created));
		ASSERT_TRUE(Pending.FinalizeItem("Chosen", Created));
		EXPECT_EQ(1, Source->CreatedFolders);
		EXPECT_EQ(Source, Created.GetOwnerDataSource());
		EXPECT_EQ("/ProbeFolders/Chosen", Created.GetVirtualPath());
		std::string Physical;
		EXPECT_FALSE(Created.GetItemPhysicalPath(Physical));
		EXPECT_FALSE(Pending.ValidateItem("Again"));
		EXPECT_FALSE(Pending.FinalizeItem("Again", Created));
		EXPECT_EQ(1, Source->CreatedFolders);
	}

} // namespace

namespace
{
TEST_F(FContentBrowserDataTests, CompiledFiltersKeepSourcePartitionsAndRespectActiveProviders)
{
	std::ofstream(Root / "Sub/nested.txt") << "nested";
	FContentBrowserDataCompiledFilter Compiled;
	Data->CompileFilter("/ProviderTests/", {.Kind = EContentBrowserItemKind::File}, Compiled);
	ASSERT_EQ(Compiled.CompiledFilters.size(), 2u);
	for (const auto &[Source, Filter] : Compiled.CompiledFilters)
		EXPECT_NE(Filter, nullptr);
	std::vector<FContentBrowserItem> Items;
	auto Enumerate = [&]
	{
		Items.clear();
		Data->EnumerateItemsMatchingFilter(Compiled,
										   [&](FContentBrowserItem &&Item)
										   {
											   Items.push_back(std::move(Item));
											   return true;
										   });
	};
	Enumerate();
	ASSERT_EQ(Items.size(), 1u);
	EXPECT_EQ(TestName(Items.front()), "source.txt");
	ASSERT_TRUE(Data->DeactivateDataSource(FName("File")));
	Enumerate(); EXPECT_TRUE(Items.empty());
	ASSERT_TRUE(Data->ActivateDataSource(FName("File")));
	Enumerate(); EXPECT_EQ(1u, Items.size());
	Data->CompileFilter("/ProviderTests/", {.bRecursive = true, .Kind = EContentBrowserItemKind::File}, Compiled);
	Enumerate();
	ASSERT_EQ(Items.size(), 2u);
	EXPECT_TRUE(std::ranges::all_of(Items, [](const auto &Item) { return TestKind(Item) == EContentBrowserItemKind::File; }));
}
} // namespace

namespace
{
	TEST_F(FContentBrowserDataTests, LogicalAndContributionKeysUseVirtualTypeAndSource)
	{
		auto* Asset=Data->GetActiveDataSource(FName("Asset"));
		auto* File=Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(Asset, nullptr);
		ASSERT_NE(File, nullptr);
		FContentBrowserItemData AssetFolder(*Asset, EContentBrowserItemFlags::Type_Folder | EContentBrowserItemFlags::Category_Asset, "/ProviderTests/Sub/", "", "", {});
		FContentBrowserItemData FileFolder(*File, EContentBrowserItemFlags::Type_Folder | EContentBrowserItemFlags::Category_Misc, "/ProviderTests/./Sub", "", "", {});
		EXPECT_EQ(AssetFolder.GetItemKey(), FileFolder.GetItemKey());
		EXPECT_EQ(AssetFolder.GetItemKey().GetHash(), FileFolder.GetItemKey().GetHash());
		EXPECT_NE(AssetFolder.GetContributionKey(), FileFolder.GetContributionKey());
		FContentBrowserItemData First(*Asset, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Asset, "/ProviderTests/Shared", {}, {}, {});
		FContentBrowserItemData Same(*Asset, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Asset, "/ProviderTests/Shared", {}, {}, {});
		FContentBrowserItemData Other(*File, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/ProviderTests/Shared", {}, {}, {});
		EXPECT_EQ(First.GetItemKey(), Same.GetItemKey());
		EXPECT_NE(First.GetItemKey(), Other.GetItemKey());
		EXPECT_NE(First.GetItemKey(), FContentBrowserItemKey(EContentBrowserItemFlags::Type_Folder, "/ProviderTests/Shared", {}));
		EXPECT_EQ(FContentBrowserItemDataKey(EContentBrowserItemFlags::Type_Folder, "/ProviderTests/Sub"),
			FContentBrowserItemDataKey(EContentBrowserItemFlags::Type_Folder | EContentBrowserItemFlags::Category_Asset | EContentBrowserItemFlags::Temporary_Creation, "/ProviderTests/Sub/"));
		EXPECT_NE(FContentBrowserItemDataKey(EContentBrowserItemFlags::Type_Folder, "/ProviderTests/Sub"),
			FContentBrowserItemDataKey(EContentBrowserItemFlags::Type_Folder, "/ProviderTests/sub"));
		const auto Logical=FileFolder.GetItemKey();
		const auto Captured=FileFolder.GetContributionKey();
		ASSERT_TRUE(Data->DeactivateDataSource(FName("File")));
		ASSERT_TRUE(Data->ActivateDataSource(FName("File")));
		EXPECT_EQ(FileFolder.GetItemKey(), Logical);
		EXPECT_EQ(FileFolder.GetContributionKey(), Captured);
		EXPECT_EQ(FileFolder.GetOwnerDataSource(), File);
		EXPECT_EQ(FContentBrowserItemData(*File, EContentBrowserItemFlags::Type_Folder | EContentBrowserItemFlags::Category_Misc, "/ProviderTests/Sub", "", "", {}).GetItemKey(), Logical);
		EXPECT_EQ(FContentBrowserItemData(*File, EContentBrowserItemFlags::Type_Folder | EContentBrowserItemFlags::Category_Misc, "/ProviderTests/Sub", "", "", {}).GetContributionKey(), Captured);
		std::unordered_set<FContentBrowserItemKey, FContentBrowserItemKeyHash> Keys{First.GetItemKey(), Same.GetItemKey(), Other.GetItemKey()};
		EXPECT_EQ(Keys.size(), 2u);
	}
}

namespace
{
	TEST_F(FContentBrowserDataTests, NativeAttributesPreservePrecisionAndMetadataAcrossDeactivation)
	{
		auto Item=File();
		FContentBrowserItemDataAttributeValue Value;
		ASSERT_TRUE(Item.GetItemAttribute(ContentBrowserItemAttributes::ItemDiskSize, Value));
		ASSERT_NE(Value.GetValue<uint64>(), nullptr);
		EXPECT_EQ(*Value.GetValue<uint64>(), std::filesystem::file_size(Root/"source.txt"));
		EXPECT_FALSE(Value.GetMetaData().has_value());
		ASSERT_TRUE(Item.GetItemAttribute(ContentBrowserItemAttributes::ItemDiskSize, Value, true));
		ASSERT_TRUE(Value.GetMetaData().has_value());
		EXPECT_EQ(Value.GetMetaData()->Suffix, "bytes");
		FContentBrowserItemDataAttributeValues Values;
		ASSERT_TRUE(Item.GetItemAttributes(Values));
		EXPECT_TRUE(Values.contains(std::string(ContentBrowserItemAttributes::LastWriteTime)));
		std::string Physical;
		ASSERT_TRUE(Item.GetItemPhysicalPath(Physical));
		EXPECT_EQ(Physical, (Root/"source.txt").generic_string());
		EXPECT_FALSE(Item.GetItemAttribute("Missing", Value));
		EXPECT_FALSE(Value.IsValid());
		ASSERT_TRUE(Data->DeactivateDataSource(FName("File")));
		Value=FContentBrowserItemDataAttributeValue("stale"); Physical="stale";
		EXPECT_TRUE(Item.GetItemAttribute(ContentBrowserItemAttributes::ItemDiskSize, Value));
		EXPECT_TRUE(Value.IsValid());
		EXPECT_TRUE(Item.GetItemAttributes(Values)); EXPECT_FALSE(Values.empty());
		EXPECT_TRUE(Item.GetItemPhysicalPath(Physical)); EXPECT_EQ((Root / "source.txt").generic_string(), Physical);
	}
	TEST_F(FContentBrowserDataTests, NonphysicalAttributesForwardTypedMetadata)
	{
		auto* Source=NewObject<FProbeContentSource>(Editor.Get(), {});
		Extras.emplace_back(Source); Source->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		FContentBrowserItem Item;
		Item.InternalItems.emplace_back(*Source, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/Probe/Item", std::string{}, std::string{}, nullptr);
		FContentBrowserItemDataAttributeValue Value;
		ASSERT_TRUE(Item.GetItemAttribute("ProbeCounter", Value, true));
		ASSERT_NE(Value.GetValue<uint64>(), nullptr);
		EXPECT_EQ(*Value.GetValue<uint64>(), std::numeric_limits<uint64>::max()-1);
		ASSERT_TRUE(Value.GetMetaData().has_value());
		FContentBrowserItemDataAttributeValues Values;
		ASSERT_TRUE(Item.GetItemAttributes(Values));
		EXPECT_EQ(Values.size(), 1u);
		std::string Physical="stale";
		EXPECT_FALSE(Item.GetItemPhysicalPath(Physical)); EXPECT_TRUE(Physical.empty());
	}
}

namespace
{
	TEST_F(FContentBrowserDataTests, AttributeValuesKeepScalarTypesAndTreatNullTextAsAbsent)
	{
		FContentBrowserItemDataAttributeValue Signed(-7), Unsigned(7u), Boolean(true), Text("text"), Absent(static_cast<const char*>(nullptr));
		ASSERT_NE(Signed.GetValue<int64>(), nullptr); EXPECT_EQ(*Signed.GetValue<int64>(), -7);
		ASSERT_NE(Unsigned.GetValue<uint64>(), nullptr); EXPECT_EQ(*Unsigned.GetValue<uint64>(), 7u);
		ASSERT_NE(Boolean.GetValue<bool>(), nullptr); EXPECT_TRUE(*Boolean.GetValue<bool>());
		ASSERT_NE(Text.GetValue<std::string>(), nullptr); EXPECT_EQ(*Text.GetValue<std::string>(), "text");
		EXPECT_EQ(Text.GetValue<bool>(), nullptr); EXPECT_FALSE(Absent.IsValid());
	}
}

namespace
{
	TEST_F(FContentBrowserDataTests, ItemDataOwnsNamesFlagsAndDescriptionAfterWithdrawal)
	{
		auto* Source=Data->GetActiveDataSource(FName("File")); ASSERT_NE(Source, nullptr);
		FContentBrowserItemData Description(*Source, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc | EContentBrowserItemFlags::Temporary_Creation, "/ProviderTests/StorageName", "InternalName", "Visible name", nullptr);
		FContentBrowserItemData Folder(*Source, EContentBrowserItemFlags::Type_Folder | EContentBrowserItemFlags::Category_Misc, "/ProviderTests/Sub/", std::string{}, std::string{}, nullptr);
		EXPECT_EQ(Folder.GetItemType(), EContentBrowserItemFlags::Type_Folder); EXPECT_EQ(Folder.GetItemName(), "Sub");
		EXPECT_EQ(Description.GetItemName(), "InternalName"); EXPECT_EQ(Description.GetDisplayName(), "Visible name");
		EXPECT_NE(static_cast<uint16>(Description.GetItemFlags()) & static_cast<uint16>(EContentBrowserItemFlags::Temporary_MASK), 0);
		EXPECT_EQ(Description.GetItemType(), EContentBrowserItemFlags::Type_File);
		FContentBrowserItem Item; Item.InternalItems.push_back(Description);
		EXPECT_EQ(Item.GetItemName(), "InternalName"); EXPECT_EQ(Item.GetDisplayName(), "Visible name");
		EXPECT_EQ(Item.GetVirtualPath(), "/ProviderTests/StorageName");
		EXPECT_EQ(FContentBrowserItemKey(Item), FContentBrowserItemKey(Description));
		ASSERT_TRUE(Data->DeactivateDataSource(FName("File")));
		EXPECT_EQ(Description.GetOwnerDataSource(), Source); EXPECT_EQ(Description.GetDisplayName(), "Visible name");
	}
}

namespace
{
	TEST_F(FContentBrowserDataTests, MergedNativeFolderRenamesThroughBothOwnersAndRetainsRedirectors)
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/Sub/Example", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(Asset, nullptr);
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		const auto Input = Durin::ContentBrowser::Private::MakeFixtureItem({.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path}, true);
		const std::array Handle{Source->BindItemData(*Input.GetPrimaryInternalItem())};
		ASSERT_TRUE(Data->BulkSaveItems(Handle, EContentBrowserItemSaveFlags::SaveOnlyIfDirty));
		auto Rows = Enumerate();
		auto Folder = std::ranges::find_if(Rows, [](const auto& Item) { return Item.GetItemKey().GetVirtualPath() == "/ProviderTests/Sub"; });
		ASSERT_NE(Folder, Rows.end());
		ASSERT_EQ(Folder->GetInternalItems().size(), 2u);
		EXPECT_NE(Folder->GetInternalItems()[0].GetContributionKey(), Folder->GetInternalItems()[1].GetContributionKey());
		EXPECT_EQ(Folder->GetInternalItems()[0].GetItemKey(), Folder->GetInternalItems()[1].GetItemKey());
		ASSERT_TRUE(Folder->CanRename("Relocated"));
		FContentBrowserItem Renamed;
		ASSERT_TRUE(Folder->Rename("Relocated", Renamed));
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0));
		EXPECT_TRUE(std::ranges::any_of(PublishedUpdates, [](const auto& Update) {
			const auto* Owner = Update.GetItemData().GetOwnerDataSource();
			return Owner && Owner->GetDataSourceName() == FName("File") && Update.GetUpdateType() == EContentBrowserItemUpdateType::Added;
		}));
		// Native package relocation may retain the source directory for redirectors.
		EXPECT_TRUE(std::filesystem::exists(Root / "Relocated/Example.dasset"));
		Rows = Enumerate();
		const auto Result = std::ranges::find_if(Rows, [](const auto& Item) { return Item.GetItemKey().GetVirtualPath() == "/ProviderTests/Relocated"; });
		ASSERT_NE(Result, Rows.end());
		EXPECT_EQ(Result->GetInternalItems().size(), 2u);
	}
	TEST_F(FContentBrowserDataTests, MixedFolderDeleteCommitsPackagesFilesAndDirectoriesThroughTheirOwners)
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/Sub/MixedDelete", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(nullptr, Asset);
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		FContentBrowserItem Item = Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		auto Handle = Source->BindItemData(*Item.GetPrimaryInternalItem());
		ASSERT_TRUE(Data->BulkSaveItems(std::array{Handle}, EContentBrowserItemSaveFlags::SaveOnlyIfDirty));
		std::ofstream(Root / "Sub/delete.txt") << "ordinary bytes";
		const auto Rows = Enumerate();
		const auto Folder = std::ranges::find_if(Rows, [](const auto& Row) { return Row.GetVirtualPath() == "/ProviderTests/Sub"; });
		ASSERT_NE(Folder, Rows.end());
		ASSERT_EQ(2u, Folder->GetInternalItems().size());
		auto Context = Data->PrepareDeleteItems(Folder->GetInternalItems());
		ASSERT_TRUE(Context.CanExecute());
		const auto Result = Data->BulkDeleteItems(Folder->GetInternalItems(), Context);
		ASSERT_TRUE(Result);
		EXPECT_TRUE(Result);
		EXPECT_FALSE(Context.CanExecute());
		EXPECT_FALSE(std::filesystem::exists(Root / "Sub"));
	}

	TEST_F(FContentBrowserDataTests, EditorTicksServiceWithoutViewsAndQueriesNeverTickProviders)
	{
		TStrongObjectPtr<FProbeContentSource> Probe(NewObject<FProbeContentSource>(Editor.Get(), {}));
		Extras.emplace_back(Probe.Get()); Probe->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		FEditorEngineTestAccess::SetStartupState(*Editor, EHostStartupState::Ready);
		ASSERT_EQ(nullptr, Editor->GetWorld());
		for (int I = 0; I < 3; ++I) { (void)Enumerate(); (void)Enumerate(); }
		EXPECT_EQ(0, Probe->Ticks);
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.0f, false));
		EXPECT_EQ(1, Probe->Ticks);
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.0f, true));
		EXPECT_EQ(2, Probe->Ticks);
		Editor->PreExit();
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.0f, false));
		EXPECT_EQ(2, Probe->Ticks);
	}
	TEST_F(FContentBrowserDataTests, ProviderBootstrapCanRetireAndRecreateWithoutRetiringDataService)
	{
		auto Handle = File();
		auto* Original = Handle.GetOwnerDataSource();
		ASSERT_NE(nullptr, Original);
		Editor->RemoveEditorSubsystem(DContentBrowserFileProviderSubsystem::StaticClass());
		EXPECT_EQ(nullptr, Data->GetActiveDataSource(FName("File")));
		EXPECT_EQ(Original, Handle.GetOwnerDataSource());
		EXPECT_TRUE(Data->IsActive());
		ASSERT_NE(nullptr, Editor->AddEditorSubsystem(DContentBrowserFileProviderSubsystem::StaticClass()));
		EXPECT_NE(Original, Data->GetActiveDataSource(FName("File")));
		EXPECT_NE(nullptr, File().GetOwnerDataSource());
		EXPECT_EQ(Original, Handle.GetOwnerDataSource());
	}
	TEST_F(FContentBrowserDataTests, SingleItemFiltersMatchEnumerationAndRejectForeignContributions)
	{
		const auto Folder = ContentBrowser::Private::MakeFixtureItem({.Kind = EContentBrowserItemKind::Folder,
			.Name = "Sub", .VirtualPath = "/ProviderTests/Sub", .PhysicalPath = (Root / "Sub").generic_string()}, true);
		auto* AssetSource = Data->GetActiveDataSource(FName("Asset"));
		const auto EmptyAssetFolder = AssetSource->BindItemData(*Folder.GetPrimaryInternalItem());
		FContentBrowserDataCompiledFilter AllKinds;
		Data->CompileFilter("/ProviderTests/", {}, AllKinds);
		EXPECT_FALSE(AssetSource->DoesItemPassFilter(EmptyAssetFolder, AllKinds));
		std::ofstream(Root / "Sub/nested.txt") << "nested";
		FContentBrowserDataCompiledFilter Compiled;
		Data->CompileFilter("/ProviderTests/", {.Kind = EContentBrowserItemKind::File}, Compiled);
		std::vector<FContentBrowserItem> Items;
		Data->EnumerateItemsUnderPath("/ProviderTests/", {.bRecursive = true}, [&](FContentBrowserItem&& Item) {
			Items.push_back(std::move(Item)); return true;
		});
		for (const auto& Item : Items)
			for (const auto& Contribution : Item.GetInternalItems())
			{
				auto* Owner = Contribution.GetOwnerDataSource();
				ASSERT_NE(Owner, nullptr);
				const bool Expected = Item.GetVirtualPath() == "/ProviderTests/source.txt";
				EXPECT_EQ(Owner->DoesItemPassFilter(Contribution, Compiled), Expected);
				for (const auto& [Other, Filter] : Compiled.CompiledFilters)
					if (Other != Owner) EXPECT_FALSE(Other->DoesItemPassFilter(Contribution, Compiled));
			}
		Data->CompileFilter("/ProviderTests/", {.bRecursive = true, .Kind = EContentBrowserItemKind::File}, Compiled);
		for (const auto& Item : Items)
			for (const auto& Contribution : Item.GetInternalItems())
				EXPECT_EQ(Contribution.GetOwnerDataSource()->DoesItemPassFilter(Contribution, Compiled), !Item.IsFolder() && !Item.IsAsset());
	}

	TEST_F(FContentBrowserDataTests, AssetEventsPublishWithoutViewsAndWithoutReplayingCommittedUpdates)
	{
		FEditorEngineTestAccess::SetStartupState(*Editor, EHostStartupState::Ready);
		FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/NoBrowser", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(nullptr, Asset);
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.0f, false));
		const auto Count = std::ranges::count_if(PublishedUpdates, [&](const auto& Update) { return Update.GetItemData().GetVirtualPath() == Asset->GetObjectPath() && Update.GetUpdateType() == EContentBrowserItemUpdateType::Added; });
		EXPECT_EQ(1, Count);
		const auto Added = std::ranges::find_if(PublishedUpdates, [&](const auto& Update) { return Update.GetItemData().GetVirtualPath() == Asset->GetObjectPath(); });
		ASSERT_NE(Added, PublishedUpdates.end());
		EXPECT_EQ(Added->GetItemData().GetItemName(), "NoBrowser");
		FContentBrowserItemDataAttributeValue Class;
		ASSERT_TRUE(Added->GetItemData().GetItemAttribute(ContentBrowserItemAttributes::Class, Class));
		ASSERT_NE(Class.GetValue<std::string>(), nullptr);
		EXPECT_EQ(*Class.GetValue<std::string>(), "Durin::DObject");
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.0f, false));
		EXPECT_FALSE(std::ranges::any_of(PublishedUpdates, [&](const auto& Update) { return Update.GetItemData().GetVirtualPath() == Asset->GetObjectPath() && Update.GetUpdateType() == EContentBrowserItemUpdateType::Added; }));
		const auto Rows = Enumerate();
		EXPECT_TRUE(std::ranges::any_of(Rows, [&](const auto& Item) { return Item.GetVirtualPath() == Asset->GetObjectPath(); }));
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		FContentBrowserItem Native = ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		auto Handle = Source->BindItemData(*Native.GetPrimaryInternalItem());
		ASSERT_TRUE(Data->BulkSaveItems(std::array{Handle}, EContentBrowserItemSaveFlags::SaveOnlyIfDirty));
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.0f, false));
		EXPECT_FALSE(std::ranges::any_of(PublishedUpdates, [&](const auto& Update) { return Update.GetItemData().GetVirtualPath() == Asset->GetObjectPath() && Update.GetUpdateType() == EContentBrowserItemUpdateType::Added; }));
	}
	TEST_F(FContentBrowserDataTests, AssetEventsPublishParentFolderContributionsOnlyOnMembershipTransitions)
	{
		FEditorEngineTestAccess::SetStartupState(*Editor, EHostStartupState::Ready);
		FPackagePath FirstPath, SecondPath;
		ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/NewParent/First", FirstPath));
		ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/NewParent/Second", SecondPath));
		auto* First = IAssetTools::Get().CreatePackageLeafAssetForTesting(FirstPath, DObject::StaticClass());
		ASSERT_NE(First, nullptr);
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.0f, false));
		auto HasFolder = [&](const auto& Update) {
			return Update.GetItemData().GetItemType() == EContentBrowserItemFlags::Type_Folder
				&& Update.GetItemData().GetVirtualPath() == "/ProviderTests/NewParent";
		};
		const auto Folder = std::ranges::find_if(PublishedUpdates, HasFolder);
		ASSERT_NE(Folder, PublishedUpdates.end());
		EXPECT_EQ(Folder->GetUpdateType(), EContentBrowserItemUpdateType::Added);
		EXPECT_EQ(Folder->GetItemData().GetItemName(), "NewParent");
		const auto FolderData = Folder->GetItemData();
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		FContentBrowserDataCompiledFilter Filter;
		Source->CompileFilter("/ProviderTests/", {}, Filter);
		EXPECT_TRUE(Source->DoesItemPassFilter(Folder->GetItemData(), Filter));
		auto* Second = IAssetTools::Get().CreatePackageLeafAssetForTesting(SecondPath, DObject::StaticClass());
		ASSERT_NE(Second, nullptr);
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.0f, false));
		EXPECT_FALSE(std::ranges::any_of(PublishedUpdates, HasFolder));
		ASSERT_NE(NewObject<DObject>(First->GetPackage(), "Sibling"), nullptr);
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.0f, false));
		const auto Sibling = std::ranges::find_if(PublishedUpdates, [](const auto& Update) {
			return Update.GetItemData().GetVirtualPath() == "/ProviderTests/NewParent/First.Sibling";
		});
		ASSERT_NE(Sibling, PublishedUpdates.end());
		EXPECT_EQ(Sibling->GetItemData().GetItemName(), "Sibling");
		EXPECT_FALSE(std::ranges::any_of(PublishedUpdates, HasFolder));
		PublishedUpdates.clear();
		MarkObjectHierarchyAsGarbage(First->GetPackage());
		MarkObjectHierarchyAsGarbage(Second->GetPackage());
		CollectGarbage(EObjectFlags::Standalone);
		(void)CaptureAssetQuerySnapshot();
		PumpGameThreadDeferredWork(); Editor->Tick(0.0f, false);
		const auto Removed = std::ranges::find_if(PublishedUpdates, HasFolder);
		ASSERT_NE(Removed, PublishedUpdates.end());
		EXPECT_EQ(Removed->GetUpdateType(), EContentBrowserItemUpdateType::Removed);
		EXPECT_FALSE(Source->DoesItemPassFilter(FolderData, Filter));
	}
	TEST_F(FContentBrowserDataTests, FilesystemChangesPublishWithoutViewsAndLaterQueriesSeeCurrentState)
	{
		FEditorEngineTestAccess::SetStartupState(*Editor, EHostStartupState::Ready);
		auto PumpUntil = [&](const auto& Predicate) {
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do { (PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.5f, false)); if (Predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); } while (std::chrono::steady_clock::now() < Deadline);
			return false;
		};
		auto* FileSource = Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(nullptr, FileSource);
		// Wait for File's own baseline, not a projected peer-source refresh.
		bool BaselinePublished = false;
		const auto BaselineHandle = ContentStorage::OnContentDirectoriesChanged().AddLambda(
			[&](const FContentChangeBatch&, const DObject* Origin, ContentStorage::EContentDirectoryChangeReason Reason) {
				if (Origin == FileSource && Reason == ContentStorage::EContentDirectoryChangeReason::Discovery) BaselinePublished = true;
			});
		struct FUnsubscribe { FDelegateHandle Handle; ~FUnsubscribe() { ContentStorage::OnContentDirectoriesChanged().Remove(Handle); } } Subscription{BaselineHandle};
		ASSERT_TRUE(PumpUntil([&] { return BaselinePublished; }));
		const auto Path = Root / "external.txt";
		std::ofstream(Path) << "external bytes";
		auto HasUpdate = [&](auto Type) { return std::ranges::any_of(PublishedUpdates, [&](const auto& Update) { return Update.GetItemData().GetVirtualPath() == "/ProviderTests/external.txt" && Update.GetUpdateType() == Type; }); };
		ASSERT_TRUE(PumpUntil([&] { return HasUpdate(EContentBrowserItemUpdateType::Added); }));
		const auto Added = std::ranges::find_if(PublishedUpdates, [](const auto& Update) { return Update.GetItemData().GetVirtualPath() == "/ProviderTests/external.txt"; });
		ASSERT_NE(Added, PublishedUpdates.end());
		EXPECT_EQ(Added->GetItemData().GetItemName(), "external.txt");
		FContentBrowserItemDataAttributeValue Size, Extension;
		ASSERT_TRUE(Added->GetItemData().GetItemAttribute(ContentBrowserItemAttributes::ItemDiskSize, Size));
		ASSERT_TRUE(Added->GetItemData().GetItemAttribute(ContentBrowserItemAttributes::Extension, Extension));
		ASSERT_NE(Size.GetValue<uint64>(), nullptr);
		EXPECT_EQ(*Size.GetValue<uint64>(), std::filesystem::file_size(Path));
		ASSERT_NE(Extension.GetValue<std::string>(), nullptr);
		EXPECT_EQ(*Extension.GetValue<std::string>(), ".txt");
		auto Rows = Enumerate();
		EXPECT_TRUE(std::ranges::any_of(Rows, [](const auto& Item) { return Item.GetVirtualPath() == "/ProviderTests/external.txt"; }));
		std::ofstream(Path) << "changed external bytes";
		ASSERT_TRUE(PumpUntil([&] { return HasUpdate(EContentBrowserItemUpdateType::Modified); }));
		ASSERT_TRUE(std::filesystem::remove(Path));
		ASSERT_TRUE(PumpUntil([&] { return HasUpdate(EContentBrowserItemUpdateType::Removed); }));
		Rows = Enumerate();
		EXPECT_FALSE(std::ranges::any_of(Rows, [](const auto& Item) { return Item.GetVirtualPath() == "/ProviderTests/external.txt"; }));
	}

	TEST_F(FContentBrowserDataTests, MountChangesCancelInFlightFilesystemDiscovery)
	{
		FEditorEngineTestAccess::SetStartupState(*Editor, EHostStartupState::Ready);
		bool Refreshed = false;
		const auto Handle = Data->OnItemDataRefreshed().AddLambda([&] { Refreshed = true; });
		struct FUnsubscribe { DContentBrowserDataSubsystem* Data; FDelegateHandle Handle; ~FUnsubscribe() { Data->OnItemDataRefreshed().Remove(Handle); } } Subscription{Data, Handle};
		auto PumpUntilUpdates = [&] {
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do { (Refreshed = false, PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.0f, false)); if (Refreshed || !PublishedUpdates.empty()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); } while (std::chrono::steady_clock::now() < Deadline);
			return false;
		};
		ASSERT_TRUE(PumpUntilUpdates());
		auto Old = File();
		std::ofstream(Root / "old-only.txt") << "old activation";
		(PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Editor->Tick(0.5f, false)); // Launch, without publishing the worker result.
		Testing::RegisterMountPointForTests("/OtherDiscovery/", (Root / "Sub").generic_string() + "/");
		ASSERT_TRUE(std::filesystem::remove(Root / "old-only.txt"));
		ASSERT_TRUE(PumpUntilUpdates());
		EXPECT_FALSE(std::ranges::any_of(PublishedUpdates, [](const auto& Update) { return Update.GetItemData().GetVirtualPath() == "/ProviderTests/old-only.txt"; }));
		EXPECT_NE(nullptr, File().GetOwnerDataSource());
	}
	TEST_F(FContentBrowserDataTests, WithdrawnAssetSubscriptionCannotPublishUntilFreshActivation)
	{
		ASSERT_TRUE(Data->DeactivateDataSource(FName("Asset")));
		FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/InactiveAsset", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(nullptr, Asset);
		(void)CaptureAssetQuerySnapshot(); (PublishedUpdates.clear(), PumpGameThreadDeferredWork(), Data->Tick(0.0f));
		EXPECT_FALSE(std::ranges::any_of(PublishedUpdates, [&](const auto& Update) { return Update.GetItemData().GetVirtualPath() == Asset->GetObjectPath(); }));
		ASSERT_TRUE(Data->ActivateDataSource(FName("Asset")));
		const auto Rows = Enumerate();
		EXPECT_TRUE(std::ranges::any_of(Rows, [&](const auto& Item) { return Item.GetVirtualPath() == Asset->GetObjectPath(); }));
	}

}
