#include <gtest/gtest.h>

#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "Asset/AssetCook.h"
#if DURIN_WITH_EDITOR
	#include "AssetMaintenance/CompatibilityAudit.h"
	#include "AssetMaintenance/CanonicalResave.h"
#endif
#include "Asset/AssetPackageV9Codec.h"
#include "Asset/AssetPackageByteSource.h"
#include "Asset/AssetPropertyKindTraits.h"
#include "Asset/PackageVersionPolicy.h"
#include "Asset/BulkData.h"
#include "Asset/EditorBulkData.h"
#include "Asset/EditorBulkDataStorage.h"
#include "Asset/Testing.h"
#include "AssetRegistry/Publication.h"
#include "CoreGlobals.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DefaultDeltaPlan.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/PackageFormat.h"
#include "DObject/PropertyKindTraits.h"
#include "DObject/WeakObjectPtr.h"
#include "Misc/FileTime.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/FileHelper.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"
#include "Threading/RunnableThread.h"

#include <chrono>
#include <bit>
#include <iostream>
#include <limits>

namespace AssetStructTest
{
	inline uint64 CodecSerializeLoadCount = 0;
	inline uint64 CodecPostDeserializeCount = 0;
	inline Durin::EDStructDeserializeSource CodecPostDeserializeSource =
		Durin::EDStructDeserializeSource::RuntimeArchive;
	inline uint32 CodecPostDeserializeVersion = 0;
	inline bool RejectCodecPostDeserialize = false;

	struct FCodecSource
	{
		int32 Value = 0;
	};

	struct FCodecTarget
	{
		int32 Value = 0;
	};

	struct FStructMigrationVersion
	{
		inline static constexpr Durin::FGuid Guid{
			0x51aa15c4, 0x14df423f, 0x81dc1a82, 0x638fc15e};
		enum Type : int32
		{
			BeforeCustomVersionWasAdded = -1,
			Changed = 1,
			LatestVersion = Changed,
		};
	};

	struct FMigratingValue
	{
		float Value = 0.0f;
		int32 Value_DEPRECATED = 0;
	};

	inline uint64 MigrationPostDeserializeCount = 0;
	inline bool RejectMigrationPostDeserialize = false;
} // namespace AssetStructTest

namespace Durin
{
	template<>
	struct TDStructOpsTraits<AssetStructTest::FCodecSource>
		: TDStructOpsTraitsBase<AssetStructTest::FCodecSource>
	{
		static constexpr bool bWithSerializer = true;
		static constexpr bool bWithPostDeserialize = true;

		static auto Serialize(FArchive& Ar, AssetStructTest::FCodecSource& Value) -> void
		{
			if (Ar.IsLoading()) ++AssetStructTest::CodecSerializeLoadCount;
			auto Type = FArchiveLogicalTypeDescriptor::Scalar(true, 32);
			auto Field = EnterArchiveField(
				Ar, {FName("Tests::FCodecSource"), FName("Value"), Type});
			Ar << Value.Value;
		}

		static auto PostDeserialize(AssetStructTest::FCodecSource&,
			FDStructPostDeserializeContext& Context) -> bool
		{
			++AssetStructTest::CodecPostDeserializeCount;
			AssetStructTest::CodecPostDeserializeSource = Context.Source;
			AssetStructTest::CodecPostDeserializeVersion = Context.SourceVersion;
			if (!AssetStructTest::RejectCodecPostDeserialize) return true;
			if (Context.Error) *Context.Error = "Injected custom struct repair rejection.";
			return false;
		}
	};

	template<>
	struct TDStructOpsTraits<AssetStructTest::FCodecTarget>
		: TDStructOpsTraitsBase<AssetStructTest::FCodecTarget>
	{
		static constexpr bool bHasCompleteAuthoredFields = false;
	};

	template<>
	struct TDStructOpsTraits<AssetStructTest::FMigratingValue>
		: TDStructOpsTraitsBase<AssetStructTest::FMigratingValue>
	{
		static constexpr bool bWithPostDeserialize = true;

		static auto PostDeserialize(AssetStructTest::FMigratingValue& Value,
			FDStructPostDeserializeContext& Context) -> bool
		{
			++AssetStructTest::MigrationPostDeserializeCount;
			if (AssetStructTest::RejectMigrationPostDeserialize)
				return Context.Fail("Injected migrating struct rejection.");
			const FArchiveCustomVersion* Version = Context.VersionContext
				? Context.VersionContext->FindCustom(AssetStructTest::FStructMigrationVersion::Guid)
				: nullptr;
			if ((!Version ? AssetStructTest::FStructMigrationVersion::BeforeCustomVersionWasAdded
				: Version->Version) < AssetStructTest::FStructMigrationVersion::Changed)
				Value.Value = static_cast<float>(Value.Value_DEPRECATED) * 10.0f;
			return true;
		}
	};
} // namespace Durin

namespace
{
	auto MakeTopLevelObjectPath(const Durin::FPackagePath& PackagePath,
		std::string_view AssetName) -> Durin::FObjectPath
	{
		Durin::FTopLevelAssetPath AssetPath;
		Durin::FObjectPath ObjectPath;
		EXPECT_TRUE(Durin::FTopLevelAssetPath::TryCreate(
			PackagePath, AssetName, AssetPath));
		EXPECT_TRUE(Durin::FObjectPath::TryCreate(
			AssetPath, std::span<const std::string>{}, ObjectPath));
		return ObjectPath;
	}

	static_assert(Durin::OrdinaryAssetPackageWriterVersion ==
		Durin::ObjectPackage::DastV9FormatVersion);
	static_assert(Durin::ObjectPackage::SupportedPackageReaderVersions ==
		decltype(Durin::ObjectPackage::SupportedPackageReaderVersions){
			Durin::ObjectPackage::DastV9FormatVersion});
	static_assert(!Durin::ObjectPackage::IsSupportedPackageReaderVersion(
		4));
	static_assert(!Durin::ObjectPackage::IsSupportedPackageReaderVersion(
		5u));
	static_assert(Durin::ObjectPackage::IsSupportedPackageReaderVersion(
		Durin::ObjectPackage::DastV9FormatVersion));
	static_assert(!Durin::ObjectPackage::IsSupportedPackageReaderVersion(
		7u));

	auto MakeFormerMainObjectPath(const Durin::FPackagePath& PackagePath)
		-> Durin::FObjectPath
	{
		return MakeTopLevelObjectPath(PackagePath, PackagePath.GetPackageName());
	}

	auto RelocateAssetsForTest(
		std::span<const Durin::FAssetRelocationMapping> Mappings
	)
		-> Durin::FAssetResult
	{
		Durin::FAssetRelocationSummary Summary;
		Durin::FAssetMutationJob Job;
		Durin::FAssetResult Result =
			Durin::PrepareAssetRelocationJob(
				Mappings, Summary, Job);
		if (Result) Result = Job.ResumeForward();
		return Result;
	}

	auto RelocateAssetForTest(
		const Durin::FPackagePath& Source,
		const Durin::FPackagePath& Destination
	)
		-> Durin::FAssetResult
	{
		const Durin::FAssetRelocationMapping Mapping{
			Source, Destination
		};
		return RelocateAssetsForTest(std::span{&Mapping, 1});
	}

	auto FixUpRedirectorsForTest(
		std::span<const Durin::FPackagePath> Redirectors,
		Durin::EAssetRedirectorFixupMode Mode =
			Durin::EAssetRedirectorFixupMode::RewriteAndDelete)
		-> Durin::FAssetResult
	{
		Durin::FAssetRedirectorFixupSummary Summary;
		Durin::FAssetMutationJob Job;
		Durin::FAssetResult Result =
			Durin::PrepareRedirectorFixupJob(
				Redirectors, Mode, Summary, Job);
		return Result ? Job.ResumeForward() : Result;
	}

	// Test cleanup follows the production target-plus-alias closure contract while
	// avoiding a second editor-level destructive operation in asset tests.
	auto DeleteAssetClosureForTest(
		std::initializer_list<Durin::FPackagePath> Paths
	)
		-> Durin::FAssetResult
	{
		const std::vector<Durin::FPackagePath> DeletionPaths(Paths);
		Durin::FAssetDeletionJob Job;
		std::vector<Durin::FAssetDeletionBatchBlocker> Blockers;
		Durin::FAssetResult Result =
			Durin::PrepareAssetDeletionJob(
				DeletionPaths, {}, Job, Blockers
			);
		if (!Result) return Result;
		if (!Blockers.empty())
			return {
				Durin::EAssetError::InUse,
				Blockers.front().Details
			};
		const auto RemoveFiles = [&]() -> Durin::FAssetResult {
		for (const Durin::FAssetDeletionBatchEntry& Entry : Job.GetEntries())
		{
			std::error_code Error;
			if (!std::filesystem::remove(Entry.RegistryEntry.PhysicalPath, Error)
				|| Error)
				return {
					Durin::EAssetError::IoError,
					std::format(
						"Could not remove test asset {}: {}",
						Entry.RegistryEntry.PackagePath.ToString(),
						Error.message()
					)
				};
			for (const std::filesystem::path& Companion : Entry.CompanionFiles)
			{
				Error.clear();
				if (!std::filesystem::remove(Companion, Error) || Error)
					return {
						Durin::EAssetError::IoError,
						std::format(
							"Could not remove test companion {}: {}",
							Companion.generic_string(), Error.message()
						)
					};
			}
		}
		return {};
		};
		return Job.Delete({
			.Delete = RemoveFiles,
		});
	}

	template<typename T>
	auto VectorNum(const void* Container) -> uint64 { return static_cast<const std::vector<T>*>(Container)->size(); }
	template<typename T>
	auto VectorGet(const void* Container, uint64 Index) -> const void* { return &(*static_cast<const std::vector<T>*>(Container))[Index]; }
	template<typename T>
	auto VectorGetMutable(void* Container, uint64 Index) -> void* { return &(*static_cast<std::vector<T>*>(Container))[Index]; }
	template<typename T>
	auto VectorResize(void* Container, uint64 Num) -> bool
	{
		static_cast<std::vector<T>*>(Container)->resize(Num);
		return true;
	}

	auto GIntVectorHelper() -> const Durin::FArrayOps* { return Durin::ResolveArrayOps<std::vector<int32>>(); }
	auto GGuidVectorHelper() -> const Durin::FArrayOps* { return Durin::ResolveArrayOps<std::vector<Durin::FGuid>>(); }
	auto GVector3VectorHelper() -> const Durin::FArrayOps* { return Durin::ResolveArrayOps<std::vector<Durin::FVector3>>(); }
	auto GMigratingValueVectorHelper() -> const Durin::FArrayOps*
	{
		return Durin::ResolveArrayOps<std::vector<AssetStructTest::FMigratingValue>>();
	}

	using FScoreMap = std::unordered_map<std::string, int32>;
	auto MapNum(const void* Container) -> uint64 { return static_cast<const FScoreMap*>(Container)->size(); }
	auto MapKey(const void* Container, uint64 Index) -> const void*
	{
		auto It = static_cast<const FScoreMap*>(Container)->begin();
		std::advance(It, Index);
		return &It->first;
	}
	auto MapValue(const void* Container, uint64 Index) -> const void*
	{
		auto It = static_cast<const FScoreMap*>(Container)->begin();
		std::advance(It, Index);
		return &It->second;
	}
	auto MapMutableValue(void* Container, uint64 Index) -> void*
	{
		auto It = static_cast<FScoreMap*>(Container)->begin();
		std::advance(It, Index);
		return &It->second;
	}
	auto MapClear(void* Container) -> void { static_cast<FScoreMap*>(Container)->clear(); }
	auto CreateString() -> void* { return new std::string(); }
	auto CopyString(const void* Value) -> void* { return new std::string(*static_cast<const std::string*>(Value)); }
	auto DestroyString(void* Value) -> void { delete static_cast<std::string*>(Value); }
	auto CreateInt() -> void* { return new int32(); }
	auto DestroyInt(void* Value) -> void { delete static_cast<int32*>(Value); }
	auto MapInsert(void* Container, const void* Key, const void* Value) -> bool
	{
		static_cast<FScoreMap*>(Container)->insert_or_assign(*static_cast<const std::string*>(Key), *static_cast<const int32*>(Value));
		return true;
	}
	auto MapContains(const void* Container, const void* Key) -> bool
	{
		return static_cast<const FScoreMap*>(Container)->contains(*static_cast<const std::string*>(Key));
	}
	auto MapRenameKey(void* Container, const void* OldKey, const void* NewKey) -> bool
	{
		auto* Map = static_cast<FScoreMap*>(Container);
		const std::string OldKeyCopy = *static_cast<const std::string*>(OldKey);
		const std::string NewKeyCopy = *static_cast<const std::string*>(NewKey);
		if (OldKeyCopy == NewKeyCopy || Map->contains(NewKeyCopy)) return false;
		auto Node = Map->extract(OldKeyCopy);
		if (Node.empty()) return false;
		Node.key() = NewKeyCopy;
		Map->insert(std::move(Node));
		return true;
	}
	auto MapRemove(void* Container, const void* Key) -> bool
	{
		return static_cast<FScoreMap*>(Container)->erase(*static_cast<const std::string*>(Key)) != 0;
	}
	auto GScoreMapHelper() -> const Durin::FMapOps* { return Durin::ResolveMapOps<FScoreMap>(); }

	using FVectorMap = std::unordered_map<std::string, Durin::FVector3>;
	auto VectorMapNum(const void* Container) -> uint64 { return static_cast<const FVectorMap*>(Container)->size(); }
	auto VectorMapKey(const void* Container, uint64 Index) -> const void*
	{
		auto It = static_cast<const FVectorMap*>(Container)->begin();
		std::advance(It, Index);
		return &It->first;
	}
	auto VectorMapValue(const void* Container, uint64 Index) -> const void*
	{
		auto It = static_cast<const FVectorMap*>(Container)->begin();
		std::advance(It, Index);
		return &It->second;
	}
	auto VectorMapMutableValue(void* Container, uint64 Index) -> void*
	{
		auto It = static_cast<FVectorMap*>(Container)->begin();
		std::advance(It, Index);
		return &It->second;
	}
	auto VectorMapClear(void* Container) -> void { static_cast<FVectorMap*>(Container)->clear(); }
	auto CreateVector3() -> void* { return new Durin::FVector3(0.0); }
	auto DestroyVector3(void* Value) -> void { delete static_cast<Durin::FVector3*>(Value); }
	auto VectorMapInsert(void* Container, const void* Key, const void* Value) -> bool
	{
		static_cast<FVectorMap*>(Container)->insert_or_assign(
			*static_cast<const std::string*>(Key), *static_cast<const Durin::FVector3*>(Value)
		);
		return true;
	}
	auto VectorMapContains(const void* Container, const void* Key) -> bool
	{
		return static_cast<const FVectorMap*>(Container)->contains(*static_cast<const std::string*>(Key));
	}
	auto VectorMapRenameKey(void* Container, const void* OldKey, const void* NewKey) -> bool
	{
		auto* Map = static_cast<FVectorMap*>(Container);
		const std::string OldKeyCopy = *static_cast<const std::string*>(OldKey);
		const std::string NewKeyCopy = *static_cast<const std::string*>(NewKey);
		if (OldKeyCopy == NewKeyCopy || Map->contains(NewKeyCopy)) return false;
		auto Node = Map->extract(OldKeyCopy);
		if (Node.empty()) return false;
		Node.key() = NewKeyCopy;
		Map->insert(std::move(Node));
		return true;
	}
	auto VectorMapRemove(void* Container, const void* Key) -> bool
	{
		return static_cast<FVectorMap*>(Container)->erase(*static_cast<const std::string*>(Key)) != 0;
	}
	auto GVectorMapHelper() -> const Durin::FMapOps* { return Durin::ResolveMapOps<FVectorMap>(); }
	uint64 GSoftPackageConstructionCount = 0;
	uint64 GImportMetadataConstructionCount = 0;

