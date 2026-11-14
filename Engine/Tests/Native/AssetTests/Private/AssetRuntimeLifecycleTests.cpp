#include "Asset/Load.h"
#include "Misc/UnifiedError.h"
#include "Asset/Cook.h"
#include "Asset/Mutation.h"
#include "NativePackageUnloadTestSupport.h"
#include "Asset/RegistryOperations.h"
#include "AssetRegistry/Scan.h"
#include "Engine/StreamableManager.h"
#include "Asset/AssetRuntimeStateInternal.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectPtr.h"
#include "DObject/Package.h"
#include "DObject/PackageFormat.h"
#include "DObject/PackageLinker.h"
#include "DObject/SoftObjectPtr.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPathTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	FSoftObjectPath DefaultAssetPath;

	// Two registration batches exercise both initial and newly loaded module CDOs.
	template<int Index>
	class TAssetDefaultOwner : public DObject
	{
	public:
		inline static FClassRegistrationInfo Registration;
		inline static bool bLoadSucceeded = false;
		inline static std::string LoadMessage;
		inline static bool bSawCookedDomain = false;
		inline static bool bConstructedBeforeObjectInit = false;
		static constexpr const char* Name = Index == 0 ? "DBootAssetDefaultOwner" : "DLateAssetDefaultOwner";

		explicit TAssetDefaultOwner(const FObjectInitializer& Initializer) : DObject(Initializer)
		{
			std::optional<Durin::UnifiedError::FError> ResultError;
			if (!IsClassDefaultObject()) return;
			bSawCookedDomain = GetAssetRuntimeConfiguration().IsCooked();
			bConstructedBeforeObjectInit = !IsDObjectInitialized();
			const auto Result = LoadObject(DefaultAssetPath, DObject::StaticClass(), nullptr, &ResultError);
			bLoadSucceeded = (Result != nullptr);
			LoadMessage = ResultError ? ResultError->ToString() : "";
			Asset = Result;
		}
		static void __DefaultConstructor(const FObjectInitializer& Initializer)
		{
			new (Initializer.GetObj()) TAssetDefaultOwner(Initializer);
		}
		static auto StaticClassNoRegister() -> DClass*
		{
			if (!Registration.InnerSingleton)
				GetPrivateStaticClassBody("", Name, Registration.InnerSingleton, nullptr,
					sizeof(TAssetDefaultOwner), alignof(TAssetDefaultOwner), EClassFlags::None,
					&InternalConstructor<TAssetDefaultOwner>, &DObject::StaticClass);
			return Registration.InnerSingleton;
		}
		static auto StaticClass() -> DClass*
		{
			if (!Registration.OuterSingleton)
			{
				using namespace DurinCodeGen;
				static const FObjectPropertyParams AssetProperty = FObjectPropertyParams::ObjectPtr<DObject>(
					"Asset", EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(TAssetDefaultOwner, Asset), &DObject::StaticClass);
				static const FPropertyParamsBase* Properties[]{&AssetProperty};
				static const FClassParams Params{&StaticClassNoRegister, Name, Name, Properties, std::size(Properties)};
				Registration.OuterSingleton = ConstructDClass(Params);
			}
			return Registration.OuterSingleton;
		}
		static auto Register() -> void
		{
			RegisterCompiledInInfo(&StaticClass, &StaticClassNoRegister, Name, Registration);
		}
		TObjectPtr<DObject> Asset;
	};
}

