#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserNativeDataSource.h"
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
#include <gtest/gtest.h>

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
		int Saves = 0;
		std::function<void()> BeforeSave;
		auto CanSaveItem(const ContentBrowser::FContentBrowserItemData& Item, ContentBrowser::EContentBrowserItemSaveFlags, std::string*) const -> bool override { return Item.GetOwnerDataSource() == this; }
		auto BulkSaveItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::EContentBrowserItemSaveFlags) -> bool override
		{
			++Saves;
			if (BeforeSave) BeforeSave();
			return true;
		}
		std::string Name = "Probe";
		std::string ItemPath = "/Probe/Item";
		ContentBrowser::EContentBrowserItemKind Kind = ContentBrowser::EContentBrowserItemKind::File;
		auto GetDataSourceName() const -> FName override { return FName(Name); }
		auto CanCopyItem(const ContentBrowser::FContentBrowserItemData& Item, std::string_view, std::string*) const -> bool override
		{
			return Item.GetOwnerDataSource() == this;
		}
		auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool override
		{
			++Copies;
			return true;
		}
		auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter&, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>& Callback, const ContentBrowser::FContentBrowserHostContext&) -> void override
		{
			ContentBrowser::FContentBrowserItem Item{.Kind = Kind, .VirtualPath = ItemPath, .PhysicalPath = "/probe/item"};
			Item.InternalItems.emplace_back(*this, Item.Kind, Item.VirtualPath, nullptr);
			Callback(std::move(Item));
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
	class FContentBrowserDataTests : public testing::Test
	{
	protected:
		TStrongObjectPtr<DEditorEngine> Editor;
		DContentBrowserDataSubsystem* Data = nullptr;
		std::unique_ptr<Testing::FScopedMountRegistryFixture> Mounts;
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> Extras;
		std::filesystem::path Root;
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			FModuleManager::Get().LoadModuleChecked("ContentBrowser");
			Editor = TStrongObjectPtr<DEditorEngine>(NewObject<DEditorEngine>(nullptr, {}));
			FEditorEngineTestAccess::StartEditor(*Editor);
			Data = Editor->GetEditorSubsystem<DContentBrowserDataSubsystem>();
			ASSERT_NE(nullptr, Data);
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
				if (Item.PhysicalPath.ends_with("source.txt")) return Item.InternalItems.front();
			return {};
		}
	};
	TEST_F(FContentBrowserDataTests, ProvidersEnumerateOpaqueItemsAndMergeFolders)
	{
		auto Items = Enumerate();
		auto Folder = std::ranges::find_if(Items, [](const auto& Item) { return Item.PhysicalPath.ends_with("Sub"); });
		ASSERT_NE(Items.end(), Folder);
		EXPECT_EQ(1u, Folder->InternalItems.size());
		auto Item = File();
		ASSERT_NE(nullptr, Item.GetOwnerDataSource());
		EXPECT_EQ(FName("File"), Item.GetOwnerDataSource()->GetDataSourceName());
		EXPECT_NE(nullptr, Item.GetPayload());
	}
	TEST_F(FContentBrowserDataTests, ReactivationAndWithdrawalCannotReviveHandles)
	{
		auto Item = File();
		auto* Owner = Item.GetOwnerDataSource();
		ASSERT_NE(nullptr, Owner);
		EXPECT_TRUE(Data->DeactivateDataSource(FName("File")));
		EXPECT_EQ(nullptr, Item.GetOwnerDataSource());
		EXPECT_TRUE(Data->ActivateDataSource(FName("File")));
		EXPECT_EQ(nullptr, Item.GetOwnerDataSource());
		auto Fresh = File();
		EXPECT_EQ(Owner, Fresh.GetOwnerDataSource());
		Owner->UnregisterDataSource();
		EXPECT_EQ(nullptr, Fresh.GetOwnerDataSource());
		Data->Tick(0.0f);
		EXPECT_EQ(nullptr, Data->GetActiveDataSource(FName("File")));
	}
	TEST_F(FContentBrowserDataTests, RoutesCopyAndRenameToTheOwningProvider)
	{
		auto Item = File();
		ASSERT_NE(nullptr, Item.GetOwnerDataSource());
		const std::array Items{Item};
		EXPECT_TRUE(Data->BulkCopyItems(Items, "/ProviderTests/Target/"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/source.txt"));
		FContentBrowserItem Renamed;
		EXPECT_TRUE(Item.GetOwnerDataSource()->RenameItem(Item, "renamed.txt", Renamed));
		EXPECT_TRUE(std::filesystem::exists(Root / "renamed.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "source.txt"));
		EXPECT_EQ(Item.GetOwnerDataSource(), Renamed.InternalItems.front().GetOwnerDataSource());
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
		EXPECT_EQ(nullptr, Item.GetOwnerDataSource());
		FContentBrowserItem Out;
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
			if (Item.VirtualPath == "/Probe/Item") Items.push_back(std::move(Item));
			return true;
		});
		ASSERT_EQ(1u, Items.size());
		EXPECT_TRUE(Data->BulkCopyItems(Items.front().InternalItems, "/unused"));
		EXPECT_EQ(1, Probe->Copies);
		auto FileItem = File();
		std::array Mixed{Items.front().InternalItems.front(), FileItem};
		EXPECT_FALSE(Data->BulkCopyItems(Mixed, "/unused"));
		EXPECT_EQ(1, Probe->Copies);
		Probe->Shutdown();
	}
	TEST_F(FContentBrowserDataTests, ReplacementSourceAndMountChangesInvalidateCapturedItems)
	{
		TStrongObjectPtr<FProbeContentSource> First(NewObject<FProbeContentSource>(Editor.Get(), {}));
		Extras.emplace_back(First.Get());
		First->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		FContentBrowserItemData Old(*First, EContentBrowserItemKind::File, "/Probe/Old", nullptr);
		First->Shutdown();
		TStrongObjectPtr<FProbeContentSource> Replacement(NewObject<FProbeContentSource>(Editor.Get(), {}));
		Extras.emplace_back(Replacement.Get());
		Replacement->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		EXPECT_EQ(nullptr, Old.GetOwnerDataSource());
		auto Item = File();
		ASSERT_NE(nullptr, Item.GetOwnerDataSource());
		Testing::RegisterMountPointForTests("/OtherProviderTests/", (Root / "Sub").generic_string() + "/");
		EXPECT_EQ(nullptr, Item.GetOwnerDataSource());
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
		FContentBrowserItemData Item(*Source, EContentBrowserItemKind::File, "/opaque", Payload);
		Payload.reset();
		EXPECT_FALSE(Weak.expired());
		Data->DeactivateDataSource(FName("File"));
		EXPECT_EQ(nullptr, Item.GetOwnerDataSource());
		Item = {};
		EXPECT_TRUE(Weak.expired());
	}
	TEST_F(FContentBrowserDataTests, RetirementInMutationPolicyCannotAuthorizeFileWrites)
	{
		auto* Source = Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(nullptr, Source);
		FContentBrowserItem Item{.Kind = EContentBrowserItemKind::File, .VirtualPath = "/ProviderTests/source.txt", .PhysicalPath = (Root / "source.txt").generic_string()};
		FContentBrowserHostContext Host;
		Host.CanMutateContent = [&] { Data->DeactivateDataSource(FName("File")); return true; };
		auto Handle = Source->CreateItemData(Item, Host);
		FContentBrowserItem Out;
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
		const auto Folder = std::ranges::find_if(Items, [](const auto& Item) { return Item.VirtualPath == "/ProviderTests/Sub"; });
		ASSERT_NE(Items.end(), Folder);
		EXPECT_EQ(2u, Folder->InternalItems.size());
	}
	TEST_F(FContentBrowserDataTests, ItemFacadeCopiesInPlaceAndExactPathQueriesReturnOnlyTheirTarget)
	{
		std::vector<FContentBrowserItem> Exact;
		Data->EnumerateItemsAtPath("/ProviderTests/source.txt", [&](FContentBrowserItem&& Item) { Exact.push_back(std::move(Item)); return true; });
		ASSERT_EQ(1u, Exact.size());
		EXPECT_EQ("/ProviderTests/source.txt", Exact.front().InternalItems.front().GetVirtualPath());
		EXPECT_TRUE(Exact.front().CanCopy("/ProviderTests/"));
		EXPECT_TRUE(Exact.front().Copy("/ProviderTests/"));
		size_t Files = 0;
		for (const auto& Entry : std::filesystem::directory_iterator(Root))
			if (Entry.path().extension() == ".txt") ++Files;
		EXPECT_EQ(2u, Files);
		FContentBrowserItem Renamed;
		EXPECT_TRUE(Exact.front().Rename("facade.txt", Renamed));
		EXPECT_EQ("/ProviderTests/facade.txt", Renamed.VirtualPath);
		EXPECT_TRUE(Renamed.Move("/ProviderTests/Target/"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/facade.txt"));
	}
	TEST_F(FContentBrowserDataTests, HostMutationPolicyIsRevalidatedAfterCapabilityQueries)
	{
		auto* Source = Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(nullptr, Source);
		FContentBrowserItem Item{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "source.txt").generic_string()};
		bool Allowed = true;
		FContentBrowserHostContext Host;
		Host.CanMutateContent = [&] { return Allowed; };
		auto Handle = Source->CreateItemData(Item, Host);
		EXPECT_TRUE(Source->CanRenameItem(Handle, "changed.txt"));
		Allowed = false;
		FContentBrowserItem Out;
		EXPECT_FALSE(Source->RenameItem(Handle, "changed.txt", Out));
		EXPECT_TRUE(std::filesystem::exists(Root / "source.txt"));
	}
	TEST_F(FContentBrowserDataTests, OneProviderBatchesDifferentCapturesAndRechecksEveryMutationPolicy)
	{
		std::ofstream(Root / "second.txt") << "second";
		auto* Source = Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(nullptr, Source);
		bool Allowed = true;
		FContentBrowserHostContext Host;
		Host.CanMutateContent = [&] { return Allowed; };
		FContentBrowserItem First{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "source.txt").generic_string()};
		FContentBrowserItem Second{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "second.txt").generic_string()};
		const std::array Handles{Source->CreateItemData(First), Source->CreateItemData(Second, Host)};
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
		Data->Tick(0.0f);
		EXPECT_TRUE(std::ranges::any_of(Data->GetItemDataUpdates(), [](const auto& Update) {
			return Update.GetUpdateType() == EContentBrowserItemUpdateType::Added
				   && Update.GetItemData().GetVirtualPath() == "/ProviderTests/Target/source.txt";
		}));
		FContentBrowserItem Out;
		EXPECT_TRUE(Source->RenameItem(Handle, "delta.txt", Out));
		Data->Tick(0.0f);
		EXPECT_TRUE(std::ranges::any_of(Data->GetItemDataUpdates(), [](const auto& Update) {
			return Update.GetUpdateType() == EContentBrowserItemUpdateType::Moved
				   && Update.GetPreviousVirtualPath() == "/ProviderTests/source.txt"
				   && Update.GetItemData().GetVirtualPath() == "/ProviderTests/delta.txt";
		}));
		const auto Revision = Data->GetRevision();
		FContentBrowserItem Invalid;
		EXPECT_FALSE(Source->RenameItem(Out.InternalItems.front(), "", Invalid));
		Data->Tick(0.0f);
		EXPECT_EQ(Revision, Data->GetRevision());
		EXPECT_TRUE(Data->GetItemDataUpdates().empty());
	}
	TEST_F(FContentBrowserDataTests, RetiringAProviderDropsUndeliveredCapturedItems)
	{
		std::ofstream(Root / "second.txt") << "second";
		size_t Files = 0;
		Data->EnumerateItemsUnderPath("/ProviderTests/", {}, [&](FContentBrowserItem&& Item) {
			if (Item.Kind == EContentBrowserItemKind::File)
			{
				++Files;
				Data->DeactivateDataSource(FName("File"));
			}
			return true;
		});
		EXPECT_EQ(1u, Files);
	}
	TEST_F(FContentBrowserDataTests, BatchDispatchRechecksActivationAfterAnotherProviderCallback)
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
		A->BeforeSave = [&] { Data->DeactivateDataSource(FName("SaveB")); Data->ActivateDataSource(FName("SaveB")); };
		B->BeforeSave = [&] { Data->DeactivateDataSource(FName("SaveA")); Data->ActivateDataSource(FName("SaveA")); };
		const std::array Handles{
			FContentBrowserItemData(*A, EContentBrowserItemKind::File, "/SaveA/Item", nullptr),
			FContentBrowserItemData(*B, EContentBrowserItemKind::File, "/SaveB/Item", nullptr)
		};
		EXPECT_FALSE(Data->BulkSaveItems(Handles, EContentBrowserItemSaveFlags::None));
		EXPECT_EQ(1, A->Saves + B->Saves);
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
		Data->Tick(0.0f);
		CollectGarbage(EObjectFlags::Standalone);
		EXPECT_EQ(nullptr, Weak.Get());
	}
	TEST_F(FContentBrowserDataTests, InactiveItemsCannotBecomeCallableOnFirstActivation)
	{
		TStrongObjectPtr<FProbeContentSource> Source(NewObject<FProbeContentSource>(Editor.Get(), {}));
		Extras.emplace_back(Source.Get());
		Source->Initialize(false);
		Source->SetDataSink(Data);
		FContentBrowserItemData Inactive(*Source, EContentBrowserItemKind::File, "/inactive", nullptr);
		EXPECT_EQ(nullptr, Inactive.GetOwnerDataSource());
		Source->RegisterDataSource();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		EXPECT_EQ(nullptr, Inactive.GetOwnerDataSource());
		FContentBrowserItemData Active(*Source, EContentBrowserItemKind::File, "/active", nullptr);
		EXPECT_EQ(Source.Get(), Active.GetOwnerDataSource());
	}
	TEST_F(FContentBrowserDataTests, AssetSaveFlagsUseTheExistingPackagePolicy)
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/SaveAsset", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(nullptr, Asset);
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		ASSERT_NE(nullptr, Source);
		FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		const auto Handle = Source->CreateItemData(Item);
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
	TEST_F(FContentBrowserDataTests, NativeAssetAndFileBatchSharesOneCopyAndConfirmedDelete)
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/MixedAsset", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(nullptr, Asset);
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		ASSERT_NE(nullptr, Source);
		FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		auto AssetData = Source->CreateItemData(Item);
		ASSERT_TRUE(Data->BulkSaveItems(std::array{AssetData}, EContentBrowserItemSaveFlags::SaveOnlyIfDirty));
		const auto Rows = Enumerate();
		std::vector<FContentBrowserItemData> Batch;
		for (const auto& Row : Rows)
			if (Row.PhysicalPath.ends_with("source.txt") || Row.PackagePath == Path) Batch.insert(Batch.end(), Row.InternalItems.begin(), Row.InternalItems.end());
		ASSERT_EQ(2u, Batch.size());
		EXPECT_NE(Batch[0].GetOwnerDataSource(), Batch[1].GetOwnerDataSource());
		std::ranges::reverse(Batch); // File adapter coordinates this mixed batch.
		ASSERT_TRUE(Data->BulkCopyItems(Batch, "/ProviderTests/Target/"));
		Data->Tick(0.0f);
		EXPECT_TRUE(std::ranges::any_of(Data->GetItemDataUpdates(), [](const auto& Update) {
			auto* Owner = Update.GetItemData().GetOwnerDataSource();
			return Update.GetItemData().GetItemKind() == EContentBrowserItemKind::Asset && Owner && Owner->GetDataSourceName() == FName("Asset");
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
	TEST_F(FContentBrowserDataTests, NativeFolderTemporaryContextValidatesAndRejectsReactivation)
	{
		FContentBrowserItemDataTemporaryContext Pending;
		ASSERT_TRUE(Data->CreateFolder("/ProviderTests/New Folder", Pending));
		EXPECT_FALSE(std::filesystem::exists(Root / "New Folder"));
		EXPECT_FALSE(Pending.ValidateItem("../escape"));
		FContentBrowserItem Created;
		ASSERT_TRUE(Pending.FinalizeItem("Chosen", Created));
		EXPECT_EQ("/ProviderTests/Chosen", Created.VirtualPath);
		EXPECT_TRUE(std::filesystem::is_directory(Root / "Chosen"));
		EXPECT_FALSE(Pending.FinalizeItem("Again", Created));
		ASSERT_TRUE(Data->CreateFolder("/ProviderTests/Another", Pending));
		ASSERT_TRUE(Data->DeactivateDataSource(FName("Asset")));
		ASSERT_TRUE(Data->ActivateDataSource(FName("Asset")));
		EXPECT_FALSE(Pending.ValidateItem("Late"));
		EXPECT_FALSE(Pending.FinalizeItem("Late", Created));
		EXPECT_FALSE(std::filesystem::exists(Root / "Late"));
	}

} // namespace