	class DImportMetadataForTest : public Durin::DObject
	{
	public:
		explicit DImportMetadataForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
			++GImportMetadataConstructionCount;
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DImportMetadataForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DImportMetadataForTest",
					sizeof(DImportMetadataForTest), alignof(DImportMetadataForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DImportMetadataForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DImportMetadataForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			using namespace Durin;
			using namespace Durin::DurinCodeGen;
			static const FUInt32PropertyParams SchemaVersionProperty{
				"SchemaVersion", EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DImportMetadataForTest, SchemaVersion)};
			static const FStringPropertyParams SourcePathProperty{
				"SourcePath", EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DImportMetadataForTest, SourcePath)};
			static const FPropertyParamsBase* Properties[] = {
				&SchemaVersionProperty, &SourcePathProperty};
			static const FClassParams Params{
				&StaticClassNoRegister, "Tests::DImportMetadataForTest",
				"DImportMetadataForTest", Properties, std::size(Properties)};
			static DClass* Class = ConstructDClass(Params);
			return Class;
		}

		uint32 SchemaVersion = 1;
		std::string SourcePath;
	};

	class DReplayImportMetadataForTest : public DImportMetadataForTest
	{
	public:
		explicit DReplayImportMetadataForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DImportMetadataForTest(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DReplayImportMetadataForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DReplayImportMetadataForTest",
					sizeof(DReplayImportMetadataForTest),
					alignof(DReplayImportMetadataForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DReplayImportMetadataForTest>);
				Class->SetSuperStructBase(DImportMetadataForTest::StaticClass());
				Class->Register(
					Durin::DClass::StaticClass, "", "DReplayImportMetadataForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			using namespace Durin;
			using namespace Durin::DurinCodeGen;
			static const FStringPropertyParams TranslatorProperty{
				"Translator", EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DReplayImportMetadataForTest, Translator)};
			static const FUInt64PropertyParams FingerprintProperty{
				"Fingerprint", EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DReplayImportMetadataForTest, Fingerprint)};
			static const FPropertyParamsBase* Properties[] = {
				&TranslatorProperty, &FingerprintProperty};
			static const FClassParams Params{
				&StaticClassNoRegister, "Tests::DReplayImportMetadataForTest",
				"DReplayImportMetadataForTest", Properties, std::size(Properties)};
			static DClass* Class = ConstructDClass(Params);
			return Class;
		}

		std::string Translator;
		uint64 Fingerprint = 0;
	};

	class DImportMetadataOwnerForTest : public Durin::DObject
	{
	public:
		explicit DImportMetadataOwnerForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DImportMetadataOwnerForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DImportMetadataOwnerForTest",
					sizeof(DImportMetadataOwnerForTest), alignof(DImportMetadataOwnerForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DImportMetadataOwnerForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DImportMetadataOwnerForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			using namespace Durin;
			using namespace Durin::DurinCodeGen;
			static const FObjectPropertyParams ImportDataProperty =
				FObjectPropertyParams::ObjectPtr<DImportMetadataForTest>(
					"AssetImportData", EPropertyFlags::EditorOnly, 1,
					STRUCT_OFFSET_UINT16(DImportMetadataOwnerForTest, AssetImportData),
					&DImportMetadataForTest::StaticClass);
			static const FUInt32PropertyParams RuntimeValueProperty{
				"RuntimeValue", EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DImportMetadataOwnerForTest, RuntimeValue)};
			static const FPropertyParamsBase* Properties[] = {
				&ImportDataProperty, &RuntimeValueProperty};
			static const FClassParams Params{
				&StaticClassNoRegister, "Tests::DImportMetadataOwnerForTest",
				"DImportMetadataOwnerForTest", Properties, std::size(Properties)};
			static DClass* Class = ConstructDClass(Params);
			return Class;
		}

		Durin::TObjectPtr<DImportMetadataForTest> AssetImportData;
		uint32 RuntimeValue = 0;
	};

	class DPackageAssetForTest : public Durin::DObject
	{
	public:
		explicit DPackageAssetForTest(const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
			DefaultChild = Durin::NewObject<Durin::DObject>(this, "DefaultChild");
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X) { new (X.GetObj()) DPackageAssetForTest(X); }

		auto PostLoad(std::string& OutError) -> bool override
		{
			return DObject::PostLoad(OutError);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(Durin::EC_StaticConstructor, "DPackageAssetForTest", sizeof(DPackageAssetForTest), alignof(DPackageAssetForTest), Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None, Durin::EClassCastFlags::DClass, (Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DPackageAssetForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DPackageAssetForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static const char* const ValueLegacyNames[] = {"LegacyValue"};
			static const Durin::DurinCodeGen::FInt32PropertyParams ValueProp =
				Durin::DurinCodeGen::WithLegacyNames(
					Durin::DurinCodeGen::FInt32PropertyParams{
						"Value", Durin::EPropertyFlags::None, 1,
						STRUCT_OFFSET_UINT16(DPackageAssetForTest, Value)},
					ValueLegacyNames, std::size(ValueLegacyNames));
			static const Durin::DurinCodeGen::FStringPropertyParams LabelProp = {"Label", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DPackageAssetForTest, Label)};
			static const Durin::DurinCodeGen::FNamePropertyParams DisplayNameProp = {"DisplayName", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DPackageAssetForTest, DisplayName)};
			static const Durin::DurinCodeGen::FGuidPropertyParams GuidProp = {"PersistentId", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DPackageAssetForTest, PersistentId)};
			static const Durin::DurinCodeGen::FGuidPropertyParams GuidInner = {"RelatedIds_Inner", Durin::EPropertyFlags::None, 1, 0};
			static const Durin::DurinCodeGen::FArrayPropertyParams GuidsProp = {"RelatedIds", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DPackageAssetForTest, RelatedIds), &GuidInner, &GGuidVectorHelper};
			static const Durin::DurinCodeGen::FInt32PropertyParams ScoreInner = {"Scores_Inner", Durin::EPropertyFlags::None, 1, 0};
			static const Durin::DurinCodeGen::FArrayPropertyParams ScoresProp = {"Scores", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DPackageAssetForTest, Scores), &ScoreInner, &GIntVectorHelper};
			static const Durin::DurinCodeGen::FStringPropertyParams MapKeyProp = {"NamedScores_Key", Durin::EPropertyFlags::None, 1, 0};
			static const Durin::DurinCodeGen::FInt32PropertyParams MapValueProp = {"NamedScores_Value", Durin::EPropertyFlags::None, 1, 0};
			static const Durin::DurinCodeGen::FMapPropertyParams NamedScoresProp = {"NamedScores", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DPackageAssetForTest, NamedScores), &MapKeyProp, &MapValueProp, &GScoreMapHelper};
			static const Durin::DurinCodeGen::FObjectPropertyParams ChildProp = Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>("DefaultChild", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DPackageAssetForTest, DefaultChild), &Durin::DObject::StaticClass);
			static const Durin::DurinCodeGen::FObjectPropertyParams ExternalProp = Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>("ExternalReference", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DPackageAssetForTest, ExternalReference), &Durin::DObject::StaticClass);
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&ValueProp, &LabelProp, &DisplayNameProp, &GuidProp, &GuidsProp, &ScoresProp,
				&NamedScoresProp, &ChildProp, &ExternalProp
			};
			static const Durin::DurinCodeGen::FClassParams Params = {&StaticClassNoRegister, "Tests::DPackageAssetForTest", "DPackageAssetForTest", Properties, std::size(Properties)};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		int32 Value = 0;
		std::string Label;
		Durin::FName DisplayName;
		Durin::FGuid PersistentId;
		std::vector<Durin::FGuid> RelatedIds;
		std::vector<int32> Scores;
		FScoreMap NamedScores;
		Durin::TObjectPtr<Durin::DObject> DefaultChild;
		Durin::TObjectPtr<Durin::DObject> ExternalReference;
	};

	class DBulkPackageAssetForTest : public Durin::DObject
	{
	public:
		explicit DBulkPackageAssetForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer), Payload(Durin::FGuid{0x55112233, 0x44556677,
				0x8899aabb, 0xccddeeff})
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DBulkPackageAssetForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(Durin::EC_StaticConstructor,
					"DBulkPackageAssetForTest", sizeof(DBulkPackageAssetForTest),
					alignof(DBulkPackageAssetForTest), Durin::EObjectFlags::NoFlags,
					Durin::EClassFlags::None, Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DBulkPackageAssetForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "",
					"DBulkPackageAssetForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static const Durin::DurinCodeGen::FBulkDataPropertyParams PayloadProp =
				Durin::DurinCodeGen::FBulkDataPropertyParams::Create<
					Durin::FEditorBulkData>(
						"Payload", Durin::EPropertyFlags::None, 1,
						STRUCT_OFFSET_UINT16(DBulkPackageAssetForTest, Payload));
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&PayloadProp};
			static const Durin::DurinCodeGen::FClassParams Params = {
				&StaticClassNoRegister, "Tests::DBulkPackageAssetForTest",
				"DBulkPackageAssetForTest", Properties, std::size(Properties)};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		Durin::FEditorBulkData Payload;
	};

	struct FSchemaMigrationVersion
	{
		inline static constexpr Durin::FGuid Guid{
			0x78d61a4e, 0x29554b39, 0x93bbf092, 0x9c8eb965};
		enum Type : int32
		{
			BeforeCustomVersionWasAdded = -1,
			Initial = 0,
			Changed = 1,
			LatestVersion = Changed,
		};
	};

	bool GRejectSchemaMigrationPostLoad = false;

	auto GetMigratingValueStructNoRegister() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = new Durin::DStruct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FMigratingValue"),
			Durin::FName("FMigratingValue"), sizeof(AssetStructTest::FMigratingValue),
			alignof(AssetStructTest::FMigratingValue), Durin::EObjectFlags::Transient);
		return Struct;
	}

	auto GetMigratingValueStruct() -> Durin::DStruct*
	{
		using namespace Durin;
		using namespace Durin::DurinCodeGen;
		static const char* const Targets[] = {"Value"};
		static const FPropertyDeprecationParams Deprecation{
			AssetStructTest::FStructMigrationVersion::Guid,
			AssetStructTest::FStructMigrationVersion::Changed,
			AssetStructTest::FStructMigrationVersion::LatestVersion,
			"Value", Targets, std::size(Targets)};
		static const FFloatPropertyParams ValueProp{
			"Value", EPropertyFlags::None, 1,
			STRUCT_OFFSET_UINT16(AssetStructTest::FMigratingValue, Value)};
		static const FInt32PropertyParams DeprecatedProp = WithDeprecation(
			FInt32PropertyParams{"Value_DEPRECATED", EPropertyFlags::Deprecated, 1,
				STRUCT_OFFSET_UINT16(AssetStructTest::FMigratingValue, Value_DEPRECATED)},
			&Deprecation);
		static const FPropertyParamsBase* Properties[] = {&ValueProp, &DeprecatedProp};
		static const FStructParams Params{
			&GetMigratingValueStructNoRegister, "Tests::FMigratingValue", "FMigratingValue",
			sizeof(AssetStructTest::FMigratingValue), alignof(AssetStructTest::FMigratingValue),
			Properties, std::size(Properties),
			&GetDStructOps<AssetStructTest::FMigratingValue>()};
		return ConstructDStruct(Params);
	}

	class DSchemaMigrationAssetForTest : public Durin::DObject
	{
	public:
		explicit DSchemaMigrationAssetForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DSchemaMigrationAssetForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DSchemaMigrationAssetForTest",
					sizeof(DSchemaMigrationAssetForTest), alignof(DSchemaMigrationAssetForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DSchemaMigrationAssetForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DSchemaMigrationAssetForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			using namespace Durin;
			using namespace Durin::DurinCodeGen;
			static const char* const ATargets[] = {"A", "B"};
			static const char* const LeftTargets[] = {"Merged"};
			static const char* const RightTargets[] = {"Merged"};
			static const char* const DistanceTargets[] = {"Distance"};
			static const FPropertyDeprecationParams ADeprecation{
				FSchemaMigrationVersion::Guid, FSchemaMigrationVersion::Changed,
				FSchemaMigrationVersion::LatestVersion, "A", ATargets, std::size(ATargets)};
			static const FPropertyDeprecationParams LeftDeprecation{
				FSchemaMigrationVersion::Guid, FSchemaMigrationVersion::Changed,
				FSchemaMigrationVersion::LatestVersion, "Left", LeftTargets, std::size(LeftTargets)};
			static const FPropertyDeprecationParams RightDeprecation{
				FSchemaMigrationVersion::Guid, FSchemaMigrationVersion::Changed,
				FSchemaMigrationVersion::LatestVersion, "Right", RightTargets, std::size(RightTargets)};
			static const FPropertyDeprecationParams DistanceDeprecation{
				FSchemaMigrationVersion::Guid, FSchemaMigrationVersion::Changed,
				FSchemaMigrationVersion::LatestVersion, "Distance", DistanceTargets,
				std::size(DistanceTargets)};
			static const FFloatPropertyParams AProp{
				"A", EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DSchemaMigrationAssetForTest, A)};
			static const FFloatPropertyParams BProp{
				"B", EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DSchemaMigrationAssetForTest, B)};
			static const FInt32PropertyParams MergedProp{
				"Merged", EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DSchemaMigrationAssetForTest, Merged)};
			static const FFloatPropertyParams DistanceProp{
				"Distance", EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DSchemaMigrationAssetForTest, Distance)};
			static const FInt32PropertyParams AnchorProp{
				"Anchor", EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DSchemaMigrationAssetForTest, Anchor)};
			static const FStructPropertyParams StructDataProp{
				"StructData", EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DSchemaMigrationAssetForTest, StructData),
				&GetMigratingValueStruct};
			static const FInt32PropertyParams ADeprecatedProp = WithDeprecation(
				FInt32PropertyParams{"A_DEPRECATED", EPropertyFlags::Deprecated, 1,
					STRUCT_OFFSET_UINT16(DSchemaMigrationAssetForTest, A_DEPRECATED)},
				&ADeprecation);
			static const FInt32PropertyParams LeftDeprecatedProp = WithDeprecation(
				FInt32PropertyParams{"Left_DEPRECATED", EPropertyFlags::Deprecated, 1,
					STRUCT_OFFSET_UINT16(DSchemaMigrationAssetForTest, Left_DEPRECATED)},
				&LeftDeprecation);
			static const FInt32PropertyParams RightDeprecatedProp = WithDeprecation(
				FInt32PropertyParams{"Right_DEPRECATED", EPropertyFlags::Deprecated, 1,
					STRUCT_OFFSET_UINT16(DSchemaMigrationAssetForTest, Right_DEPRECATED)},
				&RightDeprecation);
			static const FFloatPropertyParams DistanceDeprecatedProp = WithDeprecation(
				FFloatPropertyParams{"Distance_DEPRECATED", EPropertyFlags::Deprecated, 1,
					STRUCT_OFFSET_UINT16(DSchemaMigrationAssetForTest, Distance_DEPRECATED)},
				&DistanceDeprecation);
			static const FPropertyParamsBase* Properties[] = {
				&AProp, &BProp, &MergedProp, &DistanceProp, &AnchorProp, &StructDataProp, &ADeprecatedProp,
				&LeftDeprecatedProp, &RightDeprecatedProp, &DistanceDeprecatedProp};
			static const FClassParams Params{
				&StaticClassNoRegister, "Tests::DSchemaMigrationAssetForTest",
				"DSchemaMigrationAssetForTest", Properties, std::size(Properties)};
			static DClass* Class = ConstructDClass(Params);
			return Class;
		}

		auto PostLoad(std::string& OutError) -> bool override
		{
			if (GRejectSchemaMigrationPostLoad)
			{
				OutError = "Injected schema migration rejection.";
				return false;
			}
			if (GetLoadedCustomVersion(FSchemaMigrationVersion::Guid).value_or(
				FSchemaMigrationVersion::BeforeCustomVersionWasAdded)
				< FSchemaMigrationVersion::Changed)
			{
				A = static_cast<float>(A_DEPRECATED) * 0.5f;
				B = static_cast<float>(A_DEPRECATED) * 2.0f;
				Merged = Left_DEPRECATED + Right_DEPRECATED;
				Distance = Distance_DEPRECATED * 100.0f;
			}
			return DObject::PostLoad(OutError);
		}

		float A = 0.0f;
		float B = 0.0f;
		int32 Merged = 0;
		float Distance = 0.0f;
		int32 Anchor = 0;
		AssetStructTest::FMigratingValue StructData;
		int32 A_DEPRECATED = 0;
		int32 Left_DEPRECATED = 0;
		int32 Right_DEPRECATED = 0;
		float Distance_DEPRECATED = 0.0f;
	};

	class DContainerMigrationAssetForTest : public Durin::DObject
	{
	public:
		explicit DContainerMigrationAssetForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DContainerMigrationAssetForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DContainerMigrationAssetForTest",
					sizeof(DContainerMigrationAssetForTest), alignof(DContainerMigrationAssetForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DContainerMigrationAssetForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DContainerMigrationAssetForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			using namespace Durin;
			using namespace Durin::DurinCodeGen;
			static const FStructPropertyParams ValuesInner{
				"Values_Inner", EPropertyFlags::None, 1, 0, &GetMigratingValueStruct};
			static const FArrayPropertyParams ValuesProperty{
				"Values", EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DContainerMigrationAssetForTest, Values),
				&ValuesInner, &GMigratingValueVectorHelper};
			static const FPropertyParamsBase* Properties[] = {&ValuesProperty};
			static const FClassParams Params{
				&StaticClassNoRegister, "Tests::DContainerMigrationAssetForTest",
				"DContainerMigrationAssetForTest", Properties, std::size(Properties)};
			static DClass* Class = ConstructDClass(Params);
			return Class;
		}

		std::vector<AssetStructTest::FMigratingValue> Values;
	};

	std::vector<Durin::EArchivePurpose> GAuthoredArchivePurposes;
	std::vector<uint32> GAuthoredArchiveFormatVersions;
	uint64 GAuthoredConstructCount = 0;
	uint64 GAuthoredLoadSerializeCount = 0;
	uint64 GCookedSerializeCount = 0;
	bool GRejectAuthoredLoad = false;
	bool GRejectAuthoredPostLoad = false;

	class DAuthoredArchiveAssetForTest : public Durin::DObject
	{
	public:
		explicit DAuthoredArchiveAssetForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
			++GAuthoredConstructCount;
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DAuthoredArchiveAssetForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DAuthoredArchiveAssetForTest",
					sizeof(DAuthoredArchiveAssetForTest), alignof(DAuthoredArchiveAssetForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DAuthoredArchiveAssetForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DAuthoredArchiveAssetForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static const Durin::DurinCodeGen::FClassParams Params = {
				&StaticClassNoRegister, "Tests::DAuthoredArchiveAssetForTest",
				"DAuthoredArchiveAssetForTest", nullptr, 0};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		auto Serialize(Durin::FArchive& Ar) -> void override
		{
			GAuthoredArchivePurposes.push_back(Ar.GetPurpose());
			if (const Durin::FArchiveFormatVersion* Format =
				Ar.GetVersionContext().FindFormat(Durin::FName("DAST")))
				GAuthoredArchiveFormatVersions.push_back(Format->Version);
			if (Ar.IsLoading() && Ar.GetPurpose() == Durin::EArchivePurpose::AuthoredPackage)
			{
				++GAuthoredLoadSerializeCount;
				if (GRejectAuthoredLoad)
				{
					Ar.Fail(Durin::EArchiveFailureCode::InvalidData,
						"Injected authored load rejection.");
					return;
				}
			}
			if (!bSkipSuper) DObject::Serialize(Ar);
			const Durin::FName DeclaringType("Tests::DAuthoredArchiveAssetForTest");
			auto NativeType = Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32);
			NativeType.NativeFieldVersion = 7;
			{
				auto Field = Durin::EnterArchiveField(
					Ar, {DeclaringType, Durin::FName("NativeValue"), NativeType});
				Ar << NativeValue;
			}
			if (bDuplicateField)
			{
				auto Field = Durin::EnterArchiveField(
					Ar, {DeclaringType, Durin::FName("NativeValue"), NativeType});
				Ar << NativeValue;
			}
			{
				auto Field = Durin::EnterArchiveField(Ar, {DeclaringType, Durin::FName("HardReference"),
					Durin::FArchiveLogicalTypeDescriptor::Object(Durin::DObject::StaticClass()->GetQualifiedName())});
				Durin::DObject* Value = HardReference.Get();
				Durin::SerializeArchiveObjectReference(Ar, Value);
				if (Ar.IsLoading() && !Ar.HasError()) HardReference = Value;
			}
			{
				auto Field = Durin::EnterArchiveField(Ar, {DeclaringType, Durin::FName("SoftReference"),
					Durin::FArchiveLogicalTypeDescriptor::SoftObject(Durin::DObject::StaticClass()->GetQualifiedName())});
				Durin::SerializeArchiveSoftObjectValue(Ar, SoftReference);
			}
			if (bLateField && !Ar.IsDiscovering())
			{
				auto Field = Durin::EnterArchiveField(Ar, {DeclaringType, Durin::FName("EmissionOnly"),
					Durin::FArchiveLogicalTypeDescriptor::Scalar(false, 8)});
				uint8 Value = 1;
				Ar << Value;
			}
			if (bUnsupportedCustomVersion)
				Ar.Fail(Durin::EArchiveFailureCode::UnsupportedVersion,
					"The package object stream cannot persist the requested authored custom version.");
		}

		auto SerializeCooked(Durin::FArchive& Ar) -> void override
		{
			++GCookedSerializeCount;
			if (!Ar.IsCooking() || !Ar.IsFilterEditorOnly()
				|| Ar.GetTarget().Platform != "Win64" || Ar.GetTarget().Profile != "Game")
			{
				Ar.Fail(Durin::EArchiveFailureCode::InvalidData,
					"Cooked test projection requires an explicit Win64 Game context.");
				return;
			}
			DObject::Serialize(Ar);
			const Durin::FName DeclaringType("Tests::DAuthoredArchiveAssetForTest");
			{
				auto Field = Durin::EnterArchiveField(Ar, {DeclaringType,
					Durin::FName("CookedValue"),
					Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)});
				int32 Projection = NativeValue * 2;
				Ar << Projection;
			}
			{
				auto BulkField = Durin::EnterArchiveField(Ar, {DeclaringType,
					Durin::FName("CookedBulk"),
					Durin::FArchiveLogicalTypeDescriptor::BulkData()});
				CookedBulk.Serialize(Ar, {
					.Alignment = 16,
					.StoragePolicy = Durin::EArchiveBulkDataStoragePolicy::AllowExternal});
			}
		}

		auto PostLoad(std::string& OutError) -> bool override
		{
			if (GRejectAuthoredPostLoad)
			{
				OutError = "Injected authored PostLoad rejection.";
				return false;
			}
			return DObject::PostLoad(OutError);
		}

		int32 NativeValue = 73;
		Durin::FBulkData CookedBulk;
		Durin::TObjectPtr<Durin::DObject> HardReference;
		Durin::FObjectPath SoftReference;
		bool bSkipSuper = false;
		bool bDuplicateField = false;
		bool bLateField = false;
		bool bUnsupportedCustomVersion = false;
	};

	class DSoftPackageAssetForTest : public DPackageAssetForTest
	{
	public:
		using FSoftReference = Durin::TSoftObjectPtr<DPackageAssetForTest>;
		using FSoftReferenceArray = std::vector<FSoftReference>;
		using FSoftReferenceMap = std::unordered_map<std::string, FSoftReference>;

		explicit DSoftPackageAssetForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get()
		)
			: DPackageAssetForTest(Initializer)
		{
			++GSoftPackageConstructionCount;
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DSoftPackageAssetForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DSoftPackageAssetForTest",
					sizeof(DSoftPackageAssetForTest), alignof(DSoftPackageAssetForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DSoftPackageAssetForTest>
				);
				Class->SetSuperStructBase(DPackageAssetForTest::StaticClass());
				Class->Register(
					Durin::DClass::StaticClass, "", "DSoftPackageAssetForTest"
				);
			}
			return Class;
		}

		static auto ResolveSoftArrayOps() -> const Durin::FArrayOps*
		{
			return Durin::ResolveArrayOps<FSoftReferenceArray>();
		}

		static auto ResolveSoftMapOps() -> const Durin::FMapOps*
		{
			return Durin::ResolveMapOps<FSoftReferenceMap>();
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static const auto DirectProp =
				Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftReference>(
					"Direct", Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DSoftPackageAssetForTest, Direct),
					&DPackageAssetForTest::StaticClass
				);
			static const auto FixedProp =
				Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftReference>(
					"Fixed", Durin::EPropertyFlags::None, 2,
					STRUCT_OFFSET_UINT16(DSoftPackageAssetForTest, Fixed),
					&DPackageAssetForTest::StaticClass
				);
			static const auto ArrayInner =
				Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftReference>(
					"Array_Inner", Durin::EPropertyFlags::None, 1, 0,
					&DPackageAssetForTest::StaticClass
				);
			static const Durin::DurinCodeGen::FArrayPropertyParams ArrayProp = {
				"Array", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DSoftPackageAssetForTest, Array),
				&ArrayInner, &ResolveSoftArrayOps
			};
			static const Durin::DurinCodeGen::FStringPropertyParams MapKey = {
				"Map_Key", Durin::EPropertyFlags::None, 1, 0
			};
			static const auto MapValue =
				Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftReference>(
					"Map_Value", Durin::EPropertyFlags::None, 1, 0,
					&DPackageAssetForTest::StaticClass
				);
			static const Durin::DurinCodeGen::FMapPropertyParams MapProp = {
				"Map", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DSoftPackageAssetForTest, Map),
				&MapKey, &MapValue, &ResolveSoftMapOps
			};
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&DirectProp, &FixedProp, &ArrayProp, &MapProp
			};
			static const Durin::DurinCodeGen::FClassParams Params = {
				&StaticClassNoRegister, "Tests::DSoftPackageAssetForTest",
				"DSoftPackageAssetForTest", Properties, std::size(Properties)
			};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		FSoftReference Direct;
		FSoftReference Fixed[2];
		FSoftReferenceArray Array;
		FSoftReferenceMap Map;
	};

	auto GetCodecSourceStructNoRegister() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = new Durin::DStruct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FCodecSource"),
			Durin::FName("FCodecSource"), sizeof(AssetStructTest::FCodecSource),
			alignof(AssetStructTest::FCodecSource), Durin::EObjectFlags::Transient
		);
		return Struct;
	}

	auto GetCodecSourceStruct() -> Durin::DStruct*
	{
		static const Durin::DurinCodeGen::FInt32PropertyParams Value = {
			"Value", Durin::EPropertyFlags::None, 1,
			STRUCT_OFFSET_UINT16(AssetStructTest::FCodecSource, Value)
		};
		static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {&Value};
		static const Durin::DurinCodeGen::FStructParams Params = {
			&GetCodecSourceStructNoRegister, "Tests::FCodecSource", "FCodecSource",
			sizeof(AssetStructTest::FCodecSource), alignof(AssetStructTest::FCodecSource),
			Properties, std::size(Properties),
			&Durin::GetDStructOps<AssetStructTest::FCodecSource>()
		};
		return Durin::DurinCodeGen::ConstructDStruct(Params);
	}

	auto GetCodecTargetStructNoRegister() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = new Durin::DStruct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FCodecTarget"),
			Durin::FName("FCodecTarget"), sizeof(AssetStructTest::FCodecTarget),
			alignof(AssetStructTest::FCodecTarget), Durin::EObjectFlags::Transient
		);
		return Struct;
	}

	auto GetCodecTargetStruct() -> Durin::DStruct*
	{
		static const Durin::DurinCodeGen::FInt32PropertyParams Value = {
			"Value", Durin::EPropertyFlags::None, 1,
			STRUCT_OFFSET_UINT16(AssetStructTest::FCodecTarget, Value)
		};
		static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {&Value};
		static const Durin::DurinCodeGen::FStructParams Params = {
			&GetCodecTargetStructNoRegister, "Tests::FCodecTarget", "FCodecTarget",
			sizeof(AssetStructTest::FCodecTarget), alignof(AssetStructTest::FCodecTarget),
			Properties, std::size(Properties),
			&Durin::GetDStructOps<AssetStructTest::FCodecTarget>()
		};
		return Durin::DurinCodeGen::ConstructDStruct(Params);
	}

	template<typename TValue>
	class TCodecAssetForTest : public Durin::DObject
	{
	public:
		explicit TCodecAssetForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get()
		)
			: DObject(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) TCodecAssetForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			constexpr bool bSource = std::is_same_v<TValue, AssetStructTest::FCodecSource>;
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor,
					bSource ? "DCodecSourceAsset" : "DCodecTargetAsset",
					sizeof(TCodecAssetForTest), alignof(TCodecAssetForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<TCodecAssetForTest>
				);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(
					Durin::DClass::StaticClass, "",
					bSource ? "DCodecSourceAsset" : "DCodecTargetAsset"
				);
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			constexpr bool bSource = std::is_same_v<TValue, AssetStructTest::FCodecSource>;
			static const Durin::DurinCodeGen::FStructPropertyParams ValueProp = {
				"Value", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(TCodecAssetForTest, Value),
				bSource ? &GetCodecSourceStruct : &GetCodecTargetStruct
			};
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&ValueProp
			};
			static const Durin::DurinCodeGen::FClassParams Params = {
				&StaticClassNoRegister,
				bSource ? "Tests::DCodecSourceAsset" : "Tests::DCodecTargetAsset",
				bSource ? "DCodecSourceAsset" : "DCodecTargetAsset",
				Properties, std::size(Properties)
			};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		TValue Value;
	};

	using DCodecSourceAsset = TCodecAssetForTest<AssetStructTest::FCodecSource>;
	using DCodecTargetAsset = TCodecAssetForTest<AssetStructTest::FCodecTarget>;

	class DMathStructAssetForTest : public Durin::DObject
	{
	public:
		explicit DMathStructAssetForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get()
		)
			: DObject(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DMathStructAssetForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DMathStructAssetForTest",
					sizeof(DMathStructAssetForTest), alignof(DMathStructAssetForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DMathStructAssetForTest>
				);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(
					Durin::DClass::StaticClass, "", "DMathStructAssetForTest"
				);
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static const Durin::DurinCodeGen::FStructPropertyParams VectorProp = {
				"Vector", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DMathStructAssetForTest, Vector),
				&Durin::Z_Construct_DStruct_FVector3
			};
			static const Durin::DurinCodeGen::FStructPropertyParams TransformProp = {
				"Transform", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DMathStructAssetForTest, Transform),
				&Durin::Z_Construct_DStruct_FTransform
			};
			static const Durin::DurinCodeGen::FStructPropertyParams FloatQuatProp = {
				"FloatQuat", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DMathStructAssetForTest, FloatQuat),
				&Durin::Z_Construct_DStruct_FQuatf
			};
			static const Durin::DurinCodeGen::FStructPropertyParams FloatMatrixProp = {
				"FloatMatrix", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DMathStructAssetForTest, FloatMatrix),
				&Durin::Z_Construct_DStruct_FMatrix4f
			};
			static const Durin::DurinCodeGen::FStructPropertyParams VectorInner = {
				"Vectors_Inner", Durin::EPropertyFlags::None, 1, 0,
				&Durin::Z_Construct_DStruct_FVector3
			};
			static const Durin::DurinCodeGen::FArrayPropertyParams VectorsProp = {
				"Vectors", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DMathStructAssetForTest, Vectors),
				&VectorInner, &GVector3VectorHelper
			};
			static const Durin::DurinCodeGen::FStringPropertyParams VectorMapKey = {
				"VectorMap_Key", Durin::EPropertyFlags::None, 1, 0
			};
			static const Durin::DurinCodeGen::FStructPropertyParams VectorMapValue = {
				"VectorMap_Value", Durin::EPropertyFlags::None, 1, 0,
				&Durin::Z_Construct_DStruct_FVector3
			};
			static const Durin::DurinCodeGen::FMapPropertyParams VectorMapProp = {
				"VectorMap", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DMathStructAssetForTest, VectorMap),
				&VectorMapKey, &VectorMapValue, &GVectorMapHelper
			};
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&VectorProp, &TransformProp, &FloatQuatProp, &FloatMatrixProp,
				&VectorsProp, &VectorMapProp
			};
			static const Durin::DurinCodeGen::FClassParams Params = {
				&StaticClassNoRegister, "Tests::DMathStructAssetForTest",
				"DMathStructAssetForTest", Properties, std::size(Properties)
			};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		Durin::FVector3 Vector{0.0};
		Durin::FTransform Transform;
		Durin::FQuatf FloatQuat{1.0f, 0.0f, 0.0f, 0.0f};
		Durin::FMatrix4f FloatMatrix{1.0f};
		std::vector<Durin::FVector3> Vectors;
		FVectorMap VectorMap;
	};

	auto RegisterTestDeleteContributor() -> void
	{
		static const bool Registered = [] {
			Durin::RegisterAssetDeleteContributor(DPackageAssetForTest::StaticClass(), [](const Durin::FAssetData&, const Durin::FAssetPackageInspection& Inspection, Durin::FAssetDeleteContribution& Out) -> Durin::FAssetResult {
				const Durin::FAssetPackageField* LabelField = Inspection.FindField("Label");
				std::string Label;
				if (LabelField && LabelField->TryReadString(Label) && Label.starts_with("companion:"))
					Out.Files.emplace_back(Label.substr(10));
				return {};
			});
			return true;
		}();
		(void)Registered;
	}

	auto InitializeAssetTests() -> void
	{
		static const bool Initialized = [] {
			Durin::Testing::InitializeDObjectSystemForTests();
			(void)DImportMetadataForTest::StaticClass();
			(void)DReplayImportMetadataForTest::StaticClass();
			(void)DImportMetadataOwnerForTest::StaticClass();
			(void)DPackageAssetForTest::StaticClass();
			(void)DBulkPackageAssetForTest::StaticClass();
			(void)DAuthoredArchiveAssetForTest::StaticClass();
			(void)DSoftPackageAssetForTest::StaticClass();
			(void)DCodecSourceAsset::StaticClass();
			(void)DCodecTargetAsset::StaticClass();
			(void)DMathStructAssetForTest::StaticClass();
			(void)DSchemaMigrationAssetForTest::StaticClass();
			return true;
		}();
		(void)Initialized;

		const std::filesystem::path RecoveryRoot =
			Durin::Testing::GetTestWorkDirectory()
			/ "AssetMutationRecovery";
		Durin::SetAssetMutationRecoveryDirectoryForTesting(RecoveryRoot);
		Durin::ShutdownAssetManager();
		Durin::CollectGarbage();
		Durin::Testing::RemoveTestWorkDirectory(RecoveryRoot);
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "Assets";
		Durin::Testing::RemoveTestWorkDirectory(Root);
		const std::filesystem::path DerivedDataRoot =
			Durin::Testing::GetTestWorkDirectory() / "DerivedDataCache";
		Durin::Testing::RemoveTestWorkDirectory(DerivedDataRoot);
		Durin::FPaths::SetDerivedDataCacheDirForTests(
			DerivedDataRoot.generic_string());
		Durin::Testing::RegisterMountPointForTests(
			"/TestAssets/", Root.generic_string() + "/");
		Durin::SetAssetRelocationFailurePointForTesting(
			Durin::EAssetRelocationFailurePoint::None);
		Durin::SetAssetRedirectorFixupFailurePointForTesting(
			Durin::EAssetRedirectorFixupFailurePoint::None);
		Durin::SetAssetMutationRecoveryFailurePointForTesting(
			Durin::EAssetMutationRecoveryFailurePoint::None
		);
		Durin::InitializeAssetManager();
		if (!Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation))
		{
			throw std::runtime_error(
				"Failed to establish a complete empty asset registry for the test fixture.");
		}
	}

	class FMemoryAssetReferenceStore final
		: public Durin::IAssetReferenceStore
	{
	public:
		explicit FMemoryAssetReferenceStore(
			Durin::FPackagePath InPath,
			bool bInCookRoot = false,
			std::string InExpectedClass = {}
		)
			: Path(std::move(InPath))
			, ExpectedClass(std::move(InExpectedClass))
			, bCookRoot(bInCookRoot)
		{
		}

		auto CaptureSnapshot(
			Durin::FAssetReferenceStoreSnapshot& OutSnapshot
		)
			-> Durin::FAssetResult override
		{
			OutSnapshot = {
				.ProviderId = "Tests.MemoryReferenceStore",
				.ProviderVersion = 1,
				.Fingerprint = Path.ToString(),
				.Occurrences = {{.ProviderId = "Tests.MemoryReferenceStore", .StableId = "root", .TargetPath = Path, .DisplayRoute = "Root", .ExpectedClass = ExpectedClass, .bCookRoot = bCookRoot}}
			};
			return {};
		}

		auto PrepareRewrite(
			std::span<const Durin::FAssetReferenceRewrite> Rewrites,
			std::string_view ExpectedFingerprint,
			Durin::FAssetReferenceStoreRewriteContribution& OutContribution
		)
			-> Durin::FAssetResult override
		{
			if (ExpectedFingerprint != Path.ToString() || Rewrites.size() != 1
				|| Rewrites.front().StableId != "root"
				|| Rewrites.front().SourcePath != Path)
				return {Durin::EAssetError::StaleData, "Memory reference store preparation is stale."};
			const Durin::FPackagePath PrePath = Path;
			const Durin::FPackagePath PostPath = Rewrites.front().DestinationPath;
			OutContribution = {
				.Fingerprint = std::string(ExpectedFingerprint),
				.Rewrites = {Rewrites.front()},
				.Revalidate = [this, PrePath] { return Path == PrePath ? Durin::FAssetResult{} : Durin::FAssetResult{Durin::EAssetError::StaleData, "Memory reference store changed."}; },
				.Apply = [this, PostPath] {
					Path = PostPath;
					return Durin::FAssetResult{}; },
				.Restore = [this, PrePath] {
					Path = PrePath;
					return Durin::FAssetResult{}; },
				.Verify = [this, PostPath] { return Path == PostPath ? Durin::FAssetResult{} : Durin::FAssetResult{Durin::EAssetError::StaleData, "Memory reference store verification failed."}; }
			};
			return {};
		}

		Durin::FPackagePath Path;
		std::string ExpectedClass;
		bool bCookRoot = false;
	};

	class FScopedReferenceStoreRegistration
	{
	public:
		explicit FScopedReferenceStoreRegistration(
			Durin::IAssetReferenceStore* Store
		)
			: Handle(Durin::RegisterAssetReferenceStore(Store))
		{
		}

		~FScopedReferenceStoreRegistration()
		{
			Reset();
		}

		auto Reset() -> void
		{
			if (Handle == 0) return;
			Durin::UnregisterAssetReferenceStore(Handle);
			Handle = 0;
		}

	private:
		Durin::FAssetReferenceStoreHandle Handle = 0;
	};

	auto ShutdownAssetManagerForRestart() -> void
	{
		Durin::ShutdownAssetManager();
		Durin::CollectGarbage();
		Durin::InitializeAssetManager();
	}

	auto WriteTestBytes(const std::filesystem::path& Path, std::span<const std::byte> Bytes) -> void
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(Stream.is_open());
		Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		ASSERT_TRUE(Stream.good());
	}

	auto RenameSerializedString(
		Durin::FByteArray& Bytes,
		std::string_view OldValue,
		std::string_view NewValue
	) -> bool
	{
		if (OldValue.size() != NewValue.size()) return false;
		Durin::FByteArray Pattern(sizeof(uint64) + OldValue.size());
		const uint64 Length = OldValue.size();
		std::memcpy(Pattern.data(), &Length, sizeof(Length));
		std::memcpy(Pattern.data() + sizeof(Length), OldValue.data(), OldValue.size());
		const auto It = std::search(Bytes.begin(), Bytes.end(), Pattern.begin(), Pattern.end());
		if (It == Bytes.end()) return false;
	std::memcpy(std::to_address(It + sizeof(Length)), NewValue.data(), NewValue.size());
		return true;
	}

	auto RenameAllSerializedStrings(
		Durin::FByteArray& Bytes,
		std::string_view OldValue,
		std::string_view NewValue
	) -> uint64
	{
		uint64 Count = 0;
		while (RenameSerializedString(Bytes, OldValue, NewValue))
			++Count;
		return Count;
	}

	auto RenameSerializedStringOccurrence(
		Durin::FByteArray& Bytes,
		std::string_view OldValue,
		std::string_view NewValue,
		size_t Occurrence
	) -> bool
	{
		if (OldValue.size() != NewValue.size()) return false;
		Durin::FByteArray Pattern(sizeof(uint64) + OldValue.size());
		const uint64 Length = OldValue.size();
		std::memcpy(Pattern.data(), &Length, sizeof(Length));
		std::memcpy(Pattern.data() + sizeof(Length), OldValue.data(), OldValue.size());
		auto SearchStart = Bytes.begin();
		for (size_t Index = 0; Index <= Occurrence; ++Index)
		{
			const auto It = std::search(
				SearchStart, Bytes.end(), Pattern.begin(), Pattern.end()
			);
			if (It == Bytes.end()) return false;
			if (Index == Occurrence)
			{
				std::memcpy(std::to_address(It + sizeof(Length)), NewValue.data(), NewValue.size());
				return true;
			}
			SearchStart = It + static_cast<std::ptrdiff_t>(Pattern.size());
		}
		return false;
	}

#if DURIN_WITH_EDITOR
	auto MakeCompatibilityProbeInput(
		const Durin::FPackagePath& PackagePath,
		const std::filesystem::path& PhysicalPath
	)
		-> Durin::FAssetPackageCompatibilityProbeInput
	{
		std::error_code Error;
		const auto LastWriteTime = std::filesystem::last_write_time(PhysicalPath, Error);
		EXPECT_FALSE(Error);
		Durin::FByteArray Bytes;
		EXPECT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, PhysicalPath));
		return {
			.PackagePath = PackagePath,
			.PhysicalPath = PhysicalPath.generic_string(),
			.ExpectedFileSize = std::filesystem::file_size(PhysicalPath, Error),
			.ExpectedLastWriteTimeTicks = Durin::FileTime::ToStableTicks(LastWriteTime),
			.ExpectedContentHash = Durin::FXxHash128::HashBuffer(Bytes)};
	}
#endif

	auto HexDigit(char Character) -> uint8
	{
		if (Character >= '0' && Character <= '9') return static_cast<uint8>(Character - '0');
		if (Character >= 'A' && Character <= 'F') return static_cast<uint8>(Character - 'A' + 10);
		if (Character >= 'a' && Character <= 'f') return static_cast<uint8>(Character - 'a' + 10);
		ADD_FAILURE() << "Invalid hexadecimal fixture digit.";
		return 0;
	}

	auto ReadCompatibilityFixtureBytes(std::string_view Name) -> Durin::FByteArray
	{
		std::ifstream Stream(std::filesystem::path(DURIN_TEST_DATA_DIR) / std::format("{}.dasset.hex", Name));
		EXPECT_TRUE(Stream.is_open());
		std::string Hex;
		Stream >> Hex;
		EXPECT_FALSE(Hex.empty());
		EXPECT_EQ(Hex.size() % 2, 0u);
		Durin::FByteArray Bytes(Hex.size() / 2);
		for (size_t Index = 0; Index < Bytes.size(); ++Index)
			Bytes[Index] = static_cast<std::byte>((HexDigit(Hex[Index * 2]) << 4) | HexDigit(Hex[Index * 2 + 1]));
		return Bytes;
	}

	auto WriteCompatibilityFixture(std::string_view Name, const std::filesystem::path& Destination) -> void
	{
		std::ifstream Stream(std::filesystem::path(DURIN_TEST_DATA_DIR) / std::format("{}.dasset.hex", Name));
		ASSERT_TRUE(Stream.is_open());
		std::string Hex;
		Stream >> Hex;
		ASSERT_FALSE(Hex.empty());
		ASSERT_EQ(Hex.size() % 2, 0u);
		Durin::FByteArray Bytes(Hex.size() / 2);
		for (size_t Index = 0; Index < Bytes.size(); ++Index)
			Bytes[Index] = static_cast<std::byte>((HexDigit(Hex[Index * 2]) << 4) | HexDigit(Hex[Index * 2 + 1]));
		WriteTestBytes(Destination, Bytes);
	}

	auto RunRedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletionTest()
		-> void;
	auto RunRedirectorFixupVerificationFailureResumesRemainingParticipantsTest()
		-> void;
	auto RunRedirectorFixupRejectsUnavailableProviderWithoutMutationTest()
		-> void;
	auto RunRedirectorFixupRejectsReadOnlyAndChangedPackageInputsTest()
		-> void;
	auto RunRedirectorFixupPublicationFailuresResumeForwardTest()
		-> void;
} // namespace

TEST(FPackageAssetTests, EngineAssetErrorsExposeStructuredDiagnostics)
{
	const Durin::FAssetResult Result{
		Durin::EAssetError::InvalidObjectGraph,
		"The live object graph is invalid."};
	const Durin::FDiagnostic Diagnostic = Result.GetDiagnostic();
	EXPECT_EQ(Diagnostic.Domain, "Asset");
	EXPECT_EQ(Diagnostic.Code, "InvalidObjectGraph");
	EXPECT_TRUE(Diagnostic.IsError());
	EXPECT_EQ(Diagnostic.Message, Result.Message);
}

TEST(FPackageAssetTests, ByteToolRawScalarKindsMatchThePayloadContract)
{
	using enum Durin::DurinCodeGen::EPropertyGenFlags;
	constexpr std::array RawScalarKinds{
		Bool,
		Int8, Int16, Int32, Int64,
		UInt8, UInt16, UInt32, UInt64,
		Float, Double,
		Enum,
		Byte,
	};

	for (const auto Kind : Durin::DurinCodeGen::AllPropertyKinds)
	{
		const bool bExpected = std::ranges::find(RawScalarKinds, Kind) != RawScalarKinds.end();
		EXPECT_EQ(Durin::AssetPrivate::IsByteToolRawScalarKind(Kind), bExpected);
	}
	EXPECT_FALSE(Durin::AssetPrivate::IsByteToolRawScalarKind(Count));
}

TEST(FPackageAssetTests, RedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletion)
{
	RunRedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletionTest();
}

TEST(FEditorBulkDataTests, SharesImmutableBytesAndReplacesTransactionally)
{
	const Durin::FGuid PayloadId{1, 2, 3, 4};
	const std::array Initial{std::byte{1}, std::byte{2}, std::byte{3}};
	Durin::FEditorBulkData First(PayloadId);
	ASSERT_TRUE(First.ReplaceBytes(Initial));
	Durin::FEditorBulkData Shared = First;
	const Durin::FSharedByteBuffer FirstPayload = First.GetPayload().Wait().Buffer;
	const Durin::FSharedByteBuffer SharedPayload = Shared.GetPayload().Wait().Buffer;
	ASSERT_TRUE(FirstPayload.SharesStorageWith(SharedPayload));
	EXPECT_TRUE(First.Identical(Shared));

	const std::array Replacement{std::byte{9}, std::byte{8}};
	ASSERT_TRUE(Shared.ReplaceBytes(Replacement));
	EXPECT_TRUE(std::ranges::equal(First.GetPayload().Wait().Buffer.GetBytes(), Initial));
	EXPECT_TRUE(std::ranges::equal(Shared.GetPayload().Wait().Buffer.GetBytes(), Replacement));
	EXPECT_FALSE(First.Identical(Shared));
	EXPECT_FALSE(Shared.ReplaceBytes({}, Replacement));
	EXPECT_TRUE(std::ranges::equal(Shared.GetPayload().Wait().Buffer.GetBytes(), Replacement));
}


TEST(FPackageAssetTests, RegistryFailureKeepsCommittedStableClosure)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/V8FirstFailure", Path));
	DBulkPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	Durin::FByteArray Payload(
		static_cast<size_t>(Durin::EditorBulkDataExternalThreshold + 1),
		std::byte{0x51});
	ASSERT_TRUE(Asset->Payload.UpdatePayload(Payload));
	Durin::DPackage* Packages[] = {Asset->GetPackage()};
	const Durin::FAssetResult Result = Durin::SavePackagesAtomically(
		Packages,
		{.RootPackage = Asset->GetPackage(),
			.ShouldFail = [](Durin::EAssetBundleSavePhase Phase, size_t) {
				return Phase == Durin::EAssetBundleSavePhase::PublishRegistry;
			}});
	EXPECT_EQ(Result.Error, Durin::EAssetError::StaleData);
	EXPECT_EQ(Result.Disposition,
		Durin::EAssetResultDisposition::ContentCommittedProjectionPending);
	EXPECT_NE(Result.Message.find("ContentCommittedProjectionPending"),
		std::string::npos);
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "Assets";
	EXPECT_TRUE(std::filesystem::exists(Root / "V8FirstFailure.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "V8FirstFailure.dbulk"));
	EXPECT_FALSE(std::filesystem::exists(
		Root / "V8FirstFailure.dbulk.durin-backup"));
	EXPECT_TRUE(Durin::IsAssetRegistryProjectionFenced(Path));
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	EXPECT_FALSE(Durin::IsAssetRegistryProjectionFenced(Path));
	ASSERT_TRUE(Durin::DeleteAssetForTesting(Path));
}

TEST(FPackageAssetTests, OrdinaryV8PublishesLoadsAndRollsBackExternalClosure)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/V6ExternalClosure", Path));
	DBulkPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	Durin::FByteArray Payload(
		static_cast<size_t>(Durin::EditorBulkDataExternalThreshold + 17),
		std::byte{0x5a});
	ASSERT_TRUE(Asset->Payload.ReplaceBytes(Payload));

	const Durin::FAssetResult V6Save = Durin::SavePackage(
		Asset->GetPackage());
	ASSERT_TRUE(V6Save) << V6Save.Message;
	const Durin::FAssetCatalogEntry V6Data =
		Durin::FindAssetExact(Path);
	ASSERT_TRUE(V6Data);
	EXPECT_EQ(V6Data->FormatVersion, Durin::ObjectPackage::DastV9FormatVersion);
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(V6Data->PhysicalPath, Inspection));
	EXPECT_EQ(Inspection.Header.FormatVersion,
		Durin::ObjectPackage::DastV9FormatVersion);
	std::vector<Durin::FEditorBulkDataStorageDescriptor> Descriptors;
	std::string Error;
	ASSERT_TRUE(Durin::InspectEditorBulkDataStorageDescriptors(
		Inspection, Descriptors, &Error)) << Error;
	ASSERT_EQ(Descriptors.size(), 1u);
	EXPECT_EQ(Descriptors.front().StorageKind,
		Durin::EEditorBulkDataStorageKind::External);
	std::vector<std::filesystem::path> Companions;
	ASSERT_TRUE(Durin::InspectEditorBulkDataCompanionPaths(
		V6Data->PhysicalPath, Inspection, Companions, &Error)) << Error;
	ASSERT_EQ(Companions.size(), 1u);
	EXPECT_TRUE(std::filesystem::is_regular_file(Companions.front()));
	EXPECT_EQ(Companions.front().filename(), "V6ExternalClosure.dbulk");
	std::filesystem::path BackupPath = Companions.front();
	BackupPath += Durin::EditorBulkDataCompanionBackupSuffix;
	EXPECT_FALSE(std::filesystem::exists(BackupPath));
	std::filesystem::copy_file(
		Companions.front(), BackupPath,
		std::filesystem::copy_options::overwrite_existing);
	std::vector<std::filesystem::path> Orphans;
	ASSERT_TRUE(Durin::InspectOrphanedEditorBulkDataCompanionPaths(
		V6Data->PhysicalPath, Inspection, Orphans, &Error)) << Error;
	EXPECT_TRUE(Orphans.empty());

	Durin::FByteArray LoadedPayload;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(LoadedPayload, Companions.front()));
	EXPECT_TRUE(std::ranges::equal(LoadedPayload, Payload));
	EXPECT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	EXPECT_FALSE(std::filesystem::exists(BackupPath));

	Durin::FByteArray BeforeFailedReplacement;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		BeforeFailedReplacement, Companions.front()));
	std::ranges::fill(Payload, std::byte{0x63});
	ASSERT_TRUE(Asset->Payload.ReplaceBytes(Payload));
	Durin::DPackage* ReplacementUnit[] = {Asset->GetPackage()};
	const Durin::FAssetResult FailedReplacement =
		Durin::SavePackagesAtomically(ReplacementUnit,
			{.RootPackage = Asset->GetPackage(),
				.ShouldFail = [](Durin::EAssetBundleSavePhase Phase, size_t) {
					return Phase == Durin::EAssetBundleSavePhase::PublishRegistry;
				}});
	EXPECT_EQ(FailedReplacement.Error, Durin::EAssetError::StaleData);
	Durin::FByteArray AfterFailedReplacement;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AfterFailedReplacement, Companions.front()));
	EXPECT_NE(AfterFailedReplacement, BeforeFailedReplacement);
	EXPECT_FALSE(std::filesystem::exists(BackupPath));
	EXPECT_TRUE(Durin::IsAssetRegistryProjectionFenced(Path));
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	EXPECT_FALSE(Durin::IsAssetRegistryProjectionFenced(Path));
	Durin::FByteArray AfterCommittedReplacement;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AfterCommittedReplacement, Companions.front()));
	EXPECT_EQ(AfterCommittedReplacement, AfterFailedReplacement);
	EXPECT_FALSE(std::filesystem::exists(BackupPath));
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	DBulkPackageAssetForTest* ReloadedBulk = nullptr;
	const Durin::FAssetResult ReloadBulkResult =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), ReloadedBulk);
	ASSERT_TRUE(ReloadBulkResult) << ReloadBulkResult.Message;
	ASSERT_NE(ReloadedBulk, nullptr);
	EXPECT_FALSE(ReloadedBulk->Payload.IsMemoryResident());
	const Durin::FPackageResourceReadResult ReloadedPayload =
		ReloadedBulk->Payload.GetPayload().Wait();
	ASSERT_TRUE(ReloadedPayload) << ReloadedPayload.Message;
	EXPECT_FALSE(ReloadedBulk->Payload.IsMemoryResident());
	EXPECT_TRUE(std::ranges::equal(ReloadedPayload.Buffer.GetBytes(), Payload));

	Durin::FPackagePath LivePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/V6LiveLoad", LivePath));
	DPackageAssetForTest* LiveAsset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(LivePath, LiveAsset));
	LiveAsset->Value = 73;
	ASSERT_TRUE(Durin::SavePackage(LiveAsset->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(LivePath));
	DPackageAssetForTest* Reloaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(LivePath), Reloaded));
	EXPECT_EQ(Reloaded->Value, 73);
	const Durin::FAssetCatalogEntry LiveData =
		Durin::FindAssetExact(LivePath);
	ASSERT_TRUE(LiveData);
	EXPECT_EQ(Durin::FindAssetExact(LivePath)->FormatVersion,
		Durin::ObjectPackage::DastV9FormatVersion);
}


