#include "Threading/TaskRuntime.h"
#include "ContentBrowserProviderSubsystems.h"
#include "Editor/ContentBrowserItemTestSupport.h"
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
		int Saves = 0;
		bool bFailSave = false;
		std::function<void()> BeforeSave;
		auto CanSaveItem(const ContentBrowser::FContentBrowserItemData& Item, ContentBrowser::EContentBrowserItemSaveFlags, std::string*) const -> bool override { return Item.GetOwnerDataSource() == this; }
		auto BulkSaveItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, ContentBrowser::EContentBrowserItemSaveFlags) -> ContentBrowser::FContentBrowserOperationResult override
		{
			++Saves;
			if (BeforeSave) BeforeSave();
			ContentBrowser::FContentBrowserOperationResult Result;
			for (const auto& Item : Items) Result.Add(Item.GetContributionKey(), bFailSave ? ContentBrowser::EContentBrowserEffectStatus::Failed : ContentBrowser::EContentBrowserEffectStatus::Committed, bFailSave ? "Injected save failure" : "");
			return Result;
		}
		std::string Name = "Probe";
		std::string ItemPath = "/Probe/Item";
		ContentBrowser::EContentBrowserItemKind Kind = ContentBrowser::EContentBrowserItemKind::File;
		int Ticks = 0;
		auto Tick(float) -> void override { ++Ticks; }
		auto GetDataSourceName() const -> FName override { return FName(Name); }
		auto CanCopyItem(const ContentBrowser::FContentBrowserItemData& Item, std::string_view, std::string*) const -> bool override
		{
			return Item.GetOwnerDataSource() == this;
		}
		auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, std::string_view) -> ContentBrowser::FContentBrowserOperationResult override
		{
			++Copies;
			ContentBrowser::FContentBrowserOperationResult Result;
			for (const auto& Item : Items) Result.Add(Item.GetContributionKey(), ContentBrowser::EContentBrowserEffectStatus::Committed);
			return Result;
		}
		auto PrepareCopyItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, std::string_view Destination) -> std::unique_ptr<ContentBrowser::FContentBrowserItemDataTransferContext> override
		{
			std::vector<ContentBrowser::FContentBrowserItemData> Captured(Items.begin(), Items.end());
			std::vector<ContentBrowser::FContentBrowserItemContributionKey> Keys;
			for (const auto& Item : Captured) Keys.push_back(Item.GetContributionKey());
			return std::make_unique<ContentBrowser::FContentBrowserItemDataTransferContext>(std::move(Keys), std::vector<ContentBrowser::FContentBrowserTransferDestination>{},
				[this, Captured, Destination = std::string(Destination)](std::string*) { return std::ranges::all_of(Captured, [&](const auto& Item) { return Item.GetOwnerDataSource() == this && CanCopyItem(Item, Destination, nullptr) && Item.GetOwnerDataSource() == this; }); },
				[this, Captured, Destination = std::string(Destination)](const auto&) { return BulkCopyItems(Captured, Destination); });
		}

		auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter&, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>& Callback, const ContentBrowser::FContentBrowserOperationContext&) -> void override
		{
			ContentBrowser::FContentBrowserItem Item;
			Item.InternalItems.emplace_back(*this, Kind == ContentBrowser::EContentBrowserItemKind::Folder ? ContentBrowser::EContentBrowserItemFlags::Type_Folder : ContentBrowser::EContentBrowserItemFlags::Type_File, ItemPath, "", "", nullptr);
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
		bool OwnScheduler = false;
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			OwnScheduler = !IsTaskSchedulerRunning();
			if (OwnScheduler) ASSERT_TRUE(InitializeTaskScheduler(2));
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
		EXPECT_TRUE(Data->BulkCopyItems(Items, "/ProviderTests/Target/").IsComplete());
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/source.txt"));
		FContentBrowserItemData Renamed;
		EXPECT_TRUE(Item.GetOwnerDataSource()->RenameItem(Item, "renamed.txt", Renamed).IsComplete());
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
		EXPECT_FALSE(Data->BulkDeleteItems({}, Context).IsComplete());
		std::ofstream(Root / "source.txt", std::ios::app) << "changed";
		EXPECT_FALSE(Data->BulkDeleteItems(Items, Context).IsComplete());
		EXPECT_TRUE(std::filesystem::exists(Root / "source.txt"));
		EXPECT_TRUE(Data->BulkDeleteItems(Items, Context).IsComplete());
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
		FContentBrowserItemData Out;
		EXPECT_FALSE(Source->RenameItem(Item, "other.txt", Out).IsComplete());
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
		EXPECT_TRUE(Data->BulkCopyItems(Items.front().InternalItems, "/unused").IsComplete());
		EXPECT_EQ(1, Probe->Copies);
		auto FileItem = File();
		std::array Mixed{Items.front().InternalItems.front(), FileItem};
		EXPECT_FALSE(Data->BulkCopyItems(Mixed, "/unused").IsComplete());
		EXPECT_EQ(1, Probe->Copies);
		Probe->Shutdown();
	}
	TEST_F(FContentBrowserDataTests, ReplacementSourceAndMountChangesInvalidateCapturedItems)
	{
		TStrongObjectPtr<FProbeContentSource> First(NewObject<FProbeContentSource>(Editor.Get(), {}));
		Extras.emplace_back(First.Get());
		First->Initialize();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		FContentBrowserItemData Old(*First, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/Probe/Old", std::string{}, std::string{}, nullptr);
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
		FContentBrowserItemData Item(*Source, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/opaque", {}, {}, Payload);
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
		FContentBrowserItem Item = Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::File, .VirtualPath = "/ProviderTests/source.txt", .PhysicalPath = (Root / "source.txt").generic_string()};
		FContentBrowserOperationContext Host;
		Host.CanMutateContent = [&] { Data->DeactivateDataSource(FName("File")); return true; };
		auto Handle = Source->CreateItemData(Item, Host);
		FContentBrowserItemData Out;
		EXPECT_FALSE(Source->RenameItem(Handle, "retired.txt", Out).IsComplete());
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
		EXPECT_FALSE(Exact.front().Copy("/ProviderTests/").IsComplete());
		size_t Files = 0;
		for (const auto& Entry : std::filesystem::directory_iterator(Root))
			if (Entry.path().extension() == ".txt") ++Files;
		EXPECT_EQ(1u, Files);
		FContentBrowserItem Renamed;
		EXPECT_TRUE(Exact.front().Rename("facade.txt", Renamed).IsComplete());
		EXPECT_EQ("/ProviderTests/facade.txt", TestVirtualPath(Renamed));
		EXPECT_TRUE(Renamed.Move("/ProviderTests/Target/").IsComplete());
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/facade.txt"));
	}
	TEST_F(FContentBrowserDataTests, HostMutationPolicyIsRevalidatedAfterCapabilityQueries)
	{
		auto* Source = Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(nullptr, Source);
		FContentBrowserItem Item = Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "source.txt").generic_string()};
		bool Allowed = true;
		FContentBrowserOperationContext Host;
		Host.CanMutateContent = [&] { return Allowed; };
		auto Handle = Source->CreateItemData(Item, Host);
		EXPECT_TRUE(Source->CanRenameItem(Handle, "changed.txt"));
		Allowed = false;
		FContentBrowserItemData Out;
		EXPECT_FALSE(Source->RenameItem(Handle, "changed.txt", Out).IsComplete());
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
		FContentBrowserItem First = Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "source.txt").generic_string()};
		FContentBrowserItem Second = Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "second.txt").generic_string()};
		const std::array Handles{Source->CreateItemData(First), Source->CreateItemData(Second, Host)};
		Allowed = false;
		EXPECT_FALSE(Data->BulkCopyItems(Handles, "/ProviderTests/Target/").IsComplete());
		EXPECT_FALSE(std::filesystem::exists(Root / "Target/source.txt"));
		Allowed = true;
		EXPECT_TRUE(Data->BulkCopyItems(Handles, "/ProviderTests/Target/").IsComplete());
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
		EXPECT_TRUE(Data->BulkCopyItems(Items, "/ProviderTests/Target/").IsComplete());
		Data->Tick(0.0f);
		EXPECT_TRUE(std::ranges::any_of(Data->GetItemDataUpdates(), [](const auto& Update) {
			return Update.GetUpdateType() == EContentBrowserItemUpdateType::Added
				   && Update.GetItemData().GetVirtualPath() == "/ProviderTests/Target/source.txt";
		}));
		FContentBrowserItemData Out;
		EXPECT_TRUE(Source->RenameItem(Handle, "delta.txt", Out).IsComplete());
		Data->Tick(0.0f);
		EXPECT_TRUE(std::ranges::any_of(Data->GetItemDataUpdates(), [](const auto& Update) {
			return Update.GetUpdateType() == EContentBrowserItemUpdateType::Moved
				   && Update.GetPreviousVirtualPath() == "/ProviderTests/source.txt"
				   && Update.GetItemData().GetVirtualPath() == "/ProviderTests/delta.txt";
		}));
		const auto Revision = Data->GetRevision();
		FContentBrowserItemData Invalid;
		EXPECT_FALSE(Source->RenameItem(Out, "", Invalid).IsComplete());
		Data->Tick(0.0f);
		EXPECT_EQ(Revision, Data->GetRevision());
		EXPECT_TRUE(Data->GetItemDataUpdates().empty());
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
			FContentBrowserItemData(*A, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/SaveA/Item", {}, {}, nullptr),
			FContentBrowserItemData(*B, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/SaveB/Item", {}, {}, nullptr)
		};
		const auto Result = Data->BulkSaveItems(Handles, EContentBrowserItemSaveFlags::None);
		EXPECT_EQ(EContentBrowserOperationStatus::Partial, Result.GetStatus());
		ASSERT_EQ(2u, Result.Effects.size());
		EXPECT_EQ(EContentBrowserEffectStatus::Committed, Result.Effects[0].Status);
		EXPECT_EQ(Handles[1].GetContributionKey(), Result.Effects[1].OriginalIdentity);
		EXPECT_EQ(EContentBrowserEffectStatus::Unattempted, Result.Effects[1].Status);
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
		FContentBrowserItemData Inactive(*Source, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/inactive", std::string{}, std::string{}, nullptr);
		EXPECT_EQ(nullptr, Inactive.GetOwnerDataSource());
		Source->RegisterDataSource();
		ASSERT_TRUE(Data->ActivateDataSource(FName("Probe")));
		EXPECT_EQ(nullptr, Inactive.GetOwnerDataSource());
		FContentBrowserItemData Active(*Source, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/active", std::string{}, std::string{}, nullptr);
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
		FContentBrowserItem Item = Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		const auto Handle = Source->CreateItemData(Item);
		const std::array Items{Handle};
		EXPECT_TRUE(Source->CanSaveItem(Handle, EContentBrowserItemSaveFlags::SaveOnlyIfDirty));
		EXPECT_TRUE(Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::SaveOnlyIfDirty).IsComplete());
		Asset->MarkPackageDirty();
		EXPECT_TRUE(Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::SaveOnlyIfDirty | EContentBrowserItemSaveFlags::SaveOnlyIfLoaded).IsComplete());
		EXPECT_FALSE(Asset->GetPackage()->IsDirty());
		EXPECT_TRUE(std::filesystem::exists(Root / "SaveAsset.dasset"));
		EXPECT_TRUE(Source->CanSaveItem(Handle, EContentBrowserItemSaveFlags::SaveOnlyIfLoaded));
		EXPECT_TRUE(Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::SaveOnlyIfLoaded).IsComplete());
		EXPECT_FALSE(Source->CanSaveItem(Handle, static_cast<EContentBrowserItemSaveFlags>(4)));
		EXPECT_TRUE(Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::None).IsComplete());
	}
	TEST_F(FContentBrowserDataTests, BulkSaveContinuesIndependentSourceFailuresInNameOrder)
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
		EXPECT_EQ((std::vector<std::string>{"SaveA", "SaveB", "SaveC"}), Order);
		EXPECT_EQ(EContentBrowserOperationStatus::Partial, Result.GetStatus());
		ASSERT_EQ(3u, Result.Effects.size());
		EXPECT_EQ(EContentBrowserEffectStatus::Committed, Result.Effects[0].Status);
		EXPECT_EQ(EContentBrowserEffectStatus::Failed, Result.Effects[1].Status);
		EXPECT_EQ(EContentBrowserEffectStatus::Committed, Result.Effects[2].Status);
		EXPECT_EQ(Items[1].GetContributionKey(), Result.Effects[1].OriginalIdentity);
		EXPECT_FALSE(Result.Effects[1].Diagnostic.empty());
	}
	TEST_F(FContentBrowserDataTests, AssetBulkSaveKeepsCommittedPackagesAndContinuesMissingPackage)
	{
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		ASSERT_NE(nullptr, Source);
		std::vector<FContentBrowserItemData> Items;
		int Notifications = 0;
		FContentBrowserOperationContext Host;
		Host.NotifyScopedContentMutation = [&](FContentChangeBatch) { ++Notifications; };
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
			FContentBrowserItem Description = Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = VirtualPath, .PackagePath = Path};
			Items.push_back(Source->CreateItemData(Description, Host));
		}
		const auto Result = Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::SaveOnlyIfDirty);
		EXPECT_EQ(EContentBrowserOperationStatus::Partial, Result.GetStatus());
		ASSERT_EQ(3u, Result.Effects.size());
		EXPECT_EQ(EContentBrowserEffectStatus::Committed, Result.Effects[0].Status);
		EXPECT_EQ(EContentBrowserEffectStatus::Failed, Result.Effects[1].Status);
		EXPECT_EQ(EContentBrowserEffectStatus::Committed, Result.Effects[2].Status);
		EXPECT_EQ(Items[2].GetContributionKey(), Result.Effects[0].OriginalIdentity);
		EXPECT_EQ(Items[1].GetContributionKey(), Result.Effects[1].OriginalIdentity);
		EXPECT_EQ(Items[0].GetContributionKey(), Result.Effects[2].OriginalIdentity);
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
			FContentBrowserItem Description = Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
			Items.push_back(Source->CreateItemData(Description));
		}
		int Stages = 0;
		Testing::FScopedPackageSaveFault Fault([&](auto Point, size_t) {
			return Point == Testing::EPackageSaveFaultPoint::StagePackage && ++Stages == 2;
		});
		const auto Result = Data->BulkSaveItems(Items, EContentBrowserItemSaveFlags::SaveOnlyIfDirty);
		EXPECT_EQ(EContentBrowserOperationStatus::Partial, Result.GetStatus());
		EXPECT_TRUE(Result.bStopped);
		ASSERT_EQ(3u, Result.Effects.size());
		EXPECT_EQ(EContentBrowserEffectStatus::Committed, Result.Effects[0].Status);
		EXPECT_EQ(EContentBrowserEffectStatus::Failed, Result.Effects[1].Status);
		EXPECT_EQ(EContentBrowserEffectStatus::Unattempted, Result.Effects[2].Status);
		EXPECT_FALSE(Result.Effects[1].Diagnostic.empty());
		EXPECT_NE(std::string::npos, Result.Effects[2].Diagnostic.find("recovery"));
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
		EXPECT_EQ(EContentBrowserOperationStatus::Partial, Result.GetStatus());
		EXPECT_TRUE(Result.HasCommittedEffects());
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/source.txt"));
		EXPECT_TRUE(std::filesystem::exists(Root / "source.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "Target/second_Copy.txt"));
		const auto Failed = std::ranges::find_if(Result.Effects, [](const auto& Effect) { return Effect.Status == EContentBrowserEffectStatus::Failed; });
		ASSERT_NE(Result.Effects.end(), Failed);
		const auto Retry = std::ranges::find_if(Inputs, [&](const auto& Item) { return Item.GetContributionKey() == Failed->OriginalIdentity; });
		ASSERT_NE(Inputs.end(), Retry);
		std::filesystem::remove(Root / "Target/second.txt");
		EXPECT_TRUE(Data->BulkCopyItems(std::span{&*Retry, 1}, "/ProviderTests/Target/").IsComplete());
	}
	TEST_F(FContentBrowserDataTests, MoveRetainsEarlierCommitAndLeavesConflictingSource)
	{
		std::ofstream(Root / "a.txt") << "a";
		std::ofstream(Root / "b.txt") << "b";
		std::ofstream(Root / "Target/b.txt") << "occupied";
		std::vector<FContentBrowserItemData> Inputs;
		for (const auto& Row : Enumerate()) if (Row.GetVirtualPath().ends_with("a.txt") || Row.GetVirtualPath().ends_with("b.txt")) Inputs.push_back(Row.InternalItems.front());
		const auto Result = Data->BulkMoveItems(Inputs, "/ProviderTests/Target/");
		EXPECT_EQ(EContentBrowserOperationStatus::Partial, Result.GetStatus());
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/a.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "a.txt"));
		EXPECT_TRUE(std::filesystem::exists(Root / "b.txt"));
		ASSERT_EQ(2u, Result.Effects.size());
		EXPECT_EQ(EContentBrowserEffectStatus::Committed, Result.Effects.front().Status);
		EXPECT_EQ(EContentBrowserEffectStatus::Failed, Result.Effects.back().Status);
	}
	TEST_F(FContentBrowserDataTests, PreparedTransferCancellationKeepsCommittedEffectsAndConsumesAttempt)
	{
		std::ofstream(Root / "a.txt") << "a";
		std::ofstream(Root / "b.txt") << "b";
		auto* Source = Data->GetActiveDataSource(FName("File"));
		ASSERT_NE(nullptr, Source);
		FContentBrowserItemDataTransferContext* Active = nullptr;
		FContentBrowserOperationContext Host;
		Host.CanMutateContent = [&] { if (Active && std::filesystem::exists(Root / "Target/a.txt")) Active->RequestCancel(); return true; };
		std::vector<FContentBrowserItemData> Inputs;
		for (const auto* Name : {"a.txt", "b.txt"})
		{
			FContentBrowserItem Description = Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::File, .VirtualPath = "/ProviderTests/" + std::string(Name), .PhysicalPath = (Root / Name).generic_string()};
			Inputs.push_back(Source->CreateItemData(Description, Host));
		}
		auto Context = Source->PrepareCopyItems(Inputs, "/ProviderTests/Target/");
		ASSERT_NE(nullptr, Context);
		Active = Context.get();
		const auto Result = Context->Execute();
		EXPECT_EQ(EContentBrowserOperationStatus::Cancelled, Result.GetStatus());
		EXPECT_TRUE(Result.HasCommittedEffects());
		EXPECT_TRUE(std::filesystem::exists(Root / "Target/a.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "Target/b.txt"));
		EXPECT_FALSE(Context->Execute().HasCommittedEffects());
	}
	TEST_F(FContentBrowserDataTests, FileFolderDeletionRetainsPackagesForTheirAssetOwner)
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/Sub/Owned", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(nullptr, Asset);
		auto* AssetSource = Data->GetActiveDataSource(FName("Asset"));
		FContentBrowserItem Description = Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		const auto AssetData = AssetSource->CreateItemData(Description);
		ASSERT_TRUE(Data->BulkSaveItems(std::span{&AssetData, 1}, EContentBrowserItemSaveFlags::SaveOnlyIfDirty).IsComplete());
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
		EXPECT_TRUE(Result.HasCommittedEffects());
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
		FContentBrowserItem Item = Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		auto AssetData = Source->CreateItemData(Item);
		ASSERT_TRUE(Data->BulkSaveItems(std::array{AssetData}, EContentBrowserItemSaveFlags::SaveOnlyIfDirty).IsComplete());
		const auto Rows = Enumerate();
		std::vector<FContentBrowserItemData> Batch;
		for (const auto& Row : Rows)
			if (TestPhysicalPath(Row).ends_with("source.txt") || TestPackagePath(Row) == Path) Batch.insert(Batch.end(), Row.InternalItems.begin(), Row.InternalItems.end());
		ASSERT_EQ(2u, Batch.size());
		EXPECT_NE(Batch[0].GetOwnerDataSource(), Batch[1].GetOwnerDataSource());
		std::ranges::reverse(Batch); // Generic dispatch partitions this mixed batch by source.
		ASSERT_TRUE(Data->BulkCopyItems(Batch, "/ProviderTests/Target/").IsComplete());
		Data->Tick(0.0f);
		EXPECT_TRUE(std::ranges::any_of(Data->GetItemDataUpdates(), [](const auto& Update) {
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
		EXPECT_TRUE(Data->BulkDeleteItems(Batch, Context).IsComplete());
		EXPECT_FALSE(std::filesystem::exists(Root / "source.txt"));
		EXPECT_FALSE(std::filesystem::exists(Root / "MixedAsset.dasset"));
	}
	TEST_F(FContentBrowserDataTests, NativeFolderTemporaryContextValidatesAndRejectsReactivation)
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
		ASSERT_TRUE(Data->ActivateDataSource(FName("Asset")));
		EXPECT_FALSE(Pending.ValidateItem("Late"));
		EXPECT_FALSE(Pending.FinalizeItem("Late", Created));
		EXPECT_FALSE(std::filesystem::exists(Root / "Late"));
	}
	TEST_F(FContentBrowserDataTests, NativeFolderTemporaryContextRejectsMountChangesAndRetriesNameConflicts)
	{
		FContentBrowserItemDataTemporaryContext Pending;
		ASSERT_TRUE(Data->CreateFolder("/ProviderTests/New Folder", Pending));
		std::filesystem::create_directory(Root / "Taken");
		FContentBrowserItemData Created;
		EXPECT_FALSE(Pending.FinalizeItem("Taken", Created));
		ASSERT_TRUE(Pending.FinalizeItem("Available", Created));
		EXPECT_TRUE(std::filesystem::is_directory(Root / "Available"));
		ASSERT_TRUE(Data->CreateFolder("/ProviderTests/Later", Pending));
		Testing::RegisterMountPointForTests("/ChangedAdmission/", Root.generic_string() + "/");
		EXPECT_FALSE(Pending.ValidateItem("Late"));
		EXPECT_FALSE(Pending.FinalizeItem("Late", Created));
		EXPECT_FALSE(std::filesystem::exists(Root / "Late"));
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
TEST_F(FContentBrowserDataTests, CompiledFiltersKeepSourcePartitionsAndRejectOldActivation)
{
	std::ofstream(Root / "Sub/nested.txt") << "nested";
	FContentBrowserDataCompiledFilter Compiled;
	Data->CompileFilter("/ProviderTests/", {.Kind = EContentBrowserItemKind::File}, Compiled);
	ASSERT_EQ(Compiled.CompiledFilters.size(), 2u);
	for (const auto &[Source, Filter] : Compiled.CompiledFilters)
		EXPECT_NE(Filter.Data, nullptr);
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
	ASSERT_TRUE(Data->ActivateDataSource(FName("File")));
	Enumerate();
	EXPECT_TRUE(Items.empty());
	Data->CompileFilter("/ProviderTests/", {.bRecursive = true, .Kind = EContentBrowserItemKind::File}, Compiled);
	Enumerate();
	ASSERT_EQ(Items.size(), 2u);
	EXPECT_TRUE(std::ranges::all_of(Items, [](const auto &Item) { return TestKind(Item) == EContentBrowserItemKind::File; }));
}
} // namespace

namespace
{
	TEST_F(FContentBrowserDataTests, LogicalKeysUseVirtualTypeAndSourceWhileAdmissionKeysRetire)
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
		EXPECT_EQ(FileFolder.GetOwnerDataSource(), nullptr);
		EXPECT_EQ(FContentBrowserItemData(*File, EContentBrowserItemFlags::Type_Folder | EContentBrowserItemFlags::Category_Misc, "/ProviderTests/Sub", "", "", {}).GetItemKey(), Logical);
		EXPECT_NE(FContentBrowserItemData(*File, EContentBrowserItemFlags::Type_Folder | EContentBrowserItemFlags::Category_Misc, "/ProviderTests/Sub", "", "", {}).GetContributionKey(), Captured);
		std::unordered_set<FContentBrowserItemKey, FContentBrowserItemKeyHash> Keys{First.GetItemKey(), Same.GetItemKey(), Other.GetItemKey()};
		EXPECT_EQ(Keys.size(), 2u);
	}
}

namespace
{
	TEST_F(FContentBrowserDataTests, NativeAttributesPreservePrecisionMetadataAndStaleOutputRules)
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
		EXPECT_FALSE(Item.GetItemAttribute(ContentBrowserItemAttributes::ItemDiskSize, Value));
		EXPECT_FALSE(Value.IsValid());
		EXPECT_FALSE(Item.GetItemAttributes(Values)); EXPECT_TRUE(Values.empty());
		EXPECT_FALSE(Item.GetItemPhysicalPath(Physical)); EXPECT_TRUE(Physical.empty());
	}
	TEST_F(FContentBrowserDataTests, NonphysicalAttributesForwardAndRejectCallbackReactivation)
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
		Source->BeforeAttribute=[&] { Data->DeactivateDataSource(FName("Probe")); Data->ActivateDataSource(FName("Probe")); };
		EXPECT_FALSE(Item.GetItemAttribute("ProbeCounter", Value)); EXPECT_FALSE(Value.IsValid());
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
		EXPECT_EQ(Description.GetOwnerDataSource(), nullptr); EXPECT_EQ(Description.GetDisplayName(), "Visible name");
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
		const auto Input = Durin::ContentBrowser::Private::MakeNativeItem({.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path}, true);
		const std::array Handle{Source->CreateItemData(Input)};
		ASSERT_TRUE(Data->BulkSaveItems(Handle, EContentBrowserItemSaveFlags::SaveOnlyIfDirty).IsComplete());
		auto Rows = Enumerate();
		auto Folder = std::ranges::find_if(Rows, [](const auto& Item) { return Item.GetItemKey().GetVirtualPath() == "/ProviderTests/Sub"; });
		ASSERT_NE(Folder, Rows.end());
		ASSERT_EQ(Folder->GetInternalItems().size(), 2u);
		EXPECT_NE(Folder->GetInternalItems()[0].GetContributionKey(), Folder->GetInternalItems()[1].GetContributionKey());
		EXPECT_EQ(Folder->GetInternalItems()[0].GetItemKey(), Folder->GetInternalItems()[1].GetItemKey());
		ASSERT_TRUE(Folder->CanRename("Relocated"));
		FContentBrowserItem Renamed;
		ASSERT_TRUE(Folder->Rename("Relocated", Renamed).IsComplete());
		Data->Tick(0);
		EXPECT_TRUE(std::ranges::any_of(Data->GetItemDataUpdates(), [](const auto& Update) {
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
		FContentBrowserItem Item = Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		auto Handle = Source->CreateItemData(Item);
		ASSERT_TRUE(Data->BulkSaveItems(std::array{Handle}, EContentBrowserItemSaveFlags::SaveOnlyIfDirty).IsComplete());
		std::ofstream(Root / "Sub/delete.txt") << "ordinary bytes";
		const auto Rows = Enumerate();
		const auto Folder = std::ranges::find_if(Rows, [](const auto& Row) { return Row.GetVirtualPath() == "/ProviderTests/Sub"; });
		ASSERT_NE(Folder, Rows.end());
		ASSERT_EQ(2u, Folder->GetInternalItems().size());
		auto Context = Data->PrepareDeleteItems(Folder->GetInternalItems());
		ASSERT_TRUE(Context.CanExecute());
		const auto Result = Data->BulkDeleteItems(Folder->GetInternalItems(), Context);
		ASSERT_TRUE(Result.IsComplete());
		EXPECT_TRUE(Result.HasCommittedEffects());
		EXPECT_FALSE(Context.CanExecute());
		EXPECT_FALSE(std::filesystem::exists(Root / "Sub"));
		EXPECT_TRUE(std::ranges::any_of(Result.Effects, [](const auto& Effect) { return Effect.OriginalIdentity.Item.GetVirtualPath().ends_with("MixedDelete.MixedDelete"); }));
		EXPECT_TRUE(std::ranges::any_of(Result.Effects, [](const auto& Effect) { return Effect.OriginalIdentity.Item.GetVirtualPath().ends_with("delete.txt"); }));
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
		Editor->Tick(0.0f, false);
		EXPECT_EQ(1, Probe->Ticks);
		Editor->Tick(0.0f, true);
		EXPECT_EQ(2, Probe->Ticks);
		Editor->PreExit();
		Editor->Tick(0.0f, false);
		EXPECT_EQ(2, Probe->Ticks);
	}
	TEST_F(FContentBrowserDataTests, ProviderBootstrapCanRetireAndRecreateWithoutRetiringDataService)
	{
		auto Handle = File();
		auto* Original = Handle.GetOwnerDataSource();
		ASSERT_NE(nullptr, Original);
		Editor->RemoveEditorSubsystem(DContentBrowserFileProviderSubsystem::StaticClass());
		EXPECT_EQ(nullptr, Data->GetActiveDataSource(FName("File")));
		EXPECT_EQ(nullptr, Handle.GetOwnerDataSource());
		EXPECT_TRUE(Data->IsActive());
		ASSERT_NE(nullptr, Editor->AddEditorSubsystem(DContentBrowserFileProviderSubsystem::StaticClass()));
		EXPECT_NE(Original, Data->GetActiveDataSource(FName("File")));
		EXPECT_NE(nullptr, File().GetOwnerDataSource());
		EXPECT_EQ(nullptr, Handle.GetOwnerDataSource());
	}
	TEST_F(FContentBrowserDataTests, AssetEventsPublishWithoutViewsAndWithoutReplayingCommittedUpdates)
	{
		FEditorEngineTestAccess::SetStartupState(*Editor, EHostStartupState::Ready);
		FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/NoBrowser", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(nullptr, Asset);
		Editor->Tick(0.0f, false);
		const auto Count = std::ranges::count_if(Data->GetItemDataUpdates(), [&](const auto& Update) { return Update.GetItemData().GetVirtualPath() == Asset->GetObjectPath() && Update.GetUpdateType() == EContentBrowserItemUpdateType::Added; });
		EXPECT_EQ(1, Count);
		Editor->Tick(0.0f, false);
		EXPECT_FALSE(std::ranges::any_of(Data->GetItemDataUpdates(), [&](const auto& Update) { return Update.GetItemData().GetVirtualPath() == Asset->GetObjectPath() && Update.GetUpdateType() == EContentBrowserItemUpdateType::Added; }));
		const auto Rows = Enumerate();
		EXPECT_TRUE(std::ranges::any_of(Rows, [&](const auto& Item) { return Item.GetVirtualPath() == Asset->GetObjectPath(); }));
		auto* Source = Data->GetActiveDataSource(FName("Asset"));
		FContentBrowserItem Native = ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Asset->GetObjectPath(), .PackagePath = Path};
		auto Handle = Source->CreateItemData(Native);
		ASSERT_TRUE(Data->BulkSaveItems(std::array{Handle}, EContentBrowserItemSaveFlags::SaveOnlyIfDirty).IsComplete());
		Editor->Tick(0.0f, false);
		EXPECT_FALSE(std::ranges::any_of(Data->GetItemDataUpdates(), [&](const auto& Update) { return Update.GetItemData().GetVirtualPath() == Asset->GetObjectPath() && Update.GetUpdateType() == EContentBrowserItemUpdateType::Added; }));
	}
	TEST_F(FContentBrowserDataTests, FilesystemChangesPublishWithoutViewsAndLaterQueriesSeeCurrentState)
	{
		FEditorEngineTestAccess::SetStartupState(*Editor, EHostStartupState::Ready);
		auto PumpUntil = [&](const auto& Predicate) {
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do { Editor->Tick(0.5f, false); if (Predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); } while (std::chrono::steady_clock::now() < Deadline);
			return false;
		};
		ASSERT_TRUE(PumpUntil([&] { return !Data->GetItemDataUpdates().empty(); }));
		const auto Path = Root / "external.txt";
		std::ofstream(Path) << "external bytes";
		auto HasUpdate = [&](auto Type) { return std::ranges::any_of(Data->GetItemDataUpdates(), [&](const auto& Update) { return Update.GetItemData().GetVirtualPath() == "/ProviderTests/external.txt" && Update.GetUpdateType() == Type; }); };
		ASSERT_TRUE(PumpUntil([&] { return HasUpdate(EContentBrowserItemUpdateType::Added); }));
		auto Rows = Enumerate();
		EXPECT_TRUE(std::ranges::any_of(Rows, [](const auto& Item) { return Item.GetVirtualPath() == "/ProviderTests/external.txt"; }));
		std::ofstream(Path) << "changed external bytes";
		ASSERT_TRUE(PumpUntil([&] { return HasUpdate(EContentBrowserItemUpdateType::Modified); }));
		ASSERT_TRUE(std::filesystem::remove(Path));
		ASSERT_TRUE(PumpUntil([&] { return HasUpdate(EContentBrowserItemUpdateType::Removed); }));
		Rows = Enumerate();
		EXPECT_FALSE(std::ranges::any_of(Rows, [](const auto& Item) { return Item.GetVirtualPath() == "/ProviderTests/external.txt"; }));
	}

	TEST_F(FContentBrowserDataTests, InFlightFilesystemSnapshotCannotPublishIntoAReactivatedSource)
	{
		FEditorEngineTestAccess::SetStartupState(*Editor, EHostStartupState::Ready);
		auto PumpUntilUpdates = [&] {
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do { Editor->Tick(0.0f, false); if (!Data->GetItemDataUpdates().empty()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); } while (std::chrono::steady_clock::now() < Deadline);
			return false;
		};
		ASSERT_TRUE(PumpUntilUpdates());
		auto Old = File();
		std::ofstream(Root / "old-only.txt") << "old activation";
		Editor->Tick(0.5f, false); // Launch, without publishing the worker result.
		ASSERT_TRUE(Data->DeactivateDataSource(FName("File")));
		ASSERT_TRUE(std::filesystem::remove(Root / "old-only.txt"));
		ASSERT_TRUE(Data->ActivateDataSource(FName("File")));
		EXPECT_EQ(nullptr, Old.GetOwnerDataSource());
		ASSERT_TRUE(PumpUntilUpdates());
		EXPECT_FALSE(std::ranges::any_of(Data->GetItemDataUpdates(), [](const auto& Update) { return Update.GetItemData().GetVirtualPath() == "/ProviderTests/old-only.txt"; }));
		EXPECT_NE(nullptr, File().GetOwnerDataSource());
	}
	TEST_F(FContentBrowserDataTests, WithdrawnAssetSubscriptionCannotPublishUntilFreshActivation)
	{
		ASSERT_TRUE(Data->DeactivateDataSource(FName("Asset")));
		FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ProviderTests/InactiveAsset", Path));
		auto* Asset = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path, DObject::StaticClass());
		ASSERT_NE(nullptr, Asset);
		(void)GetAssetQueryRevision(); Data->Tick(0.0f);
		EXPECT_FALSE(std::ranges::any_of(Data->GetItemDataUpdates(), [&](const auto& Update) { return Update.GetItemData().GetVirtualPath() == Asset->GetObjectPath(); }));
		ASSERT_TRUE(Data->ActivateDataSource(FName("Asset")));
		const auto Rows = Enumerate();
		EXPECT_TRUE(std::ranges::any_of(Rows, [&](const auto& Item) { return Item.GetVirtualPath() == Asset->GetObjectPath(); }));
	}

}