// Own a fresh process: no global test environment may initialize Engine Asset.
TEST(FAssetRuntimeLifecycleTests, ColdCookedBootAndReflectedDefaultsPreserveFinalDomain)
{
	std::optional<Durin::UnifiedError::FError> BeforeInitError;
	std::optional<Durin::UnifiedError::FError> ClosedPackageError;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId();
	GIsGameThreadIdInitialized = true;
	FNameInit();
	ASSERT_FALSE(IsDObjectInitialized());
	const auto Root = Testing::CreateTestFixtureDirectory("CookedBootstrap");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/Boot/", (Root / "Content").generic_string());
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/Boot/Default.Default", DefaultAssetPath));
	const auto InitialObjectCount = GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates).size();
	auto& Runtime = FAssetRuntimeState::Get();
	EXPECT_FALSE(Runtime.IsAcceptingRequests());
	const auto BeforeInit = LoadObject(DefaultAssetPath, nullptr, nullptr, &BeforeInitError);
	ASSERT_FALSE(BeforeInit);
	ASSERT_TRUE(BeforeInitError);
	EXPECT_TRUE(BeforeInitError->Is(AssetLoadErrors::NotInitialized));
	EXPECT_EQ(GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates).size(), InitialObjectCount);
	EXPECT_EQ(SavePackage(nullptr).Error, EAssetWriteError::InUse);
	bool bCallback = false;
	const auto Rejected = GetStreamableManager().RequestAsyncLoad(DefaultAssetPath, [&](const auto&) { bCallback = true; });
	ASSERT_TRUE(Rejected->IsComplete());
	EXPECT_TRUE(Rejected->HasError());
	ProcessAsyncLoading();
	EXPECT_FALSE(bCallback);
	ShutdownAssetRuntime(); // A cold shutdown must not require reflected types or choose a domain.
	EXPECT_FALSE(IsDObjectInitialized());

	// Encode an empty static asset without registering any reflected type first.
	// With no fields its complete Authored and Cooked representations are identical.
	ObjectPackage::FLinkerTables Linker;
	Linker.Summary.PackagePath = DefaultAssetPath.GetPackagePath();
	ObjectPackage::FPackageIndex Export;
	ASSERT_TRUE(ObjectPackage::FPackageIndex::TryExport(0, Export));
	Linker.Summary.TopLevelAssets.push_back({Export, DefaultAssetPath.GetAssetPath(), "Durin::DObject"});
	Linker.Exports.push_back({.ObjectName = "Default", .ClassName = "Durin::DObject"});
	FByteBuffer Bytes, Bulk;
	const auto Encoded = ObjectPackage::WritePackage(Linker, Bytes, Bulk);
	ASSERT_TRUE(Encoded) << ObjectPackage::FormatPackageError(Encoded);
	std::filesystem::create_directories(Root / "Boot");
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, Root / "Boot/Default.dasset"));
	const auto Hash = FXxHash128::HashBuffer(Bytes);
	const FCookManifest Manifest{.TargetPlatform = ECookTargetPlatform::Win64, .TargetProfile = ECookTargetProfile::Game,
		.Entries = {{ECookManifestEntryKind::CookedPackage, CookManifestEntryPresent, "Boot/Default.dasset", Bytes.size(), Hash.HashLow, Hash.HashHigh}}};
	ASSERT_TRUE(EncodeCookManifest(Manifest, Bytes));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, Root / "CookManifest.bin"));
	FAssetRuntimeConfiguration Configuration = FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(Root, Configuration));
	ASSERT_TRUE(InitializeAssetRuntime(Configuration));
	EXPECT_FALSE(IsDObjectInitialized());
	EXPECT_EQ(GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates).size(), InitialObjectCount);
	EXPECT_TRUE(InitializeAssetRuntime(Configuration));
	EXPECT_FALSE(InitializeAssetRuntime());
	TAssetDefaultOwner<0>::Register();
	DObjectInit();
	EXPECT_TRUE(TAssetDefaultOwner<0>::bSawCookedDomain);
	EXPECT_TRUE(TAssetDefaultOwner<0>::bConstructedBeforeObjectInit);
	EXPECT_TRUE(TAssetDefaultOwner<0>::bLoadSucceeded) << TAssetDefaultOwner<0>::LoadMessage;
	ASSERT_NE(TAssetDefaultOwner<0>::StaticClass()->GetDefaultObject(), nullptr);

	TAssetDefaultOwner<1>::Register();
	ProcessNewlyLoadedDObjects();
	ASSERT_TRUE(TAssetDefaultOwner<1>::bLoadSucceeded) << TAssetDefaultOwner<1>::LoadMessage;
	EXPECT_TRUE(TAssetDefaultOwner<1>::bSawCookedDomain);
	const auto* Default = static_cast<const TAssetDefaultOwner<1>*>(TAssetDefaultOwner<1>::StaticClass()->GetDefaultObject());
	ASSERT_NE(Default, nullptr);
	ASSERT_NE(Default->Asset.Get(), nullptr);
	TSoftObjectPtr<DObject> Soft(DefaultAssetPath);
	ASSERT_EQ(Soft.Get(), Default->Asset.Get());
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Soft.Get(), Default->Asset.Get());
	EXPECT_FALSE(Default->Asset->IsGarbage());
	EXPECT_TRUE(InitializeAssetRuntime(Configuration));
	EXPECT_FALSE(Default->Asset->IsGarbage());

	Runtime.StopAcceptingRequests();
	EXPECT_FALSE(InitializeAssetRuntime(Configuration));
	EXPECT_FALSE(InitializeAssetRuntime());
	EXPECT_EQ(GetAssetRuntimeConfiguration(), Configuration);
	EXPECT_EQ(Soft.Get(), nullptr); // Cached soft references cannot bypass closed admission.
	const auto ClosedPackage = LoadPackage(DefaultAssetPath.GetPackagePath(), nullptr, &ClosedPackageError);
	ASSERT_FALSE(ClosedPackage);
	ASSERT_TRUE(ClosedPackageError);
	EXPECT_TRUE(ClosedPackageError->Is(AssetLoadErrors::ShuttingDown));
	ReleaseClassDefaultObjects();
	ShutdownAssetRuntime();
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(InitializeAssetRuntime());
	EXPECT_TRUE(GetAssetRuntimeConfiguration().IsAuthored());
	ShutdownAssetRuntime();
	ShutdownAssetRuntime();
}