TEST(FPackageAssetTests, V8FieldBulkClosureMeetsBoundedLooseFixtureBudgets)
{
	InitializeAssetTests();
	using namespace Durin;
	using namespace Durin;
	constexpr uint64 PayloadBytes = 4ull * 1024ull * 1024ull;
	constexpr double MetadataLoadBudgetMilliseconds = 500.0;
	constexpr double FirstAccessBudgetMilliseconds = 500.0;
	constexpr double SaveBudgetMilliseconds = 2000.0;

	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TestAssets/FieldBulkQualification", Path));
	DBulkPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
	Durin::FByteArray Payload(static_cast<size_t>(PayloadBytes));
	for (size_t Index = 0; Index < Payload.size(); ++Index)
		Payload[Index] = static_cast<std::byte>((Index * 131u + 17u) & 0xffu);
	ASSERT_TRUE(Asset->Payload.UpdatePayload(Payload));
	const FAssetResult SaveResult = SavePackage(Asset->GetPackage());
	ASSERT_TRUE(SaveResult) << SaveResult.Message;
	ASSERT_TRUE(UnloadPackage(Path));

	const FAssetCatalogEntry Entry = FindAssetExact(Path);
	ASSERT_TRUE(Entry);
	std::filesystem::path SegmentPath = Entry->PhysicalPath;
	SegmentPath.replace_extension(".dbulk");
	ASSERT_TRUE(std::filesystem::is_regular_file(SegmentPath));
	EXPECT_EQ(std::filesystem::file_size(SegmentPath), PayloadBytes);
	EXPECT_EQ(GetPackageResourceManager().GetRegisteredPackageCount(), 0u);

	const auto MetadataStart = std::chrono::steady_clock::now();
	DObject* LoadedObject = nullptr;
	ASSERT_TRUE(LoadObject(MakeFormerMainObjectPath(Path), LoadedObject));
	const double MetadataMilliseconds = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - MetadataStart).count();
	auto* Loaded = Cast<DBulkPackageAssetForTest>(LoadedObject);
	ASSERT_NE(Loaded, nullptr);
	EXPECT_LT(MetadataMilliseconds, MetadataLoadBudgetMilliseconds);
	EXPECT_FALSE(Loaded->Payload.IsMemoryResident());
	EXPECT_EQ(GetPackageResourceManager().GetRegisteredPackageCount(), 1u);
	const FPackageResourceHandle Resource =
		GetPackageResourceManager().FindPackage(Path.ToString());
	ASSERT_NE(Resource, nullptr);
	const FPackageResourceReadStats MetadataReadStats = Resource->GetReadStats();
	EXPECT_GT(MetadataReadStats.ValidationReadCount, 0u);
	EXPECT_EQ(MetadataReadStats.ValidationBytesRead, PayloadBytes);
	EXPECT_LE(MetadataReadStats.PeakValidationScratchBytes, 64u * 1024u);
	EXPECT_EQ(MetadataReadStats.RequestCount, 0u);
	EXPECT_EQ(MetadataReadStats.RequestedBytes, 0u);

	const auto AccessStart = std::chrono::steady_clock::now();
	const FPackageResourceReadResult LoadedPayload = Loaded->Payload.GetPayload().Wait();
	const double AccessMilliseconds = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - AccessStart).count();
	ASSERT_TRUE(LoadedPayload) << LoadedPayload.Message;
	EXPECT_LT(AccessMilliseconds, FirstAccessBudgetMilliseconds);
	EXPECT_EQ(LoadedPayload.Buffer.GetSize(), PayloadBytes);
	EXPECT_TRUE(std::ranges::equal(LoadedPayload.Buffer.GetBytes(), Payload));
	EXPECT_FALSE(Loaded->Payload.IsMemoryResident());
	const FPackageResourceReadStats AccessReadStats = Resource->GetReadStats();
	EXPECT_EQ(AccessReadStats.ValidationBytesRead, PayloadBytes);
	EXPECT_EQ(AccessReadStats.RequestCount, 1u);
	EXPECT_EQ(AccessReadStats.RequestedBytes, PayloadBytes);

	const auto SaveStart = std::chrono::steady_clock::now();
	ASSERT_TRUE(SavePackage(Loaded->GetPackage()));
	const double SaveMilliseconds = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - SaveStart).count();
	EXPECT_LT(SaveMilliseconds, SaveBudgetMilliseconds);
	EXPECT_EQ(std::filesystem::file_size(SegmentPath), PayloadBytes);

	std::cout << "FieldBulkQualification payload_bytes=" << PayloadBytes
		<< " segment_bytes=" << std::filesystem::file_size(SegmentPath)
		<< " resident_field_bytes=0"
		<< " validation_bytes=" << MetadataReadStats.ValidationBytesRead
		<< " validation_peak_scratch=" << MetadataReadStats.PeakValidationScratchBytes
		<< " range_request_bytes=" << AccessReadStats.RequestedBytes
		<< " metadata_load_ms=" << MetadataMilliseconds
		<< " first_access_ms=" << AccessMilliseconds
		<< " save_ms=" << SaveMilliseconds << '\n';
	EXPECT_TRUE(UnloadPackage(Path));
	EXPECT_EQ(GetPackageResourceManager().GetRegisteredPackageCount(), 0u);
}

TEST(FPackageAssetTests, V8BundleAndRelocationPreserveCurrentFormat)
{
	InitializeAssetTests();
	Durin::FPackagePath SourcePath;
	Durin::FPackagePath DestinationPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/V6MoveSource", SourcePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/V6MoveDestination", DestinationPath));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SourcePath, Asset));
	Asset->Value = 91;
	Durin::DPackage* Packages[] = {Asset->GetPackage()};
	ASSERT_TRUE(Durin::SavePackagesAtomically(Packages,
		{.RootPackage = Asset->GetPackage()}));
	ASSERT_EQ(Durin::FindAssetExact(SourcePath)->FormatVersion,
		Durin::ObjectPackage::DastV9FormatVersion);

	const Durin::FAssetRelocationMapping Mapping{
		SourcePath, DestinationPath};
	ASSERT_TRUE(RelocateAssetsForTest(std::span(&Mapping, 1)));
	const Durin::FAssetCatalogEntry Redirector =
		Durin::FindAssetExact(SourcePath);
	const Durin::FAssetCatalogEntry Moved =
		Durin::FindAssetExact(DestinationPath);
	ASSERT_TRUE(Redirector);
	ASSERT_TRUE(Moved);
	EXPECT_EQ(Redirector->FormatVersion,
		Durin::ObjectPackage::DastV9FormatVersion);
	EXPECT_EQ(Moved->FormatVersion, Durin::ObjectPackage::DastV9FormatVersion);
	EXPECT_EQ(Redirector->EntryKind,
		Durin::EAssetRegistryEntryKind::Redirector);
	DPackageAssetForTest* Resolved = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(SourcePath), Resolved));
	EXPECT_EQ(Resolved->Value, 91);
}

TEST(FPackageAssetTests, RelocationAndDeletionOwnStableAuthoredCompanion)
{
	InitializeAssetTests();
	Durin::FPackagePath SourcePath;
	Durin::FPackagePath DestinationPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/StableBulkMoveSource", SourcePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/StableBulkMoveDestination", DestinationPath));
	DBulkPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SourcePath, Asset));
	Durin::FByteArray Payload(
		static_cast<size_t>(Durin::EditorBulkDataExternalThreshold + 3),
		std::byte{0x71});
	ASSERT_TRUE(Asset->Payload.ReplaceBytes(Payload));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));

	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "Assets";
	const std::filesystem::path SourceCompanion =
		Root / "StableBulkMoveSource.dbulk";
	const std::filesystem::path DestinationCompanion =
		Root / "StableBulkMoveDestination.dbulk";
	ASSERT_TRUE(std::filesystem::is_regular_file(SourceCompanion));
	const Durin::FAssetResult Relocated =
		RelocateAssetForTest(SourcePath, DestinationPath);
	ASSERT_TRUE(Relocated) << Relocated.Message;
	EXPECT_FALSE(std::filesystem::exists(SourceCompanion));
	EXPECT_TRUE(std::filesystem::is_regular_file(DestinationCompanion));
	ASSERT_TRUE(DeleteAssetClosureForTest({SourcePath, DestinationPath}));
	EXPECT_FALSE(std::filesystem::exists(DestinationCompanion));
}

TEST(FPackageAssetTests, RedirectorFixupVerificationFailureRestoresPackagesStoresAndAlias)
{
	RunRedirectorFixupVerificationFailureResumesRemainingParticipantsTest();
}

TEST(FPackageAssetTests, RedirectorFixupRejectsUnavailableProviderWithoutMutation)
{
	RunRedirectorFixupRejectsUnavailableProviderWithoutMutationTest();
}

TEST(FPackageAssetTests, RedirectorFixupPublicationFailuresRestoreAllParticipants)
{
	RunRedirectorFixupPublicationFailuresResumeForwardTest();
}

TEST(FPackageAssetTests, RedirectorFixupRewriteOnlyReportsRetainedAlias)
{
	InitializeAssetTests();
	Durin::FPackagePath OldPath;
	Durin::FPackagePath NewPath;
	Durin::FPackagePath OwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/FixupRewriteOnlyOld", OldPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/FixupRewriteOnlyNew", NewPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/FixupRewriteOnlyOwner", OwnerPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Target));
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(OldPath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));

	Durin::FAssetRedirectorFixupSummary Summary;
	Durin::FAssetMutationJob Job;
	ASSERT_TRUE(Durin::PrepareRedirectorFixupJob(
		std::span{&OldPath, 1},
		Durin::EAssetRedirectorFixupMode::RewriteOnly,
		Summary,
		Job));
	EXPECT_TRUE(Summary.GetDeletableRedirectors().empty());
	const Durin::FAssetResult Resumed = Job.ResumeForward();
	ASSERT_TRUE(Resumed) << Resumed.Message;
	const Durin::FAssetMutationResultDetails Details =
		Job.GetLastResultDetails();
	EXPECT_EQ(Details.RewrittenPaths, std::vector{OwnerPath});
	EXPECT_EQ(Details.RetainedPaths, std::vector{OldPath});
	EXPECT_TRUE(Details.DeletedPaths.empty());
	ASSERT_NE(Durin::FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Job.GetState(),
		Durin::EAssetMutationJobState::Completed);
	ASSERT_TRUE(Durin::DeleteAssetForTesting(OwnerPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
}

TEST(FPackageAssetTests, HeaderReaderStopsBeforeLargeObjectPayload)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/LargeHeaderOnly", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	Asset->Scores.resize(100000, 7);
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "LargeHeaderOnly.dasset";
	ASSERT_GT(std::filesystem::file_size(File), 8u * 1024u);

	Durin::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::ReadAssetPackageHeader(File.generic_string(), Path, Header));
	EXPECT_EQ(Header.AssetClassName, "Tests::DPackageAssetForTest");
	EXPECT_EQ(Header.FormatVersion, Durin::ObjectPackage::DastV9FormatVersion);
	EXPECT_EQ(Header.EntryKind, Durin::EAssetRegistryEntryKind::Asset);
	EXPECT_FALSE(Header.RedirectDestination.IsValid());
	EXPECT_EQ(Header.ObjectCount, 2u);
	EXPECT_LT(Header.BytesRead, 1024u);
	EXPECT_EQ(Header.FileBytesRead, Header.BytesRead);
	EXPECT_LT(Header.FileBytesRead, std::filesystem::file_size(File));
}

TEST(FPackageAssetTests, OrdinaryWriterEmitsDurfV1Prefix)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/DurfV1Prefix", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));

	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DurfV1Prefix.dasset";
	Durin::FByteArray Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File));
	constexpr std::array<std::byte, 8> ExpectedPrefix = {
		std::byte{0x44}, std::byte{0x55}, std::byte{0x52}, std::byte{0x46},
		std::byte{0x01}, std::byte{0x00}, std::byte{0x40}, std::byte{0x00}
	};
	ASSERT_GE(Bytes.size(), ExpectedPrefix.size());
	EXPECT_TRUE(std::ranges::equal(
		ExpectedPrefix,
		std::span<const std::byte>(Bytes).first(ExpectedPrefix.size())
	));
	EXPECT_TRUE(Durin::UnloadPackage(Path));
}

TEST(FPackageAssetTests, PackageCodecPolicyIsCompleteUniqueAndIndependentOfWireVersion)
{
	InitializeAssetTests();
	std::string Error;
	EXPECT_TRUE(Durin::ValidateAssetPackageVersionPolicy(Error)) << Error;
	EXPECT_NE(Durin::ObjectPackage::PackageReaderPolicyFingerprint,
		7u);
	EXPECT_TRUE(Durin::ObjectPackage::DastFormatId.IsValid());
	EXPECT_EQ(Durin::ObjectPackage::DastFormatId,
		(Durin::FGuid{0x3c59d1a9, 0x6ceb4e4c, 0xb059452d, 0xb0a5af56}));
	EXPECT_EQ(Durin::ObjectPackage::DastFormatName, "Durin.BinaryFormat.DAST");

	const auto& V9 = Durin::AssetPrivate::DastV9::GetCodec();
	std::array DuplicateKeys{V9, V9};
	DuplicateKeys[1].CodecId = "dast-v8-alias";
	EXPECT_FALSE(Durin::AssetPrivate::ValidateAssetPackageCodecTable(DuplicateKeys, Error));
	std::ranges::reverse(DuplicateKeys);
	EXPECT_FALSE(Durin::AssetPrivate::ValidateAssetPackageCodecTable(DuplicateKeys, Error));

	std::array DuplicateNames{V9, V9};
	DuplicateNames[1].FormatVersion = Durin::ObjectPackage::DastV9FormatVersion + 1;
	EXPECT_FALSE(Durin::AssetPrivate::ValidateAssetPackageCodecTable(DuplicateNames, Error));
	std::ranges::reverse(DuplicateNames);
	EXPECT_FALSE(Durin::AssetPrivate::ValidateAssetPackageCodecTable(DuplicateNames, Error));

	std::array Incomplete{V9};
	Incomplete[0].Validate = nullptr;
	EXPECT_FALSE(Durin::AssetPrivate::ValidateAssetPackageCodecTable(Incomplete, Error));
	std::array InvalidVersion{V9};
	InvalidVersion[0].FormatVersion = 0;
	EXPECT_FALSE(Durin::AssetPrivate::ValidateAssetPackageCodecTable(InvalidVersion, Error));
}


TEST(FPackageAssetTests, EnvelopeDispatchUsesPermanentIdentityAndFailsBeforeCodec)
{
	using namespace Durin;
	using namespace Durin;
	using namespace Durin::AssetPrivate;
	constexpr FBinaryEnvelopeLimits Limits{16ull * 1024ull * 1024ull,
		1024ull * 1024ull * 1024ull};

	std::array<std::byte, BinaryEnvelopePreambleBytes> UnsupportedV6{};
	const FBinaryEnvelopePreamble V6Preamble{
		.FormatId = ObjectPackage::DastFormatId,
		.FormatVersion = 6,
		.HeaderBytes = UnsupportedV6.size(),
		.FileBytes = UnsupportedV6.size()};
	ASSERT_TRUE(EncodeBinaryEnvelopePreamble(V6Preamble, UnsupportedV6));
	ASSERT_TRUE(FinalizeBinaryEnvelopeHeader(
		UnsupportedV6, UnsupportedV6.size(), Limits));

	const FAssetPackageCodec* Codec = reinterpret_cast<const FAssetPackageCodec*>(1);
	uint32 FormatVersion = 0;
	const FAssetResult Resolved = ResolveAssetPackageReader(
		UnsupportedV6, Codec, &FormatVersion);
	EXPECT_EQ(Resolved.Error, EAssetError::UnsupportedVersion);
	EXPECT_EQ(Codec, nullptr);
	EXPECT_EQ(FormatVersion, 0u);

	std::array<std::byte, 8> Legacy{
		std::byte{0x44}, std::byte{0x41}, std::byte{0x53}, std::byte{0x54},
		std::byte{0x05}, std::byte{}, std::byte{}, std::byte{}};
	EXPECT_EQ(ResolveAssetPackageReader(Legacy, Codec).Error,
		EAssetError::UnsupportedVersion);

	std::array<std::byte, BinaryEnvelopePreambleBytes> Unknown{};
	const FBinaryEnvelopePreamble UnknownPreamble{
		.FormatId = {1, 2, 3, 4},
		.FormatVersion = 7,
		.HeaderBytes = Unknown.size(),
		.FileBytes = Unknown.size()};
	ASSERT_TRUE(EncodeBinaryEnvelopePreamble(UnknownPreamble, Unknown));
	ASSERT_TRUE(FinalizeBinaryEnvelopeHeader(Unknown, Unknown.size(), Limits));
	EXPECT_EQ(ResolveAssetPackageReader(Unknown, Codec).Error, EAssetError::UnsupportedVersion);

	std::array<std::byte, BinaryEnvelopePreambleBytes> Supported{};
	const FBinaryEnvelopePreamble SupportedPreamble{
		.FormatId = ObjectPackage::DastFormatId,
		.FormatVersion = ObjectPackage::DastV9FormatVersion,
		.HeaderBytes = Supported.size(),
		.FileBytes = Supported.size()};
	ASSERT_TRUE(EncodeBinaryEnvelopePreamble(SupportedPreamble, Supported));
	ASSERT_TRUE(FinalizeBinaryEnvelopeHeader(Supported, Supported.size(), Limits));
	EXPECT_TRUE(ResolveAssetPackageReader(Supported, Codec));
	Supported[48] ^= std::byte{1};
	EXPECT_EQ(ResolveAssetPackageReader(Supported, Codec).Error,
		EAssetError::CorruptFile);
}

TEST(FPackageAssetTests, V9CodecMatchesLiveWriteInspectReferenceAndLoadSemantics)
{
	InitializeAssetTests();
	using namespace Durin;
	using namespace Durin;
	using namespace Durin::AssetPrivate;
	FPackagePath TargetPath;
	FPackagePath ReplacementPath;
	FPackagePath SourcePath;
	FPackagePath RelocatedPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/TestAssets/V6Target", TargetPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/TestAssets/V6Replacement", ReplacementPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/TestAssets/V6Source", SourcePath));
	ASSERT_TRUE(FPackagePath::TryCreate("/TestAssets/V6Relocated", RelocatedPath));
	DPackageAssetForTest* Target = nullptr;
	DPackageAssetForTest* Source = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(TargetPath, Target));
	const FAssetResult TargetSave = SavePackage(Target->GetPackage());
	ASSERT_TRUE(TargetSave) << TargetSave.Message;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SourcePath, Source));
	Source->Value = 417;
	Source->ExternalReference = Target;
	ASSERT_TRUE(SavePackage(Source->GetPackage()));

	const FAssetCatalogEntry SourceData = FindAssetExact(SourcePath);
	ASSERT_TRUE(SourceData);
	Durin::FByteArray V8;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(V8, SourceData->PhysicalPath));
	Durin::FByteArray Bulk;
	std::filesystem::path BulkPath(SourceData->PhysicalPath);
	BulkPath.replace_extension(".dbulk");
	if (std::filesystem::is_regular_file(BulkPath))
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bulk, BulkPath));
	const FAssetPackageCodec& Codec = DastV9::GetCodec();
	const FAssetPackageReadContext Context{V8, Bulk, SourcePath, V8.size()};
	ASSERT_TRUE(Codec.Validate(Context));
	FAssetPackageHeader Header;
	uint64 HeaderByteCount = 0;
	ASSERT_TRUE(Durin::ReadLittleEndianAt(V8, 32, HeaderByteCount));
	ASSERT_TRUE(Codec.ReadHeader(Context, Header));
	EXPECT_EQ(Header.FormatVersion, ObjectPackage::DastV9FormatVersion);
	EXPECT_EQ(Header.ObjectCount, 2);
	EXPECT_EQ(Header.Dependencies, std::vector{TargetPath});
	FAssetPackageInspection Inspection;
	ASSERT_TRUE(Codec.Inspect(Context, Inspection));
	EXPECT_EQ(Inspection.Header.FormatVersion, ObjectPackage::DastV9FormatVersion);
	EXPECT_EQ(Inspection.Fingerprint.ReaderVersion, ObjectPackage::DastV9FormatVersion);

	std::vector<FAssetReferenceEdge> References;
	ASSERT_TRUE(Codec.ExtractReferences(Context, References));
	ASSERT_EQ(References.size(), 1);
	EXPECT_EQ(std::ranges::count(References, MakeFormerMainObjectPath(TargetPath),
		&FAssetReferenceEdge::TargetPath), 1);
	const FReflectionSchemaCatalog Catalog = FReflectionSchemaCatalog::Capture();
	FPackageSchemaInspection SchemaInspection;
	FMemoryAssetPackageByteSource SchemaSource(V8);
	ASSERT_TRUE(Codec.InspectSchema(
		SchemaSource, SourcePath, Catalog, SchemaInspection, nullptr, false, {}));
	EXPECT_EQ(SchemaInspection.FormatVersion, ObjectPackage::DastV9FormatVersion);

	FAssetPackageEncodedClosure DirectWrite;
	ASSERT_TRUE(Codec.Write(Source->GetPackage(), DirectWrite,
		EDefaultDeltaMode::NoDelta, {}));
	EXPECT_EQ(DirectWrite.PackageBytes, V8);
	EXPECT_EQ(DirectWrite.BulkBytes, Bulk);
	EXPECT_TRUE(Codec.bCanMutate);
	FAssetPackageEncodedClosure Relocated;
	ASSERT_TRUE(Codec.Relocate(Context, RelocatedPath, Relocated));
	ASSERT_TRUE(Codec.Validate({Relocated.PackageBytes, Relocated.BulkBytes,
		RelocatedPath, Relocated.PackageBytes.size()}));
	const FAssetRedirectorFixupMapping Mapping{TargetPath, ReplacementPath};
	FAssetPackageEncodedClosure Rewritten;
	ASSERT_TRUE(Codec.RewriteReferences(
		Context, std::span(&Mapping, 1), 1, Rewritten));
	References.clear();
	ASSERT_TRUE(Codec.ExtractReferences({Rewritten.PackageBytes, Rewritten.BulkBytes,
		SourcePath, Rewritten.PackageBytes.size()}, References));
	EXPECT_EQ(std::ranges::count(References, MakeTopLevelObjectPath(
		ReplacementPath, TargetPath.GetPackageName()),
		&FAssetReferenceEdge::TargetPath), 1);
	FAssetPackageEncodedClosure Redirector;
	const FAssetRedirectorWriteMapping RedirectMapping{
		.Source = MakeTopLevelObjectPath(
			SourcePath, SourcePath.GetPackageName()).GetAssetPath(),
		.Destination = MakeTopLevelObjectPath(
			TargetPath, TargetPath.GetPackageName())};
	ASSERT_TRUE(Codec.WriteRedirector(
		SourcePath, std::span{&RedirectMapping, 1}, Redirector));
	ASSERT_TRUE(Codec.ReadHeader({Redirector.PackageBytes, Redirector.BulkBytes,
		SourcePath, Redirector.PackageBytes.size()}, Header));
	EXPECT_EQ(Header.EntryKind, EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Header.RedirectDestination, TargetPath);

	ASSERT_TRUE(UnloadPackage(SourcePath));
	DPackage* Loaded = nullptr;
	FAssetLoadReport Report;
	ASSERT_TRUE(Codec.Load(Context, Loaded, &Report, {}, {}));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->HasAnyObjectFlags(EObjectFlags::Standalone));
	EXPECT_FALSE(Loaded->HasAnyInternalFlags(EObjectInternalFlags::RootSet));
	auto* LoadedAsset = static_cast<DPackageAssetForTest*>(
		Loaded->FindTopLevelAsset(SourcePath.GetPackageName()));
	ASSERT_NE(LoadedAsset, nullptr);
	EXPECT_EQ(LoadedAsset->Value, 417);
	EXPECT_EQ(LoadedAsset->ExternalReference, Target);
	MarkObjectHierarchyAsGarbage(Loaded);
	CollectGarbage();
	ASSERT_TRUE(DeleteAssetClosureForTest({SourcePath, TargetPath}));
}

TEST(FPackageAssetTests, V9PreservesExternalPayloadBytesAndPlacement)
{
	InitializeAssetTests();
	using namespace Durin;
	using namespace Durin;
	using namespace Durin::AssetPrivate;
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TestAssets/V6ExternalPayload", Path));
	DBulkPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
	Durin::FByteArray Payload(
		static_cast<size_t>(EditorBulkDataExternalThreshold + 17), std::byte{0x6b});
	ASSERT_TRUE(Asset->Payload.ReplaceBytes(Payload));
	const FAssetResult BulkSaveResult = SavePackage(Asset->GetPackage());
	ASSERT_TRUE(BulkSaveResult) << BulkSaveResult.Message;
	const FAssetCatalogEntry Data = FindAssetExact(Path);
	ASSERT_TRUE(Data);
	Durin::FByteArray V9;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(V9, Data->PhysicalPath));
	std::filesystem::path BulkPath(Data->PhysicalPath);
	BulkPath.replace_extension(".dbulk");
	Durin::FByteArray Bulk;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bulk, BulkPath));
	ObjectPackage::FLinkerTables Linker;
	ObjectPackage::FPackageReaderDiagnostic Diagnostic;
	ASSERT_TRUE(ObjectPackage::ReadPackageV9(
		V9, Bulk, Path, Linker, &Diagnostic)) << Diagnostic.Message;
	ASSERT_EQ(Linker.Exports.size(), 1);
	ASSERT_EQ(Linker.Exports.front().Properties.size(), 1);
	const auto& Value = Linker.Exports.front().Properties.front().Value;
	EXPECT_EQ(Value.BulkStorage, ObjectPackage::EBulkStorageKind::External);
	EXPECT_EQ(Value.Bytes, Payload);
	EXPECT_EQ(Bulk, Payload);
	ObjectPackage::FLinkerTables MetadataLinker;
	ASSERT_TRUE(ObjectPackage::ReadPackageV9Metadata(
		V9, Bulk.size(), Path, MetadataLinker, &Diagnostic))
		<< Diagnostic.Message;
	ASSERT_EQ(MetadataLinker.Exports.size(), 1u);
	ASSERT_EQ(MetadataLinker.Exports.front().Properties.size(), 1u);
	const auto& MetadataValue =
		MetadataLinker.Exports.front().Properties.front().Value;
	EXPECT_EQ(MetadataValue.BulkStorage,
		ObjectPackage::EBulkStorageKind::External);
	EXPECT_FALSE(MetadataValue.bBulkPayloadAvailable);
	EXPECT_TRUE(MetadataValue.Bytes.empty());
	EXPECT_EQ(MetadataValue.BulkStoredSize, Payload.size());
	EXPECT_EQ(MetadataValue.BulkContentHash,
		FXxHash128::HashBuffer(Payload));
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST(FPackageAssetTests, HeaderReaderRejectsMalformedAndUnboundedDeclarations)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/HeaderValidationSource", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	const auto Root = Durin::Testing::GetTestWorkDirectory() / "Assets";
	const auto Source = Root / "HeaderValidationSource.dasset";
	Durin::FByteArray Valid;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Valid, Source));
	ASSERT_GT(Valid.size(), 16u);
	Durin::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::ReadAssetPackageHeader(
		Source.generic_string(), Path, Header));

	auto Truncated = std::span<const std::byte>(Valid).first(4);
	const auto TruncatedFile = Root / "HeaderTruncated.dasset";
	WriteTestBytes(TruncatedFile, Truncated);
	EXPECT_EQ(Durin::ReadAssetPackageHeader(
		TruncatedFile.generic_string(), Path, Header).Error,
		Durin::EAssetRegistryError::CorruptFile);

	auto Corrupt = Valid;
	Corrupt[0] ^= std::byte{0xff};
	const auto CorruptFile = Root / "HeaderCorrupt.dasset";
	WriteTestBytes(CorruptFile, Corrupt);
	EXPECT_EQ(Durin::ReadAssetPackageHeader(
		CorruptFile.generic_string(), Path, Header).Error,
		Durin::EAssetRegistryError::CorruptFile);

	for (const uint32 Version : {3u, 4u, 7u})
	{
		auto Unsupported = Valid;
		std::memcpy(Unsupported.data() + sizeof(uint32), &Version, sizeof(Version));
		const auto UnsupportedFile = Root / std::format("HeaderUnsupported{}.dasset", Version);
		WriteTestBytes(UnsupportedFile, Unsupported);
		EXPECT_EQ(
			Durin::ReadAssetPackageHeader(
				UnsupportedFile.generic_string(), Path, Header).Error,
			Durin::EAssetRegistryError::CorruptFile);
		EXPECT_EQ(
			Durin::ValidateAssetPackageBytes(Unsupported, Path).Error,
			Durin::EAssetError::CorruptFile);
	}

}

TEST(FPackageAssetTests, RedirectorsRoundTripAndResolveWithoutLoading)
{
	InitializeAssetTests();
	Durin::FPackagePath TargetPath, AliasPath, NormalizedAliasPath, MissingPath,
		UnregisteredPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RedirectRoundTripTarget", TargetPath
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RedirectRoundTripAlias", AliasPath
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RedirectNormalizedAlias", NormalizedAliasPath
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RedirectMissingTarget", MissingPath
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/DoesNotExist", UnregisteredPath
	));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(TargetPath, Target));
	Target->Value = 99;
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));

	Durin::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::CreateAssetRedirectorForTesting(
		AliasPath, TargetPath, Redirector
	));
	ASSERT_NE(Redirector, nullptr);
	EXPECT_EQ(Redirector->GetDestinationObject(), Target);
	ASSERT_TRUE(Durin::SavePackage(Redirector->GetPackage()));
	const auto AliasFile = Durin::Testing::GetTestWorkDirectory()
						   / "Assets" / "RedirectRoundTripAlias.dasset";
	Durin::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::ReadAssetPackageHeader(
		AliasFile.generic_string(), AliasPath, Header
	));
	EXPECT_EQ(Header.FormatVersion, Durin::ObjectPackage::DastV9FormatVersion);
	EXPECT_EQ(Header.AssetClassName, "Durin::DAssetRedirector");
	EXPECT_EQ(Header.EntryKind, Durin::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Header.RedirectDestination, TargetPath);
	EXPECT_EQ(Header.Dependencies, (std::vector<Durin::FPackagePath>{TargetPath}));
	EXPECT_EQ(Header.ObjectCount, 1u);
	EXPECT_LT(Header.BytesRead, std::filesystem::file_size(AliasFile));

	Durin::DAssetRedirector* Normalized = nullptr;
	ASSERT_TRUE(Durin::CreateAssetRedirectorForTesting(
		NormalizedAliasPath, AliasPath, Normalized
	));
	ASSERT_NE(Normalized, nullptr);
	EXPECT_EQ(Normalized->GetDestinationObject(), Target);
	ASSERT_TRUE(Durin::SavePackage(Normalized->GetPackage()));
	const auto NormalizedFile = Durin::Testing::GetTestWorkDirectory()
								/ "Assets" / "RedirectNormalizedAlias.dasset";
	ASSERT_TRUE(Durin::ReadAssetPackageHeader(
		NormalizedFile.generic_string(), NormalizedAliasPath, Header
	));
	EXPECT_EQ(Header.RedirectDestination, TargetPath);
	EXPECT_EQ(Durin::CreateAssetRedirectorForTesting(MissingPath, MissingPath, Redirector).Error, Durin::EAssetError::InvalidPath);
	EXPECT_EQ(Durin::CreateAssetRedirectorForTesting(MissingPath, UnregisteredPath, Redirector).Error, Durin::EAssetError::NotFound);

	ASSERT_TRUE(Durin::UnloadPackage(NormalizedAliasPath));
	ASSERT_TRUE(Durin::UnloadPackage(AliasPath));
	ASSERT_TRUE(Durin::UnloadPackage(TargetPath));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation
	));
	EXPECT_EQ(Durin::FindResidentPackage(AliasPath), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(TargetPath), nullptr);
	Durin::FAssetCatalogEntry Exact =
		Durin::FindAssetExact(AliasPath);
	ASSERT_NE(Exact, nullptr);
	EXPECT_EQ(Exact->EntryKind, Durin::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Exact->RedirectDestination, TargetPath);
	const auto Reverse = Durin::FindRedirectorsTo(TargetPath);
	EXPECT_EQ(Reverse, (std::vector<Durin::FPackagePath>{NormalizedAliasPath, AliasPath}));
	const Durin::FAssetPathResolveResult Resolved =
		Durin::ResolveAssetPath(AliasPath);
	ASSERT_TRUE(Resolved);
	EXPECT_EQ(Resolved.CatalogRevision, Durin::GetAssetCatalogRevision());
	EXPECT_EQ(Resolved.RequestedPath, AliasPath);
	EXPECT_EQ(Resolved.FinalPath, TargetPath);
	EXPECT_EQ(Resolved.RedirectChain, (std::vector<Durin::FPackagePath>{AliasPath}));
	ASSERT_TRUE(Resolved.FinalAssetData.has_value());
	EXPECT_EQ(Resolved.FinalAssetData->PackagePath, TargetPath);
	EXPECT_EQ(Durin::FindResidentPackage(AliasPath), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(TargetPath), nullptr);

	ShutdownAssetManagerForRestart();
	const Durin::FAssetCatalogRefreshResult RestartRefresh =
		Durin::RefreshAssetRegistry();
	ASSERT_TRUE(RestartRefresh);
	EXPECT_GE(RestartRefresh.CatalogStats.Reused, 2u);
	EXPECT_EQ(RestartRefresh.CatalogStats.Redirectors, 2u);
	Exact = Durin::FindAssetExact(AliasPath);
	ASSERT_NE(Exact, nullptr);
	EXPECT_EQ(Exact->RedirectDestination, TargetPath);
	const auto RegistryCache = std::filesystem::path(
								   Durin::FPaths::DerivedDataCacheDir()
							   )
							   / "AssetRegistry" / "Registry.bin";
	const std::array<std::byte, 3> CorruptCache = {std::byte{1}, std::byte{2}, std::byte{3}};
	WriteTestBytes(RegistryCache, CorruptCache);
	const Durin::FAssetCatalogRefreshResult CacheRecoveryRefresh =
		Durin::RefreshAssetRegistry();
	ASSERT_TRUE(CacheRecoveryRefresh);
	EXPECT_FALSE(CacheRecoveryRefresh.CatalogCacheWarning.empty());
	Exact = Durin::FindAssetExact(AliasPath);
	ASSERT_NE(Exact, nullptr);
	EXPECT_EQ(Exact->RedirectDestination, TargetPath);
	DPackageAssetForTest* RedirectedTarget = nullptr;
	Durin::FAssetLoadReport RedirectedReport;
	ASSERT_TRUE(Durin::LoadObject(
		MakeFormerMainObjectPath(AliasPath), RedirectedTarget, &RedirectedReport));
	ASSERT_NE(RedirectedTarget, nullptr);
	EXPECT_EQ(RedirectedReport.RequestedPath, AliasPath);
	EXPECT_EQ(RedirectedReport.FinalPath, TargetPath);
	EXPECT_EQ(RedirectedReport.PackagePath, TargetPath);
	EXPECT_EQ(RedirectedReport.CatalogRevision, Durin::GetAssetCatalogRevision());
	EXPECT_EQ(RedirectedReport.RedirectChain,
		(std::vector<Durin::FPackagePath>{AliasPath}));
	EXPECT_EQ(RedirectedReport.FinalAssetClassName,
		DPackageAssetForTest::StaticClass()->GetQualifiedName().ToString());
	EXPECT_EQ(RedirectedReport.Error, Durin::EAssetError::None);
	EXPECT_EQ(RedirectedReport.PackageFileReadCount, 1u);
	EXPECT_EQ(RedirectedTarget->GetPackage()->GetPackagePath(), TargetPath.ToString());
	EXPECT_EQ(Durin::FindResidentPackage(AliasPath), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(TargetPath), RedirectedTarget->GetPackage());
	Redirector = nullptr;
	Durin::FAssetLoadReport WrongTypeReport;
	EXPECT_EQ(
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AliasPath), Redirector, &WrongTypeReport).Error,
		Durin::EAssetError::TypeMismatch
	);
	EXPECT_EQ(Redirector, nullptr);
	EXPECT_EQ(WrongTypeReport.RequestedPath, AliasPath);
	EXPECT_EQ(WrongTypeReport.FinalPath, TargetPath);
	EXPECT_EQ(WrongTypeReport.Error, Durin::EAssetError::TypeMismatch);
	EXPECT_EQ(WrongTypeReport.PackageFileReadCount, 0u);

	EXPECT_EQ(
		Durin::DeleteAssetForTesting(AliasPath).Error,
		Durin::EAssetError::InUse
	);
	ASSERT_TRUE(DeleteAssetClosureForTest(
		{NormalizedAliasPath, AliasPath, TargetPath}
	));
}

TEST(FPackageAssetTests, AuthoredArchiveFreezesNativeFieldsReferencesAndFailures)
{
	InitializeAssetTests();
	Durin::FPackagePath SourcePath;
	Durin::FPackagePath TargetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/AuthoredArchive", SourcePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/AuthoredArchiveTarget", TargetPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(TargetPath, Target));
	DAuthoredArchiveAssetForTest* Source = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SourcePath, Source));
	Source->NativeValue = 0x12345678;
	Source->HardReference = Target;
	ASSERT_TRUE(Durin::FObjectPath::TryCreate(
		"/TestAssets/AuthoredArchiveSoftOnly.AuthoredArchiveSoftOnly", Source->SoftReference));

	GAuthoredArchivePurposes.clear();
	GAuthoredArchiveFormatVersions.clear();
	Durin::FByteArray FirstBytes;
	Durin::FByteArray SecondBytes;
	ASSERT_TRUE(Durin::SerializeAssetPackageBytes(Source->GetPackage(), FirstBytes));
	ASSERT_TRUE(Durin::SerializeAssetPackageBytes(Source->GetPackage(), SecondBytes));
	EXPECT_EQ(FirstBytes, SecondBytes);
	EXPECT_EQ(std::ranges::count(GAuthoredArchivePurposes, Durin::EArchivePurpose::Discovery), 4);
	EXPECT_EQ(std::ranges::count(GAuthoredArchivePurposes, Durin::EArchivePurpose::AuthoredPackage), 4);
	EXPECT_TRUE(std::ranges::all_of(GAuthoredArchiveFormatVersions, [](uint32 Version) {
		return Version == Durin::ObjectPackage::DastV9FormatVersion;
	}));

	const auto File = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "AuthoredArchive.dasset";
	WriteTestBytes(File, FirstBytes);
	const uint64 ConstructCountBeforeTools = GAuthoredConstructCount;
	const size_t SerializeCountBeforeTools = GAuthoredArchivePurposes.size();
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(File.generic_string(), Inspection));
	ASSERT_EQ(Inspection.Header.Dependencies.size(), 1u);
	EXPECT_EQ(Inspection.Header.Dependencies.front(), TargetPath);
	const auto* NativeField = Inspection.FindField("NativeValue");
	ASSERT_NE(NativeField, nullptr);
	EXPECT_EQ(NativeField->DeclaringClass, "Tests::DAuthoredArchiveAssetForTest");
	EXPECT_EQ(NativeField->TypeSignature, "4:4");
	int32 NativeValue = 0;
	ASSERT_TRUE(NativeField->TryReadScalar(NativeValue));
	EXPECT_EQ(NativeValue, Source->NativeValue);
	const auto* HardField = Inspection.FindField("HardReference");
	ASSERT_NE(HardField, nullptr);
	Durin::FAssetPackageObjectReference HardReference;
	ASSERT_TRUE(HardField->TryReadObjectReference(HardReference));
	EXPECT_EQ(HardReference.Kind, Durin::EAssetPackageObjectReferenceKind::External);
	EXPECT_EQ(HardReference.ExternalPath, MakeFormerMainObjectPath(TargetPath));
	ASSERT_NE(Inspection.FindField("SoftReference"), nullptr);
	EXPECT_EQ(GAuthoredConstructCount, ConstructCountBeforeTools);
	EXPECT_EQ(GAuthoredArchivePurposes.size(), SerializeCountBeforeTools);

	auto ExpectAtomicFailure = [&](auto Configure, Durin::EAssetError ExpectedError) {
		Source->bSkipSuper = false;
		Source->bDuplicateField = false;
		Source->bLateField = false;
		Source->bUnsupportedCustomVersion = false;
		Configure();
		Durin::FByteArray Sentinel{std::byte{9}, std::byte{8}, std::byte{7}};
		const Durin::FAssetResult Result =
			Durin::SerializeAssetPackageBytes(Source->GetPackage(), Sentinel);
		EXPECT_EQ(Result.Error, ExpectedError);
		EXPECT_EQ(Sentinel, (Durin::FByteArray{std::byte{9}, std::byte{8}, std::byte{7}}));
	};
	ExpectAtomicFailure([&] { Source->bSkipSuper = true; },
		Durin::EAssetError::UnsupportedProperty);
	ExpectAtomicFailure([&] { Source->bDuplicateField = true; },
		Durin::EAssetError::UnsupportedProperty);
	ExpectAtomicFailure([&] { Source->bLateField = true; },
		Durin::EAssetError::UnsupportedProperty);
	ExpectAtomicFailure([&] { Source->bUnsupportedCustomVersion = true; },
		Durin::EAssetError::UnsupportedVersion);
}

TEST(FPackageAssetTests, CookedArchiveDispatchesImmutableTargetProjection)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/CookedArchiveProjection", Path));
	DAuthoredArchiveAssetForTest* Source = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Source));
	Source->NativeValue = 37;
	const std::array BulkBytes{std::byte{1}, std::byte{3}, std::byte{5}};
	std::string BulkError;
	ASSERT_TRUE(Durin::FBulkData::TryCreateDetached(
		BulkBytes, Source->CookedBulk, &BulkError)) << BulkError;

	Durin::FAssetPackageSerializationOptions Options;
	Options.Domain = Durin::EAssetPackageSaveDomain::Cooked;
	Durin::FByteArray Bytes{std::byte{0x7f}};
	EXPECT_FALSE(Durin::SerializeAssetPackageBytes(
		Source->GetPackage(), Bytes, Options));
	EXPECT_EQ(Bytes, (Durin::FByteArray{std::byte{0x7f}}));

	Options.TargetPlatform = Durin::ECookTargetPlatform::Win64;
	Options.TargetProfile = Durin::ECookTargetProfile::Game;
	GCookedSerializeCount = 0;
	const Durin::FAssetResult CookResult =
		Durin::SerializeAssetPackageBytes(Source->GetPackage(), Bytes, Options);
	ASSERT_TRUE(CookResult) << CookResult.Message;
	EXPECT_GT(GCookedSerializeCount, 0u);
	EXPECT_EQ(Source->NativeValue, 37);
	EXPECT_EQ(Source->CookedBulk.GetState(), Durin::EBulkDataState::Detached);

	const auto File = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "CookedArchiveProjection.dasset";
	WriteTestBytes(File, Bytes);
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(File.generic_string(), Inspection));
	EXPECT_EQ(Inspection.FindField("NativeValue"), nullptr);
	const auto* CookedField = Inspection.FindField("CookedValue");
	ASSERT_NE(CookedField, nullptr);
	int32 CookedValue = 0;
	ASSERT_TRUE(CookedField->TryReadScalar(CookedValue));
	EXPECT_EQ(CookedValue, 74);
	EXPECT_NE(Inspection.FindField("CookedBulk"), nullptr);
	EXPECT_EQ(Source->NativeValue, 37);
	EXPECT_EQ(Source->CookedBulk.GetState(), Durin::EBulkDataState::Detached);
}

TEST(FPackageAssetTests, CookPublishesHeaderlessRawPlatformDataFields)
{
	InitializeAssetTests();
	const auto CookRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookedRawPlatformData");
	const auto GameRoot = CookRoot / "Game";
	std::filesystem::create_directories(GameRoot);
	const std::array MountDefinitions{Durin::FMountPoint{
		.VirtualRoot = "/Game/",
		.Owner = Durin::EMountOwner::Test,
		.Root = GameRoot,
		.ContentPath = ".",
		.bAutoScan = false}};
	Durin::Testing::FScopedMountRegistryFixture Mounts(MountDefinitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedRawField", Path));
	DAuthoredArchiveAssetForTest* Source = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Source));
	const Durin::FByteArray BulkBytes(
		static_cast<size_t>(Durin::EditorBulkDataExternalThreshold + 1),
		std::byte{0x5a});
	std::string Error;
	ASSERT_TRUE(Durin::FBulkData::TryCreateDetached(
		BulkBytes, Source->CookedBulk, &Error)) << Error;
	Durin::FCookContext Context(
		CookRoot, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Context.AddPackage(Path.ToString(), Source->GetPackage(), &Error)) << Error;
	ASSERT_TRUE(Context.Publish(&Error)) << Error;
	EXPECT_EQ(Source->CookedBulk.GetState(), Durin::EBulkDataState::Detached);

	std::filesystem::path PackagePath;
	ASSERT_TRUE(Durin::ResolveCookedPackagePath(
		CookRoot, Path.GetView(), PackagePath, &Error)) << Error;
	std::filesystem::path SegmentPath = PackagePath;
	SegmentPath.replace_extension(".dbulk");
	Durin::FByteArray Segment;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Segment, SegmentPath));
	EXPECT_EQ(Segment, BulkBytes);
	ASSERT_GE(Segment.size(), 4u);
	EXPECT_NE(std::string_view(reinterpret_cast<const char*>(Segment.data()), 4), "DURF");

	Durin::FByteArray ManifestBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		ManifestBytes, CookRoot / "CookManifest.bin"));
	Durin::FCookManifest Manifest;
	ASSERT_TRUE(Durin::DecodeCookManifest(ManifestBytes, Manifest, &Error)) << Error;
	EXPECT_NE(std::ranges::find(Manifest.Entries,
		Durin::ECookManifestEntryKind::PackageBulk,
		&Durin::FCookManifestEntry::Kind), Manifest.Entries.end());
	const auto PackageManifestEntry = std::ranges::find(Manifest.Entries,
		Durin::ECookManifestEntryKind::CookedPackage,
		&Durin::FCookManifestEntry::Kind);
	ASSERT_NE(PackageManifestEntry, Manifest.Entries.end());
	EXPECT_NE(PackageManifestEntry->Flags
		& Durin::CookManifestEntryCookedFieldProjection, 0u);
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(PackagePath.generic_string(), Inspection));
	const Durin::FAssetPackageField* PlatformField =
		Inspection.FindField("CookedBulk");
	ASSERT_NE(PlatformField, nullptr);
	Durin::FEditorBulkDataStorageDescriptor PlatformStorage;
	ASSERT_TRUE(PlatformField->TryReadBulkDataStorageDescriptor(PlatformStorage));
	EXPECT_EQ(PlatformStorage.LogicalByteCount, BulkBytes.size());
	EXPECT_EQ(PlatformStorage.StoredByteCount, BulkBytes.size());
	EXPECT_EQ(PlatformStorage.StorageKind,
		Durin::EEditorBulkDataStorageKind::External);

	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	auto Runtime = Durin::FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(Durin::FAssetRuntimeConfiguration::Cooked(CookRoot, Runtime));
	ASSERT_TRUE(Durin::InitializeAssetManager(std::move(Runtime)));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	ASSERT_TRUE(Durin::AdmitAssetPackageToCatalog(Path));
	DAuthoredArchiveAssetForTest* Loaded = nullptr;
	const Durin::FAssetResult LoadResult = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded);
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->CookedBulk.GetState(), Durin::EBulkDataState::Attached);
	const Durin::FPackageResourceHandle Resource =
		Durin::GetPackageResourceManager().FindPackage(Path.ToString());
	ASSERT_NE(Resource, nullptr);
	ASSERT_TRUE(Loaded->CookedBulk.ReloadAsync().Wait()) << Error;
	std::span<const std::byte> LoadedBytes;
	ASSERT_TRUE(Loaded->CookedBulk.LockReadOnly(LoadedBytes, &Error)) << Error;
	EXPECT_TRUE(std::ranges::equal(LoadedBytes, BulkBytes));
	ASSERT_TRUE(Loaded->CookedBulk.UnlockReadOnly(&Error)) << Error;
	EXPECT_EQ(Loaded->CookedBulk.GetState(), Durin::EBulkDataState::Resident);
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();

	Segment.front() ^= std::byte{1};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Segment, SegmentPath));
	auto CorruptRuntime = Durin::FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(Durin::FAssetRuntimeConfiguration::Cooked(CookRoot, CorruptRuntime));
	ASSERT_TRUE(Durin::InitializeAssetManager(std::move(CorruptRuntime)));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	EXPECT_EQ(Durin::AdmitAssetPackageToCatalog(Path).Error,
		Durin::EAssetError::CorruptFile);
	Loaded = nullptr;
	EXPECT_EQ(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded).Error,
		Durin::EAssetError::NotFound);
	EXPECT_EQ(Loaded, nullptr);
	EXPECT_EQ(Durin::GetPackageResourceManager().FindPackage(Path.ToString()), nullptr);
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::InitializeAssetManager());
}

TEST(FPackageAssetTests, CookedInlineOnlyProjectionLoadsWithoutBulkCompanion)
{
	InitializeAssetTests();
	const auto CookRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookedInlineOnlyProjection");
	const auto GameRoot = CookRoot / "Game";
	std::filesystem::create_directories(GameRoot);
	const std::array MountDefinitions{Durin::FMountPoint{
		.VirtualRoot = "/Game/",
		.Owner = Durin::EMountOwner::Test,
		.Root = GameRoot,
		.ContentPath = ".",
		.bAutoScan = false}};
	Durin::Testing::FScopedMountRegistryFixture Mounts(MountDefinitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/MetadataOnly", Path));
	DAuthoredArchiveAssetForTest* Source = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Source));
	Source->NativeValue = 41;
	const std::array InlineBytes{std::byte{2}, std::byte{4}, std::byte{6}};
	std::string Error;
	ASSERT_TRUE(Durin::FBulkData::TryCreateDetached(
		InlineBytes, Source->CookedBulk, &Error)) << Error;
	Durin::FCookContext Context(
		CookRoot, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Context.AddPackage(Path.ToString(), Source->GetPackage(), &Error)) << Error;
	ASSERT_TRUE(Context.Publish(&Error)) << Error;

	std::filesystem::path PackagePath;
	ASSERT_TRUE(Durin::ResolveCookedPackagePath(
		CookRoot, Path.GetView(), PackagePath, &Error)) << Error;
	std::filesystem::path SegmentPath = PackagePath;
	SegmentPath.replace_extension(".dbulk");
	EXPECT_FALSE(std::filesystem::exists(SegmentPath));
	Durin::FByteArray ManifestBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		ManifestBytes, CookRoot / "CookManifest.bin"));
	Durin::FCookManifest Manifest;
	ASSERT_TRUE(Durin::DecodeCookManifest(ManifestBytes, Manifest, &Error)) << Error;
	ASSERT_EQ(Manifest.Entries.size(), 1u);
	EXPECT_EQ(Manifest.Entries.front().Kind,
		Durin::ECookManifestEntryKind::CookedPackage);
	EXPECT_NE(Manifest.Entries.front().Flags
		& Durin::CookManifestEntryCookedFieldProjection, 0u);

	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	auto Runtime = Durin::FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(Durin::FAssetRuntimeConfiguration::Cooked(CookRoot, Runtime));
	ASSERT_TRUE(Durin::InitializeAssetManager(std::move(Runtime)));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	ASSERT_TRUE(Durin::AdmitAssetPackageToCatalog(Path));
	DAuthoredArchiveAssetForTest* Loaded = nullptr;
	const Durin::FAssetResult LoadResult = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded);
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->CookedBulk.GetState(), Durin::EBulkDataState::Detached);
	std::span<const std::byte> LoadedBytes;
	ASSERT_TRUE(Loaded->CookedBulk.LockReadOnly(LoadedBytes, &Error)) << Error;
	EXPECT_TRUE(std::ranges::equal(LoadedBytes, InlineBytes));
	ASSERT_TRUE(Loaded->CookedBulk.UnlockReadOnly(&Error)) << Error;
	EXPECT_EQ(Durin::GetPackageResourceManager().FindPackage(Path.ToString()), nullptr);
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::InitializeAssetManager());
}

TEST(FPackageAssetTests, PerSaveOverridesOwnValuesOmitFieldsAndPreserveLiveState)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SaveOverrides", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	Asset->Value = 17;
	Asset->Label = "LiveLabel";
	const bool bDirtyBefore = Asset->GetPackage()->IsDirty();
	Durin::FPackagePath ForeignPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/ForeignSaveOverride", ForeignPath));
	DPackageAssetForTest* Foreign = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ForeignPath, Foreign));

	Durin::FProperty* ValueProperty = Asset->GetClass()->FindPropertyByName("Value");
	Durin::FProperty* LabelProperty = Asset->GetClass()->FindPropertyByName("Label");
	Durin::FProperty* ChildProperty = Asset->GetClass()->FindPropertyByName("DefaultChild");
	Durin::FProperty* ExternalProperty =
		Asset->GetClass()->FindPropertyByName("ExternalReference");
	ASSERT_NE(ValueProperty, nullptr);
	ASSERT_NE(LabelProperty, nullptr);
	ASSERT_NE(ChildProperty, nullptr);
	ASSERT_NE(ExternalProperty, nullptr);
	const int32 Replacement = 91;
	Durin::TObjectPtr<Durin::DObject> ReplacementExternal = Foreign;
	auto Overrides = std::make_shared<Durin::FObjectSaveOverrides>();
	std::string Error;
	ASSERT_TRUE(Overrides->AddPropertyValue(
		*Asset, *ValueProperty, Replacement, &Error)) << Error;
	ASSERT_TRUE(Overrides->AddPropertyOmission(
		*Asset, *LabelProperty, &Error)) << Error;
	ASSERT_TRUE(Overrides->AddPropertyOmission(
		*Asset, *ChildProperty, &Error)) << Error;
	ASSERT_TRUE(Overrides->AddPropertyValue(
		*Asset, *ExternalProperty, ReplacementExternal, &Error)) << Error;
	ASSERT_TRUE(Overrides->AddObjectOmission(
		*Asset->DefaultChild.Get(), &Error)) << Error;
	EXPECT_FALSE(Overrides->AddPropertyOmission(
		*Asset, *ValueProperty, &Error));
	const uint64 WrongType = 91;
	Durin::FObjectSaveOverrides TypeMismatchOverrides;
	EXPECT_FALSE(TypeMismatchOverrides.AddPropertyValue(
		*Asset, *ValueProperty, WrongType, &Error));
	const uint32 SameSizeWrongType = 91;
	EXPECT_FALSE(TypeMismatchOverrides.AddPropertyValue(
		*Asset, *ValueProperty, SameSizeWrongType, &Error));

	Durin::FAssetPackageSerializationOptions Options;
	Options.SaveOverrides = Overrides;
	Durin::FByteArray FirstBytes;
	Durin::FByteArray SecondBytes;
	const Durin::FAssetResult FirstPackageResult =
		Durin::SerializeAssetPackageBytes(
			Asset->GetPackage(), FirstBytes, Options);
	ASSERT_TRUE(FirstPackageResult) << FirstPackageResult.Message;
	const Durin::FAssetResult SecondResult = Durin::SerializeAssetPackageBytes(
		Asset->GetPackage(), SecondBytes, Options);
	ASSERT_TRUE(SecondResult) << SecondResult.Message;
	EXPECT_EQ(FirstBytes, SecondBytes);
	EXPECT_EQ(Asset->Value, 17);
	EXPECT_EQ(Asset->Label, "LiveLabel");
	EXPECT_EQ(Asset->ExternalReference.Get(), nullptr);
	EXPECT_EQ(Asset->GetPackage()->IsDirty(), bDirtyBefore);

	const std::filesystem::path File = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "SaveOverrides.dasset";
	WriteTestBytes(File, FirstBytes);
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(File.generic_string(), Inspection));
	ASSERT_EQ(Inspection.Objects.size(), 1u);
	ASSERT_EQ(Inspection.Header.Dependencies.size(), 1u);
	EXPECT_EQ(Inspection.Header.Dependencies.front(), ForeignPath);
	const Durin::FAssetPackageField* ValueField = Inspection.FindField("Value");
	ASSERT_NE(ValueField, nullptr);
	int32 SavedValue = 0;
	ASSERT_TRUE(ValueField->TryReadScalar(SavedValue));
	EXPECT_EQ(SavedValue, Replacement);
	EXPECT_EQ(Inspection.FindField("Label"), nullptr);

	Durin::FProperty* ForeignValueProperty = Foreign->GetClass()->FindPropertyByName("Value");
	ASSERT_NE(ForeignValueProperty, nullptr);
	auto ForeignOverrides = std::make_shared<Durin::FObjectSaveOverrides>();
	ASSERT_TRUE(ForeignOverrides->AddPropertyValue(
		*Foreign, *ForeignValueProperty, Replacement, &Error));
	Durin::FAssetPackageSerializationOptions ForeignOptions;
	ForeignOptions.SaveOverrides = std::move(ForeignOverrides);
	Durin::FByteArray Sentinel{std::byte{1}, std::byte{2}};
	const Durin::FAssetResult ForeignResult =
		Durin::SerializeAssetPackageBytes(Asset->GetPackage(), Sentinel, ForeignOptions);
	EXPECT_FALSE(ForeignResult);
	EXPECT_EQ(Sentinel, (Durin::FByteArray{std::byte{1}, std::byte{2}}));
	EXPECT_EQ(Asset->Value, 17);
	EXPECT_EQ(Asset->Label, "LiveLabel");
	EXPECT_EQ(Asset->GetPackage()->IsDirty(), bDirtyBefore);

	Durin::FPackagePath SoftOwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftSaveOverride", SoftOwnerPath));
	DSoftPackageAssetForTest* SoftOwner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SoftOwnerPath, SoftOwner));
	Durin::FProperty* DirectProperty = SoftOwner->GetClass()->FindPropertyByName("Direct");
	ASSERT_NE(DirectProperty, nullptr);
	DSoftPackageAssetForTest::FSoftReference ReplacementSoft(MakeFormerMainObjectPath(ForeignPath));
	auto SoftOverrides = std::make_shared<Durin::FObjectSaveOverrides>();
	ASSERT_TRUE(SoftOverrides->AddPropertyValue(
		*SoftOwner, *DirectProperty, ReplacementSoft, &Error)) << Error;
	Durin::FAssetPackageSerializationOptions SoftOptions;
	SoftOptions.SaveOverrides = std::move(SoftOverrides);
	Durin::FByteArray SoftBytes;
	ASSERT_TRUE(Durin::SerializeAssetPackageBytes(
		SoftOwner->GetPackage(), SoftBytes, SoftOptions));
	EXPECT_TRUE(SoftOwner->Direct.IsNull());
	const std::span<const std::byte> ForeignPathBytes =
		std::as_bytes(std::span{ForeignPath.GetView().data(), ForeignPath.GetView().size()});
	EXPECT_NE(std::search(SoftBytes.begin(), SoftBytes.end(),
		ForeignPathBytes.begin(), ForeignPathBytes.end()), SoftBytes.end());
}

TEST(FPackageAssetTests, SavesLoadsContainersReferencesAndRegistryMetadata)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/RoundTrip", Path));

	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	Asset->Value = 42;
	Asset->Label = "RoundTrip";
	Asset->DisplayName = Durin::FName("RoundTripName");
	Asset->PersistentId = Durin::FGuid(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff);
	Asset->RelatedIds = {Durin::FGuid(1, 2, 3, 4), Durin::FGuid(5, 6, 7, 8)};
	Asset->Scores = {3, 5, 8};
	Asset->NamedScores = {{"Alpha", 11}, {"Beta", 17}};
	ASSERT_NE(Asset->DefaultChild.Get(), nullptr);
	EXPECT_EQ(Asset->DefaultChild->GetObjectPath(), "/TestAssets/RoundTrip.RoundTrip:DefaultChild");

	Durin::DPackage* Package = Asset->GetPackage();
	ASSERT_NE(Package, nullptr);
	EXPECT_TRUE(Package->HasAnyObjectFlags(Durin::EObjectFlags::Standalone));
	EXPECT_FALSE(Package->HasAnyInternalFlags(
		Durin::EObjectInternalFlags::RootSet));
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::FindResidentPackage(Path), Package);
	ASSERT_TRUE(Durin::SavePackage(Package));
	ASSERT_TRUE(Durin::FindAssetExact(Path));
	ASSERT_TRUE(Durin::UnloadPackage(Path));

	DPackageAssetForTest* Loaded = nullptr;
	Durin::FAssetLoadReport LoadReport;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded, &LoadReport));
	EXPECT_EQ(LoadReport.PackageFileReadCount, 1u);
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->GetPackage()->HasAnyObjectFlags(
		Durin::EObjectFlags::Standalone));
	EXPECT_FALSE(Loaded->GetPackage()->HasAnyInternalFlags(
		Durin::EObjectInternalFlags::RootSet));
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::FindResidentPackage(Path), Loaded->GetPackage());
	EXPECT_EQ(Loaded->Value, 42);
	EXPECT_EQ(Loaded->Label, "RoundTrip");
	EXPECT_EQ(Loaded->DisplayName, Durin::FName("RoundTripName"));
	EXPECT_EQ(Loaded->PersistentId, (Durin::FGuid(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff)));
	EXPECT_EQ(Loaded->RelatedIds, (std::vector<Durin::FGuid>{Durin::FGuid(1, 2, 3, 4), Durin::FGuid(5, 6, 7, 8)}));
	EXPECT_EQ(Loaded->Scores, (std::vector<int32>{3, 5, 8}));
	EXPECT_EQ(Loaded->NamedScores.at("Alpha"), 11);
	EXPECT_EQ(Loaded->NamedScores.at("Beta"), 17);
	ASSERT_NE(Loaded->DefaultChild.Get(), nullptr);
	EXPECT_EQ(Durin::GDObjectArray.GetObjectsWithOuter(Loaded, Durin::EObjectQueryScope::LiveOnly).size(), 1u);
	EXPECT_EQ(Loaded->DefaultChild->GetObjectPath(), "/TestAssets/RoundTrip.RoundTrip:DefaultChild");
	EXPECT_EQ(Durin::FindResidentPackage(Path), Loaded->GetPackage());
	DPackageAssetForTest* Cached = nullptr;
	Durin::FAssetLoadReport CachedReport;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Cached, &CachedReport));
	EXPECT_EQ(Cached, Loaded);
	EXPECT_EQ(CachedReport.PackageFileReadCount, 0u);

	EXPECT_TRUE(Durin::UnloadPackage(Path));
}

TEST(FPackageAssetTests, PackageAndExactObjectLoadsSelectMultipleTopLevelAssets)
{
	InitializeAssetTests();
	Durin::FPackagePath PackagePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/MultiTopLevelLoad", PackagePath));
	DPackageAssetForTest* Primary = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(PackagePath, Primary));
	auto* Secondary = Durin::NewObject<DPackageAssetForTest>(
		Primary->GetPackage(), "Secondary");
	ASSERT_NE(Secondary, nullptr);
	Primary->Value = 11;
	Secondary->Value = 29;
	const Durin::FAssetResult SaveResult =
		Durin::SavePackage(Primary->GetPackage());
	ASSERT_TRUE(SaveResult) << SaveResult.Message;
	Durin::FPackagePath OwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/MultiTopLevelOwner", OwnerPath));
	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->ExternalReference = Secondary->DefaultChild.Get();
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::UnloadPackage(PackagePath));
	DPackageAssetForTest* LoadedOwner = nullptr;
	const Durin::FAssetResult OwnerLoadResult =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(OwnerPath), LoadedOwner);
	ASSERT_TRUE(OwnerLoadResult) << OwnerLoadResult.Message;
	ASSERT_NE(LoadedOwner->ExternalReference.Get(), nullptr);
	EXPECT_EQ(LoadedOwner->ExternalReference->GetObjectPath(),
		"/TestAssets/MultiTopLevelLoad.Secondary:DefaultChild");
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::UnloadPackage(PackagePath));

	Durin::DPackage* LoadedPackage = nullptr;
	ASSERT_TRUE(Durin::LoadPackage(PackagePath, LoadedPackage));
	ASSERT_NE(LoadedPackage, nullptr);
	ASSERT_EQ(LoadedPackage->GetTopLevelAssets().size(), 2u);
	Durin::FTopLevelAssetPath SecondaryAssetPath;
	Durin::FObjectPath SecondaryObjectPath;
	ASSERT_TRUE(Durin::FTopLevelAssetPath::TryCreate(
		PackagePath, "Secondary", SecondaryAssetPath));
	ASSERT_TRUE(Durin::FObjectPath::TryCreate(
		SecondaryAssetPath, std::span<const std::string>{}, SecondaryObjectPath));
	DPackageAssetForTest* LoadedSecondary = nullptr;
	ASSERT_TRUE(Durin::LoadObject(SecondaryObjectPath, LoadedSecondary));
	ASSERT_NE(LoadedSecondary, nullptr);
	EXPECT_EQ(LoadedSecondary->Value, 29);
	EXPECT_EQ(LoadedSecondary->GetFName(), Durin::FName("Secondary"));
	ASSERT_TRUE(Durin::UnloadPackage(PackagePath));
	ASSERT_TRUE(DeleteAssetClosureForTest({OwnerPath, PackagePath}));
}

TEST(FPackageAssetTests, EditorOnlyInnerObjectPersistsInspectsAndPrunesForCook)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/EditorOnlyInnerObject", Path));
	DImportMetadataOwnerForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Owner));
	auto* ImportData = Durin::NewObject<DReplayImportMetadataForTest>(
		Owner, "AssetImportData");
	ImportData->SchemaVersion = 7;
	ImportData->SourcePath = "/TestSources/EditorOnlyInnerObject.png";
	ImportData->Translator = "Tests.EditorOnlyTranslator";
	ImportData->Fingerprint = 0x123456789abcdef0ull;
	Owner->AssetImportData = ImportData;
	Owner->RuntimeValue = 91;
	const std::string ExpectedObjectPath = ImportData->GetObjectPath();

	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	const Durin::FAssetCatalogEntry Data = Durin::FindAssetExact(Path);
	ASSERT_TRUE(Data);
	const uint64 ConstructionCountBeforeInspection =
		GImportMetadataConstructionCount;
	Durin::FAssetPackageInspection AuthoredInspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		Data->PhysicalPath, AuthoredInspection));
	EXPECT_EQ(GImportMetadataConstructionCount, ConstructionCountBeforeInspection);
	ASSERT_EQ(AuthoredInspection.Objects.size(), 2u);
	const auto* ReferenceField = AuthoredInspection.FindField("AssetImportData");
	ASSERT_NE(ReferenceField, nullptr);
	Durin::FAssetPackageObjectReference Reference;
	ASSERT_TRUE(ReferenceField->TryReadObjectReference(Reference));
	ASSERT_EQ(Reference.Kind,
		Durin::EAssetPackageObjectReferenceKind::Internal);
	const auto* InspectedImportData = AuthoredInspection.FindObject(Reference.ObjectId);
	ASSERT_NE(InspectedImportData, nullptr);
	EXPECT_EQ(InspectedImportData->OuterId, 1u);
	EXPECT_EQ(InspectedImportData->ClassName,
		"Tests::DReplayImportMetadataForTest");
	EXPECT_EQ(InspectedImportData->ObjectPath,
		"EditorOnlyInnerObject/AssetImportData");
	const auto* InspectedSource = InspectedImportData->FindField("SourcePath");
	ASSERT_NE(InspectedSource, nullptr);
	std::string SourcePath;
	ASSERT_TRUE(InspectedSource->TryReadString(SourcePath));
	EXPECT_EQ(SourcePath, ImportData->SourcePath);

	ASSERT_TRUE(Durin::UnloadPackage(Path));
	DImportMetadataOwnerForTest* LoadedOwner = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), LoadedOwner));
	auto* LoadedImportData = Durin::Cast<DReplayImportMetadataForTest>(
		LoadedOwner->AssetImportData.Get());
	ASSERT_NE(LoadedImportData, nullptr);
	EXPECT_EQ(LoadedImportData->GetOuter(), LoadedOwner);
	EXPECT_EQ(LoadedImportData->GetObjectPath(), ExpectedObjectPath);
	EXPECT_EQ(LoadedImportData->SchemaVersion, 7u);
	EXPECT_EQ(LoadedImportData->SourcePath,
		"/TestSources/EditorOnlyInnerObject.png");
	EXPECT_EQ(LoadedImportData->Translator, "Tests.EditorOnlyTranslator");
	EXPECT_EQ(LoadedImportData->Fingerprint, 0x123456789abcdef0ull);

	Durin::FAssetPackageSerializationOptions CookOptions;
	CookOptions.Domain = Durin::EAssetPackageSaveDomain::Cooked;
	CookOptions.TargetPlatform = Durin::ECookTargetPlatform::Win64;
	CookOptions.TargetProfile = Durin::ECookTargetProfile::Game;
	Durin::FByteArray CookedBytes;
	ASSERT_TRUE(Durin::SerializeAssetPackageBytes(
		LoadedOwner->GetPackage(), CookedBytes, CookOptions));
	const auto CookedFile = std::filesystem::path(Data->PhysicalPath);
	WriteTestBytes(CookedFile, CookedBytes);
	Durin::FAssetPackageInspection CookedInspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		CookedFile.generic_string(), CookedInspection));
	ASSERT_EQ(CookedInspection.Objects.size(), 1u);
	EXPECT_EQ(CookedInspection.FindField("AssetImportData"), nullptr);
	EXPECT_NE(CookedInspection.FindField("RuntimeValue"), nullptr);
	const auto ContainsText = [&](std::string_view Text) {
		const auto Bytes = std::as_bytes(std::span{Text.data(), Text.size()});
		return std::search(CookedBytes.begin(), CookedBytes.end(),
			Bytes.begin(), Bytes.end()) != CookedBytes.end();
	};
	EXPECT_FALSE(ContainsText("DReplayImportMetadataForTest"));
	EXPECT_FALSE(ContainsText("Tests.EditorOnlyTranslator"));
	EXPECT_FALSE(ContainsText("/TestSources/EditorOnlyInnerObject.png"));
	EXPECT_FALSE(ContainsText("SchemaVersion"));

	Durin::TWeakObjectPtr<DImportMetadataForTest> WeakImportData = LoadedImportData;
	LoadedOwner->AssetImportData = nullptr;
	LoadedImportData = nullptr;
	Durin::CollectGarbage();
	EXPECT_FALSE(WeakImportData.IsValid());
	EXPECT_NE(LoadedOwner, nullptr);
	EXPECT_EQ(LoadedOwner->RuntimeValue, 91u);
	EXPECT_TRUE(Durin::GDObjectArray.GetObjectsWithOuter(
		LoadedOwner, Durin::EObjectQueryScope::LiveOnly).empty());
	ASSERT_TRUE(Durin::UnloadPackage(Path));
}

TEST(FPackageAssetTests, CoreRegisteredPackageIsResidentWithoutAssetLayerAdoption)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/CoreRegisteredPackage", Path));
	Durin::DPackage* Package = Durin::CreatePackage(Path);
	ASSERT_NE(Package, nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(Path), Package);
	EXPECT_FALSE(Package->IsNewlyCreated());
	ASSERT_TRUE(Durin::UnloadPackage(
		Package, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
}

TEST(FPackageAssetTests, SoftObjectResolveAndLoadPreservePathAcrossResidencyChanges)
{
	InitializeAssetTests();
	Durin::FPackagePath Path, AliasPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftObjectTarget", Path));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftObjectAlias", AliasPath));

	const uint64 CatalogRevisionBeforeDraft =
		Durin::GetAssetCatalogRevision();
	DPackageAssetForTest* Created = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Created));
	EXPECT_EQ(Durin::GetAssetCatalogRevision(), CatalogRevisionBeforeDraft);
	EXPECT_FALSE(Durin::FindAssetExact(Path));
	EXPECT_EQ(Durin::FindResidentPackage(Path), Created->GetPackage());
	EXPECT_TRUE(Created->GetPackage()->IsNewlyCreated());
	DPackageAssetForTest* DraftLoad = nullptr;
	const Durin::FAssetResult DraftLoadResult =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), DraftLoad);
	EXPECT_TRUE(DraftLoadResult);
	EXPECT_EQ(DraftLoad, Created);
	EXPECT_EQ(Durin::FindResidentPackage(Path), Created->GetPackage());
	Durin::TSoftObjectPtr<DPackageAssetForTest> UnpublishedReference(MakeFormerMainObjectPath(Path));
	const auto UnpublishedResolve =
		Durin::ResolveSoftObject(UnpublishedReference);
	EXPECT_TRUE(UnpublishedResolve);
	EXPECT_EQ(UnpublishedResolve.State, Durin::ESoftObjectResolveState::Loaded);
	EXPECT_EQ(UnpublishedResolve.Object, Created);
	ASSERT_TRUE(Durin::SavePackage(Created->GetPackage()));
	EXPECT_EQ(Durin::FindResidentPackage(Path), Created->GetPackage());
	EXPECT_FALSE(Created->GetPackage()->IsNewlyCreated());
	EXPECT_TRUE(Durin::FindAssetExact(Path));
	EXPECT_GT(Durin::GetAssetCatalogRevision(), CatalogRevisionBeforeDraft);
	Durin::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::CreateAssetRedirectorForTesting(AliasPath, Path, Redirector));
	ASSERT_TRUE(Durin::SavePackage(Redirector->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(AliasPath));

	Durin::TSoftObjectPtr<DPackageAssetForTest> Reference(MakeFormerMainObjectPath(AliasPath));
	auto Resolved = Durin::ResolveSoftObject(Reference);
	ASSERT_TRUE(Resolved);
	EXPECT_EQ(Resolved.State, Durin::ESoftObjectResolveState::Loaded);
	EXPECT_EQ(Resolved.Object, Created);
	EXPECT_TRUE(Resolved.bRedirected);
	EXPECT_EQ(Resolved.ResolvedPath, MakeFormerMainObjectPath(Path));
	EXPECT_EQ(Reference.Get(), Created);
	EXPECT_EQ(Reference.GetPath().GetPackagePath(), AliasPath);
	EXPECT_EQ(Durin::FindResidentPackage(AliasPath), nullptr);
	Durin::FObjectPath AliasChildPath;
	ASSERT_TRUE(Durin::FObjectPath::TryCreate(std::format(
		"{}.{}:DefaultChild", AliasPath.ToString(), AliasPath.GetPackageName()),
		AliasChildPath));
	Durin::TSoftObjectPtr<Durin::DObject> ChildReference(AliasChildPath);
	const auto ChildResolved = Durin::ResolveSoftObject(ChildReference);
	ASSERT_TRUE(ChildResolved);
	EXPECT_EQ(ChildResolved.State, Durin::ESoftObjectResolveState::Loaded);
	EXPECT_EQ(ChildResolved.Object, Created->DefaultChild.Get());
	EXPECT_EQ(ChildReference.GetPath(), AliasChildPath);
	EXPECT_EQ(ChildReference.Get(), Created->DefaultChild.Get());

	ASSERT_TRUE(Durin::UnloadPackage(Path));
	EXPECT_EQ(Reference.Get(), nullptr);
	EXPECT_EQ(ChildReference.Get(), nullptr);
	EXPECT_EQ(Reference.GetPath().GetPackagePath(), AliasPath);
	Resolved = Durin::ResolveSoftObject(Reference);
	ASSERT_TRUE(Resolved);
	EXPECT_EQ(Resolved.State, Durin::ESoftObjectResolveState::NotLoaded);
	EXPECT_EQ(Resolved.Object, nullptr);
	EXPECT_TRUE(Resolved.bRedirected);
	EXPECT_EQ(Resolved.ResolvedPath, MakeFormerMainObjectPath(Path));
	Durin::DObject* LoadedChild = nullptr;
	ASSERT_TRUE(Durin::LoadSoftObject(ChildReference, LoadedChild));
	ASSERT_NE(LoadedChild, nullptr);
	EXPECT_EQ(LoadedChild->GetFName(), Durin::FName("DefaultChild"));
	EXPECT_EQ(ChildReference.GetPath(), AliasChildPath);

	DPackageAssetForTest* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadSoftObject(Reference, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Reference.Get(), Loaded);
	EXPECT_EQ(Reference.GetPath().GetPackagePath(), AliasPath);
	EXPECT_EQ(Loaded->GetPackage()->GetPackagePath(), Path.ToString());
	Durin::FPackagePath OwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/SoftRedirectOwner", OwnerPath
	));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(AliasPath));
	DPackageAssetForTest* CachedForOwner = nullptr;
	ASSERT_TRUE(Durin::LoadSoftObject(Owner->Direct, CachedForOwner));
	EXPECT_EQ(CachedForOwner, Loaded);
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));

	Durin::TSoftObjectPtr<DCodecSourceAsset> WrongType(MakeFormerMainObjectPath(AliasPath));
	auto WrongResolve = Durin::ResolveSoftObject(WrongType);
	EXPECT_FALSE(WrongResolve);
	EXPECT_EQ(WrongResolve.Result.Error, Durin::EAssetError::TypeMismatch);
	DCodecSourceAsset* WrongLoaded = nullptr;
	EXPECT_EQ(
		Durin::LoadSoftObject(WrongType, WrongLoaded).Error,
		Durin::EAssetError::TypeMismatch
	);
	EXPECT_EQ(WrongLoaded, nullptr);
	EXPECT_EQ(WrongType.GetPath().GetPackagePath(), AliasPath);

	ASSERT_TRUE(Durin::UnloadPackage(Path));
	WrongResolve = Durin::ResolveSoftObject(WrongType);
	EXPECT_FALSE(WrongResolve);
	EXPECT_EQ(WrongResolve.Result.Error, Durin::EAssetError::TypeMismatch);
	EXPECT_EQ(Reference.Get(), nullptr);
	ASSERT_TRUE(Durin::LoadSoftObject(Reference, Loaded));
	EXPECT_EQ(Reference.Get(), Loaded);
	EXPECT_TRUE(Durin::UnloadPackage(Path));
	Owner = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(OwnerPath), Owner));
	ASSERT_NE(Owner, nullptr);
	EXPECT_EQ(Owner->Direct.GetPath().GetPackagePath(), AliasPath);
	EXPECT_FALSE(Owner->Direct.IsLoaded());
	EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::DeleteAssetForTesting(OwnerPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({AliasPath, Path}));
}

TEST(FPackageAssetTests, ResidentUnloadRequiresExplicitUnsavedDiscard)
{
	InitializeAssetTests();
	Durin::FPackagePath NewlyCreatedPath;
	Durin::FPackagePath PublishedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/ResidentNewlyCreated", NewlyCreatedPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/ResidentPublished", PublishedPath));

	DPackageAssetForTest* NewlyCreated = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(NewlyCreatedPath, NewlyCreated));
	EXPECT_EQ(
		Durin::UnloadPackage(NewlyCreatedPath).Error,
		Durin::EAssetError::InUse);
	NewlyCreated->GetPackage()->ClearDirty();
	EXPECT_EQ(
		Durin::UnloadPackage(NewlyCreatedPath).Error,
		Durin::EAssetError::InUse);
	EXPECT_EQ(
		Durin::FindResidentPackage(NewlyCreatedPath),
		NewlyCreated->GetPackage());
	ASSERT_TRUE(Durin::UnloadPackage(
		NewlyCreatedPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_EQ(Durin::FindResidentPackage(NewlyCreatedPath), nullptr);
	EXPECT_FALSE(Durin::FindAssetExact(NewlyCreatedPath));

	DPackageAssetForTest* Published = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(PublishedPath, Published));
	Published->Value = 17;
	ASSERT_TRUE(Durin::SavePackage(Published->GetPackage()));
	Published->Value = 29;
	Published->MarkPackageDirty();
	EXPECT_EQ(
		Durin::UnloadPackage(PublishedPath).Error,
		Durin::EAssetError::InUse);
	ASSERT_TRUE(Durin::UnloadPackage(
		PublishedPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Durin::FindAssetExact(PublishedPath));

	Published = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(PublishedPath), Published));
	ASSERT_NE(Published, nullptr);
	EXPECT_EQ(Published->Value, 17);
	ASSERT_TRUE(Durin::UnloadPackage(PublishedPath));
	ASSERT_TRUE(Durin::DeleteAssetForTesting(PublishedPath));
}

TEST(FPackageAssetTests, SoftObjectNullAndMissingPoliciesReturnStableResults)
{
	InitializeAssetTests();
	Durin::TSoftObjectPtr<DPackageAssetForTest> NullReference;
	auto RejectedNull = Durin::ResolveSoftObject(NullReference);
	EXPECT_FALSE(RejectedNull);
	EXPECT_EQ(RejectedNull.Result.Error, Durin::EAssetError::InvalidPath);
	EXPECT_EQ(RejectedNull.State, Durin::ESoftObjectResolveState::Null);

	auto AllowedNull = Durin::ResolveSoftObject(
		NullReference, Durin::ESoftObjectNullPolicy::Allow
	);
	EXPECT_TRUE(AllowedNull);
	EXPECT_EQ(AllowedNull.State, Durin::ESoftObjectResolveState::Null);
	DPackageAssetForTest* NullObject = reinterpret_cast<DPackageAssetForTest*>(1);
	EXPECT_TRUE(Durin::LoadSoftObject(
		NullReference, NullObject, Durin::ESoftObjectNullPolicy::Allow
	));
	EXPECT_EQ(NullObject, nullptr);

	Durin::FObjectPath MissingPath;
	ASSERT_TRUE(Durin::FObjectPath::TryCreate(
		"/TestAssets/MissingSoftObject.MissingSoftObject", MissingPath));
	Durin::TSoftObjectPtr<DPackageAssetForTest> MissingReference(MissingPath);
	const Durin::FObjectPath OriginalPath = MissingReference.GetPath();
	auto MissingResolve = Durin::ResolveSoftObject(MissingReference);
	ASSERT_FALSE(MissingResolve);
	EXPECT_EQ(MissingResolve.Result.Error, Durin::EAssetError::NotFound);
	EXPECT_EQ(MissingResolve.State, Durin::ESoftObjectResolveState::NotLoaded);

	DPackageAssetForTest* MissingObject = nullptr;
	EXPECT_EQ(
		Durin::LoadSoftObject(MissingReference, MissingObject).Error,
		Durin::EAssetError::NotFound
	);
	EXPECT_EQ(MissingObject, nullptr);
	EXPECT_EQ(MissingReference.GetPath(), OriginalPath);
	EXPECT_FALSE(MissingReference.IsLoaded());
}

TEST(FPackageAssetTests, SoftArchiveUsesBoundedPathOnlyPayloadsTransactionally)
{
	InitializeAssetTests();
	Durin::FPackagePath OwnerPath;
	Durin::FPackagePath TargetPath;
	Durin::FPackagePath SentinelPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftArchiveOwner", OwnerPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftArchiveTarget", TargetPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftArchiveSentinel", SentinelPath));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(TargetPath));
	auto* Property = static_cast<Durin::FSoftObjectProperty*>(
		DSoftPackageAssetForTest::StaticClass()->FindPropertyByName(
			Durin::FName("Direct"), false
		)
	);
	ASSERT_NE(Property, nullptr);

	Durin::FByteArray Bytes;
	Durin::FMemoryWriter Writer(Bytes);
	Durin::SerializeReflectedPropertyValue(Writer, *Property, Owner);
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
	ASSERT_EQ(Bytes.front(), std::byte{1});
	Owner->Direct.SetPath(MakeFormerMainObjectPath(SentinelPath));
	Durin::FMemoryReader Reader(Bytes);
	Durin::SerializeReflectedPropertyValue(Reader, *Property, Owner);
	ASSERT_FALSE(Reader.HasError()) << Reader.GetError();
	EXPECT_EQ(Owner->Direct.GetPath().GetPackagePath(), TargetPath);
	EXPECT_FALSE(Owner->Direct.IsLoaded());

	Durin::FByteArray OversizedBytes;
	Durin::FMemoryWriter OversizedWriter(OversizedBytes);
	uint8 ReferenceKind = 1;
	uint64 OversizedPathSize = 1024 * 1024 + 1;
	OversizedWriter << ReferenceKind << OversizedPathSize;
	Owner->Direct.SetPath(MakeFormerMainObjectPath(SentinelPath));
	Durin::FMemoryReader OversizedReader(OversizedBytes);
	Durin::SerializeReflectedPropertyValue(OversizedReader, *Property, Owner);
	ASSERT_TRUE(OversizedReader.HasError());
	EXPECT_NE(OversizedReader.GetError().find("1 MiB"), std::string_view::npos);
	EXPECT_EQ(Owner->Direct.GetPath().GetPackagePath(), SentinelPath);
	OversizedReader.SetError("must remain sticky");
	EXPECT_NE(OversizedReader.GetError().find("1 MiB"), std::string_view::npos);

	Durin::FByteArray NullBytes{std::byte{0}};
	Durin::FMemoryReader NullReader(NullBytes);
	Durin::SerializeReflectedPropertyValue(NullReader, *Property, Owner);
	ASSERT_FALSE(NullReader.HasError());
	EXPECT_TRUE(Owner->Direct.IsNull());
}

TEST(FPackageAssetTests, DastSoftFieldsRoundTripWithoutHardDependenciesOrTargetLoads)
{
	InitializeAssetTests();
	Durin::FPackagePath OwnerPath;
	Durin::FPackagePath TargetPath;
	Durin::FPackagePath MissingPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftStage3Owner", OwnerPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftStage3Target", TargetPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftStage3Missing", MissingPath));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(TargetPath, Target));
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	ASSERT_TRUE(Owner->Direct.TrySetObject(Target));
	Owner->Fixed[0].SetPath(MakeFormerMainObjectPath(MissingPath));
	Owner->Array = {
		DSoftPackageAssetForTest::FSoftReference(MakeFormerMainObjectPath(TargetPath)),
		DSoftPackageAssetForTest::FSoftReference(MakeFormerMainObjectPath(MissingPath))
	};
	Owner->Map.emplace(
		"hero", DSoftPackageAssetForTest::FSoftReference(MakeFormerMainObjectPath(TargetPath))
	);
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));

	const Durin::FAssetCatalogEntry OwnerData =
		Durin::FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	EXPECT_TRUE(OwnerData->Dependencies.empty());
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		OwnerData->PhysicalPath, Inspection
	));
	const Durin::FAssetPackageField* DirectField =
		Inspection.FindField("Direct");
	ASSERT_NE(DirectField, nullptr);
	EXPECT_EQ(
		DirectField->TypeSignature,
		"SoftObject:Tests::DPackageAssetForTest:v1"
	);
	const Durin::FReflectionSchemaCatalog Catalog =
		Durin::FReflectionSchemaCatalog::Capture();
	const auto* CatalogClass = Catalog.FindClass("Tests::DSoftPackageAssetForTest");
	ASSERT_NE(CatalogClass, nullptr);
	const auto* CatalogField = Catalog.FindField(
		*CatalogClass, "Tests::DSoftPackageAssetForTest", "Direct"
	);
	ASSERT_NE(CatalogField, nullptr);
	EXPECT_EQ(CatalogField->TypeSignature, DirectField->TypeSignature);
	ASSERT_GE(DirectField->Payload.size(), 1u + sizeof(uint64));
	EXPECT_EQ(DirectField->Payload.front(), std::byte{1});
	const Durin::FAssetPackageField* FixedField =
		Inspection.FindField("Fixed");
	ASSERT_NE(FixedField, nullptr);
	const size_t FirstFixedValueBytes =
		1 + sizeof(uint64) + MakeFormerMainObjectPath(MissingPath).ToString().size();
	ASSERT_EQ(FixedField->Payload.size(), FirstFixedValueBytes + 1);
	EXPECT_EQ(FixedField->Payload[FirstFixedValueBytes], std::byte{0});

	std::vector<Durin::FAssetReferenceEdge> Extracted;
	ASSERT_TRUE(Durin::ExtractAssetReferences(
		OwnerPath, Inspection, Extracted
	));
	ASSERT_EQ(Extracted.size(), 5u);
	EXPECT_EQ(std::ranges::count(Extracted, MakeFormerMainObjectPath(TargetPath),
		&Durin::FAssetReferenceEdge::TargetPath), 3);
	EXPECT_EQ(std::ranges::count(Extracted, MakeFormerMainObjectPath(MissingPath),
		&Durin::FAssetReferenceEdge::TargetPath), 2);
	EXPECT_TRUE(std::ranges::any_of(Extracted, [](const auto& Reference) {
		return Reference.DisplayRoute == "Fixed[fixed:0]"
			   && Reference.Route.size() == 1
			   && Reference.Route.front().Kind
					  == Durin::EAssetReferenceRouteKind::FixedArray;
	}));
	EXPECT_TRUE(std::ranges::any_of(Extracted, [](const auto& Reference) {
		return Reference.DisplayRoute.starts_with("Map[key:")
			   && Reference.Route.size() == 1
			   && Reference.Route.front().Kind
					  == Durin::EAssetReferenceRouteKind::MapValue;
	}));

	auto Referencers = Durin::CaptureAssetReferenceIndex().FindReferencers(TargetPath);
	ASSERT_EQ(Referencers.size(), 1u);
	EXPECT_EQ(Referencers.front().SourcePackage, OwnerPath);
	auto Targets = Durin::CaptureAssetReferenceIndex().FindTargets(OwnerPath);
	ASSERT_EQ(Targets.size(), 2u);
	EXPECT_EQ(Targets[0], MissingPath);
	EXPECT_EQ(Targets[1], TargetPath);

	// The loaded owner and a populated weak cache do not block target unload.
	Durin::FByteArray CachedBytes;
	ASSERT_TRUE(Durin::SerializeAssetPackageBytes(
		Owner->GetPackage(), CachedBytes
	));
	ASSERT_TRUE(Durin::UnloadPackage(TargetPath));
	EXPECT_EQ(Durin::FindResidentPackage(TargetPath), nullptr);
	Durin::FByteArray UnloadedBytes;
	ASSERT_TRUE(Durin::SerializeAssetPackageBytes(
		Owner->GetPackage(), UnloadedBytes
	));
	EXPECT_EQ(UnloadedBytes, CachedBytes);
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
	DSoftPackageAssetForTest* LoadedOwner = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(OwnerPath), LoadedOwner));
	ASSERT_NE(LoadedOwner, nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(TargetPath), nullptr);
	EXPECT_EQ(LoadedOwner->Direct.GetPath().GetPackagePath(), TargetPath);
	EXPECT_EQ(LoadedOwner->Fixed[0].GetPath().GetPackagePath(), MissingPath);
	EXPECT_EQ(LoadedOwner->Array[1].GetPath().GetPackagePath(), MissingPath);
	EXPECT_EQ(LoadedOwner->Map.at("hero").GetPath().GetPackagePath(), TargetPath);
	EXPECT_FALSE(LoadedOwner->Direct.IsLoaded());
	DPackageAssetForTest* Missing = nullptr;
	EXPECT_EQ(
		Durin::LoadSoftObject(LoadedOwner->Fixed[0], Missing).Error,
		Durin::EAssetError::NotFound
	);
	EXPECT_EQ(Missing, nullptr);
}

TEST(FPackageAssetTests, SoftInspectionRejectsMalformedPayloadsAndPreservesUnknownFields)
{
	InitializeAssetTests();
	Durin::FPackagePath OwnerPath;
	Durin::FPackagePath TargetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftInspectionOwner", OwnerPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftInspectionTarget", TargetPath));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(TargetPath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	const auto Data = Durin::FindAssetExact(OwnerPath);
	ASSERT_NE(Data, nullptr);
	Durin::FAssetPackageInspection Valid;
	ASSERT_TRUE(Durin::InspectAssetPackage(Data->PhysicalPath, Valid));

	auto FindMutableField = [](Durin::FAssetPackageInspection& Inspection,
							   std::string_view Name) -> Durin::FAssetPackageField* {
		for (auto& Object : Inspection.Objects)
			for (auto& Field : Object.Fields)
				if (Field.Name == Name) return &Field;
		return nullptr;
	};
	auto MakePathPayload = [](uint8 Kind, std::string_view Path) {
		Durin::FByteArray Payload{static_cast<std::byte>(Kind)};
		const uint64 Size = Path.size();
		const auto SizeBytes = std::as_bytes(std::span{&Size, 1});
		Payload.insert(Payload.end(), SizeBytes.begin(), SizeBytes.end());
		const auto PathBytes = std::as_bytes(std::span{Path});
		Payload.insert(Payload.end(), PathBytes.begin(), PathBytes.end());
		return Payload;
	};
	std::vector<Durin::FAssetReferenceEdge> References;

	auto Unknown = Valid;
	ASSERT_NE(FindMutableField(Unknown, "Direct"), nullptr);
	FindMutableField(Unknown, "Direct")->Name = "RetiredSoftField";
	EXPECT_EQ(Durin::ExtractAssetReferences(OwnerPath, Unknown, References).Error, Durin::EAssetError::TypeMismatch);

	auto WrongSignature = Valid;
	FindMutableField(WrongSignature, "Direct")->TypeSignature =
		"SoftObject:Tests::DCodecSourceAsset:v1";
	EXPECT_EQ(Durin::ExtractAssetReferences(OwnerPath, WrongSignature, References).Error, Durin::EAssetError::TypeMismatch);

	auto UnknownTag = Valid;
	FindMutableField(UnknownTag, "Direct")->Payload = {std::byte{2}};
	EXPECT_EQ(Durin::ExtractAssetReferences(OwnerPath, UnknownTag, References).Error, Durin::EAssetError::CorruptFile);

	auto Truncated = Valid;
	FindMutableField(Truncated, "Direct")->Payload = {std::byte{1}};
	EXPECT_EQ(Durin::ExtractAssetReferences(OwnerPath, Truncated, References).Error, Durin::EAssetError::CorruptFile);

	auto Overlong = Valid;
	FindMutableField(Overlong, "Direct")->Payload = {std::byte{1}};
	const uint64 OverlongSize = 1024 * 1024 + 1;
	const auto OverlongBytes = std::as_bytes(std::span{&OverlongSize, 1});
	FindMutableField(Overlong, "Direct")->Payload.insert(
		FindMutableField(Overlong, "Direct")->Payload.end(), OverlongBytes.begin(), OverlongBytes.end());
	EXPECT_EQ(Durin::ExtractAssetReferences(OwnerPath, Overlong, References).Error, Durin::EAssetError::CorruptFile);

	auto InvalidPath = Valid;
	FindMutableField(InvalidPath, "Direct")->Payload =
		MakePathPayload(1, "/TestAssets//Invalid");
	EXPECT_EQ(Durin::ExtractAssetReferences(OwnerPath, InvalidPath, References).Error, Durin::EAssetError::InvalidPath);

	auto TrailingNull = Valid;
	FindMutableField(TrailingNull, "Direct")->Payload = {std::byte{0}, std::byte{0}};
	EXPECT_EQ(Durin::ExtractAssetReferences(OwnerPath, TrailingNull, References).Error, Durin::EAssetError::CorruptFile);

	auto RuntimeMismatch = Valid;
	RuntimeMismatch.Header.AssetClassName = "Tests::DCodecSourceAsset";
	EXPECT_EQ(Durin::ExtractAssetReferences(OwnerPath, RuntimeMismatch, References).Error, Durin::EAssetError::TypeMismatch);

	Durin::FPackagePath OldPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftInspectionOld", OldPath));
	DPackageAssetForTest* OldAsset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, OldAsset));
	ASSERT_TRUE(Durin::SavePackage(OldAsset->GetPackage()));
	const auto OldData = Durin::FindAssetExact(OldPath);
	ASSERT_NE(OldData, nullptr);
	Durin::FAssetPackageInspection OldInspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		OldData->PhysicalPath, OldInspection
	));
	ASSERT_TRUE(Durin::ExtractAssetReferences(
		OldPath, OldInspection, References
	));
	EXPECT_TRUE(References.empty());

	Durin::FPackagePath OmittedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/SoftOmittedFields", OmittedPath
	));
	DSoftPackageAssetForTest* Omitted = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OmittedPath, Omitted));
	ASSERT_TRUE(Durin::SavePackage(Omitted->GetPackage()));
	Durin::FByteArray OmittedSoftBytes;
	Durin::FAssetPackageSerializationOptions OmitSoftFields;
	OmitSoftFields.PropertyFilter = [](const Durin::DObject*, const Durin::FProperty* Property) {
		return Property->NamePrivate.ToString() != "Direct"
			   && Property->NamePrivate.ToString() != "Fixed"
			   && Property->NamePrivate.ToString() != "Array"
			   && Property->NamePrivate.ToString() != "Map";
	};
	ASSERT_TRUE(Durin::SerializeAssetPackageBytes(
		Omitted->GetPackage(), OmittedSoftBytes, OmitSoftFields
	));
	ASSERT_TRUE(Durin::DeleteAssetForTesting(OmittedPath));
	WriteTestBytes(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "SoftOmittedFields.dasset",
		OmittedSoftBytes
	);
	EXPECT_EQ(
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(OmittedPath), Omitted).Error,
		Durin::EAssetError::NotFound);
	ASSERT_TRUE(Durin::AdmitAssetPackageToCatalog(OmittedPath));
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(OmittedPath), Omitted));
	ASSERT_NE(Omitted, nullptr);
	EXPECT_TRUE(Omitted->Direct.IsNull());
	EXPECT_TRUE(Omitted->Fixed[0].IsNull());
	EXPECT_TRUE(Omitted->Array.empty());
	EXPECT_TRUE(Omitted->Map.empty());
}

TEST(FPackageAssetTests, SoftCookReachabilityAddsSoftTargetsButRejectsMissingAndWrongTypes)
{
	InitializeAssetTests();
	Durin::FPackagePath TargetPath;
	Durin::FPackagePath OwnerPath;
	Durin::FPackagePath MissingPath;
	Durin::FPackagePath WrongTypePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftCookTarget", TargetPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftCookOwner", OwnerPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftCookMissing", MissingPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftCookWrongType", WrongTypePath));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(TargetPath, Target));
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(TargetPath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	std::vector<Durin::FPackagePath> Reachable;
	ASSERT_TRUE(Durin::BuildCookReachability(
		std::span<const Durin::FPackagePath>(&OwnerPath, 1), Reachable
	));
	EXPECT_EQ(Reachable, (std::vector<Durin::FPackagePath>{OwnerPath, TargetPath}));

	Owner->Direct.SetPath(MakeFormerMainObjectPath(MissingPath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	EXPECT_EQ(Durin::BuildCookReachability(std::span<const Durin::FPackagePath>(&OwnerPath, 1), Reachable).Error, Durin::EAssetError::MissingDependency);

	DMathStructAssetForTest* WrongType = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(WrongTypePath, WrongType));
	ASSERT_TRUE(Durin::SavePackage(WrongType->GetPackage()));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(WrongTypePath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	EXPECT_EQ(Durin::BuildCookReachability(std::span<const Durin::FPackagePath>(&OwnerPath, 1), Reachable).Error, Durin::EAssetError::TypeMismatch);

	Owner->Direct.SetPath(MakeFormerMainObjectPath(TargetPath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	DSoftPackageAssetForTest* CycleTarget = nullptr;
	Durin::FPackagePath CyclePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftCookCycle", CyclePath));
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(CyclePath, CycleTarget));
	CycleTarget->Direct.SetPath(MakeFormerMainObjectPath(OwnerPath));
	ASSERT_TRUE(Durin::SavePackage(CycleTarget->GetPackage()));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(CyclePath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::BuildCookReachability(
		std::span<const Durin::FPackagePath>(&OwnerPath, 1), Reachable
	));
	EXPECT_EQ(Reachable, (std::vector<Durin::FPackagePath>{CyclePath, OwnerPath}));
}

TEST(FPackageAssetTests, CookCanonicalizesRedirectedRootsReferencesAndPublishedBytes)
{
	InitializeAssetTests();
	Durin::FPackagePath OwnerPath;
	Durin::FPackagePath OldTargetPath;
	Durin::FPackagePath FinalTargetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/CookCanonicalOwner", OwnerPath
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/CookCanonicalTargetOld", OldTargetPath
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/CookCanonicalTargetFinal", FinalTargetPath
	));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldTargetPath, Target));
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->ExternalReference = Target;
	Owner->Direct.SetPath(MakeFormerMainObjectPath(OldTargetPath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));

	const Durin::FAssetCatalogEntry OwnerData =
		Durin::FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	const std::string OwnerPhysicalPath = OwnerData->PhysicalPath;
	Durin::FByteArray AuthoredBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AuthoredBytes, OwnerPhysicalPath
	));
	ASSERT_TRUE(RelocateAssetForTest(OldTargetPath, FinalTargetPath));

	FMemoryAssetReferenceStore RuntimeRoot(
		OldTargetPath,
		true,
		DPackageAssetForTest::StaticClass()->GetQualifiedName().ToString()
	);
	FScopedReferenceStoreRegistration RuntimeRootRegistration(&RuntimeRoot);
	std::vector<Durin::FPackagePath> Reachable;
	ASSERT_TRUE(Durin::BuildCookReachability(
		std::span{&OwnerPath, 1}, Reachable
	));
	std::vector ExpectedReachable{OwnerPath, FinalTargetPath};
	std::ranges::sort(ExpectedReachable, [](const Durin::FPackagePath& Left, const Durin::FPackagePath& Right) {
		return Left.GetView() < Right.GetView();
	});
	EXPECT_EQ(Reachable, ExpectedReachable);
	EXPECT_EQ(RuntimeRoot.Path, OldTargetPath);

	Durin::FByteArray CanonicalBytes;
	Durin::FByteArray CanonicalBulkBytes;
	const Durin::FAssetResult CanonicalResult =
		Durin::CanonicalizeAssetPackageForCook(
			AuthoredBytes, {}, OwnerPath, CanonicalBytes, CanonicalBulkBytes
		);
	ASSERT_TRUE(CanonicalResult) << CanonicalResult.Message;
	EXPECT_NE(CanonicalBytes, AuthoredBytes);
	const std::filesystem::path CanonicalFile = OwnerPhysicalPath;
	WriteTestBytes(CanonicalFile, CanonicalBytes);
	Durin::FAssetPackageInspection CanonicalInspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		CanonicalFile.generic_string(), CanonicalInspection
	));
	EXPECT_EQ(
		CanonicalInspection.Header.Dependencies,
		(std::vector<Durin::FPackagePath>{FinalTargetPath})
	);
	std::vector<Durin::FAssetReferenceEdge> CanonicalReferences;
	ASSERT_TRUE(Durin::ExtractAssetReferences(
		OwnerPath, CanonicalInspection, CanonicalReferences
	));
	ASSERT_EQ(CanonicalReferences.size(), 2u);
	EXPECT_EQ(std::ranges::count(CanonicalReferences, MakeTopLevelObjectPath(
		FinalTargetPath, OldTargetPath.GetPackageName()),
		&Durin::FAssetReferenceEdge::TargetPath), 1);
	EXPECT_EQ(std::ranges::count(CanonicalReferences,
		MakeFormerMainObjectPath(FinalTargetPath),
		&Durin::FAssetReferenceEdge::TargetPath), 1);
	WriteTestBytes(CanonicalFile, AuthoredBytes);

	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CanonicalCookOutput"
	);
	Durin::Testing::RemoveTestWorkDirectory(CookRoot);
	Durin::FCookContext Cook(
		CookRoot,
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game
	);
	ASSERT_TRUE(Cook.AddPackage(
		OwnerPath.ToString(), AuthoredBytes, {}
	));
	std::string CookError;
	ASSERT_TRUE(Cook.Publish(&CookError)) << CookError;
	Durin::FAssetPackageHeader PublishedHeader;
	ASSERT_TRUE(Durin::ReadAssetPackageHeader(
		(CookRoot / "TestAssets/CookCanonicalOwner.dasset").generic_string(),
		OwnerPath, PublishedHeader));
	EXPECT_EQ(
		PublishedHeader.Dependencies,
		(std::vector<Durin::FPackagePath>{FinalTargetPath})
	);

	Durin::FByteArray AuthoredAfterCook;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AuthoredAfterCook, OwnerPhysicalPath
	));
	EXPECT_EQ(AuthoredAfterCook, AuthoredBytes);

	const Durin::FAssetCatalogEntry AliasData =
		Durin::FindAssetExact(OldTargetPath);
	ASSERT_NE(AliasData, nullptr);
	Durin::FByteArray AliasBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AliasBytes, AliasData->PhysicalPath
	));

	const std::filesystem::path RedirectorRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "RedirectorCookOutput"
	);
	Durin::Testing::RemoveTestWorkDirectory(RedirectorRoot);
	Durin::FCookContext RedirectorCook(
		RedirectorRoot,
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game
	);
	ASSERT_TRUE(RedirectorCook.AddPackage(
		OldTargetPath.ToString(), AliasBytes, {}
	));
	EXPECT_FALSE(RedirectorCook.Publish(&CookError));
	EXPECT_NE(CookError.find("redirector packages are uncooked-only"), std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(
		RedirectorRoot / "CookManifest.bin"
	));
}

TEST(FPackageAssetTests, ReferenceIndexInvalidatesSourceSaveMoveAndDeleteMutations)
{
	InitializeAssetTests();
	Durin::FPackagePath TargetPath;
	Durin::FPackagePath SourcePath;
	Durin::FPackagePath MovedSourcePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftMutationTarget", TargetPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftMutationSource", SourcePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftMutationMoved", MovedSourcePath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(TargetPath, Target));
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Source = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SourcePath, Source));
	Source->Direct.SetPath(MakeFormerMainObjectPath(TargetPath));
	ASSERT_TRUE(Durin::SavePackage(Source->GetPackage()));
	EXPECT_EQ(Durin::CaptureAssetReferenceIndex().FindTargets(SourcePath), (std::vector<Durin::FPackagePath>{TargetPath}));

	Source->Direct.Reset();
	ASSERT_TRUE(Durin::SavePackage(Source->GetPackage()));
	EXPECT_TRUE(Durin::CaptureAssetReferenceIndex().FindTargets(SourcePath).empty());
	Source->Direct.SetPath(MakeFormerMainObjectPath(TargetPath));
	ASSERT_TRUE(Durin::SavePackage(Source->GetPackage()));
	ASSERT_TRUE(RelocateAssetForTest(SourcePath, MovedSourcePath));
	EXPECT_EQ(Durin::CaptureAssetReferenceIndex().FindTargets(SourcePath), (std::vector<Durin::FPackagePath>{MovedSourcePath}));
	EXPECT_EQ(Durin::CaptureAssetReferenceIndex().FindTargets(MovedSourcePath), (std::vector<Durin::FPackagePath>{TargetPath}));
	ASSERT_TRUE(DeleteAssetClosureForTest({SourcePath, MovedSourcePath}));
	EXPECT_TRUE(Durin::CaptureAssetReferenceIndex().FindTargets(MovedSourcePath).empty());
	EXPECT_TRUE(Durin::CaptureAssetReferenceIndex().FindReferencers(TargetPath).empty());
}

TEST(FPackageAssetTests, RelocationPreservesLoadedAndUnloadedSoftAuthoredPaths)
{
	InitializeAssetTests();
	Durin::FPackagePath OldPath;
	Durin::FPackagePath NewPath;
	Durin::FPackagePath LoadedOwnerPath;
	Durin::FPackagePath UnloadedOwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftMoveTarget", OldPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftMoveTargetRenamed", NewPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftMoveLoadedOwner", LoadedOwnerPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftMoveUnloadedOwner", UnloadedOwnerPath));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Target));
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* LoadedOwner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(LoadedOwnerPath, LoadedOwner));
	LoadedOwner->Direct.SetPath(MakeFormerMainObjectPath(OldPath));
	LoadedOwner->Fixed[0].SetPath(MakeFormerMainObjectPath(OldPath));
	LoadedOwner->Array.emplace_back(MakeFormerMainObjectPath(OldPath));
	LoadedOwner->Map.emplace("loaded", DSoftPackageAssetForTest::FSoftReference(MakeFormerMainObjectPath(OldPath)));
	ASSERT_TRUE(LoadedOwner->Direct.TrySetLoadedObject(Target));
	ASSERT_TRUE(Durin::SavePackage(LoadedOwner->GetPackage()));

	DSoftPackageAssetForTest* UnloadedOwner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(UnloadedOwnerPath, UnloadedOwner));
	UnloadedOwner->Direct.SetPath(MakeFormerMainObjectPath(OldPath));
	UnloadedOwner->Fixed[1].SetPath(MakeFormerMainObjectPath(OldPath));
	UnloadedOwner->Array.emplace_back(MakeFormerMainObjectPath(OldPath));
	UnloadedOwner->Map.emplace("unloaded", DSoftPackageAssetForTest::FSoftReference(MakeFormerMainObjectPath(OldPath)));
	ASSERT_TRUE(Durin::SavePackage(UnloadedOwner->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(UnloadedOwnerPath));
	const uint64 ConstructionsBeforeMove = GSoftPackageConstructionCount;

	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	EXPECT_EQ(GSoftPackageConstructionCount, ConstructionsBeforeMove);
	EXPECT_EQ(Durin::FindResidentPackage(UnloadedOwnerPath), nullptr);
	EXPECT_EQ(LoadedOwner->Direct.GetPath().GetPackagePath(), OldPath);
	EXPECT_EQ(LoadedOwner->Fixed[0].GetPath().GetPackagePath(), OldPath);
	ASSERT_EQ(LoadedOwner->Array.size(), 1u);
	EXPECT_EQ(LoadedOwner->Array[0].GetPath().GetPackagePath(), OldPath);
	EXPECT_EQ(LoadedOwner->Map.at("loaded").GetPath().GetPackagePath(), OldPath);
	EXPECT_FALSE(LoadedOwner->Direct.IsLoaded());

	auto LoadedTargets = Durin::CaptureAssetReferenceIndex().FindTargets(LoadedOwnerPath);
	auto UnloadedTargets = Durin::CaptureAssetReferenceIndex().FindTargets(UnloadedOwnerPath);
	EXPECT_EQ(LoadedTargets, (std::vector<Durin::FPackagePath>{OldPath}));
	EXPECT_EQ(UnloadedTargets, (std::vector<Durin::FPackagePath>{OldPath}));
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	EXPECT_EQ(Durin::CaptureAssetReferenceIndex().FindTargets(UnloadedOwnerPath), (std::vector<Durin::FPackagePath>{OldPath}));
	DSoftPackageAssetForTest* ReloadedOwner = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(UnloadedOwnerPath), ReloadedOwner));
	EXPECT_EQ(ReloadedOwner->Direct.GetPath().GetPackagePath(), OldPath);
	EXPECT_EQ(ReloadedOwner->Fixed[1].GetPath().GetPackagePath(), OldPath);
	ASSERT_EQ(ReloadedOwner->Array.size(), 1u);
	EXPECT_EQ(ReloadedOwner->Array[0].GetPath().GetPackagePath(), OldPath);
	EXPECT_EQ(ReloadedOwner->Map.at("unloaded").GetPath().GetPackagePath(), OldPath);
}

TEST(FPackageAssetTests, RelocationIgnoresStaleAndReadOnlyUnloadedSoftReferencers)
{
	InitializeAssetTests();
	auto RunCase = [](std::string_view Suffix, bool bMakeReadOnly) {
		Durin::FPackagePath OldPath;
		Durin::FPackagePath NewPath;
		Durin::FPackagePath OwnerPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			std::format("/TestAssets/SoftMove{}Target", Suffix), OldPath
		));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			std::format("/TestAssets/SoftMove{}TargetRenamed", Suffix), NewPath
		));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			std::format("/TestAssets/SoftMove{}Owner", Suffix), OwnerPath
		));
		DPackageAssetForTest* Target = nullptr;
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Target));
		ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
		DSoftPackageAssetForTest* Owner = nullptr;
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
		Owner->Direct.SetPath(MakeFormerMainObjectPath(OldPath));
		ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
		ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
		const std::filesystem::path OwnerFile =
			Durin::Testing::GetTestWorkDirectory() / "Assets"
			/ std::format("SoftMove{}Owner.dasset", Suffix);
		const std::filesystem::perms OriginalPermissions =
			std::filesystem::status(OwnerFile).permissions();
		std::error_code Ec;
		if (bMakeReadOnly)
			std::filesystem::permissions(
				OwnerFile,
				std::filesystem::perms::owner_read
					| std::filesystem::perms::group_read
					| std::filesystem::perms::others_read,
				std::filesystem::perm_options::replace,
				Ec
			);
		else
			std::filesystem::last_write_time(
				OwnerFile,
				std::filesystem::last_write_time(OwnerFile)
					+ std::chrono::seconds(2),
				Ec
			);
		ASSERT_FALSE(Ec);
		const Durin::FAssetResult Result =
			RelocateAssetForTest(OldPath, NewPath);
		if (bMakeReadOnly)
		{
			std::filesystem::permissions(
				OwnerFile, OriginalPermissions,
				std::filesystem::perm_options::replace, Ec
			);
			ASSERT_FALSE(Ec);
		}
		EXPECT_TRUE(Result) << Result.Message;
		ASSERT_NE(Durin::FindAssetExact(OldPath), nullptr);
		EXPECT_EQ(Durin::FindAssetExact(OldPath)->EntryKind, Durin::EAssetRegistryEntryKind::Redirector);
		EXPECT_NE(Durin::FindAssetExact(NewPath), nullptr);
		EXPECT_EQ(Durin::CaptureAssetReferenceIndex().FindTargets(OwnerPath), (std::vector<Durin::FPackagePath>{OldPath}));
	};
	RunCase("Stale", false);
	RunCase("ReadOnly", true);
}

TEST(FPackageAssetTests, RelocationDoesNotInspectUnrelatedReferencerBytes)
{
	InitializeAssetTests();
	Durin::FPackagePath OldPath;
	Durin::FPackagePath NewPath;
	Durin::FPackagePath OwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/DeferredFixupTarget", OldPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/DeferredFixupTargetMoved", NewPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/DeferredFixupOwner", OwnerPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Target));
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(OldPath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));

	const std::filesystem::path OwnerFile =
		Durin::Testing::GetTestWorkDirectory() / "Assets"
		/ "DeferredFixupOwner.dasset";
	Durin::FByteArray OwnerBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(OwnerBytes, OwnerFile));
	const std::array CorruptBytes{std::byte{0x7f}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(
		CorruptBytes, OwnerFile));

	const Durin::FAssetResult Result =
		RelocateAssetForTest(OldPath, NewPath);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(
		OwnerBytes, OwnerFile));
	EXPECT_TRUE(Result) << Result.Message;
	ASSERT_NE(Durin::FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Durin::FindAssetExact(OldPath)->EntryKind,
		Durin::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Durin::CaptureAssetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FPackagePath>{OldPath}));
}

TEST(FPackageAssetTests, RelocationPublicationFailureRestoresAuthoredState)
{
	InitializeAssetTests();
	Durin::FPackagePath OldPath;
	Durin::FPackagePath NewPath;
	Durin::FPackagePath OwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/RelocationRollbackTarget", OldPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/RelocationRollbackRenamed", NewPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/RelocationRollbackOwner", OwnerPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Target));
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(OldPath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));

	Durin::TSoftObjectPtr<DPackageAssetForTest> ExternalSetting(MakeFormerMainObjectPath(OldPath));
	Durin::FAssetRelocationSummary Summary;
	Durin::FAssetMutationJob Job;
	const Durin::FAssetRelocationMapping Mapping{OldPath, NewPath};
	ASSERT_TRUE(Durin::PrepareAssetRelocationJob(
		std::span{&Mapping, 1}, Summary, Job));
	Durin::SetAssetRelocationFailurePointForTesting(
		Durin::EAssetRelocationFailurePoint::PublishRedirector
	);
	const Durin::FAssetResult Result = Job.ResumeForward();
	EXPECT_EQ(Result.Error, Durin::EAssetError::IoError);
	EXPECT_EQ(Result.Disposition,
		Durin::EAssetResultDisposition::ForwardPending);
	EXPECT_FALSE(Result.OperationId.empty());
	EXPECT_EQ(Result.DesiredDirection, "Forward");
	EXPECT_FALSE(Result.RecoveryLocation.empty());
	EXPECT_EQ(ExternalSetting.GetPath().GetPackagePath(), OldPath);
	ASSERT_NE(Durin::FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Durin::FindAssetExact(OldPath)->EntryKind, Durin::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::FindAssetExact(NewPath), nullptr);
	EXPECT_EQ(Durin::CaptureAssetReferenceIndex().FindTargets(OwnerPath), (std::vector<Durin::FPackagePath>{OldPath}));
}

TEST(FPackageAssetTests, RelocationDoesNotPublishWhenJournalStateCannotPersist)
{
	InitializeAssetTests();
	Durin::FPackagePath SourcePath;
	Durin::FPackagePath DestinationPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/JournalFailureSource", SourcePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/JournalFailureDestination", DestinationPath));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SourcePath, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));

	const std::filesystem::path RecoveryRoot =
		Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / ".durin-asset-mutation";
	std::unordered_set<std::string> ExistingOperations;
	if (std::filesystem::is_directory(RecoveryRoot))
		for (const std::filesystem::directory_entry& Entry :
			std::filesystem::directory_iterator(RecoveryRoot))
			ExistingOperations.insert(Entry.path().filename().generic_string());

	const Durin::FAssetRelocationMapping Mapping{
		SourcePath, DestinationPath};
	Durin::FAssetRelocationSummary Summary;
	Durin::FAssetMutationJob Job;
	ASSERT_TRUE(Durin::PrepareAssetRelocationJob(
		std::span{&Mapping, 1}, Summary, Job));
	std::filesystem::path OperationRoot;
	for (const std::filesystem::directory_entry& Entry :
		std::filesystem::directory_iterator(RecoveryRoot))
		if (!ExistingOperations.contains(
				Entry.path().filename().generic_string()))
		{
			OperationRoot = Entry.path();
			break;
		}
	ASSERT_FALSE(OperationRoot.empty());
	std::error_code ErrorCode;
	ASSERT_TRUE(std::filesystem::remove(OperationRoot / "journal", ErrorCode));
	ASSERT_FALSE(ErrorCode);
	ASSERT_TRUE(std::filesystem::create_directory(
		OperationRoot / "journal", ErrorCode));
	ASSERT_FALSE(ErrorCode);

	const Durin::FAssetResult Result = Job.ResumeForward();
	EXPECT_EQ(Result.Error, Durin::EAssetError::IoError);
	EXPECT_NE(Result.Message.find("persist asset mutation journal"),
		std::string::npos);
	ASSERT_NE(Durin::FindAssetExact(SourcePath), nullptr);
	EXPECT_EQ(Durin::FindAssetExact(SourcePath)->EntryKind,
		Durin::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::FindAssetExact(DestinationPath), nullptr);
	EXPECT_EQ(Job.GetState(),
		Durin::EAssetMutationJobState::Prepared);

	ASSERT_TRUE(std::filesystem::remove(OperationRoot / "journal", ErrorCode));
	ASSERT_FALSE(ErrorCode);
	Job = {};
}

TEST(FPackageAssetTests, RelocationRecoveryReplaysAcrossRepeatedRestartInterruptions)
{
	InitializeAssetTests();
	Durin::FPackagePath SourcePath;
	Durin::FPackagePath DestinationPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RestartRelocationSource", SourcePath
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RestartRelocationDestination", DestinationPath
	));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SourcePath, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));

	const Durin::FAssetRelocationMapping Mapping{
		SourcePath, DestinationPath
	};
	Durin::FAssetRelocationSummary Summary;
	Durin::FAssetMutationJob Job;
	ASSERT_TRUE(Durin::PrepareAssetRelocationJob(
		std::span{&Mapping, 1}, Summary, Job
	));
	Durin::SetAssetRelocationFailurePointForTesting(
		Durin::EAssetRelocationFailurePoint::PublishRedirector
	);
	const Durin::FAssetResult Interrupted = Job.ResumeForward();
	ASSERT_EQ(Interrupted.Disposition, Durin::EAssetResultDisposition::ForwardPending);
	ASSERT_TRUE(std::filesystem::is_regular_file(
		Interrupted.RecoveryLocation
	));
	Job = {};
	ASSERT_TRUE(std::filesystem::is_regular_file(
		Interrupted.RecoveryLocation
	));

	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	Durin::SetAssetMutationRecoveryFailurePointForTesting(
		Durin::EAssetMutationRecoveryFailurePoint::AfterParticipantPublication
	);
	const Durin::FAssetResult FirstRestart = Durin::InitializeAssetManager();
	ASSERT_EQ(FirstRestart.Disposition, Durin::EAssetResultDisposition::ForwardPending)
		<< FirstRestart.Message;

	Durin::SetAssetMutationRecoveryFailurePointForTesting(
		Durin::EAssetMutationRecoveryFailurePoint::AfterProgressPersistence
	);
	const Durin::FAssetResult SecondRestart = Durin::InitializeAssetManager();
	ASSERT_EQ(SecondRestart.Disposition, Durin::EAssetResultDisposition::ForwardPending);

	ASSERT_TRUE(Durin::InitializeAssetManager());
	ASSERT_NE(Durin::FindAssetExact(SourcePath), nullptr);
	EXPECT_EQ(Durin::FindAssetExact(SourcePath)->EntryKind, Durin::EAssetRegistryEntryKind::Redirector);
	EXPECT_NE(Durin::FindAssetExact(DestinationPath), nullptr);
	ASSERT_TRUE(DeleteAssetClosureForTest({SourcePath, DestinationPath}));
}

TEST(FPackageAssetTests, FixupRecoveryReacquiresExternalProviderAcrossRepeatedInterruptions)
{
	InitializeAssetTests();
	Durin::FPackagePath SourcePath;
	Durin::FPackagePath DestinationPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RestartFixupSource", SourcePath
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RestartFixupDestination", DestinationPath
	));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SourcePath, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(RelocateAssetForTest(SourcePath, DestinationPath));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation
	));

	FMemoryAssetReferenceStore Store(SourcePath);
	FScopedReferenceStoreRegistration StoreRegistration(&Store);
	Durin::FAssetRedirectorFixupSummary Summary;
	Durin::FAssetMutationJob Job;
	ASSERT_TRUE(Durin::PrepareRedirectorFixupJob(
		std::span{&SourcePath, 1},
		Durin::EAssetRedirectorFixupMode::RewriteAndDelete,
		Summary, Job
	));
	Durin::SetAssetRedirectorFixupFailurePointForTesting(
		Durin::EAssetRedirectorFixupFailurePoint::ApplyStore
	);
	const Durin::FAssetResult Interrupted = Job.ResumeForward();
	ASSERT_EQ(Interrupted.Disposition, Durin::EAssetResultDisposition::ForwardPending);
	ASSERT_TRUE(std::filesystem::is_regular_file(
		Interrupted.RecoveryLocation
	));
	Job = {};
	ASSERT_TRUE(std::filesystem::is_regular_file(
		Interrupted.RecoveryLocation
	));

	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	Durin::SetAssetMutationRecoveryFailurePointForTesting(
		Durin::EAssetMutationRecoveryFailurePoint::AfterParticipantPublication
	);
	const Durin::FAssetResult FirstRestart = Durin::InitializeAssetManager();
	ASSERT_EQ(FirstRestart.Disposition, Durin::EAssetResultDisposition::ForwardPending)
		<< FirstRestart.Message;
	EXPECT_EQ(Store.Path, DestinationPath);

	Durin::SetAssetMutationRecoveryFailurePointForTesting(
		Durin::EAssetMutationRecoveryFailurePoint::AfterProgressPersistence
	);
	const Durin::FAssetResult SecondRestart = Durin::InitializeAssetManager();
	ASSERT_EQ(SecondRestart.Disposition, Durin::EAssetResultDisposition::ForwardPending);

	ASSERT_TRUE(Durin::InitializeAssetManager());
	EXPECT_EQ(Store.Path, DestinationPath);
	EXPECT_EQ(Durin::FindAssetExact(SourcePath), nullptr);
	EXPECT_NE(Durin::FindAssetExact(DestinationPath), nullptr);
	StoreRegistration.Reset();
	ASSERT_TRUE(DeleteAssetClosureForTest({DestinationPath}));
}

TEST(FPackageAssetTests, RelocationPreservesExternalAuthoredPathsAndRejectsRealCollision)
{
	InitializeAssetTests();
	Durin::FPackagePath OldPath;
	Durin::FPackagePath NewPath;
	Durin::FPackagePath CollisionPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftMoveExternalTarget", OldPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftMoveExternalTargetRenamed", NewPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftMoveExternalCollision", CollisionPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Target));
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
	DPackageAssetForTest* Collision = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(CollisionPath, Collision));
	ASSERT_TRUE(Durin::SavePackage(Collision->GetPackage()));
	EXPECT_EQ(RelocateAssetForTest(OldPath, CollisionPath).Error, Durin::EAssetError::AlreadyExists);

	Durin::TSoftObjectPtr<DPackageAssetForTest> ExternalSetting(MakeFormerMainObjectPath(OldPath));
	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	EXPECT_EQ(ExternalSetting.GetPath().GetPackagePath(), OldPath);
	ASSERT_NE(Durin::FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Durin::FindAssetExact(OldPath)->EntryKind, Durin::EAssetRegistryEntryKind::Redirector);
	EXPECT_NE(Durin::FindAssetExact(NewPath), nullptr);
}

TEST(FPackageAssetTests, RelocationRejectsReadOnlySourceWithoutStagingMutation)
{
	InitializeAssetTests();
	Durin::FPackagePath SourcePath;
	Durin::FPackagePath DestinationPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/ReadOnlyRelocationSource", SourcePath
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/ReadOnlyRelocationDestination", DestinationPath
	));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SourcePath, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	const std::filesystem::path SourceFile =
		Durin::FindAssetExact(SourcePath)->PhysicalPath;
	const std::filesystem::perms Original =
		std::filesystem::status(SourceFile).permissions();
	std::error_code ErrorCode;
	std::filesystem::permissions(
		SourceFile,
		std::filesystem::perms::owner_read
			| std::filesystem::perms::group_read
			| std::filesystem::perms::others_read,
		std::filesystem::perm_options::replace,
		ErrorCode
	);
	ASSERT_FALSE(ErrorCode);
	const Durin::FAssetResult Result =
		RelocateAssetForTest(SourcePath, DestinationPath);
	std::filesystem::permissions(
		SourceFile, Original,
		std::filesystem::perm_options::replace, ErrorCode
	);
	ASSERT_FALSE(ErrorCode);
	EXPECT_EQ(Result.Error, Durin::EAssetError::ReadOnlyMode);
	EXPECT_EQ(Durin::FindAssetExact(SourcePath)->EntryKind, Durin::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::FindAssetExact(DestinationPath), nullptr);
}

TEST(FPackageAssetTests, PreparedRelocationOwnsAndRemovesItsStagingRoot)
{
	InitializeAssetTests();
	Durin::FPackagePath SourcePath;
	Durin::FPackagePath DestinationPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/JournalCleanupSource", SourcePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/JournalCleanupDestination", DestinationPath));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SourcePath, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));

	const std::filesystem::path StagingRoot =
		Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / ".durin-asset-mutation";
	std::unordered_set<std::string> ExistingOperations;
	if (std::filesystem::is_directory(StagingRoot))
		for (const std::filesystem::directory_entry& Entry :
			std::filesystem::directory_iterator(StagingRoot))
			ExistingOperations.insert(
				Entry.path().filename().generic_string());

	{
		const Durin::FAssetRelocationMapping Mapping{
			SourcePath, DestinationPath};
		Durin::FAssetRelocationSummary Summary;
		Durin::FAssetMutationJob Job;
		ASSERT_TRUE(Durin::PrepareAssetRelocationJob(
			std::span{&Mapping, 1}, Summary, Job));

		std::filesystem::path OperationRoot;
		for (const std::filesystem::directory_entry& Entry :
			std::filesystem::directory_iterator(StagingRoot))
			if (!ExistingOperations.contains(
					Entry.path().filename().generic_string()))
			{
				ASSERT_TRUE(OperationRoot.empty());
				OperationRoot = Entry.path();
			}
		ASSERT_FALSE(OperationRoot.empty());
		const std::string OperationDirectory =
			OperationRoot.filename().generic_string();
		ASSERT_TRUE(OperationDirectory.starts_with("operation-"));
		Durin::FByteArray OwnerBytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
			OwnerBytes, OperationRoot / "owner"));
		const std::string Owner(
			reinterpret_cast<const char*>(OwnerBytes.data()),
			OwnerBytes.size());
		EXPECT_EQ(Owner, std::format(
			"durin-asset-mutation\n{}\n",
			OperationDirectory.substr(std::string_view("operation-").size())));
	}

	std::unordered_set<std::string> RemainingOperations;
	if (std::filesystem::is_directory(StagingRoot))
		for (const std::filesystem::directory_entry& Entry :
			std::filesystem::directory_iterator(StagingRoot))
			RemainingOperations.insert(
				Entry.path().filename().generic_string());
	EXPECT_EQ(RemainingOperations, ExistingOperations);
	ASSERT_TRUE(Durin::DeleteAssetForTesting(SourcePath));
}

TEST(FPackageAssetTests, RelocationFailureSeamsPreserveEveryOrdinaryBoundary)
{
	InitializeAssetTests();
	const std::array Points = {
		Durin::EAssetRelocationFailurePoint::StageOriginal,
		Durin::EAssetRelocationFailurePoint::PublishRealAsset,
		Durin::EAssetRelocationFailurePoint::PublishRedirector,
		Durin::EAssetRelocationFailurePoint::UpdateLoadedPackage,
		Durin::EAssetRelocationFailurePoint::PublishRegistry
	};
	for (size_t Index = 0; Index < Points.size(); ++Index)
	{
		Durin::FPackagePath SourcePath;
		Durin::FPackagePath DestinationPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(std::format("/TestAssets/FailureBoundary{}Source", Index), SourcePath));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(std::format("/TestAssets/FailureBoundary{}Destination", Index), DestinationPath));
		DPackageAssetForTest* Asset = nullptr;
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SourcePath, Asset));
		ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
		const Durin::FAssetRelocationMapping Mapping{
			SourcePath, DestinationPath
		};
		Durin::FAssetRelocationSummary Summary;
		Durin::FAssetMutationJob Job;
		ASSERT_TRUE(Durin::PrepareAssetRelocationJob(
			std::span{&Mapping, 1}, Summary, Job));
		Durin::SetAssetRelocationFailurePointForTesting(Points[Index]);
		EXPECT_EQ(Job.ResumeForward().Error, Durin::EAssetError::IoError);
		ASSERT_NE(Durin::FindAssetExact(SourcePath), nullptr);
		EXPECT_EQ(Durin::FindAssetExact(SourcePath)->EntryKind, Durin::EAssetRegistryEntryKind::Asset);
		EXPECT_EQ(Durin::FindAssetExact(DestinationPath), nullptr);
	}

	Durin::FPackagePath PrepareSource;
	Durin::FPackagePath PrepareDestination;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/FailurePrepareSource", PrepareSource
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/FailurePrepareDestination", PrepareDestination
	));
	DPackageAssetForTest* PrepareAsset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(PrepareSource, PrepareAsset));
	ASSERT_TRUE(Durin::SavePackage(PrepareAsset->GetPackage()));
	const Durin::FAssetRelocationMapping PrepareMapping{
		PrepareSource, PrepareDestination
	};
	Durin::SetAssetRelocationFailurePointForTesting(
		Durin::EAssetRelocationFailurePoint::PrepareOutput
	);
	Durin::FAssetRelocationSummary PrepareSummary;
	Durin::FAssetMutationJob PreparedJob;
	EXPECT_EQ(Durin::PrepareAssetRelocationJob(
		std::span{&PrepareMapping, 1}, PrepareSummary, PreparedJob).Error,
		Durin::EAssetError::IoError);
	EXPECT_EQ(Durin::FindAssetExact(PrepareSource)->EntryKind, Durin::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::FindAssetExact(PrepareDestination), nullptr);
}

TEST(FPackageAssetTests, RelocationFailureRetainsForwardProgressAndResumes)
{
	InitializeAssetTests();
	Durin::FPackagePath SourcePath;
	Durin::FPackagePath DestinationPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RecoverySource", SourcePath
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RecoveryDestination", DestinationPath
	));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SourcePath, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	const Durin::FAssetRelocationMapping Mapping{
		SourcePath, DestinationPath
	};
	Durin::FAssetRelocationSummary Summary;
	Durin::FAssetMutationJob Job;
	ASSERT_TRUE(Durin::PrepareAssetRelocationJob(
		std::span{&Mapping, 1}, Summary, Job));
	Durin::SetAssetRelocationFailurePointForTesting(
		Durin::EAssetRelocationFailurePoint::PublishRedirector
	);
	const Durin::FAssetResult Result = Job.ResumeForward();
	EXPECT_EQ(Result.Error, Durin::EAssetError::IoError);
	EXPECT_NE(Result.Message.find("AssetMutationForwardResumable"), std::string::npos);
	EXPECT_EQ(Job.GetState(),
		Durin::EAssetMutationJobState::Prepared);
	const Durin::FAssetMutationResultDetails Details =
		Job.GetLastResultDetails();
	EXPECT_TRUE(Details.bForwardResumable);
	EXPECT_FALSE(Details.bRecoveryRequired);
	const std::filesystem::path ContentRoot =
		Durin::Testing::GetTestWorkDirectory() / "Assets";
	const std::filesystem::path RecoveryRoot =
		ContentRoot / ".durin-asset-mutation";
	EXPECT_TRUE(std::filesystem::is_directory(RecoveryRoot));
	std::filesystem::path OperationRoot;
	for (const std::filesystem::directory_entry& Entry :
		 std::filesystem::directory_iterator(RecoveryRoot))
		if (Entry.is_directory())
		{
			OperationRoot = Entry.path();
			break;
		}
	ASSERT_FALSE(OperationRoot.empty());
	Durin::FByteArray JournalBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		JournalBytes, (OperationRoot / "journal")
	));
	const std::string Journal(
		reinterpret_cast<const char*>(JournalBytes.data()), JournalBytes.size()
	);
	EXPECT_NE(Journal.find("type=relocation"), std::string::npos);
	EXPECT_NE(Journal.find("original="), std::string::npos);
	EXPECT_NE(Journal.find("staged_pre="), std::string::npos);
	EXPECT_NE(Journal.find("pre_fingerprint="), std::string::npos);
	EXPECT_NE(Journal.find("completed=true"), std::string::npos);

	Durin::SetAssetRelocationFailurePointForTesting(
		Durin::EAssetRelocationFailurePoint::None);
	const Durin::FAssetResult Resumed = Job.ResumeForward();
	ASSERT_TRUE(Resumed) << Resumed.Message;
	EXPECT_EQ(Job.GetState(),
		Durin::EAssetMutationJobState::Completed);
	EXPECT_EQ(Durin::FindAssetExact(SourcePath)->EntryKind,
		Durin::EAssetRegistryEntryKind::Redirector);
	EXPECT_NE(Durin::FindAssetExact(DestinationPath), nullptr);
	Job = {};
	ASSERT_TRUE(DeleteAssetClosureForTest({SourcePath, DestinationPath}));
}

namespace
{
	auto RunRedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletionTest()
		-> void
	{
		InitializeAssetTests();
		Durin::FPackagePath OldPath;
		Durin::FPackagePath NewPath;
		Durin::FPackagePath OwnerPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/FixupOld", OldPath));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/FixupNew", NewPath));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/FixupOwner", OwnerPath));
		DPackageAssetForTest* Target = nullptr;
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Target));
		ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
		DSoftPackageAssetForTest* Owner = nullptr;
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
		Owner->Label = "unrelated-fixup-bytes";
		Owner->ExternalReference = Target;
		Owner->Direct.SetPath(MakeFormerMainObjectPath(OldPath));
		ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
		auto OwnerData = Durin::FindAssetExact(OwnerPath);
		ASSERT_NE(OwnerData, nullptr);
		Durin::FAssetPackageInspection BeforeInspection;
		ASSERT_TRUE(Durin::InspectAssetPackage(
			OwnerData->PhysicalPath, BeforeInspection
		));
		const auto* BeforeLabel = BeforeInspection.FindField("Label");
		ASSERT_NE(BeforeLabel, nullptr);
		const Durin::FByteArray UnrelatedBytes = BeforeLabel->Payload;
		ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
		ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));

		const auto Incoming = Durin::CaptureAssetReferenceIndex().FindReferencers(OldPath);
		ASSERT_EQ(Incoming.size(), 2u);
		EXPECT_TRUE(std::ranges::any_of(Incoming, [](const auto& Edge) {
			return Edge.Kind == Durin::EAssetReferenceKind::HardObject;
		}));
		EXPECT_TRUE(std::ranges::any_of(Incoming, [](const auto& Edge) {
			return Edge.Kind == Durin::EAssetReferenceKind::SoftObject;
		}));

		FMemoryAssetReferenceStore Store(OldPath);
		FScopedReferenceStoreRegistration StoreRegistration(&Store);
		Durin::FAssetRedirectorFixupSummary Summary;
		Durin::FAssetMutationJob Job;
		ASSERT_TRUE(Durin::PrepareRedirectorFixupJob(
			std::span{&OldPath, 1},
			Durin::EAssetRedirectorFixupMode::RewriteAndDelete,
			Summary,
			Job
		));
		ASSERT_EQ(Summary.GetRedirectors().size(), 1u);
		EXPECT_EQ(Summary.GetRedirectors().front(), OldPath);
		EXPECT_EQ(Summary.GetPackageOccurrences().size(), 2u);
		EXPECT_EQ(Summary.GetStoreOccurrences().size(), 1u);
		EXPECT_EQ(Summary.GetDeletableRedirectors().size(), 1u);
		const uint64 ConstructionCount = GSoftPackageConstructionCount;
		const Durin::FAssetResult Resumed = Job.ResumeForward();
		ASSERT_TRUE(Resumed) << Resumed.Message;
		const Durin::FAssetMutationResultDetails Details =
			Job.GetLastResultDetails();
		EXPECT_EQ(Details.DeletedPaths, std::vector{OldPath});
		EXPECT_EQ(Details.RewrittenPaths, std::vector{OwnerPath});
		EXPECT_EQ(GSoftPackageConstructionCount, ConstructionCount);
		EXPECT_EQ(Store.Path, NewPath);
		EXPECT_EQ(Durin::FindAssetExact(OldPath), nullptr);
		EXPECT_TRUE(Durin::CaptureAssetReferenceIndex().FindReferencers(OldPath).empty());
		EXPECT_TRUE(Durin::CaptureAssetReferenceIndex().IsComplete());

		OwnerData = Durin::FindAssetExact(OwnerPath);
		ASSERT_NE(OwnerData, nullptr);
		Durin::FAssetPackageInspection AfterInspection;
		ASSERT_TRUE(Durin::InspectAssetPackage(
			OwnerData->PhysicalPath, AfterInspection
		));
		const auto* AfterLabel = AfterInspection.FindField("Label");
		ASSERT_NE(AfterLabel, nullptr);
		EXPECT_EQ(AfterLabel->Payload, UnrelatedBytes);
		const auto NewIncoming = Durin::CaptureAssetReferenceIndex().FindReferencers(NewPath);
		EXPECT_GE(NewIncoming.size(), 2u);
		DSoftPackageAssetForTest* ReloadedOwner = nullptr;
		const Durin::FAssetResult ReloadResult =
			Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(OwnerPath), ReloadedOwner);
		ASSERT_TRUE(ReloadResult) << ReloadResult.Message;
		EXPECT_EQ(ReloadedOwner->Direct.GetPath().GetPackagePath(), NewPath);
		EXPECT_EQ(ReloadedOwner->ExternalReference.Get(), Target);
	}

	auto RunRedirectorFixupVerificationFailureResumesRemainingParticipantsTest()
		-> void
	{
		InitializeAssetTests();
		Durin::FPackagePath OldPath;
		Durin::FPackagePath NewPath;
		Durin::FPackagePath OwnerPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/FixupFailureOld", OldPath));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/FixupFailureNew", NewPath));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/FixupFailureOwner", OwnerPath));
		DPackageAssetForTest* Target = nullptr;
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Target));
		ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
		DSoftPackageAssetForTest* Owner = nullptr;
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
		Owner->ExternalReference = Target;
		Owner->Direct.SetPath(MakeFormerMainObjectPath(OldPath));
		ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
		const std::string OwnerFile = Durin::FindAssetExact(OwnerPath)
										  ->PhysicalPath;
		ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
		ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
		ASSERT_TRUE(Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation));
		Durin::FByteArray BeforeBytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeBytes, OwnerFile));

		FMemoryAssetReferenceStore Store(OldPath);
		FScopedReferenceStoreRegistration StoreRegistration(&Store);
		Durin::SetAssetRedirectorFixupFailurePointForTesting(
			Durin::EAssetRedirectorFixupFailurePoint::Verify
		);
		Durin::FAssetRedirectorFixupSummary Summary;
		Durin::FAssetMutationJob Job;
		ASSERT_TRUE(Durin::PrepareRedirectorFixupJob(
			std::span{&OldPath, 1},
			Durin::EAssetRedirectorFixupMode::RewriteAndDelete,
			Summary, Job));
		const Durin::FAssetResult FixupResult = Job.ResumeForward();
		EXPECT_EQ(FixupResult.Error, Durin::EAssetError::IoError)
			<< FixupResult.Message;
		Durin::SetAssetRedirectorFixupFailurePointForTesting(
			Durin::EAssetRedirectorFixupFailurePoint::None
		);
		const Durin::FAssetResult Resumed = Job.ResumeForward();
		ASSERT_TRUE(Resumed) << Resumed.Message;
		Durin::FByteArray AfterBytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterBytes, OwnerFile));
		EXPECT_NE(AfterBytes, BeforeBytes);
		EXPECT_EQ(Store.Path, NewPath);
		EXPECT_EQ(Durin::FindAssetExact(OldPath), nullptr);
		EXPECT_TRUE(Durin::CaptureAssetReferenceIndex().FindReferencers(OldPath).empty());
		StoreRegistration.Reset();
		ASSERT_TRUE(Durin::DeleteAssetForTesting(OwnerPath));
		ASSERT_TRUE(DeleteAssetClosureForTest({NewPath}));
	}
} // namespace

namespace
{
	auto RunRedirectorFixupRejectsUnavailableProviderWithoutMutationTest() -> void
	{
		InitializeAssetTests();
		Durin::FPackagePath OldPath;
		Durin::FPackagePath NewPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			"/TestAssets/FixupProviderGoneOld", OldPath
		));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			"/TestAssets/FixupProviderGoneNew", NewPath
		));
		DPackageAssetForTest* Target = nullptr;
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Target));
		ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
		ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
		ASSERT_TRUE(Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation
		));
		FMemoryAssetReferenceStore Store(OldPath);
		const Durin::FAssetReferenceStoreHandle Handle =
			Durin::RegisterAssetReferenceStore(&Store);
		Durin::FAssetRedirectorFixupSummary Summary;
		Durin::FAssetMutationJob Job;
		ASSERT_TRUE(Durin::PrepareRedirectorFixupJob(
			std::span{&OldPath, 1},
			Durin::EAssetRedirectorFixupMode::RewriteAndDelete,
			Summary,
			Job
		));
		Durin::UnregisterAssetReferenceStore(Handle);
		const Durin::FAssetResult Result = Job.ResumeForward();
		EXPECT_EQ(Result.Error, Durin::EAssetError::StaleData);
		EXPECT_EQ(Job.GetLastResultDetails().FailedPaths,
			std::vector{OldPath});
		EXPECT_EQ(Store.Path, OldPath);
		const auto Alias = Durin::FindAssetExact(OldPath);
		ASSERT_NE(Alias, nullptr);
		EXPECT_EQ(Alias->EntryKind, Durin::EAssetRegistryEntryKind::Redirector);
		ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
	}

	auto RunRedirectorFixupRejectsReadOnlyAndChangedPackageInputsTest() -> void
	{
		InitializeAssetTests();
		auto RunCase = [](std::string_view Suffix, bool bReadOnly) {
			Durin::FPackagePath OldPath;
			Durin::FPackagePath NewPath;
			Durin::FPackagePath OwnerPath;
			ASSERT_TRUE(Durin::FPackagePath::TryCreate(
				std::format("/TestAssets/Fixup{}Old", Suffix), OldPath
			));
			ASSERT_TRUE(Durin::FPackagePath::TryCreate(
				std::format("/TestAssets/Fixup{}New", Suffix), NewPath
			));
			ASSERT_TRUE(Durin::FPackagePath::TryCreate(
				std::format("/TestAssets/Fixup{}Owner", Suffix), OwnerPath
			));
			DPackageAssetForTest* Target = nullptr;
			ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Target));
			ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
			DSoftPackageAssetForTest* Owner = nullptr;
			ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
			Owner->Direct.SetPath(MakeFormerMainObjectPath(OldPath));
			ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
			const std::string OwnerFile = Durin::FindAssetExact(OwnerPath)
											  ->PhysicalPath;
			ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
			ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
			ASSERT_TRUE(Durin::RefreshAssetRegistry(
				Durin::EAssetRegistryScanMode::FullValidation
			));
			Durin::FByteArray PreBytes;
			ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(PreBytes, OwnerFile));
			std::filesystem::perms OriginalPermissions =
				std::filesystem::status(OwnerFile).permissions();
			Durin::FAssetRedirectorFixupSummary Summary;
			Durin::FAssetMutationJob Job;
			Durin::FAssetResult Result;
			if (bReadOnly)
			{
				std::error_code PermissionError;
				std::filesystem::permissions(
					OwnerFile,
					std::filesystem::perms::owner_write
						| std::filesystem::perms::group_write
						| std::filesystem::perms::others_write,
					std::filesystem::perm_options::remove,
					PermissionError
				);
				ASSERT_FALSE(PermissionError) << PermissionError.message();
				Result = Durin::PrepareRedirectorFixupJob(
					std::span{&OldPath, 1},
					Durin::EAssetRedirectorFixupMode::RewriteAndDelete,
					Summary,
					Job
				);
				std::filesystem::permissions(
					OwnerFile, OriginalPermissions,
					std::filesystem::perm_options::replace,
					PermissionError
				);
				ASSERT_FALSE(PermissionError) << PermissionError.message();
				EXPECT_EQ(Result.Error, Durin::EAssetError::ReadOnlyMode)
					<< Result.Message;
			}
			else
			{
				ASSERT_TRUE(Durin::PrepareRedirectorFixupJob(
					std::span{&OldPath, 1},
					Durin::EAssetRedirectorFixupMode::RewriteAndDelete,
					Summary,
					Job
				));
				Durin::FByteArray ChangedBytes = PreBytes;
				ASSERT_TRUE(RenameSerializedString(
					ChangedBytes, "Label", "Ghost"
				));
				WriteTestBytes(OwnerFile, ChangedBytes);
				Result = Job.ResumeForward();
				EXPECT_EQ(Result.Error, Durin::EAssetError::StaleData)
					<< Result.Message;
				WriteTestBytes(OwnerFile, PreBytes);
			}
			const auto Alias = Durin::FindAssetExact(OldPath);
			ASSERT_NE(Alias, nullptr);
			EXPECT_EQ(Alias->EntryKind, Durin::EAssetRegistryEntryKind::Redirector);
			ASSERT_TRUE(Durin::DeleteAssetForTesting(OwnerPath));
			ASSERT_TRUE(DeleteAssetClosureForTest({NewPath}));
		};
		RunCase("ReadOnly", true);
		RunCase("Changed", false);
	}

	auto RunRedirectorFixupPublicationFailuresResumeForwardTest() -> void
	{
		InitializeAssetTests();
		constexpr std::array FailurePoints{
			Durin::EAssetRedirectorFixupFailurePoint::PublishPackage,
			Durin::EAssetRedirectorFixupFailurePoint::ApplyStore,
			Durin::EAssetRedirectorFixupFailurePoint::DeleteRedirector,
			Durin::EAssetRedirectorFixupFailurePoint::PublishRegistry
		};
		for (size_t Index = 0; Index < FailurePoints.size(); ++Index)
		{
			Durin::FPackagePath OldPath;
			Durin::FPackagePath NewPath;
			Durin::FPackagePath OwnerPath;
			ASSERT_TRUE(Durin::FPackagePath::TryCreate(
				std::format("/TestAssets/FixupMatrix{}Old", Index), OldPath
			));
			ASSERT_TRUE(Durin::FPackagePath::TryCreate(
				std::format("/TestAssets/FixupMatrix{}New", Index), NewPath
			));
			ASSERT_TRUE(Durin::FPackagePath::TryCreate(
				std::format("/TestAssets/FixupMatrix{}Owner", Index), OwnerPath
			));
			DPackageAssetForTest* Target = nullptr;
			ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Target));
			ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
			DSoftPackageAssetForTest* Owner = nullptr;
			ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
			Owner->Direct.SetPath(MakeFormerMainObjectPath(OldPath));
			ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
			const std::string OwnerFile = Durin::FindAssetExact(OwnerPath)
											  ->PhysicalPath;
			ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
			Durin::FByteArray BeforeBytes;
			ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeBytes, OwnerFile));
			ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
			ASSERT_TRUE(Durin::RefreshAssetRegistry(
				Durin::EAssetRegistryScanMode::FullValidation
			));
			FMemoryAssetReferenceStore Store(OldPath);
			FScopedReferenceStoreRegistration StoreRegistration(&Store);
			Durin::SetAssetRedirectorFixupFailurePointForTesting(
				FailurePoints[Index]
			);
			Durin::FAssetRedirectorFixupSummary Summary;
			Durin::FAssetMutationJob Job;
			ASSERT_TRUE(Durin::PrepareRedirectorFixupJob(
				std::span{&OldPath, 1},
				Durin::EAssetRedirectorFixupMode::RewriteAndDelete,
				Summary, Job));
			const Durin::FAssetResult Result = Job.ResumeForward();
			Durin::SetAssetRedirectorFixupFailurePointForTesting(
				Durin::EAssetRedirectorFixupFailurePoint::None
			);
			EXPECT_EQ(Result.Error, Durin::EAssetError::IoError)
				<< Result.Message;
			const Durin::FAssetResult Resumed = Job.ResumeForward();
			ASSERT_TRUE(Resumed) << Resumed.Message;
			Durin::FByteArray AfterBytes;
			ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterBytes, OwnerFile));
			EXPECT_NE(AfterBytes, BeforeBytes);
			EXPECT_EQ(Store.Path, NewPath);
			EXPECT_EQ(Durin::FindAssetExact(OldPath), nullptr);
			StoreRegistration.Reset();
			ASSERT_TRUE(Durin::DeleteAssetForTesting(OwnerPath));
			ASSERT_TRUE(DeleteAssetClosureForTest({NewPath}));
		}
	}
} // namespace

TEST(FPackageAssetTests, SoftReferencedTargetDeletionLeavesDanglingPathWithoutBlockingDeletion)
{
	InitializeAssetTests();
	Durin::FPackagePath TargetPath;
	Durin::FPackagePath OwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftDeleteDanglingTarget", TargetPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftDeleteDanglingOwner", OwnerPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(TargetPath, Target));
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(TargetPath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));

	Durin::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::AnalyzeAssetDeletion(TargetPath, Analysis));
	EXPECT_TRUE(Analysis.DirectReferencers.empty());
	EXPECT_TRUE(Analysis.CanDelete());
	ASSERT_TRUE(Durin::DeleteAssetForTesting(TargetPath));
	EXPECT_EQ(Durin::FindAssetExact(TargetPath), nullptr);
	auto Referencers =
		Durin::CaptureAssetReferenceIndex().FindReferencers(TargetPath);
	ASSERT_EQ(Referencers.size(), 1u);
	EXPECT_EQ(Referencers.front().SourcePackage, OwnerPath);

	std::vector<Durin::FPackagePath> Reachable;
	EXPECT_EQ(Durin::BuildCookReachability(std::span<const Durin::FPackagePath>(&OwnerPath, 1), Reachable).Error, Durin::EAssetError::MissingDependency);
}

TEST(FPackageAssetTests, DastMapBytesAreCanonicalAcrossInsertionAndBucketHistory)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/MapOrderingBaseline", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	const std::array<std::pair<std::string, int32>, 8> Entries = {{{"alpha", 1}, {"bravo", 2}, {"charlie", 3}, {"delta", 4}, {"echo", 5}, {"foxtrot", 6}, {"golf", 7}, {"hotel", 8}}};

	Asset->NamedScores.clear();
	Asset->NamedScores.rehash(37);
	for (const auto& [Key, Value] : Entries)
		Asset->NamedScores.emplace(Key, Value);
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	const auto File = Durin::Testing::GetTestWorkDirectory()
					  / "Assets" / "MapOrderingBaseline.dasset";
	Durin::FByteArray ForwardBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(ForwardBytes, File));

	Asset->NamedScores.clear();
	Asset->NamedScores.rehash(2);
	for (auto It = Entries.rbegin(); It != Entries.rend(); ++It)
		Asset->NamedScores.emplace(It->first, It->second);
	Asset->GetPackage()->MarkDirty();
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	Durin::FByteArray ReverseBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(ReverseBytes, File));

	EXPECT_EQ(ForwardBytes, ReverseBytes);
	EXPECT_TRUE(Durin::UnloadPackage(Path));
}

TEST(FPackageAssetTests, MathStructRegistrationPreservesDirectAndNestedSchemaIdentity)
{
	InitializeAssetTests();
	Durin::DClass* Class = DMathStructAssetForTest::StaticClass();
	Durin::DStruct* VectorStruct = Durin::Z_Construct_DStruct_FVector3();
	Durin::DStruct* TransformStruct = Durin::Z_Construct_DStruct_FTransform();
	Durin::DStruct* FloatQuatStruct = Durin::Z_Construct_DStruct_FQuatf();
	Durin::DStruct* FloatMatrixStruct = Durin::Z_Construct_DStruct_FMatrix4f();
	ASSERT_NE(Class, nullptr);
	ASSERT_NE(VectorStruct, nullptr);
	ASSERT_NE(TransformStruct, nullptr);
	ASSERT_NE(FloatQuatStruct, nullptr);
	ASSERT_NE(FloatMatrixStruct, nullptr);
	auto* Vector = static_cast<Durin::FStructProperty*>(Class->FindPropertyByName("Vector", false));
	auto* Transform = static_cast<Durin::FStructProperty*>(Class->FindPropertyByName("Transform", false));
	auto* FloatQuat = static_cast<Durin::FStructProperty*>(Class->FindPropertyByName("FloatQuat", false));
	auto* FloatMatrix = static_cast<Durin::FStructProperty*>(Class->FindPropertyByName("FloatMatrix", false));
	auto* Vectors = static_cast<Durin::FArrayProperty*>(Class->FindPropertyByName("Vectors", false));
	auto* VectorMap = static_cast<Durin::FMapProperty*>(Class->FindPropertyByName("VectorMap", false));
	ASSERT_NE(Vector, nullptr);
	ASSERT_NE(Transform, nullptr);
	ASSERT_NE(FloatQuat, nullptr);
	ASSERT_NE(FloatMatrix, nullptr);
	ASSERT_NE(Vectors, nullptr);
	ASSERT_NE(VectorMap, nullptr);
	ASSERT_EQ(Class->ChildProperties, Vector);
	EXPECT_EQ(Vector->Next, Transform);
	EXPECT_EQ(Transform->Next, FloatQuat);
	EXPECT_EQ(FloatQuat->Next, FloatMatrix);
	EXPECT_EQ(FloatMatrix->Next, Vectors);
	EXPECT_EQ(Vectors->Next, VectorMap);
	EXPECT_EQ(VectorMap->Next, nullptr);

	EXPECT_EQ(Vector->GetStruct(), VectorStruct);
	EXPECT_EQ(Transform->GetStruct(), TransformStruct);
	EXPECT_EQ(FloatQuat->GetStruct(), FloatQuatStruct);
	EXPECT_EQ(FloatMatrix->GetStruct(), FloatMatrixStruct);
	EXPECT_EQ(Vector->GetPropertyFlags(), Durin::EPropertyFlags::None);
	EXPECT_EQ(Transform->GetPropertyFlags(), Durin::EPropertyFlags::None);
	EXPECT_EQ(Vector->GetArrayDim(), 1);
	EXPECT_EQ(Transform->GetArrayDim(), 1);
	EXPECT_EQ(Vector->GetOffset(), STRUCT_OFFSET_UINT16(DMathStructAssetForTest, Vector));
	EXPECT_EQ(Transform->GetOffset(), STRUCT_OFFSET_UINT16(DMathStructAssetForTest, Transform));
	EXPECT_EQ(Vector->GetElementSize(), VectorStruct->PropertiesSize);
	EXPECT_EQ(Transform->GetElementSize(), TransformStruct->PropertiesSize);
	EXPECT_EQ(Vector->GetValueAlignment(), VectorStruct->MinAlignment);
	EXPECT_EQ(Transform->GetValueAlignment(), TransformStruct->MinAlignment);

	ASSERT_NE(Vectors->GetInner(), nullptr);
	auto* VectorsInner = static_cast<Durin::FStructProperty*>(Vectors->GetInner());
	EXPECT_EQ(VectorsInner->NamePrivate.ToString(), "Vectors_Inner");
	EXPECT_EQ(VectorsInner->GetStruct(), VectorStruct);
	EXPECT_EQ(VectorsInner->GetOwnerProperty(), Vectors);
	EXPECT_EQ(VectorsInner->GetOffset(), 0);
	EXPECT_EQ(VectorsInner->GetArrayDim(), 1);
	ASSERT_NE(VectorMap->GetKeyProp(), nullptr);
	ASSERT_NE(VectorMap->GetValueProp(), nullptr);
	EXPECT_EQ(VectorMap->GetKeyProp()->NamePrivate.ToString(), "VectorMap_Key");
	EXPECT_EQ(VectorMap->GetKeyProp()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::String);
	auto* VectorMapValue = static_cast<Durin::FStructProperty*>(VectorMap->GetValueProp());
	EXPECT_EQ(VectorMapValue->NamePrivate.ToString(), "VectorMap_Value");
	EXPECT_EQ(VectorMapValue->GetStruct(), VectorStruct);
	EXPECT_EQ(VectorMapValue->GetOwnerProperty(), VectorMap);
	EXPECT_EQ(VectorMapValue->GetOffset(), 0);
	EXPECT_EQ(VectorMapValue->GetArrayDim(), 1);
	EXPECT_TRUE(VectorStruct->HasCompleteAuthoredFields());
	EXPECT_TRUE(TransformStruct->HasCompleteAuthoredFields());
	EXPECT_TRUE(FloatQuatStruct->HasCompleteAuthoredFields());
	EXPECT_TRUE(FloatMatrixStruct->HasCompleteAuthoredFields());
}

TEST(FPackageAssetTests, PrecisionSpecificMathStructsRoundTripThroughPackageLinker)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/PrecisionMath", Path));
	DMathStructAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	Asset->FloatQuat = Durin::FQuatf(0.5f, 0.25f, -0.5f, 0.75f);
	Asset->FloatMatrix = Durin::FMatrix4f(0.0f);
	Asset->FloatMatrix[0] = Durin::FVector4f(1.0f, 2.0f, 3.0f, 4.0f);
	Asset->FloatMatrix[1] = Durin::FVector4f(5.0f, 6.0f, 7.0f, 8.0f);
	Asset->FloatMatrix[2] = Durin::FVector4f(9.0f, 10.0f, 11.0f, 12.0f);
	Asset->FloatMatrix[3] = Durin::FVector4f(13.0f, 14.0f, 15.0f, 16.0f);
	const Durin::FMatrix4f ExpectedMatrix = Asset->FloatMatrix;
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(Path));

	DMathStructAssetForTest* Loaded = nullptr;
	const Durin::FAssetResult LoadResult = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded);
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->FloatQuat, Durin::FQuatf(0.5f, 0.25f, -0.5f, 0.75f));
	EXPECT_EQ(Loaded->FloatMatrix, ExpectedMatrix);
	EXPECT_TRUE(Durin::UnloadPackage(Path));
}

TEST(FPackageAssetTests, WriterUsesVersionedWireSignaturesForLogicalEncodings)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/WireSignatures", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "WireSignatures.dasset";
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(File.generic_string(), Inspection));
	ASSERT_FALSE(Inspection.Objects.empty());
	const Durin::FAssetPackageObjectInspection& Object = Inspection.Objects.front();
	ASSERT_NE(Object.FindField("Label"), nullptr);
	ASSERT_NE(Object.FindField("DisplayName"), nullptr);
	ASSERT_NE(Object.FindField("PersistentId"), nullptr);
	ASSERT_NE(Object.FindField("RelatedIds"), nullptr);
	ASSERT_NE(Object.FindField("NamedScores"), nullptr);
	EXPECT_EQ(Object.FindField("Label")->TypeSignature, "12:v1");
	EXPECT_EQ(Object.FindField("DisplayName")->TypeSignature, "18:v1");
	EXPECT_EQ(Object.FindField("PersistentId")->TypeSignature, "19:v1");
	EXPECT_EQ(Object.FindField("RelatedIds")->TypeSignature, "Array<19:v1>");
	EXPECT_EQ(Object.FindField("NamedScores")->TypeSignature, "Map<12:v1,4:4>");
}

TEST(FPackageAssetTests, CompleteInspectionContainsEveryObjectAndContentFingerprint)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/CompleteInspection", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	Asset->Value = 17;
	Asset->DefaultChild->Rename("InspectedChild");
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));

	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "CompleteInspection.dasset";
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(File.generic_string(), Inspection));
	ASSERT_EQ(Inspection.Objects.size(), 2u);
	EXPECT_EQ(Inspection.Header.ObjectCount, 2u);
	EXPECT_EQ(Inspection.Objects[0].Id, 1u);
	EXPECT_EQ(Inspection.Objects[0].ClassName, "Tests::DPackageAssetForTest");
	EXPECT_NE(Inspection.Objects[0].FindField("Value"), nullptr);
	EXPECT_EQ(Inspection.Objects[1].Id, 2u);
	EXPECT_EQ(Inspection.Objects[1].OuterId, 1u);
	EXPECT_EQ(Inspection.Objects[1].ObjectName, "InspectedChild");
	EXPECT_EQ(Inspection.Fingerprint.FileSize, std::filesystem::file_size(File));
	EXPECT_FALSE(Inspection.Fingerprint.ContentHash.IsZero());
	EXPECT_TRUE(Durin::UnloadPackage(Path));
}

#if DURIN_WITH_EDITOR
TEST(FPackageAssetTests, MountedPackageSnapshotIsDeterministicHashedAndReadOnly)
{
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "AssetPackageSnapshot";
	const std::filesystem::path AutoRoot = Root / "Auto";
	const std::filesystem::path ManualRoot = Root / "Manual";
	std::filesystem::create_directories(AutoRoot / "Nested");
	std::filesystem::create_directories(ManualRoot);
	const std::filesystem::path First = AutoRoot / "B.dasset";
	const std::filesystem::path Second = AutoRoot / "Nested" / "A.dasset";
	const std::filesystem::path Ignored = ManualRoot / "Ignored.dasset";
	{
		std::ofstream(First, std::ios::binary) << "first";
		std::ofstream(Second, std::ios::binary) << "second";
		std::ofstream(Ignored, std::ios::binary) << "ignored";
	}
	const std::array Definitions{
		Durin::FMountPoint{
			.VirtualRoot = "/Snapshot/",
			.Owner = Durin::EMountOwner::Test,
			.Root = AutoRoot,
			.ContentPath = ".",
			.bAutoScan = true},
		Durin::FMountPoint{
			.VirtualRoot = "/Manual/",
			.Owner = Durin::EMountOwner::Test,
			.Root = ManualRoot,
			.ContentPath = ".",
			.bAutoScan = false}};
	Durin::Testing::FScopedMountRegistryFixture Mounts(Definitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	Durin::FByteArray FirstBefore;
	Durin::FByteArray SecondBefore;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstBefore, First));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondBefore, Second));

	const auto Snapshot = Durin::CaptureMountedAssetPackageSnapshot();
	ASSERT_EQ(Snapshot.Status, Durin::EAssetPackageSnapshotStatus::Completed);
	ASSERT_EQ(Snapshot.Packages.size(), 2u);
	EXPECT_EQ(Snapshot.Packages[0].PackagePath.ToString(), "/Snapshot/B");
	EXPECT_EQ(Snapshot.Packages[1].PackagePath.ToString(), "/Snapshot/Nested/A");
	EXPECT_EQ(Snapshot.Packages[0].ExpectedFileSize, FirstBefore.size());
	EXPECT_EQ(Snapshot.Packages[0].ExpectedContentHash,
		Durin::FXxHash128::HashBuffer(FirstBefore));
	EXPECT_EQ(Snapshot.Packages[1].ExpectedContentHash,
		Durin::FXxHash128::HashBuffer(SecondBefore));
	EXPECT_TRUE(Snapshot.Packages[0].ExpectedReportContentHash.starts_with("sha256:"));
	EXPECT_EQ(Snapshot.Packages[0].ExpectedReportContentHash.size(), 71u);
	EXPECT_NE(Snapshot.Packages[0].ExpectedReportContentHash,
		Snapshot.Packages[1].ExpectedReportContentHash);
	Durin::FByteArray FirstAfter;
	Durin::FByteArray SecondAfter;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstAfter, First));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondAfter, Second));
	EXPECT_EQ(FirstAfter, FirstBefore);
	EXPECT_EQ(SecondAfter, SecondBefore);

	const auto Cancelled = Durin::CaptureMountedAssetPackageSnapshot([] { return true; });
	EXPECT_EQ(Cancelled.Status, Durin::EAssetPackageSnapshotStatus::Cancelled);
	EXPECT_TRUE(Cancelled.Packages.empty());
}

#endif

TEST(FPackageAssetTests, PackageSavesRejectReadOnlyContentMounts)
{
	InitializeAssetTests();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "ReadOnlyPackageSave";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(Root);
	const std::array Definitions{
		Durin::FMountPoint{
			.VirtualRoot = "/ReadOnly/",
			.Owner = Durin::EMountOwner::Extension,
			.Root = Root,
			.ContentPath = ".",
			.bAutoScan = true,
			.bContentWritable = false}};
	Durin::Testing::FScopedMountRegistryFixture Mounts(Definitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();

	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/ReadOnly/Blocked", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	Durin::DPackage* Package = Asset->GetPackage();
	const Durin::FAssetResult BundleSaved =
		Durin::SavePackagesAtomically(
			std::span<Durin::DPackage* const>(&Package, 1), {});
	EXPECT_EQ(BundleSaved.Error, Durin::EAssetError::ReadOnlyMode);
	EXPECT_EQ(BundleSaved.Message, "Content mount /ReadOnly/ is read-only.");
	const Durin::FAssetResult Saved =
		Durin::SavePackage(Package);
	EXPECT_EQ(Saved.Error, Durin::EAssetError::ReadOnlyMode);
	EXPECT_EQ(Saved.Message, "Content mount /ReadOnly/ is read-only.");
	EXPECT_FALSE(std::filesystem::exists(Root / "Blocked.dasset"));
	EXPECT_TRUE(Durin::UnloadPackage(
		Package,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
}


TEST(FPackageAssetTests, PackageLoadSnapshotReleasesOnlyPackagesIntroducedAfterCapture)
{
	InitializeAssetTests();
	Durin::FPackagePath ExistingPath;
	Durin::FPackagePath IntroducedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/ExistingOwnership", ExistingPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/IntroducedOwnership", IntroducedPath));

	DPackageAssetForTest* Existing = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ExistingPath, Existing));
	ASSERT_TRUE(Durin::SavePackage(Existing->GetPackage()));
	const Durin::FAssetPackageLoadSnapshot Snapshot =
		Durin::CapturePackageLoadSnapshot();

	DPackageAssetForTest* Introduced = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(IntroducedPath, Introduced));
	ASSERT_TRUE(Durin::SavePackage(Introduced->GetPackage()));
	ASSERT_TRUE(Durin::ReleasePackagesLoadedSince(Snapshot));

	EXPECT_NE(Durin::FindResidentPackage(ExistingPath), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(IntroducedPath), nullptr);
	EXPECT_TRUE(Durin::UnloadPackage(ExistingPath));
}

TEST(FPackageAssetTests, RejectsInvalidPaths)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	EXPECT_FALSE(Durin::FPackagePath::TryCreate("TestAssets/Relative", Path));
	EXPECT_FALSE(Durin::FPackagePath::TryCreate("/TestAssets/../Escape", Path));
	EXPECT_FALSE(Durin::FPackagePath::TryCreate("/Unknown/Asset", Path));
	EXPECT_FALSE(Durin::FPackagePath::TryCreate("/TestAssets/With.dasset", Path));
}

TEST(FPackageAssetTests, RejectsSavingCppPackages)
{
	InitializeAssetTests();
	Durin::DPackage* Package = Durin::FindOrCreateCppPackage("AssetTests");
	ASSERT_NE(Package, nullptr);
	EXPECT_EQ(Durin::SavePackage(Package).Error, Durin::EAssetError::InvalidPackageType);
}

TEST(FPackageAssetTests, PackageEditRevisionAdvancesForRepeatedDirtyEdits)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/EditRevision", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	Durin::DPackage* Package = Asset->GetPackage();
	ASSERT_NE(Package, nullptr);

	const uint64 CreatedRevision = Package->GetEditRevision();
	Package->ClearDirty();
	EXPECT_EQ(Package->GetEditRevision(), CreatedRevision);

	Package->MarkDirty();
	const uint64 FirstEditRevision = Package->GetEditRevision();
	EXPECT_GT(FirstEditRevision, CreatedRevision);
	EXPECT_TRUE(Package->IsDirty());

	Package->MarkDirty();
	EXPECT_GT(Package->GetEditRevision(), FirstEditRevision);
	EXPECT_TRUE(Package->IsDirty());
}

TEST(FPackageAssetTests, SequentialPackageSavesPublishEarlierPackagesBeforeLaterFailure)
{
	InitializeAssetTests();
	Durin::FPackagePath FirstPath;
	Durin::FPackagePath BlockedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/Stage0First", FirstPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/Stage0Blocked/Second", BlockedPath));

	DPackageAssetForTest* First = nullptr;
	DPackageAssetForTest* Second = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(FirstPath, First));
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(BlockedPath, Second));
	First->Value = 1;
	Second->Value = 2;

	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "Assets";
	const std::filesystem::path Blocker = Root / "Stage0Blocked";
	{
		std::ofstream Stream(Blocker);
		ASSERT_TRUE(Stream.is_open());
		Stream << "blocks destination directory creation";
	}

	ASSERT_TRUE(Durin::SavePackage(First->GetPackage()));
	const Durin::FAssetResult SecondResult = Durin::SavePackage(Second->GetPackage());
	EXPECT_EQ(SecondResult.Error, Durin::EAssetError::IoError);

	EXPECT_TRUE(std::filesystem::is_regular_file(Root / "Stage0First.dasset"));
	EXPECT_NE(Durin::FindAssetExact(FirstPath), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(FirstPath), First->GetPackage());
	EXPECT_FALSE(First->GetPackage()->IsNewlyCreated());
	EXPECT_FALSE(First->GetPackage()->IsDirty());

	EXPECT_FALSE(std::filesystem::exists(Root / "Stage0Blocked" / "Second.dasset"));
	EXPECT_EQ(Durin::FindAssetExact(BlockedPath), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(BlockedPath), Second->GetPackage());
	EXPECT_TRUE(Second->GetPackage()->IsNewlyCreated());
	EXPECT_TRUE(Second->GetPackage()->IsDirty());

	ASSERT_TRUE(std::filesystem::remove(Blocker));
	ASSERT_TRUE(Durin::SavePackage(Second->GetPackage()));
	EXPECT_EQ(Durin::FindResidentPackage(BlockedPath), Second->GetPackage());
	EXPECT_FALSE(Second->GetPackage()->IsNewlyCreated());
	EXPECT_TRUE(Durin::FindAssetExact(BlockedPath));
}

TEST(FPackageAssetTests, AtomicBundleRegistryFailureKeepsCommittedContent)
{
	InitializeAssetTests();
	Durin::FPackagePath ExistingPath;
	Durin::FPackagePath NewPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/AtomicBundleExisting", ExistingPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/AtomicBundleNew", NewPath));

	DPackageAssetForTest* Existing = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ExistingPath, Existing));
	Existing->Value = 11;
	ASSERT_TRUE(Durin::SavePackage(Existing->GetPackage()));
	const Durin::FAssetData ExistingRegistry =
		*Durin::FindAssetExact(ExistingPath);
	Durin::FByteArray ExistingBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		ExistingBytes, ExistingRegistry.PhysicalPath
	));

	DPackageAssetForTest* Added = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(NewPath, Added));
	Added->Value = 22;
	Existing->Value = 33;
	Existing->MarkPackageDirty();
	const std::array Packages = {
		Existing->GetPackage(),
		Added->GetPackage()
	};
	const Durin::FAssetResult Result = Durin::SavePackagesAtomically(
		Packages,
		{.RootPackage = Added->GetPackage(),
		 .ShouldFail = [](Durin::EAssetBundleSavePhase Phase, size_t) {
			 return Phase == Durin::EAssetBundleSavePhase::PublishRegistry;
		 }}
	);
	EXPECT_EQ(Result.Error, Durin::EAssetError::StaleData);
	EXPECT_EQ(Result.Disposition,
		Durin::EAssetResultDisposition::ContentCommittedProjectionPending);
	EXPECT_NE(Result.Message.find("ContentCommittedProjectionPending"),
		std::string::npos);

	Durin::FByteArray CommittedBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		CommittedBytes, ExistingRegistry.PhysicalPath
	));
	EXPECT_NE(CommittedBytes, ExistingBytes);
	EXPECT_EQ(*Durin::FindAssetExact(ExistingPath), ExistingRegistry);
	EXPECT_EQ(Durin::FindAssetExact(NewPath), nullptr);
	EXPECT_TRUE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "AtomicBundleNew.dasset"
	));
	EXPECT_FALSE(Existing->GetPackage()->IsDirty());
	EXPECT_FALSE(Added->GetPackage()->IsDirty());
	EXPECT_EQ(Durin::FindResidentPackage(NewPath), Added->GetPackage());
	EXPECT_FALSE(Added->GetPackage()->IsNewlyCreated());
	const uint64 RevisionBeforeCommit = Durin::GetAssetCatalogRevision();
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	EXPECT_EQ(Durin::GetAssetCatalogRevision(), RevisionBeforeCommit + 1);
	EXPECT_TRUE(Durin::FindAssetExact(ExistingPath));
	EXPECT_TRUE(Durin::FindAssetExact(NewPath));
	ASSERT_TRUE(Durin::UnloadPackage(Added->GetPackage(), Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FPackageAssetTests, OrdinaryV8SavesAreDeterministic)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/OrdinaryV8Policy", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	Asset->Value = 41;
	const Durin::FAssetResult InitialSave =
		Durin::SavePackage(Asset->GetPackage());
	ASSERT_TRUE(InitialSave) << InitialSave.Message;

	const Durin::FAssetData Current = *Durin::FindAssetExact(Path);
	ASSERT_EQ(Current.FormatVersion, Durin::ObjectPackage::DastV9FormatVersion);
	Durin::FByteArray FirstBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstBytes, Current.PhysicalPath));
	Durin::FByteArray FirstSerialization;
	Durin::FByteArray SecondSerialization;
	ASSERT_TRUE(Durin::SerializeAssetPackageBytes(
		Asset->GetPackage(), FirstSerialization));
	ASSERT_TRUE(Durin::SerializeAssetPackageBytes(
		Asset->GetPackage(), SecondSerialization));
	EXPECT_EQ(FirstSerialization, SecondSerialization);
	EXPECT_EQ(FirstSerialization, FirstBytes);
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	Durin::FByteArray RepeatedBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(RepeatedBytes, Current.PhysicalPath));
	EXPECT_EQ(RepeatedBytes, FirstBytes);

}


TEST(FPackageAssetTests, LoadsExternalDependenciesAndPreventsPrematureUnload)
{
	InitializeAssetTests();
	Durin::FPackagePath DependencyPath;
	Durin::FPackagePath OwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/Dependency", DependencyPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/Owner", OwnerPath));

	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(DependencyPath, Dependency));
	Dependency->Label = "Dependency";
	ASSERT_TRUE(Durin::SavePackage(Dependency->GetPackage()));

	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	ASSERT_EQ(Durin::FindAssetExact(OwnerPath)->Dependencies.size(), 1u);
	EXPECT_EQ(Durin::UnloadPackage(DependencyPath).Error, Durin::EAssetError::InUse);
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::UnloadPackage(DependencyPath));

	DPackageAssetForTest* LoadedOwner = nullptr;
	Durin::FAssetLoadReport LoadReport;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(OwnerPath), LoadedOwner, &LoadReport));
	EXPECT_EQ(LoadReport.PackageFileReadCount, 2u);
	ASSERT_NE(LoadedOwner->ExternalReference.Get(), nullptr);
	EXPECT_EQ(LoadedOwner->ExternalReference->GetObjectPath(), "/TestAssets/Dependency.Dependency");
	EXPECT_EQ(Durin::FindResidentPackage(DependencyPath)->FindTopLevelAsset(
		DependencyPath.GetPackageName()), LoadedOwner->ExternalReference.Get());

	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::UnloadPackage(DependencyPath));
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	EXPECT_TRUE(Durin::FindAssetExact(OwnerPath));
	EXPECT_TRUE(Durin::FindAssetExact(DependencyPath));
}

TEST(FPackageAssetTests, RuntimeStrongReferencePreventsUnloadAndRestoresResidency)
{
	InitializeAssetTests();
	Durin::FPackagePath TargetPath, OwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RuntimeUnloadTarget", TargetPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RuntimeUnloadOwner", OwnerPath));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(TargetPath, Target));
	ASSERT_TRUE(Durin::SavePackage(Target->GetPackage()));
	Durin::DPackage* TargetPackage = Target->GetPackage();

	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->ExternalReference = Target;
	const Durin::FAssetResult Result =
		Durin::UnloadPackage(TargetPath);
	EXPECT_EQ(Result.Error, Durin::EAssetError::InUse);
	EXPECT_EQ(Result.Message, "Package remains referenced by live objects.");
	EXPECT_EQ(Durin::FindResidentPackage(TargetPath), TargetPackage);
	EXPECT_TRUE(TargetPackage->HasAnyObjectFlags(Durin::EObjectFlags::Standalone));
	EXPECT_FALSE(TargetPackage->IsGarbage());
	EXPECT_EQ(Owner->ExternalReference.Get(), Target);

	ASSERT_TRUE(Durin::UnloadPackage(
		OwnerPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Durin::UnloadPackage(TargetPath));
}

TEST(FPackageAssetTests, RejectsTruncatedPackagesWithoutCachingPartialObjects)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/Corrupt", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(Path));

	const std::filesystem::path File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "Corrupt.dasset";
	std::filesystem::resize_file(File, 12);
	Durin::DObject* Loaded = nullptr;
	const Durin::FAssetResult Result = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Error, Durin::EAssetError::CorruptFile);
	EXPECT_EQ(Loaded, nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
}

TEST(FPackageAssetTests, RelocationJobIsForwardOnlyAndCompletesOnce)
{
	InitializeAssetTests();
	Durin::FPackagePath First;
	Durin::FPackagePath FirstMoved;
	Durin::FPackagePath Second;
	Durin::FPackagePath SecondMoved;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/BatchFirst", First));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/BatchFirstMoved", FirstMoved));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/BatchSecond", Second));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/BatchSecondMoved", SecondMoved));
	DPackageAssetForTest* FirstAsset = nullptr;
	DPackageAssetForTest* SecondAsset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(First, FirstAsset));
	ASSERT_TRUE(Durin::SavePackage(FirstAsset->GetPackage()));
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Second, SecondAsset));
	ASSERT_TRUE(Durin::SavePackage(SecondAsset->GetPackage()));

	const std::array Mappings = {
		Durin::FAssetRelocationMapping{First, FirstMoved},
		Durin::FAssetRelocationMapping{Second, SecondMoved}
	};
	Durin::FAssetRelocationSummary Summary;
	Durin::FAssetMutationJob Job;
	const Durin::FAssetResult Analysis =
		Durin::PrepareAssetRelocationJob(
			Mappings, Summary, Job);
	ASSERT_TRUE(Analysis) << Analysis.Message;
	EXPECT_EQ(Summary.GetScope().size(), 4u);
	EXPECT_EQ(Job.GetState(),
		Durin::EAssetMutationJobState::Prepared);
	const uint64 BeforeRevision =
		Durin::GetAssetCatalogRevision();
	ASSERT_TRUE(Job.ResumeForward());
	EXPECT_EQ(Job.GetState(),
		Durin::EAssetMutationJobState::Completed);
	EXPECT_EQ(Job.ResumeForward().Error, Durin::EAssetError::StaleData);
	EXPECT_EQ(Durin::GetAssetCatalogRevision(), BeforeRevision + 1);
	EXPECT_EQ(Durin::FindAssetExact(First)->EntryKind, Durin::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Durin::FindAssetExact(Second)->EntryKind, Durin::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Durin::FindAssetExact(FirstMoved)->EntryKind, Durin::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::FindAssetExact(SecondMoved)->EntryKind, Durin::EAssetRegistryEntryKind::Asset);

	EXPECT_EQ(Durin::ResolveAssetPath(First).FinalPath, FirstMoved);
	EXPECT_EQ(Durin::ResolveAssetPath(Second).FinalPath, SecondMoved);
	ASSERT_TRUE(DeleteAssetClosureForTest(
		{First, FirstMoved, Second, SecondMoved}));
}

TEST(FPackageAssetTests, RelocationJobRejectsStaleCommitWithoutMutatingState)
{
	InitializeAssetTests();
	Durin::FPackagePath Source;
	Durin::FPackagePath Destination;
	Durin::FPackagePath Unrelated;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/StaleJobSource", Source));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/StaleJobDestination", Destination));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/StaleJobUnrelated", Unrelated));
	DPackageAssetForTest* SourceAsset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Source, SourceAsset));
	ASSERT_TRUE(Durin::SavePackage(SourceAsset->GetPackage()));
	const Durin::FAssetRelocationMapping Mapping{Source, Destination};
	Durin::FAssetRelocationSummary Summary;
	Durin::FAssetMutationJob Job;
	ASSERT_TRUE(Durin::PrepareAssetRelocationJob(
		std::span{&Mapping, 1}, Summary, Job));

	DPackageAssetForTest* UnrelatedAsset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Unrelated, UnrelatedAsset));
	ASSERT_TRUE(Durin::SavePackage(UnrelatedAsset->GetPackage()));
	EXPECT_EQ(Job.ResumeForward().Error, Durin::EAssetError::StaleData);
	EXPECT_EQ(Job.GetState(),
		Durin::EAssetMutationJobState::Prepared);
	const Durin::FAssetMutationResultDetails Details =
		Job.GetLastResultDetails();
	EXPECT_TRUE(Details.bForwardResumable);
	EXPECT_FALSE(Details.bRecoveryRequired);
	EXPECT_NE(Durin::FindAssetExact(Source), nullptr);
	EXPECT_EQ(Durin::FindAssetExact(Destination), nullptr);
}

TEST(FPackageAssetTests, RepeatedRelocationLeavesAliasCompressionToFixup)
{
	InitializeAssetTests();
	Durin::FPackagePath First;
	Durin::FPackagePath Second;
	Durin::FPackagePath Third;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/RepeatedA", First));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/RepeatedB", Second));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/RepeatedC", Third));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(First, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));

	ASSERT_TRUE(RelocateAssetForTest(First, Second));
	ASSERT_TRUE(RelocateAssetForTest(Second, Third));
	const auto FirstAlias =
		Durin::FindAssetExact(First);
	const auto SecondAlias =
		Durin::FindAssetExact(Second);
	ASSERT_NE(FirstAlias, nullptr);
	ASSERT_NE(SecondAlias, nullptr);
	EXPECT_EQ(FirstAlias->RedirectDestination, Second);
	EXPECT_EQ(SecondAlias->RedirectDestination, Third);

	ASSERT_TRUE(RelocateAssetForTest(Third, First));
	EXPECT_EQ(Durin::FindAssetExact(First)->EntryKind, Durin::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::FindAssetExact(Second)->RedirectDestination, Third);
	EXPECT_EQ(Durin::FindAssetExact(Third)->RedirectDestination, First);
	EXPECT_EQ(Durin::ResolveAssetPath(Second).FinalPath, First);

	Durin::FPackagePath Unrelated;
	Durin::FPackagePath UnrelatedAlias;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RepeatedUnrelated", Unrelated
	));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/RepeatedUnrelatedAlias", UnrelatedAlias
	));
	DPackageAssetForTest* UnrelatedAsset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Unrelated, UnrelatedAsset));
	ASSERT_TRUE(Durin::SavePackage(UnrelatedAsset->GetPackage()));
	Durin::DAssetRedirector* Alias = nullptr;
	ASSERT_TRUE(Durin::CreateAssetRedirectorForTesting(
		UnrelatedAlias, Unrelated, Alias
	));
	ASSERT_TRUE(Durin::SavePackage(Alias->GetPackage()));
	EXPECT_EQ(RelocateAssetForTest(First, UnrelatedAlias).Error, Durin::EAssetError::AlreadyExists);
	EXPECT_EQ(Durin::ResolveAssetPath(UnrelatedAlias).FinalPath, Unrelated);
}

TEST(FPackageAssetTests, PackageIdentityIsEmbeddedAndRewrittenOnRelocation)
{
	InitializeAssetTests();
	Durin::FPackagePath OldPath, NewPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/MoveSource", OldPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/Sub/MoveSource", NewPath));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Asset));
	Asset->Label = "movable";
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	const auto OldFile = Durin::Testing::GetTestWorkDirectory() / "Assets" / "MoveSource.dasset";
	Durin::FByteArray Before;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Before, OldFile));
	uint32 FormatVersion = 0;
	ASSERT_TRUE(Durin::ReadLittleEndianAt(Before, 24, FormatVersion));
	EXPECT_EQ(FormatVersion, Durin::ObjectPackage::DastV9FormatVersion);
	const std::string_view OldPathView = OldPath.GetView();
	const std::span<const std::byte> OldPathBytes =
		std::as_bytes(std::span{OldPathView.data(), OldPathView.size()});
	EXPECT_NE(std::search(Before.begin(), Before.end(),
		OldPathBytes.begin(), OldPathBytes.end()), Before.end());

	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	const auto NewFile = Durin::Testing::GetTestWorkDirectory() / "Assets" / "Sub" / "MoveSource.dasset";
	Durin::FByteArray After;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(After, NewFile));
	EXPECT_NE(Before, After);
	EXPECT_TRUE(Durin::ValidateAssetPackageBytes(After, NewPath));
	const std::string_view NewPathView = NewPath.GetView();
	const std::span<const std::byte> NewPathBytes =
		std::as_bytes(std::span{NewPathView.data(), NewPathView.size()});
	EXPECT_NE(std::search(After.begin(), After.end(),
		NewPathBytes.begin(), NewPathBytes.end()), After.end());
	EXPECT_TRUE(std::filesystem::exists(OldFile));
	ASSERT_NE(Durin::FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Durin::FindAssetExact(OldPath)->EntryKind, Durin::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Durin::FindResidentPackage(OldPath), nullptr);
	EXPECT_NE(Durin::FindResidentPackage(NewPath), nullptr);
}

TEST(FPackageAssetTests, RelocationLeavesMountedReferrersAuthoredToAlias)
{
	InitializeAssetTests();
	Durin::FPackagePath OldPath, NewPath, OwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/MoveDependency", OldPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/RenamedDependency", NewPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/MoveOwner", OwnerPath));
	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OldPath, Dependency));
	ASSERT_TRUE(Durin::SavePackage(Dependency->GetPackage()));
	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));

	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	const Durin::FAssetCatalogEntry OwnerData = Durin::FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	EXPECT_EQ(std::ranges::find(OwnerData->Dependencies, NewPath), OwnerData->Dependencies.end());
	EXPECT_NE(std::ranges::find(OwnerData->Dependencies, OldPath), OwnerData->Dependencies.end());
	EXPECT_EQ(Dependency->GetName(), OldPath.GetPackageName());
}

TEST(FPackageAssetTests, DeletesUnreferencedAssetAndRegistryEntry)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/DeleteMe", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	const std::filesystem::path File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteMe.dasset";
	ASSERT_TRUE(std::filesystem::exists(File));

	Durin::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_FALSE(Analysis.bLoaded);
	EXPECT_TRUE(Analysis.CanDelete());
	ASSERT_TRUE(Durin::DeleteAssetForTesting(Path));
	EXPECT_FALSE(std::filesystem::exists(File));
	EXPECT_EQ(Durin::FindAssetExact(Path), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
}

TEST(FPackageAssetTests, UnloadsAndDeletesLoadedUnreferencedAsset)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/LoadedDelete", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	Durin::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_TRUE(Analysis.bLoaded);
	EXPECT_TRUE(Analysis.CanDelete());

	ASSERT_TRUE(Durin::DeleteAssetForTesting(Path));
	EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
	EXPECT_EQ(Durin::FindAssetExact(Path), nullptr);
	EXPECT_FALSE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "LoadedDelete.dasset"
	));
}

TEST(FPackageAssetTests, RejectsDeletingReferencedAssetWithoutChangingDisk)
{
	InitializeAssetTests();
	Durin::FPackagePath DependencyPath, OwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/DeleteDependency", DependencyPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/DeleteOwner", OwnerPath));
	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(DependencyPath, Dependency));
	ASSERT_TRUE(Durin::SavePackage(Dependency->GetPackage()));
	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));

	Durin::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::AnalyzeAssetDeletion(DependencyPath, Analysis));
	ASSERT_EQ(Analysis.DirectReferencers.size(), 1u);
	EXPECT_EQ(Analysis.DirectReferencers.front(), OwnerPath);
	EXPECT_FALSE(Analysis.CanDelete());
	EXPECT_EQ(Durin::DeleteAssetForTesting(DependencyPath).Error, Durin::EAssetError::InUse);
	EXPECT_NE(Durin::FindResidentPackage(DependencyPath), nullptr);
	EXPECT_NE(Durin::FindAssetExact(DependencyPath), nullptr);
	EXPECT_TRUE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteDependency.dasset"
	));
}

TEST(FPackageAssetTests, DeletesRegisteredCompanionFile)
{
	InitializeAssetTests();
	RegisterTestDeleteContributor();

	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/DeleteWithCompanion", Path));
	const std::filesystem::path Companion =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteWithCompanion.source";
	{
		std::ofstream Stream(Companion);
		Stream << "source";
	}
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	Asset->Label = "companion:" + Companion.generic_string();
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	Durin::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
	ASSERT_EQ(Analysis.CompanionFiles.size(), 1u);
	EXPECT_EQ(Analysis.CompanionFiles.front(), Companion);
	ASSERT_TRUE(Durin::DeleteAssetForTesting(Path));
	EXPECT_FALSE(std::filesystem::exists(Companion));
}

TEST(FPackageAssetTests, DeletesMainAssetWhenCompanionInspectionFails)
{
	InitializeAssetTests();
	RegisterTestDeleteContributor();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/DeleteCorruptPackage", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	const std::filesystem::path File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteCorruptPackage.dasset";
	Durin::FByteArray Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File));
	ASSERT_GT(Bytes.size(), 16u);
	WriteTestBytes(File, std::span<const std::byte>(Bytes).first(16));

	Durin::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_FALSE(Analysis.Warning.empty());
	EXPECT_TRUE(Analysis.CompanionFiles.empty());
	ASSERT_TRUE(Durin::DeleteAssetForTesting(Path));
	EXPECT_FALSE(std::filesystem::exists(File));
}

TEST(FPackageAssetTests, DeleteAnalysisDoesNotLeaveTemporaryDependenciesLoaded)
{
	InitializeAssetTests();
	Durin::FPackagePath DependencyPath, OwnerPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/AnalysisDependency", DependencyPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/AnalysisOwner", OwnerPath));
	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(DependencyPath, Dependency));
	ASSERT_TRUE(Durin::SavePackage(Dependency->GetPackage()));
	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::UnloadPackage(DependencyPath));

	Durin::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::AnalyzeAssetDeletion(OwnerPath, Analysis));
	EXPECT_FALSE(Analysis.bLoaded);
	EXPECT_EQ(Durin::FindResidentPackage(OwnerPath), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(DependencyPath), nullptr);
}

TEST(FPackageAssetTests, LegacyPackageIsExplicitlyUnsupportedWithoutCatalogMutation)
{
	InitializeAssetTests();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/LegacyVersion", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	const Durin::FAssetCatalogEntry BeforeRefresh =
		Durin::FindAssetExact(Path);
	ASSERT_TRUE(BeforeRefresh);
	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "LegacyVersion.dasset";
	const std::array<std::byte, 8> LegacyPackage{
		std::byte{0x44}, std::byte{0x41}, std::byte{0x53}, std::byte{0x54},
		std::byte{0x05}, std::byte{}, std::byte{}, std::byte{}};
	WriteTestBytes(File, LegacyPackage);
	Durin::DObject* Loaded = nullptr;
	EXPECT_EQ(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded).Error, Durin::EAssetError::UnsupportedVersion);
	EXPECT_EQ(Loaded, nullptr);
	const Durin::FAssetCatalogRefreshResult Refresh =
		Durin::RefreshAssetRegistry();
	EXPECT_FALSE(Refresh);
	EXPECT_FALSE(Refresh.bCatalogComplete);
	EXPECT_TRUE(Refresh.bRetainedPriorRevision);
	EXPECT_EQ(Refresh.PriorRevision, BeforeRefresh.Revision);
	EXPECT_EQ(Refresh.ResultingRevision, BeforeRefresh.Revision);
	const Durin::FAssetCatalogEntry AfterRefresh =
		Durin::FindAssetExact(Path);
	ASSERT_TRUE(AfterRefresh);
	EXPECT_EQ(AfterRefresh.Revision, BeforeRefresh.Revision);
	EXPECT_EQ(AfterRefresh.Data, BeforeRefresh.Data);
	EXPECT_TRUE(std::ranges::any_of(
		Refresh.Errors,
		[](const Durin::FAssetRegistryResult& Error) {
			return Error.Error == Durin::EAssetRegistryError::UnsupportedVersion;
		}
	));
}

TEST(FPackageAssetTests, ManualScanMountsRequireExplicitAdmissionBeforeLoading)
{
	InitializeAssetTests();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "Assets";
	const std::array Definitions{
		Durin::FMountPoint{
			.VirtualRoot = "/TestAssets/",
			.Owner = Durin::EMountOwner::Test,
			.Root = Root,
			.ContentPath = ".",
			.bAutoScan = false,
			.bContentWritable = true
		}
	};
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TestAssets/ManualScanAsset", Path
	));
	{
		Durin::Testing::FScopedMountRegistryFixture Mounts(Definitions);
		ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
		DPackageAssetForTest* Asset = nullptr;
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
		ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
		ASSERT_TRUE(Durin::UnloadPackage(Path));

		const Durin::FAssetCatalogRefreshResult ManualRefresh =
			Durin::RefreshAssetRegistry(
				Durin::EAssetRegistryScanMode::FullValidation);
		ASSERT_TRUE(ManualRefresh);
		EXPECT_EQ(ManualRefresh.CatalogStats.Enumerated, 0u);
		EXPECT_FALSE(Durin::FindAssetExact(Path));

		Durin::DObject* Loaded = nullptr;
		Durin::FAssetLoadReport MissingReport;
		const Durin::FAssetResult Missing =
			Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded, &MissingReport);
		EXPECT_EQ(Missing.Error, Durin::EAssetError::NotFound);
		EXPECT_EQ(MissingReport.Error, Durin::EAssetError::NotFound);
		EXPECT_EQ(MissingReport.RequestedPath, Path);
		EXPECT_EQ(MissingReport.CatalogRevision,
			Durin::GetAssetCatalogRevision());
		EXPECT_EQ(MissingReport.PackageFileReadCount, 0u);
		EXPECT_EQ(Loaded, nullptr);

		ASSERT_TRUE(Durin::AdmitAssetPackageToCatalog(Path));
		ASSERT_TRUE(Durin::FindAssetExact(Path));
		Durin::FAssetLoadReport LoadReport;
		ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded, &LoadReport));
		EXPECT_NE(Loaded, nullptr);
		EXPECT_EQ(LoadReport.RequestedPath, Path);
		EXPECT_EQ(LoadReport.FinalPath, Path);
		EXPECT_EQ(LoadReport.CatalogRevision,
			Durin::GetAssetCatalogRevision());
		EXPECT_EQ(LoadReport.Error, Durin::EAssetError::None);
		EXPECT_EQ(LoadReport.PackageFileReadCount, 1u);
		ASSERT_TRUE(Durin::UnloadPackage(Path));
	}

	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	ASSERT_TRUE(Durin::DeleteAssetForTesting(Path));
}

TEST(FPackageAssetTests, PersistentRegistryReconcilesChangesAndRecoversFromInvalidCache)
{
	InitializeAssetTests();
	const auto WorkRoot = Durin::Testing::GetTestWorkDirectory() / "RegistryReconciliation";
	const auto OriginalAssets = Durin::Testing::GetTestWorkDirectory() / "Assets";
	const auto ContentA = WorkRoot / "ContentA";
	const auto ContentB = WorkRoot / "ContentB";
	const auto CacheRoot = WorkRoot / "DerivedDataCache";
	Durin::Testing::RemoveTestWorkDirectory(WorkRoot);
	std::filesystem::create_directories(ContentA);
	auto CreateIdentityBoundFixture = [&](std::string_view Name) {
		Durin::FPackagePath Path;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate(
			std::format("/TestAssets/{}", Name), Path));
		DPackageAssetForTest* Asset = nullptr;
		EXPECT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Asset));
		EXPECT_TRUE(Durin::SavePackage(Asset->GetPackage()));
		EXPECT_TRUE(Durin::UnloadPackage(Path));
		return OriginalAssets / std::format("{}.dasset", Name);
	};
	const auto AlphaFixture = CreateIdentityBoundFixture("Alpha");
	const auto BetaFixture = CreateIdentityBoundFixture("Beta");
	const auto GammaFixture = CreateIdentityBoundFixture("Gamma");
	const auto DeltaFixture = CreateIdentityBoundFixture("Delta");
	std::filesystem::copy_file(AlphaFixture, ContentA / "Alpha.dasset");
	std::filesystem::copy_file(BetaFixture, ContentA / "Beta.dasset");
	Durin::Testing::RegisterMountPointForTests("/TestAssets/", ContentA.generic_string() + "/");
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	const uint64 RevisionBeforeInitialScan =
		Durin::GetAssetCatalogRevision();
	const auto InitialRefresh = Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(InitialRefresh);
	EXPECT_GT(InitialRefresh.ResultingRevision, RevisionBeforeInitialScan);
	EXPECT_EQ(InitialRefresh.CatalogStats.Enumerated, 2u);
	EXPECT_EQ(InitialRefresh.CatalogStats.Reparsed, 2u);
	EXPECT_EQ(InitialRefresh.CatalogStats.HeaderReadAttempts, 2u);
	EXPECT_GT(InitialRefresh.CatalogStats.HeaderBytesRead, 0u);
	EXPECT_GE(InitialRefresh.CatalogStats.DurationMilliseconds, 0.0);
	EXPECT_EQ(Durin::CaptureAssetCatalogSnapshot().Assets.size(), 2u);
	const auto CacheFile = CacheRoot / "AssetRegistry" / "Registry.bin";
	Durin::FByteArray FirstCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstCache, CacheFile));

	const uint64 StableRevision = Durin::GetAssetCatalogRevision();
	const auto StableRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(StableRefresh);
	EXPECT_EQ(StableRefresh.ResultingRevision, StableRevision);
	EXPECT_EQ(StableRefresh.CatalogStats.Reused, 2u);
	EXPECT_EQ(StableRefresh.CatalogStats.Reparsed, 0u);
	EXPECT_EQ(StableRefresh.CatalogStats.HeaderReadAttempts, 0u);
	EXPECT_EQ(StableRefresh.CatalogStats.HeaderBytesRead, 0u);
	EXPECT_GE(StableRefresh.CatalogStats.DurationMilliseconds, 0.0);
	Durin::FByteArray SecondCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondCache, CacheFile));
	EXPECT_EQ(SecondCache, FirstCache);

	const auto Alpha = ContentA / "Alpha.dasset";
	std::filesystem::last_write_time(Alpha, std::filesystem::last_write_time(Alpha) + std::chrono::seconds(2));
	const auto ChangedRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(ChangedRefresh);
	EXPECT_GT(ChangedRefresh.ResultingRevision, StableRevision);
	EXPECT_EQ(ChangedRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(ChangedRefresh.CatalogStats.Reparsed, 1u);
	EXPECT_EQ(ChangedRefresh.CatalogStats.HeaderReadAttempts, 1u);
	EXPECT_GT(ChangedRefresh.CatalogStats.HeaderBytesRead, 0u);
	EXPECT_GE(ChangedRefresh.CatalogStats.DurationMilliseconds, 0.0);

	std::filesystem::copy_file(GammaFixture, ContentA / "Gamma.dasset");
	std::filesystem::remove(ContentA / "Beta.dasset");
	const auto AddedRemovedRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(AddedRemovedRefresh);
	EXPECT_EQ(AddedRemovedRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(AddedRemovedRefresh.CatalogStats.Reparsed, 1u);
	EXPECT_EQ(AddedRemovedRefresh.CatalogStats.Removed, 1u);
	EXPECT_EQ(Durin::CaptureAssetCatalogSnapshot().Assets.size(), 2u);

	std::filesystem::remove(ContentA / "Gamma.dasset");
	std::filesystem::copy_file(DeltaFixture, ContentA / "Delta.dasset");
	const auto RenamedRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(RenamedRefresh);
	EXPECT_EQ(RenamedRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(RenamedRefresh.CatalogStats.Reparsed, 1u);
	EXPECT_EQ(RenamedRefresh.CatalogStats.Removed, 1u);

	const auto FullRefresh = Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(FullRefresh);
	EXPECT_EQ(FullRefresh.CatalogStats.Reused, 0u);
	EXPECT_EQ(FullRefresh.CatalogStats.Reparsed, 2u);
	EXPECT_EQ(FullRefresh.CatalogStats.HeaderReadAttempts, 2u);
	EXPECT_GT(FullRefresh.CatalogStats.HeaderBytesRead, 0u);
	EXPECT_GE(FullRefresh.CatalogStats.DurationMilliseconds, 0.0);
	EXPECT_EQ(Durin::CaptureAssetCatalogSnapshot().Assets.size(), 2u);

	const std::array<std::byte, 3> CorruptCache = {std::byte{1}, std::byte{2}, std::byte{3}};
	WriteTestBytes(CacheFile, CorruptCache);
	const auto CorruptCacheRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(CorruptCacheRefresh);
	EXPECT_EQ(CorruptCacheRefresh.CatalogStats.Reparsed, 2u);
	EXPECT_FALSE(CorruptCacheRefresh.CatalogCacheWarning.empty());

	Durin::FByteArray IncompatibleCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(IncompatibleCache, CacheFile));
	const uint32 IncompatibleSchema = 99;
	std::memcpy(IncompatibleCache.data() + sizeof(uint32), &IncompatibleSchema, sizeof(IncompatibleSchema));
	WriteTestBytes(CacheFile, IncompatibleCache);
	const auto IncompatibleCacheRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(IncompatibleCacheRefresh);
	EXPECT_EQ(IncompatibleCacheRefresh.CatalogStats.Reparsed, 2u);
	EXPECT_FALSE(IncompatibleCacheRefresh.CatalogCacheWarning.empty());

	std::filesystem::create_directories(ContentB);
	for (const auto& Source : {Alpha, ContentA / "Delta.dasset"})
	{
		const auto Destination = ContentB / Source.filename();
		std::filesystem::copy_file(Source, Destination);
		std::filesystem::last_write_time(Destination, std::filesystem::last_write_time(Source));
	}
	Durin::Testing::RegisterMountPointForTests("/TestAssets/", ContentB.generic_string() + "/");
	const auto RelocatedMountRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(RelocatedMountRefresh);
	EXPECT_EQ(RelocatedMountRefresh.CatalogStats.Reused, 2u);
	EXPECT_EQ(RelocatedMountRefresh.CatalogStats.Reparsed, 0u);

	const auto AdditionalContent = WorkRoot / "AdditionalContent";
	std::filesystem::create_directories(AdditionalContent);
	Durin::Testing::RegisterMountPointForTests("/Additional/", AdditionalContent.generic_string() + "/");
	const auto ManifestRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(ManifestRefresh);
	EXPECT_EQ(ManifestRefresh.CatalogStats.Reparsed, 2u);
	EXPECT_NE(ManifestRefresh.CatalogCacheWarning.find("mount manifest changed"), std::string::npos);

	const auto BlockedCacheRoot = WorkRoot / "BlockedCacheRoot";
	WriteTestBytes(BlockedCacheRoot, CorruptCache);
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	const auto BlockedCacheRefresh = Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(BlockedCacheRefresh);
	EXPECT_EQ(Durin::CaptureAssetCatalogSnapshot().Assets.size(), 2u);
	EXPECT_FALSE(BlockedCacheRefresh.CatalogCacheWarning.empty());
}

TEST(FPackageAssetTests, RegistryDuplicatePathsReadOnlyTheAcceptedReferenceSource)
{
	InitializeAssetTests();
	const auto WorkRoot =
		Durin::Testing::GetTestWorkDirectory() / "RegistryDuplicatePaths";
	const auto RootA = WorkRoot / "RootA";
	const auto RootB = WorkRoot / "RootB";
	const auto CacheRoot = WorkRoot / "DerivedDataCache";
	Durin::Testing::RemoveTestWorkDirectory(WorkRoot);
	std::filesystem::create_directories(RootA / "Nested");
	std::filesystem::create_directories(RootB);

	Durin::FPackagePath SeedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/Nested/Duplicate", SeedPath));
	DPackageAssetForTest* SeedAsset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SeedPath, SeedAsset));
	ASSERT_TRUE(Durin::SavePackage(SeedAsset->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(SeedPath));
	const auto SeedData = Durin::FindAssetExact(SeedPath);
	ASSERT_NE(SeedData, nullptr);
	const std::filesystem::path SeedFile = SeedData->PhysicalPath;
	std::filesystem::copy_file(SeedFile, RootA / "Nested" / "Duplicate.dasset");
	std::filesystem::copy_file(SeedFile, RootB / "Duplicate.dasset");

	const std::array Definitions{
		Durin::FMountPoint{
			.VirtualRoot = "/TestAssets/",
			.Owner = Durin::EMountOwner::Test,
			.Root = RootA,
			.ContentPath = ".",
			.bAutoScan = true,
			.bContentWritable = true},
		Durin::FMountPoint{
			.VirtualRoot = "/TestAssets/Nested/",
			.Owner = Durin::EMountOwner::Test,
			.Root = RootB,
			.ContentPath = ".",
			.bAutoScan = true,
			.bContentWritable = true}};
	Durin::Testing::FScopedMountRegistryFixture Mounts(Definitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	const Durin::FAssetCatalogSnapshot BeforeRefresh =
		Durin::CaptureAssetCatalogSnapshot();
	const Durin::FAssetCatalogRefreshResult Refresh =
		Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation);
	EXPECT_FALSE(Refresh);
	EXPECT_FALSE(Refresh.bCatalogComplete);
	EXPECT_TRUE(Refresh.bRetainedPriorRevision);
	EXPECT_EQ(Refresh.CatalogStats.Enumerated, 2u);
	EXPECT_EQ(Refresh.CatalogStats.Failed, 1u);
	const Durin::FAssetCatalogSnapshot AfterRefresh =
		Durin::CaptureAssetCatalogSnapshot();
	EXPECT_EQ(AfterRefresh.Revision, BeforeRefresh.Revision);
	EXPECT_EQ(AfterRefresh.Assets, BeforeRefresh.Assets);
	EXPECT_TRUE(std::ranges::any_of(
		Refresh.Errors,
		[](const Durin::FAssetRegistryResult& Error) {
			return Error.Error == Durin::EAssetRegistryError::AlreadyExists;
		}));
}

TEST(FPackageAssetTests, PersistentRegistryFlushesSuccessfulMutationsAndIgnoresWriteFailures)
{
	InitializeAssetTests();
	const auto WorkRoot = Durin::Testing::GetTestWorkDirectory() / "RegistryMutationLifecycle";
	const auto ContentRoot = WorkRoot / "Content";
	const auto CacheRoot = WorkRoot / "DerivedDataCache";
	Durin::Testing::RemoveTestWorkDirectory(WorkRoot);
	std::filesystem::create_directories(ContentRoot);
	Durin::Testing::RegisterMountPointForTests("/TestAssets/", ContentRoot.generic_string() + "/");
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	const uint64 EmptyRegistryRevision = Durin::GetAssetCatalogRevision();

	Durin::FPackagePath FirstPath;
	Durin::FPackagePath MovedPath;
	Durin::FPackagePath ImportedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/LifecycleFirst", FirstPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/LifecycleMoved", MovedPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/LifecycleImported", ImportedPath));

	DPackageAssetForTest* FirstAsset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(FirstPath, FirstAsset));
	EXPECT_EQ(Durin::GetAssetCatalogRevision(), EmptyRegistryRevision);
	EXPECT_FALSE(Durin::IsAssetCatalogSnapshotDirtyForTesting());
	EXPECT_EQ(Durin::FindResidentPackage(FirstPath), FirstAsset->GetPackage());
	ASSERT_TRUE(Durin::SavePackage(FirstAsset->GetPackage()));
	EXPECT_GT(Durin::GetAssetCatalogRevision(), EmptyRegistryRevision);
	EXPECT_TRUE(Durin::IsAssetCatalogSnapshotDirtyForTesting());
	ShutdownAssetManagerForRestart();
	EXPECT_FALSE(Durin::IsAssetCatalogSnapshotDirtyForTesting());
	const auto FirstWarmRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(FirstWarmRefresh);
	EXPECT_EQ(FirstWarmRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(FirstWarmRefresh.CatalogStats.Reparsed, 0u);

	Durin::DObject* Reloaded = nullptr;
	const uint64 RevisionBeforeLoad = Durin::GetAssetCatalogRevision();
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(FirstPath), Reloaded));
	EXPECT_EQ(Durin::GetAssetCatalogRevision(), RevisionBeforeLoad);
	EXPECT_FALSE(Durin::IsAssetCatalogSnapshotDirtyForTesting());
	ShutdownAssetManagerForRestart();
	const auto SecondWarmRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(SecondWarmRefresh);
	EXPECT_EQ(SecondWarmRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(SecondWarmRefresh.CatalogStats.Reparsed, 0u);

	ASSERT_TRUE(RelocateAssetForTest(FirstPath, MovedPath));
	EXPECT_TRUE(Durin::IsAssetCatalogSnapshotDirtyForTesting());
	ShutdownAssetManagerForRestart();
	const auto RelocatedRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(RelocatedRefresh);
	EXPECT_EQ(RelocatedRefresh.CatalogStats.Reused, 2u);
	EXPECT_EQ(RelocatedRefresh.CatalogStats.Reparsed, 0u);
	ASSERT_NE(Durin::FindAssetExact(FirstPath), nullptr);
	EXPECT_EQ(Durin::FindAssetExact(FirstPath)->EntryKind,
		Durin::EAssetRegistryEntryKind::Redirector);
	EXPECT_NE(Durin::FindAssetExact(MovedPath), nullptr);

	DPackageAssetForTest* ImportedAsset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ImportedPath, ImportedAsset));
	ASSERT_TRUE(Durin::SavePackage(ImportedAsset->GetPackage()));
	ShutdownAssetManagerForRestart();
	const auto ImportedRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(ImportedRefresh);
	EXPECT_EQ(ImportedRefresh.CatalogStats.Reused, 3u);
	EXPECT_EQ(ImportedRefresh.CatalogStats.Reparsed, 0u);

	ASSERT_TRUE(DeleteAssetClosureForTest({FirstPath, MovedPath}));
	EXPECT_TRUE(Durin::IsAssetCatalogSnapshotDirtyForTesting());
	ShutdownAssetManagerForRestart();
	const auto DeletedRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(DeletedRefresh);
	EXPECT_EQ(DeletedRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(DeletedRefresh.CatalogStats.Reparsed, 0u);
	EXPECT_EQ(Durin::FindAssetExact(MovedPath), nullptr);

	const auto BlockedCacheRoot = WorkRoot / "BlockedCacheRoot";
	const std::array<std::byte, 3> Blocker = {std::byte{1}, std::byte{2}, std::byte{3}};
	WriteTestBytes(BlockedCacheRoot, Blocker);
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	ImportedAsset = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ImportedPath), ImportedAsset));
	ImportedAsset->Value = 42;
	ASSERT_TRUE(Durin::SavePackage(ImportedAsset->GetPackage()));
	const auto AuthoredFile = ContentRoot / "LifecycleImported.dasset";
	Durin::FByteArray BeforeFailedFlush;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeFailedFlush, AuthoredFile));
	ShutdownAssetManagerForRestart();
	EXPECT_TRUE(Durin::IsAssetCatalogSnapshotDirtyForTesting());
	EXPECT_FALSE(Durin::GetAssetCatalogCacheWarningForTesting().empty());
	Durin::FByteArray AfterFailedFlush;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterFailedFlush, AuthoredFile));
	EXPECT_EQ(AfterFailedFlush, BeforeFailedFlush);

	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	ShutdownAssetManagerForRestart();
	EXPECT_FALSE(Durin::IsAssetCatalogSnapshotDirtyForTesting());
}

TEST(FPackageAssetTests, SoftReferenceCacheUsesCheapMetadataAndFullValidationWithoutLoadingTargets)
{
	InitializeAssetTests();
	const auto CacheRoot =
		Durin::Testing::GetTestWorkDirectory() / "SoftReferenceDerivedDataCache";
	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());

	Durin::FPackagePath OwnerPath;
	Durin::FPackagePath TargetAPath;
	Durin::FPackagePath TargetBPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftCacheOwner", OwnerPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftCacheTargetA", TargetAPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TestAssets/SoftCacheTargetB", TargetBPath));
	DPackageAssetForTest* TargetA = nullptr;
	DPackageAssetForTest* TargetB = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(TargetAPath, TargetA));
	ASSERT_TRUE(Durin::SavePackage(TargetA->GetPackage()));
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(TargetBPath, TargetB));
	ASSERT_TRUE(Durin::SavePackage(TargetB->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(TargetAPath));
	Owner->Label.assign(512u * 1024u, 'x');
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::UnloadPackage(TargetAPath));
	ASSERT_TRUE(Durin::UnloadPackage(TargetBPath));

	const auto InitialRefresh = Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(InitialRefresh);
	EXPECT_EQ(Durin::FindResidentPackage(TargetAPath), nullptr);
	EXPECT_EQ(Durin::CaptureAssetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FPackagePath>{TargetAPath}));
	const auto CacheFile = CacheRoot / "AssetRegistry" / "Registry.bin";
	ASSERT_TRUE(std::filesystem::is_regular_file(CacheFile));
	Durin::FByteArray FirstCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstCache, CacheFile
	));

	const auto WarmRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(WarmRefresh);
	Durin::FByteArray SecondCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondCache, CacheFile
	));
	EXPECT_EQ(SecondCache, FirstCache);

	const auto OwnerData = Durin::FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	const std::filesystem::path OwnerFile = OwnerData->PhysicalPath;
	const auto PreservedTime = std::filesystem::last_write_time(OwnerFile);
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(OwnerPath), Owner));
	Owner->Direct.SetPath(MakeFormerMainObjectPath(TargetBPath));
	ASSERT_TRUE(Durin::SavePackage(Owner->GetPackage()));
	std::filesystem::last_write_time(OwnerFile, PreservedTime);
	ASSERT_TRUE(Durin::UnloadPackage(OwnerPath));
	const auto PreservedTimestampRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(PreservedTimestampRefresh);
	EXPECT_EQ(Durin::CaptureAssetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FPackagePath>{TargetAPath}));
	const auto FullRefresh = Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(FullRefresh);
	EXPECT_EQ(Durin::CaptureAssetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FPackagePath>{TargetBPath}));
	EXPECT_EQ(Durin::FindResidentPackage(TargetBPath), nullptr);
	std::filesystem::last_write_time(
		OwnerFile, std::filesystem::last_write_time(OwnerFile) + std::chrono::seconds(2));
	const auto ModifiedTimestampRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(ModifiedTimestampRefresh);

	const std::array<std::byte, 3> CorruptCache = {std::byte{1}, std::byte{2}, std::byte{3}};
	WriteTestBytes(CacheFile, CorruptCache);
	const auto CorruptCacheRefresh = Durin::RefreshAssetRegistry();
	ASSERT_TRUE(CorruptCacheRefresh);
	EXPECT_EQ(Durin::CaptureAssetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FPackagePath>{TargetBPath}));
	EXPECT_EQ(Durin::FindResidentPackage(TargetBPath), nullptr);
	Durin::FByteArray RecoveredCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		RecoveredCache, CacheFile
	));
	EXPECT_NE(RecoveredCache, Durin::FByteArray(CorruptCache.begin(), CorruptCache.end()));
}
