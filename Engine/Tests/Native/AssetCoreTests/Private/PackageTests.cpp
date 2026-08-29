#include <gtest/gtest.h>

#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "AssetCook.h"
#include "Asset/CanonicalResave.h"
#include "Asset/Compatibility.h"
#include "AssetPackageV6Codec.h"
#include "Asset/PackageVersionPolicy.h"
#include "Asset/PackageObjectStreamReader.h"
#include "Asset/PackageObjectStreamWriter.h"
#include "Asset/CanonicalResave.h"
#include "Asset/EditorBulkData.h"
#include "Asset/EditorBulkDataStorage.h"
#include "Asset/Testing.h"
#include "CoreGlobals.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"
#include "Misc/FileTime.h"
#include "Misc/Paths.h"
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
	static_assert(Durin::Asset::AssetPackageObjectStreamVersion ==
		5u);
	static_assert(Durin::Asset::OrdinaryAssetPackageWriterVersion ==
		Durin::Asset::AssetPackageV6FormatVersion);
	static_assert(Durin::Asset::SupportedAssetPackageReaderVersions ==
		decltype(Durin::Asset::SupportedAssetPackageReaderVersions){
			Durin::Asset::AssetPackageV6FormatVersion});
	static_assert(!Durin::Asset::IsSupportedAssetPackageReaderVersion(
		4));
	static_assert(!Durin::Asset::IsSupportedAssetPackageReaderVersion(
		5u));
	static_assert(!Durin::Asset::IsSupportedAssetPackageReaderVersion(
		Durin::Asset::AssetPackageV6FormatVersion + 1));
	static_assert(Durin::Asset::IsSupportedAssetPackageReaderVersion(
		Durin::Asset::AssetPackageV6FormatVersion));

	auto RelocateAssetsForTest(
		std::span<const Durin::Asset::FAssetRelocationMapping> Mappings
	)
		-> Durin::Asset::FAssetResult
	{
		Durin::Asset::FAssetMutationSummary Summary;
		Durin::Asset::FAssetMutationTransaction Transaction;
		Durin::Asset::FAssetResult Result =
			Durin::Asset::PrepareAssetRelocationTransaction(
				Mappings, Summary, Transaction);
		if (Result) Result = Transaction.Commit();
		return Result;
	}

	auto RelocateAssetForTest(
		const Durin::FAssetPath& Source,
		const Durin::FAssetPath& Destination
	)
		-> Durin::Asset::FAssetResult
	{
		const Durin::Asset::FAssetRelocationMapping Mapping{
			Source, Destination
		};
		return RelocateAssetsForTest(std::span{&Mapping, 1});
	}

	auto FixUpRedirectorsForTest(
		std::span<const Durin::FAssetPath> Redirectors,
		Durin::Asset::EAssetRedirectorFixupMode Mode =
			Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete)
		-> Durin::Asset::FAssetResult
	{
		Durin::Asset::FAssetRedirectorFixupSummary Summary;
		Durin::Asset::FAssetMutationTransaction Transaction;
		Durin::Asset::FAssetResult Result =
			Durin::Asset::PrepareRedirectorFixupTransaction(
				Redirectors, Mode, Summary, Transaction);
		return Result ? Transaction.Commit() : Result;
	}

	// Test cleanup follows the production target-plus-alias closure contract while
	// avoiding a second editor-level filesystem transaction in asset tests.
	auto DeleteAssetClosureForTest(
		std::initializer_list<Durin::FAssetPath> Paths
	)
		-> Durin::Asset::FAssetResult
	{
		const std::vector<Durin::FAssetPath> DeletionPaths(Paths);
		Durin::Asset::FAssetDeletionTransaction Transaction;
		std::vector<Durin::Asset::FAssetDeletionBatchBlocker> Blockers;
		Durin::Asset::FAssetResult Result =
			Durin::Asset::PrepareAssetDeletionTransaction(
				DeletionPaths, {}, Transaction, Blockers
			);
		if (!Result) return Result;
		if (!Blockers.empty())
			return {
				Durin::Asset::EAssetError::InUse,
				Blockers.front().Details
			};
		const auto RemoveFiles = [&]() -> Durin::Asset::FAssetResult {
		for (const Durin::Asset::FAssetDeletionBatchEntry& Entry : Transaction.GetEntries())
		{
			std::error_code Error;
			if (!std::filesystem::remove(Entry.RegistryEntry.PhysicalPath, Error)
				|| Error)
				return {
					Durin::Asset::EAssetError::IoError,
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
						Durin::Asset::EAssetError::IoError,
						std::format(
							"Could not remove test companion {}: {}",
							Companion.generic_string(), Error.message()
						)
					};
			}
		}
		return {};
		};
		return Transaction.Commit({
			.Stage = RemoveFiles,
			.Restore = [] { return Durin::Asset::FAssetResult{
				Durin::Asset::EAssetError::IoError,
				"Irreversible test cleanup cannot be restored."}; },
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
					Durin::Asset::FEditorBulkData>(
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

		Durin::Asset::FEditorBulkData Payload;
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
				Durin::SerializeArchiveSoftObjectPath(Ar, SoftReference);
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
		Durin::TObjectPtr<Durin::DObject> HardReference;
		Durin::FSoftObjectPath SoftReference;
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
			Durin::Asset::RegisterAssetDeleteContributor(DPackageAssetForTest::StaticClass(), [](const Durin::Asset::FAssetData&, const Durin::Asset::FAssetPackageInspection& Inspection, Durin::Asset::FAssetDeleteContribution& Out) -> Durin::Asset::FAssetResult {
				const Durin::Asset::FAssetPackageField* LabelField = Inspection.FindField("Label");
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

		Durin::Asset::ShutdownAssetManager();
		Durin::CollectGarbage();
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "Assets";
		Durin::Testing::RemoveTestWorkDirectory(Root);
		const std::filesystem::path DerivedDataRoot =
			Durin::Testing::GetTestWorkDirectory() / "DerivedDataCache";
		Durin::Testing::RemoveTestWorkDirectory(DerivedDataRoot);
		Durin::FPaths::SetDerivedDataCacheDirForTests(
			DerivedDataRoot.generic_string());
		Durin::PathUtilities::RegisterMountPointForTests(
			"/TestAssets/", Root.generic_string() + "/");
		Durin::Asset::SetAssetRelocationFailurePointForTesting(
			Durin::Asset::EAssetRelocationFailurePoint::None);
		Durin::Asset::SetAssetRedirectorFixupFailurePointForTesting(
			Durin::Asset::EAssetRedirectorFixupFailurePoint::None);
		Durin::Asset::InitializeAssetManager();
		if (!Durin::Asset::RefreshAssetRegistry(
			Durin::Asset::EAssetRegistryScanMode::FullValidation))
		{
			throw std::runtime_error(
				"Failed to establish a complete empty asset registry for the test fixture.");
		}
	}

	class FMemoryAssetReferenceStore final
		: public Durin::Asset::IAssetReferenceStore
	{
	public:
		explicit FMemoryAssetReferenceStore(
			Durin::FAssetPath InPath,
			bool bInCookRoot = false,
			std::string InExpectedClass = {}
		)
			: Path(std::move(InPath))
			, ExpectedClass(std::move(InExpectedClass))
			, bCookRoot(bInCookRoot)
		{
		}

		auto CaptureSnapshot(
			Durin::Asset::FAssetReferenceStoreSnapshot& OutSnapshot
		)
			-> Durin::Asset::FAssetResult override
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
			std::span<const Durin::Asset::FAssetReferenceRewrite> Rewrites,
			std::string_view ExpectedFingerprint,
			Durin::Asset::FAssetReferenceStoreRewriteContribution& OutContribution
		)
			-> Durin::Asset::FAssetResult override
		{
			if (ExpectedFingerprint != Path.ToString() || Rewrites.size() != 1
				|| Rewrites.front().StableId != "root"
				|| Rewrites.front().SourcePath != Path)
				return {Durin::Asset::EAssetError::StaleData, "Memory reference store preparation is stale."};
			const Durin::FAssetPath PrePath = Path;
			const Durin::FAssetPath PostPath = Rewrites.front().DestinationPath;
			OutContribution = {
				.Fingerprint = std::string(ExpectedFingerprint),
				.Rewrites = {Rewrites.front()},
				.Revalidate = [this, PrePath] { return Path == PrePath ? Durin::Asset::FAssetResult{} : Durin::Asset::FAssetResult{Durin::Asset::EAssetError::StaleData, "Memory reference store changed."}; },
				.Apply = [this, PostPath] {
					Path = PostPath;
					return Durin::Asset::FAssetResult{}; },
				.Restore = [this, PrePath] {
					Path = PrePath;
					return Durin::Asset::FAssetResult{}; },
				.Verify = [this, PostPath] { return Path == PostPath ? Durin::Asset::FAssetResult{} : Durin::Asset::FAssetResult{Durin::Asset::EAssetError::StaleData, "Memory reference store verification failed."}; }
			};
			return {};
		}

		Durin::FAssetPath Path;
		std::string ExpectedClass;
		bool bCookRoot = false;
	};

	class FScopedReferenceStoreRegistration
	{
	public:
		explicit FScopedReferenceStoreRegistration(
			Durin::Asset::IAssetReferenceStore* Store
		)
			: Handle(Durin::Asset::RegisterAssetReferenceStore(Store))
		{
		}

		~FScopedReferenceStoreRegistration()
		{
			Durin::Asset::UnregisterAssetReferenceStore(Handle);
		}

	private:
		Durin::Asset::FAssetReferenceStoreHandle Handle = 0;
	};

	auto ShutdownAssetManagerForRestart() -> void
	{
		Durin::Asset::ShutdownAssetManager();
		Durin::CollectGarbage();
		Durin::Asset::InitializeAssetManager();
	}

	auto WriteTestBytes(const std::filesystem::path& Path, std::span<const std::byte> Bytes) -> void
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(Stream.is_open());
		Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		ASSERT_TRUE(Stream.good());
	}

	auto RenameSerializedString(
		std::vector<std::byte>& Bytes,
		std::string_view OldValue,
		std::string_view NewValue
	) -> bool
	{
		if (OldValue.size() != NewValue.size()) return false;
		std::vector<std::byte> Pattern(sizeof(uint64) + OldValue.size());
		const uint64 Length = OldValue.size();
		std::memcpy(Pattern.data(), &Length, sizeof(Length));
		std::memcpy(Pattern.data() + sizeof(Length), OldValue.data(), OldValue.size());
		const auto It = std::search(Bytes.begin(), Bytes.end(), Pattern.begin(), Pattern.end());
		if (It == Bytes.end()) return false;
	std::memcpy(std::to_address(It + sizeof(Length)), NewValue.data(), NewValue.size());
		return true;
	}

	auto RenameAllSerializedStrings(
		std::vector<std::byte>& Bytes,
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
		std::vector<std::byte>& Bytes,
		std::string_view OldValue,
		std::string_view NewValue,
		size_t Occurrence
	) -> bool
	{
		if (OldValue.size() != NewValue.size()) return false;
		std::vector<std::byte> Pattern(sizeof(uint64) + OldValue.size());
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

	auto MakeCompatibilityProbeInput(
		const Durin::FAssetPath& PackagePath,
		const std::filesystem::path& PhysicalPath
	)
		-> Durin::Asset::FAssetPackageCompatibilityProbeInput
	{
		std::error_code Error;
		const auto LastWriteTime = std::filesystem::last_write_time(PhysicalPath, Error);
		EXPECT_FALSE(Error);
		std::vector<std::byte> Bytes;
		EXPECT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, PhysicalPath));
		return {
			.PackagePath = PackagePath,
			.PhysicalPath = PhysicalPath.generic_string(),
			.ExpectedFileSize = std::filesystem::file_size(PhysicalPath, Error),
			.ExpectedLastWriteTimeTicks = Durin::FileTime::ToStableTicks(LastWriteTime),
			.ExpectedContentHash = Durin::FXxHash128::HashBuffer(Bytes)};
	}

	auto HexDigit(char Character) -> uint8
	{
		if (Character >= '0' && Character <= '9') return static_cast<uint8>(Character - '0');
		if (Character >= 'A' && Character <= 'F') return static_cast<uint8>(Character - 'A' + 10);
		if (Character >= 'a' && Character <= 'f') return static_cast<uint8>(Character - 'a' + 10);
		ADD_FAILURE() << "Invalid hexadecimal fixture digit.";
		return 0;
	}

	auto ReadCompatibilityFixtureBytes(std::string_view Name) -> std::vector<std::byte>
	{
		std::ifstream Stream(std::filesystem::path(DURIN_TEST_DATA_DIR) / std::format("{}.dasset.hex", Name));
		EXPECT_TRUE(Stream.is_open());
		std::string Hex;
		Stream >> Hex;
		EXPECT_FALSE(Hex.empty());
		EXPECT_EQ(Hex.size() % 2, 0u);
		std::vector<std::byte> Bytes(Hex.size() / 2);
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
		std::vector<std::byte> Bytes(Hex.size() / 2);
		for (size_t Index = 0; Index < Bytes.size(); ++Index)
			Bytes[Index] = static_cast<std::byte>((HexDigit(Hex[Index * 2]) << 4) | HexDigit(Hex[Index * 2 + 1]));
		WriteTestBytes(Destination, Bytes);
	}

	auto RunRedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletionTest()
		-> void;
	auto RunRedirectorFixupVerificationFailureRestoresPackagesStoresAndAliasTest()
		-> void;
	auto RunRedirectorFixupRejectsUnavailableProviderWithoutMutationTest()
		-> void;
	auto RunRedirectorFixupRejectsReadOnlyAndChangedPackageInputsTest()
		-> void;
	auto RunRedirectorFixupPublicationFailuresRestoreAllParticipantsTest()
		-> void;
} // namespace

TEST(FPackageAssetTests, RedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletion)
{
	RunRedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletionTest();
}

TEST(FEditorBulkDataTests, SharesImmutableBytesAndReplacesTransactionally)
{
	const Durin::FGuid PayloadId{1, 2, 3, 4};
	const std::array Initial{std::byte{1}, std::byte{2}, std::byte{3}};
	Durin::Asset::FEditorBulkData First(PayloadId);
	ASSERT_TRUE(First.ReplaceBytes(Initial));
	Durin::Asset::FEditorBulkData Shared = First;
	ASSERT_EQ(First.GetBulkData().GetBytes().data(),
		Shared.GetBulkData().GetBytes().data());
	EXPECT_TRUE(First.Identical(Shared));

	const std::array Replacement{std::byte{9}, std::byte{8}};
	ASSERT_TRUE(Shared.ReplaceBytes(Replacement));
	EXPECT_NE(First.GetBulkData().GetBytes().data(),
		Shared.GetBulkData().GetBytes().data());
	EXPECT_TRUE(std::ranges::equal(First.GetBulkData().GetBytes(), Initial));
	EXPECT_TRUE(std::ranges::equal(Shared.GetBulkData().GetBytes(), Replacement));
	EXPECT_FALSE(First.Identical(Shared));
	EXPECT_FALSE(Shared.ReplaceBytes({}, Replacement));
	EXPECT_TRUE(std::ranges::equal(Shared.GetBulkData().GetBytes(), Replacement));
}

TEST(FEditorBulkDataStorageTests, IsCanonicalBoundedAndRejectsCorruption)
{
	const Durin::FXxHash128 ContainerHash{0x1122334455667788ull, 0x8877665544332211ull};
	const auto MakePayload = [&](Durin::FGuid PayloadId, std::vector<std::byte> Bytes) {
		Durin::Asset::FEditorBulkDataStoragePayload Payload;
		Payload.Buffer = Durin::FSharedByteBuffer::Take(std::move(Bytes));
		Payload.Descriptor = {
			.PayloadId = PayloadId,
			.LogicalByteCount = Payload.Buffer.GetSize(),
			.StoredByteCount = Payload.Buffer.GetSize(),
			.ContentHash = Durin::FXxHash128::HashBuffer(Payload.Buffer.GetBytes()),
			.ContainerHash = ContainerHash,
			.StorageKind = Durin::Asset::EEditorBulkDataStorageKind::External};
		return Payload;
	};
	auto High = MakePayload({2, 0, 0, 0}, {std::byte{4}, std::byte{5}});
	auto Low = MakePayload({1, 0, 0, 0}, {std::byte{1}, std::byte{2}, std::byte{3}});
	std::array Payloads{High, Low};
	std::vector<std::byte> FirstBytes, SecondBytes;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::BuildEditorBulkDataCompanion(
		Payloads, ContainerHash, FirstBytes, &Error)) << Error;
	std::ranges::reverse(Payloads);
	ASSERT_TRUE(Durin::Asset::BuildEditorBulkDataCompanion(
		Payloads, ContainerHash, SecondBytes, &Error)) << Error;
	EXPECT_EQ(FirstBytes, SecondBytes);
	EXPECT_EQ(FirstBytes.size(), 274u);
	ASSERT_GE(FirstBytes.size(), 128u);
	EXPECT_TRUE(std::ranges::equal(std::span(FirstBytes).first(4),
		std::array{std::byte{'D'}, std::byte{'U'}, std::byte{'R'}, std::byte{'F'}}));
	uint32 FormatA = 0, FormatB = 0, FormatC = 0, FormatD = 0, FormatVersion = 0;
	ASSERT_TRUE(Durin::ReadLittleEndianAt(FirstBytes, 8, FormatA));
	ASSERT_TRUE(Durin::ReadLittleEndianAt(FirstBytes, 12, FormatB));
	ASSERT_TRUE(Durin::ReadLittleEndianAt(FirstBytes, 16, FormatC));
	ASSERT_TRUE(Durin::ReadLittleEndianAt(FirstBytes, 20, FormatD));
	ASSERT_TRUE(Durin::ReadLittleEndianAt(FirstBytes, 24, FormatVersion));
	EXPECT_EQ(FormatA, Durin::Asset::DabkBinaryFormatId.A);
	EXPECT_EQ(FormatB, Durin::Asset::DabkBinaryFormatId.B);
	EXPECT_EQ(FormatC, Durin::Asset::DabkBinaryFormatId.C);
	EXPECT_EQ(FormatD, Durin::Asset::DabkBinaryFormatId.D);
	EXPECT_EQ(FormatVersion, 2u);
	EXPECT_TRUE(Durin::Asset::ValidateEditorBulkDataCompanion(
		FirstBytes, ContainerHash, &Error)) << Error;
	auto UnknownFormat = FirstBytes;
	UnknownFormat[8] ^= std::byte{1};
	EXPECT_FALSE(Durin::Asset::ValidateEditorBulkDataCompanion(
		UnknownFormat, ContainerHash, &Error));

	const std::filesystem::path PackagePath =
		Durin::Testing::GetTestWorkDirectory() / "AuthoredBulk/Fixture.dasset";
	std::filesystem::create_directories(PackagePath.parent_path());
	std::filesystem::path CompanionPath;
	ASSERT_TRUE(Durin::Asset::ResolveEditorBulkDataCompanionPath(
		PackagePath, CompanionPath, &Error)) << Error;
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(FirstBytes)), CompanionPath.generic_string()));
	Durin::FSharedByteBuffer Loaded;
	ASSERT_TRUE(Durin::Asset::ReadEditorBulkDataStoragePayload(
		CompanionPath, Low.Descriptor, Loaded, &Error)) << Error;
	EXPECT_TRUE(std::ranges::equal(Loaded.GetBytes(), Low.Buffer.GetBytes()));

	std::vector<std::byte> Truncated = FirstBytes;
	Truncated.pop_back();
	EXPECT_FALSE(Durin::Asset::ValidateEditorBulkDataCompanion(
		Truncated, ContainerHash, &Error));
	std::vector<std::byte> Corrupt = FirstBytes;
	Corrupt.back() ^= std::byte{1};
	EXPECT_FALSE(Durin::Asset::ValidateEditorBulkDataCompanion(
		Corrupt, ContainerHash, &Error));
	std::vector<std::byte> NonzeroPayloadPadding = FirstBytes;
	NonzeroPayloadPadding[260] = std::byte{1};
	EXPECT_FALSE(Durin::Asset::ValidateEditorBulkDataCompanion(
		NonzeroPayloadPadding, ContainerHash, &Error));
	std::array Duplicate{Low, Low};
	EXPECT_FALSE(Durin::Asset::BuildEditorBulkDataCompanion(
		Duplicate, ContainerHash, Corrupt, &Error));
	EXPECT_FALSE(Durin::Asset::ValidateEditorBulkDataCompanion(
		FirstBytes, Durin::FXxHash128{1, 1}, &Error));
}

TEST(FEditorBulkDataStorageTests, LiveLoadRecoversStableCompanionByDescriptorHash)
{
	const Durin::FXxHash128 OldHash{11, 12};
	const Durin::FXxHash128 NewHash{21, 22};
	const Durin::FGuid PayloadId{1, 2, 3, 4};
	const auto MakePayload = [&](Durin::FXxHash128 ContainerHash, std::byte Value) {
		Durin::Asset::FEditorBulkDataStoragePayload Payload;
		Payload.Buffer = Durin::FSharedByteBuffer::Take(
			std::vector<std::byte>{Value, Value});
		Payload.Descriptor = {
			.PayloadId = PayloadId,
			.LogicalByteCount = Payload.Buffer.GetSize(),
			.StoredByteCount = Payload.Buffer.GetSize(),
			.ContentHash = Durin::FXxHash128::HashBuffer(Payload.Buffer.GetBytes()),
			.ContainerHash = ContainerHash,
			.StorageKind = Durin::Asset::EEditorBulkDataStorageKind::External};
		return Payload;
	};
	const auto OldPayload = MakePayload(OldHash, std::byte{0x31});
	const auto NewPayload = MakePayload(NewHash, std::byte{0x42});
	std::vector<std::byte> OldBytes;
	std::vector<std::byte> NewBytes;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::BuildEditorBulkDataCompanion(
		std::span{&OldPayload, 1}, OldHash, OldBytes, &Error)) << Error;
	ASSERT_TRUE(Durin::Asset::BuildEditorBulkDataCompanion(
		std::span{&NewPayload, 1}, NewHash, NewBytes, &Error)) << Error;

	const std::filesystem::path PackagePath =
		Durin::Testing::GetTestWorkDirectory() / "AuthoredBulk/Recovery.dasset";
	std::filesystem::create_directories(PackagePath.parent_path());
	std::filesystem::path CompanionPath;
	ASSERT_TRUE(Durin::Asset::ResolveEditorBulkDataCompanionPath(
		PackagePath, CompanionPath, &Error)) << Error;
	EXPECT_EQ(CompanionPath.filename(), "Recovery.dabulk");
	std::filesystem::path BackupPath = CompanionPath;
	BackupPath += Durin::Asset::EditorBulkDataCompanionBackupSuffix;
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(NewBytes, CompanionPath.generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(OldBytes, BackupPath.generic_string()));

	Durin::FSharedByteBuffer Loaded;
	EXPECT_FALSE(Durin::Asset::ReadEditorBulkDataStoragePayload(
		CompanionPath, OldPayload.Descriptor, Loaded, &Error));
	EXPECT_TRUE(std::filesystem::is_regular_file(BackupPath));
	ASSERT_TRUE(Durin::Asset::LoadEditorBulkDataStoragePayload(
		CompanionPath, OldPayload.Descriptor, Loaded, &Error)) << Error;
	EXPECT_TRUE(std::ranges::equal(Loaded.GetBytes(), OldPayload.Buffer.GetBytes()));
	EXPECT_FALSE(std::filesystem::exists(BackupPath));
	std::vector<std::byte> RecoveredBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(RecoveredBytes, CompanionPath));
	EXPECT_TRUE(Durin::Asset::ValidateEditorBulkDataCompanion(
		RecoveredBytes, OldHash, &Error)) << Error;

	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(NewBytes, BackupPath.generic_string()));
	ASSERT_TRUE(Durin::Asset::LoadEditorBulkDataStoragePayload(
		CompanionPath, OldPayload.Descriptor, Loaded, &Error)) << Error;
	EXPECT_FALSE(std::filesystem::exists(BackupPath));

	NewBytes.back() ^= std::byte{1};
	OldBytes.back() ^= std::byte{1};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(NewBytes, CompanionPath.generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(OldBytes, BackupPath.generic_string()));
	EXPECT_FALSE(Durin::Asset::LoadEditorBulkDataStoragePayload(
		CompanionPath, OldPayload.Descriptor, Loaded, &Error));
	EXPECT_TRUE(std::filesystem::is_regular_file(CompanionPath));
	EXPECT_TRUE(std::filesystem::is_regular_file(BackupPath));
}

TEST(FPackageAssetTests, V6FirstBundleFailureRemovesUncommittedStableCompanion)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/V6FirstFailure", Path));
	DBulkPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	std::vector<std::byte> Payload(
		static_cast<size_t>(Durin::Asset::EditorBulkDataExternalThreshold + 1),
		std::byte{0x51});
	ASSERT_TRUE(Asset->Payload.ReplaceBytes(Payload));
	Durin::DPackage* Packages[] = {Asset->GetPackage()};
	const Durin::Asset::FAssetResult Result = Durin::Asset::SavePackagesAtomically(
		Packages,
		{.RootPackage = Asset->GetPackage(),
			.ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
				return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRegistry;
			}});
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::IoError);
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "Assets";
	EXPECT_FALSE(std::filesystem::exists(Root / "V6FirstFailure.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "V6FirstFailure.dabulk"));
	EXPECT_FALSE(std::filesystem::exists(
		Root / "V6FirstFailure.dabulk.durin-backup"));
}

TEST(FPackageAssetTests, OrdinaryV6PublishesLoadsAndRollsBackExternalClosure)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/V6ExternalClosure", Path));
	DBulkPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	std::vector<std::byte> Payload(
		static_cast<size_t>(Durin::Asset::EditorBulkDataExternalThreshold + 17),
		std::byte{0x5a});
	ASSERT_TRUE(Asset->Payload.ReplaceBytes(Payload));

	const Durin::Asset::FAssetResult V6Save = Durin::Asset::SavePackage(
		Asset->GetPackage());
	ASSERT_TRUE(V6Save) << V6Save.Message;
	const Durin::Asset::FAssetCatalogEntry V6Data =
		Durin::Asset::FindAssetExact(Path);
	ASSERT_TRUE(V6Data);
	EXPECT_EQ(V6Data->FormatVersion, Durin::Asset::AssetPackageV6FormatVersion);
	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(V6Data->PhysicalPath, Inspection));
	EXPECT_EQ(Inspection.Header.FormatVersion,
		Durin::Asset::AssetPackageV6FormatVersion);
	std::vector<Durin::Asset::FEditorBulkDataStorageDescriptor> Descriptors;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::InspectEditorBulkDataStorageDescriptors(
		Inspection, Descriptors, &Error)) << Error;
	ASSERT_EQ(Descriptors.size(), 1u);
	EXPECT_EQ(Descriptors.front().StorageKind,
		Durin::Asset::EEditorBulkDataStorageKind::External);
	std::vector<std::filesystem::path> Companions;
	ASSERT_TRUE(Durin::Asset::InspectEditorBulkDataCompanionPaths(
		V6Data->PhysicalPath, Inspection, Companions, &Error)) << Error;
	ASSERT_EQ(Companions.size(), 1u);
	EXPECT_TRUE(std::filesystem::is_regular_file(Companions.front()));
	EXPECT_EQ(Companions.front().filename(), "V6ExternalClosure.dabulk");
	std::filesystem::path BackupPath = Companions.front();
	BackupPath += Durin::Asset::EditorBulkDataCompanionBackupSuffix;
	EXPECT_FALSE(std::filesystem::exists(BackupPath));
	const std::filesystem::path LegacyCompanion =
		Companions.front().parent_path() / "V6ExternalClosure.legacy.dabulk";
	std::filesystem::copy_file(
		Companions.front(), LegacyCompanion,
		std::filesystem::copy_options::overwrite_existing);
	std::filesystem::copy_file(
		Companions.front(), BackupPath,
		std::filesystem::copy_options::overwrite_existing);
	std::vector<std::filesystem::path> Orphans;
	ASSERT_TRUE(Durin::Asset::InspectOrphanedEditorBulkDataCompanionPaths(
		V6Data->PhysicalPath, Inspection, Orphans, &Error)) << Error;
	EXPECT_TRUE(Orphans.empty());
	EXPECT_TRUE(std::filesystem::remove(LegacyCompanion));

	Durin::FSharedByteBuffer LoadedPayload;
	ASSERT_TRUE(Durin::Asset::ReadEditorBulkDataStoragePayload(
		Companions.front(), Descriptors.front(), LoadedPayload, &Error)) << Error;
	EXPECT_TRUE(std::ranges::equal(
		LoadedPayload.GetBytes(), Payload));
	EXPECT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	EXPECT_FALSE(std::filesystem::exists(BackupPath));

	std::vector<std::byte> BeforeFailedReplacement;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		BeforeFailedReplacement, Companions.front()));
	std::ranges::fill(Payload, std::byte{0x63});
	ASSERT_TRUE(Asset->Payload.ReplaceBytes(Payload));
	Durin::DPackage* ReplacementUnit[] = {Asset->GetPackage()};
	const Durin::Asset::FAssetResult FailedReplacement =
		Durin::Asset::SavePackagesAtomically(ReplacementUnit,
			{.RootPackage = Asset->GetPackage(),
				.ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
					return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRegistry;
				}});
	EXPECT_EQ(FailedReplacement.Error, Durin::Asset::EAssetError::IoError);
	std::vector<std::byte> AfterFailedReplacement;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AfterFailedReplacement, Companions.front()));
	EXPECT_EQ(AfterFailedReplacement, BeforeFailedReplacement);
	EXPECT_FALSE(std::filesystem::exists(BackupPath));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	std::vector<std::byte> AfterCommittedReplacement;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AfterCommittedReplacement, Companions.front()));
	EXPECT_NE(AfterCommittedReplacement, BeforeFailedReplacement);
	EXPECT_FALSE(std::filesystem::exists(BackupPath));

	Durin::FAssetPath LivePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/V6LiveLoad", LivePath));
	DPackageAssetForTest* LiveAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(LivePath, LiveAsset));
	LiveAsset->Value = 73;
	ASSERT_TRUE(Durin::Asset::SavePackage(LiveAsset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(LivePath));
	DPackageAssetForTest* Reloaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(LivePath, Reloaded));
	EXPECT_EQ(Reloaded->Value, 73);
	const Durin::Asset::FAssetCatalogEntry LiveData =
		Durin::Asset::FindAssetExact(LivePath);
	ASSERT_TRUE(LiveData);
	EXPECT_EQ(Durin::Asset::FindAssetExact(LivePath)->FormatVersion,
		Durin::Asset::AssetPackageV6FormatVersion);
}

TEST(FPackageAssetTests, V6BundleAndRelocationPreserveCurrentFormat)
{
	InitializeAssetTests();
	Durin::FAssetPath SourcePath;
	Durin::FAssetPath DestinationPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/V6MoveSource", SourcePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/V6MoveDestination", DestinationPath));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Asset));
	Asset->Value = 91;
	Durin::DPackage* Packages[] = {Asset->GetPackage()};
	ASSERT_TRUE(Durin::Asset::SavePackagesAtomically(Packages,
		{.RootPackage = Asset->GetPackage()}));
	ASSERT_EQ(Durin::Asset::FindAssetExact(SourcePath)->FormatVersion,
		Durin::Asset::AssetPackageV6FormatVersion);

	const Durin::Asset::FAssetRelocationMapping Mapping{
		SourcePath, DestinationPath};
	ASSERT_TRUE(RelocateAssetsForTest(std::span(&Mapping, 1)));
	const Durin::Asset::FAssetCatalogEntry Redirector =
		Durin::Asset::FindAssetExact(SourcePath);
	const Durin::Asset::FAssetCatalogEntry Moved =
		Durin::Asset::FindAssetExact(DestinationPath);
	ASSERT_TRUE(Redirector);
	ASSERT_TRUE(Moved);
	EXPECT_EQ(Redirector->FormatVersion,
		Durin::Asset::AssetPackageV6FormatVersion);
	EXPECT_EQ(Moved->FormatVersion, Durin::Asset::AssetPackageV6FormatVersion);
	EXPECT_EQ(Redirector->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Redirector);
	DPackageAssetForTest* Resolved = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(SourcePath, Resolved));
	EXPECT_EQ(Resolved->Value, 91);
}

TEST(FPackageAssetTests, RelocationAndDeletionOwnStableAuthoredCompanion)
{
	InitializeAssetTests();
	Durin::FAssetPath SourcePath;
	Durin::FAssetPath DestinationPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/StableBulkMoveSource", SourcePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/StableBulkMoveDestination", DestinationPath));
	DBulkPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Asset));
	std::vector<std::byte> Payload(
		static_cast<size_t>(Durin::Asset::EditorBulkDataExternalThreshold + 3),
		std::byte{0x71});
	ASSERT_TRUE(Asset->Payload.ReplaceBytes(Payload));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));

	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "Assets";
	const std::filesystem::path SourceCompanion =
		Root / "StableBulkMoveSource.dabulk";
	const std::filesystem::path DestinationCompanion =
		Root / "StableBulkMoveDestination.dabulk";
	ASSERT_TRUE(std::filesystem::is_regular_file(SourceCompanion));
	const Durin::Asset::FAssetResult Relocated =
		RelocateAssetForTest(SourcePath, DestinationPath);
	ASSERT_TRUE(Relocated) << Relocated.Message;
	EXPECT_FALSE(std::filesystem::exists(SourceCompanion));
	EXPECT_TRUE(std::filesystem::is_regular_file(DestinationCompanion));
	ASSERT_TRUE(DeleteAssetClosureForTest({SourcePath, DestinationPath}));
	EXPECT_FALSE(std::filesystem::exists(DestinationCompanion));
}

TEST(FPackageAssetTests, RedirectorFixupVerificationFailureRestoresPackagesStoresAndAlias)
{
	RunRedirectorFixupVerificationFailureRestoresPackagesStoresAndAliasTest();
}

TEST(FPackageAssetTests, RedirectorFixupRejectsUnavailableProviderWithoutMutation)
{
	RunRedirectorFixupRejectsUnavailableProviderWithoutMutationTest();
}

TEST(FPackageAssetTests, RedirectorFixupPublicationFailuresRestoreAllParticipants)
{
	RunRedirectorFixupPublicationFailuresRestoreAllParticipantsTest();
}

TEST(FPackageAssetTests, RedirectorFixupRewriteOnlyReportsRetainedAlias)
{
	InitializeAssetTests();
	Durin::FAssetPath OldPath;
	Durin::FAssetPath NewPath;
	Durin::FAssetPath OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/FixupRewriteOnlyOld", OldPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/FixupRewriteOnlyNew", NewPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/FixupRewriteOnlyOwner", OwnerPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->Direct.SetPath(OldPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));

	Durin::Asset::FAssetRedirectorFixupSummary Summary;
	Durin::Asset::FAssetMutationTransaction Transaction;
	ASSERT_TRUE(Durin::Asset::PrepareRedirectorFixupTransaction(
		std::span{&OldPath, 1},
		Durin::Asset::EAssetRedirectorFixupMode::RewriteOnly,
		Summary,
		Transaction));
	EXPECT_TRUE(Summary.GetDeletableRedirectors().empty());
	ASSERT_TRUE(Transaction.Commit());
	const Durin::Asset::FAssetMutationResultDetails Details =
		Transaction.GetLastResultDetails();
	EXPECT_EQ(Details.RewrittenPaths, std::vector{OwnerPath});
	EXPECT_EQ(Details.RetainedPaths, std::vector{OldPath});
	EXPECT_TRUE(Details.DeletedPaths.empty());
	ASSERT_NE(Durin::Asset::FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Transaction.Undo().Error, Durin::Asset::EAssetError::StaleData);
	EXPECT_EQ(Transaction.GetState(),
		Durin::Asset::EAssetMutationTransactionState::Committed);
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(OwnerPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
}

TEST(FPackageAssetTests, HeaderReaderStopsBeforeLargeObjectPayload)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LargeHeaderOnly", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Scores.resize(100000, 7);
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "LargeHeaderOnly.dasset";
	ASSERT_GT(std::filesystem::file_size(File), 8u * 1024u);

	Durin::Asset::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::Asset::ReadAssetPackageHeader(File.generic_string(), Header));
	EXPECT_EQ(Header.AssetClassName, "Tests::DPackageAssetForTest");
	EXPECT_EQ(Header.FormatVersion, Durin::Asset::AssetPackageV6FormatVersion);
	EXPECT_EQ(Header.EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_FALSE(Header.RedirectDestination.IsValid());
	EXPECT_EQ(Header.ObjectCount, 2u);
	EXPECT_LT(Header.BytesRead, 1024u);
	EXPECT_EQ(Header.FileBytesRead, Header.BytesRead);
	EXPECT_LT(Header.FileBytesRead, std::filesystem::file_size(File));
}

TEST(FPackageAssetTests, OrdinaryWriterEmitsDurfV1Prefix)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DurfV1Prefix", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));

	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DurfV1Prefix.dasset";
	std::vector<std::byte> Bytes;
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
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, PackageCodecPolicyIsCompleteUniqueAndIndependentOfWireVersion)
{
	InitializeAssetTests();
	std::string Error;
	EXPECT_TRUE(Durin::Asset::ValidateAssetPackageVersionPolicy(Error)) << Error;
	EXPECT_NE(Durin::Asset::AssetPackageReaderPolicyFingerprint,
		Durin::Asset::AssetPackageV6FormatVersion);
	EXPECT_TRUE(Durin::Asset::DastBinaryFormatId.IsValid());
	EXPECT_EQ(Durin::Asset::DastBinaryFormatId,
		(Durin::FGuid{0x3c59d1a9, 0x6ceb4e4c, 0xb059452d, 0xb0a5af56}));
	EXPECT_EQ(Durin::Asset::DastBinaryFormatName, "Durin.BinaryFormat.DAST");

	const auto& V6 = Durin::Asset::Private::DastV6::GetCodec();
	std::array DuplicateKeys{V6, V6};
	DuplicateKeys[1].CodecId = "dast-v6-alias";
	EXPECT_FALSE(Durin::Asset::Private::ValidateAssetPackageCodecTable(DuplicateKeys, Error));
	std::ranges::reverse(DuplicateKeys);
	EXPECT_FALSE(Durin::Asset::Private::ValidateAssetPackageCodecTable(DuplicateKeys, Error));

	std::array DuplicateNames{V6, V6};
	DuplicateNames[1].FormatVersion = Durin::Asset::AssetPackageV6FormatVersion + 1;
	EXPECT_FALSE(Durin::Asset::Private::ValidateAssetPackageCodecTable(DuplicateNames, Error));
	std::ranges::reverse(DuplicateNames);
	EXPECT_FALSE(Durin::Asset::Private::ValidateAssetPackageCodecTable(DuplicateNames, Error));

	std::array Incomplete{V6};
	Incomplete[0].Validate = nullptr;
	EXPECT_FALSE(Durin::Asset::Private::ValidateAssetPackageCodecTable(Incomplete, Error));
	std::array InvalidVersion{V6};
	InvalidVersion[0].FormatVersion = 0;
	EXPECT_FALSE(Durin::Asset::Private::ValidateAssetPackageCodecTable(InvalidVersion, Error));
}

TEST(FPackageAssetTests, V6HeaderReadCharacterizationRemainsBounded)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/V6HeaderCost", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "V6HeaderCost.dasset";
	std::vector<std::byte> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File));
	const auto* Reader = Durin::Asset::Private::FindAssetPackageReader(
		Durin::Asset::AssetPackageV6FormatVersion);
	ASSERT_NE(Reader, nullptr);

	constexpr size_t Iterations = 2000;
	const auto Begin = std::chrono::steady_clock::now();
	uint64 LastBytesRead = 0;
	for (size_t Index = 0; Index < Iterations; ++Index)
	{
		Durin::Asset::FAssetPackageHeader Header;
		ASSERT_TRUE(Reader->ReadHeader(Bytes, Bytes.size(), Header));
		LastBytesRead = Header.BytesRead;
	}
	const double Microseconds = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - Begin).count() / Iterations;
	testing::Test::RecordProperty("v6_file_bytes", Bytes.size());
	testing::Test::RecordProperty("v6_header_bytes_read", LastBytesRead);
	testing::Test::RecordProperty("v6_header_parse_us", Microseconds);
	EXPECT_LT(Microseconds, 500.0);
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, EnvelopeDispatchUsesPermanentIdentityAndFailsBeforeCodec)
{
	using namespace Durin;
	using namespace Durin::Asset;
	using namespace Durin::Asset::Private;
	constexpr FBinaryEnvelopeLimits Limits{16ull * 1024ull * 1024ull,
		1024ull * 1024ull * 1024ull};

	std::array<std::byte, BinaryEnvelopePreambleBytes> V6{};
	const FBinaryEnvelopePreamble V6Preamble{
		.FormatId = DastBinaryFormatId,
		.FormatVersion = AssetPackageV6FormatVersion,
		.HeaderBytes = V6.size(),
		.FileBytes = V6.size()};
	ASSERT_TRUE(EncodeBinaryEnvelopePreamble(V6Preamble, V6));
	ASSERT_TRUE(FinalizeBinaryEnvelopeHeader(V6, V6.size(), Limits));

	const FAssetPackageCodec* Codec = reinterpret_cast<const FAssetPackageCodec*>(1);
	uint32 FormatVersion = 0;
	const FAssetResult Resolved = ResolveAssetPackageReader(V6, Codec, &FormatVersion);
	EXPECT_TRUE(Resolved);
	ASSERT_NE(Codec, nullptr);
	EXPECT_EQ(Codec->FormatVersion, AssetPackageV6FormatVersion);
	EXPECT_EQ(FormatVersion, AssetPackageV6FormatVersion);

	std::array<std::byte, 8> Legacy{
		std::byte{0x44}, std::byte{0x41}, std::byte{0x53}, std::byte{0x54},
		std::byte{0x05}, std::byte{}, std::byte{}, std::byte{}};
	EXPECT_EQ(ResolveAssetPackageReader(Legacy, Codec).Error,
		EAssetError::UnsupportedVersion);

	std::array<std::byte, BinaryEnvelopePreambleBytes> Unknown{};
	const FBinaryEnvelopePreamble UnknownPreamble{
		.FormatId = {1, 2, 3, 4},
		.FormatVersion = AssetPackageV6FormatVersion,
		.HeaderBytes = Unknown.size(),
		.FileBytes = Unknown.size()};
	ASSERT_TRUE(EncodeBinaryEnvelopePreamble(UnknownPreamble, Unknown));
	ASSERT_TRUE(FinalizeBinaryEnvelopeHeader(Unknown, Unknown.size(), Limits));
	EXPECT_EQ(ResolveAssetPackageReader(Unknown, Codec).Error, EAssetError::UnsupportedVersion);

	V6[48] ^= std::byte{1};
	EXPECT_EQ(ResolveAssetPackageReader(V6, Codec).Error, EAssetError::CorruptFile);
}

TEST(FPackageAssetTests, V6CodecMatchesLiveWriteInspectReferenceMutationAndLoadSemantics)
{
	InitializeAssetTests();
	using namespace Durin;
	using namespace Durin::Asset;
	using namespace Durin::Asset::Private;
	FAssetPath TargetPath;
	FAssetPath ReplacementPath;
	FAssetPath SourcePath;
	FAssetPath RelocatedPath;
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/V6Target", TargetPath));
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/V6Replacement", ReplacementPath));
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/V6Source", SourcePath));
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/V6Relocated", RelocatedPath));
	DPackageAssetForTest* Target = nullptr;
	DPackageAssetForTest* Source = nullptr;
	ASSERT_TRUE(CreateAsset(TargetPath, Target));
	ASSERT_TRUE(SavePackage(Target->GetPackage()));
	ASSERT_TRUE(CreateAsset(SourcePath, Source));
	Source->Value = 417;
	Source->ExternalReference = Target;
	ASSERT_TRUE(SavePackage(Source->GetPackage()));

	const FAssetCatalogEntry SourceData = FindAssetExact(SourcePath);
	ASSERT_TRUE(SourceData);
	std::vector<std::byte> V6;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(V6, SourceData->PhysicalPath));
	const FAssetPackageCodec& Codec = DastV6::GetCodec();
	ASSERT_TRUE(Codec.Validate(V6));
	FAssetPackageHeader Header;
	uint64 HeaderByteCount = 0;
	ASSERT_TRUE(Durin::ReadLittleEndianAt(V6, 32, HeaderByteCount));
	ASSERT_TRUE(Codec.ReadHeader(
		std::span(V6).first(static_cast<size_t>(HeaderByteCount)), V6.size(), Header));
	EXPECT_EQ(Header.FormatVersion, AssetPackageV6FormatVersion);
	EXPECT_EQ(Header.ObjectCount, 2);
	EXPECT_EQ(Header.Dependencies, std::vector{TargetPath});
	FAssetPackageInspection Inspection;
	ASSERT_TRUE(Codec.Inspect(V6, Inspection));
	EXPECT_EQ(Inspection.Header.FormatVersion, AssetPackageV6FormatVersion);
	EXPECT_EQ(Inspection.Fingerprint.ReaderVersion, AssetPackageV6FormatVersion);

	std::vector<FAssetReferenceEdge> References;
	ASSERT_TRUE(Codec.ExtractReferences(V6, SourcePath, References));
	ASSERT_EQ(References.size(), 2);
	EXPECT_EQ(std::ranges::count(References, TargetPath, &FAssetReferenceEdge::TargetPath), 1);
	const FReflectionCompatibilityCatalog Catalog = FReflectionCompatibilityCatalog::Capture();
	FAssetPackageCompatibilityRecord Compatibility;
	ASSERT_TRUE(Codec.ProbeCompatibility(
		V6, SourcePath, Catalog, Compatibility, nullptr));
	EXPECT_EQ(Compatibility.FormatVersion, AssetPackageV6FormatVersion);

	std::vector<std::byte> DirectWrite;
	ASSERT_TRUE(Codec.Write(Source->GetPackage(), DirectWrite,
		EDefaultDeltaMode::NoDelta, {}));
	EXPECT_EQ(DirectWrite, V6);
	std::vector<std::byte> Relocated;
	ASSERT_TRUE(Codec.Relocate(V6, RelocatedPath, Relocated));
	ASSERT_TRUE(Codec.Validate(Relocated));
	const FAssetRedirectorFixupMapping Mapping{TargetPath, ReplacementPath};
	std::vector<std::byte> Rewritten;
	ASSERT_TRUE(Codec.RewriteReferences(V6, std::span(&Mapping, 1), 1, Rewritten));
	References.clear();
	ASSERT_TRUE(Codec.ExtractReferences(Rewritten, SourcePath, References));
	ASSERT_EQ(References.size(), 2);
	EXPECT_EQ(std::ranges::count(
		References, ReplacementPath, &FAssetReferenceEdge::TargetPath), 1);

	std::vector<std::byte> Redirector;
	ASSERT_TRUE(Codec.WriteRedirector(SourcePath, TargetPath, Redirector));
	ASSERT_TRUE(Codec.ReadHeader(Redirector, Redirector.size(), Header));
	EXPECT_EQ(Header.EntryKind, EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Header.RedirectDestination, TargetPath);

	ASSERT_TRUE(UnloadPackage(SourcePath));
	DPackage* Loaded = nullptr;
	FAssetLoadReport Report;
	ASSERT_TRUE(Codec.Load(V6, SourcePath, Loaded, &Report, {}, {}));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->HasAnyObjectFlags(EObjectFlags::Standalone));
	EXPECT_FALSE(Loaded->HasAnyInternalFlags(EObjectInternalFlags::RootSet));
	auto* LoadedAsset = static_cast<DPackageAssetForTest*>(Loaded->GetAsset());
	ASSERT_NE(LoadedAsset, nullptr);
	EXPECT_EQ(LoadedAsset->Value, 417);
	EXPECT_EQ(LoadedAsset->ExternalReference, Target);
	MarkObjectHierarchyAsGarbage(Loaded);
	CollectGarbage();
	ASSERT_TRUE(DeleteAssetClosureForTest({SourcePath, TargetPath}));
}

TEST(FPackageAssetTests, V6PreservesExternalPayloadDirectoryAndCompanionDescriptor)
{
	InitializeAssetTests();
	using namespace Durin;
	using namespace Durin::Asset;
	using namespace Durin::Asset::Private;
	FAssetPath Path;
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/V6ExternalPayload", Path));
	DBulkPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(CreateAsset(Path, Asset));
	std::vector<std::byte> Payload(
		static_cast<size_t>(EditorBulkDataExternalThreshold + 17), std::byte{0x6b});
	ASSERT_TRUE(Asset->Payload.ReplaceBytes(Payload));
	ASSERT_TRUE(SavePackage(Asset->GetPackage()));
	const FAssetCatalogEntry Data = FindAssetExact(Path);
	ASSERT_TRUE(Data);
	std::vector<std::byte> V6;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(V6, Data->PhysicalPath));
	DastV6::FParsedPackage Parsed;
	std::string Error;
	ASSERT_TRUE(DastV6::ParsePackage(V6, Parsed, &Error)) << Error;
	ASSERT_EQ(Parsed.PayloadEntries.size(), 1);
	FAssetPackageInspection Inspection;
	ASSERT_TRUE(DastV6::GetCodec().Inspect(V6, Inspection));
	std::vector<FEditorBulkDataStorageDescriptor> Descriptors;
	ASSERT_TRUE(InspectEditorBulkDataStorageDescriptors(
		Inspection, Descriptors, &Error)) << Error;
	ASSERT_EQ(Descriptors.size(), 1);
	EXPECT_EQ(Descriptors.front().PayloadId, Parsed.PayloadEntries.front().PayloadId);
	EXPECT_EQ(Descriptors.front().ContentHash, Parsed.PayloadEntries.front().ContentHash);
	EXPECT_EQ(Descriptors.front().ContainerHash, Parsed.PayloadEntries.front().ContainerHash);
	EXPECT_EQ(Descriptors.front().StoredByteCount,
		Parsed.PayloadEntries.front().StoredByteCount);
	ASSERT_TRUE(DeleteAssetClosureForTest({Path}));
}

TEST(FPackageAssetTests, HeaderReaderRejectsMalformedAndUnboundedDeclarations)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/HeaderValidationSource", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const auto Root = Durin::Testing::GetTestWorkDirectory() / "Assets";
	const auto Source = Root / "HeaderValidationSource.dasset";
	std::vector<std::byte> Valid;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Valid, Source));
	ASSERT_GT(Valid.size(), 16u);
	Durin::Asset::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::Asset::ReadAssetPackageHeader(Source.generic_string(), Header));

	auto Truncated = std::span<const std::byte>(Valid).first(4);
	const auto TruncatedFile = Root / "HeaderTruncated.dasset";
	WriteTestBytes(TruncatedFile, Truncated);
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(TruncatedFile.generic_string(), Header).Error, Durin::Asset::EAssetError::CorruptFile);

	auto Corrupt = Valid;
	Corrupt[0] ^= std::byte{0xff};
	const auto CorruptFile = Root / "HeaderCorrupt.dasset";
	WriteTestBytes(CorruptFile, Corrupt);
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(CorruptFile.generic_string(), Header).Error, Durin::Asset::EAssetError::CorruptFile);

	for (const uint32 Version : {3u, 4u, 7u})
	{
		auto Unsupported = Valid;
		std::memcpy(Unsupported.data() + sizeof(uint32), &Version, sizeof(Version));
		const auto UnsupportedFile = Root / std::format("HeaderUnsupported{}.dasset", Version);
		WriteTestBytes(UnsupportedFile, Unsupported);
		EXPECT_EQ(
			Durin::Asset::ReadAssetPackageHeader(UnsupportedFile.generic_string(), Header).Error,
			Durin::Asset::EAssetError::CorruptFile);
		EXPECT_EQ(
			Durin::Asset::ValidateAssetPackageBytes(Unsupported).Error,
			Durin::Asset::EAssetError::CorruptFile);
	}

}

TEST(FPackageAssetTests, RedirectorsRoundTripAndResolveWithoutLoading)
{
	InitializeAssetTests();
	Durin::FAssetPath TargetPath, AliasPath, NormalizedAliasPath, MissingPath,
		UnregisteredPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectRoundTripTarget", TargetPath
	));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectRoundTripAlias", AliasPath
	));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectNormalizedAlias", NormalizedAliasPath
	));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectMissingTarget", MissingPath
	));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/DoesNotExist", UnregisteredPath
	));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetPath, Target));
	Target->Value = 99;
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));

	Durin::Asset::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirectorForTesting(
		AliasPath, TargetPath, Redirector
	));
	ASSERT_NE(Redirector, nullptr);
	EXPECT_EQ(Redirector->GetDestinationObject(), Target);
	ASSERT_TRUE(Durin::Asset::SavePackage(Redirector->GetPackage()));
	const auto AliasFile = Durin::Testing::GetTestWorkDirectory()
						   / "Assets" / "RedirectRoundTripAlias.dasset";
	Durin::Asset::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::Asset::ReadAssetPackageHeader(
		AliasFile.generic_string(), Header
	));
	EXPECT_EQ(Header.FormatVersion, Durin::Asset::AssetPackageV6FormatVersion);
	EXPECT_EQ(Header.AssetClassName, "Durin::Asset::DAssetRedirector");
	EXPECT_EQ(Header.EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Header.RedirectDestination, TargetPath);
	EXPECT_EQ(Header.Dependencies, (std::vector<Durin::FAssetPath>{TargetPath}));
	EXPECT_EQ(Header.ObjectCount, 1u);
	EXPECT_LT(Header.BytesRead, std::filesystem::file_size(AliasFile));

	Durin::Asset::DAssetRedirector* Normalized = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirectorForTesting(
		NormalizedAliasPath, AliasPath, Normalized
	));
	ASSERT_NE(Normalized, nullptr);
	EXPECT_EQ(Normalized->GetDestinationObject(), Target);
	ASSERT_TRUE(Durin::Asset::SavePackage(Normalized->GetPackage()));
	const auto NormalizedFile = Durin::Testing::GetTestWorkDirectory()
								/ "Assets" / "RedirectNormalizedAlias.dasset";
	ASSERT_TRUE(Durin::Asset::ReadAssetPackageHeader(
		NormalizedFile.generic_string(), Header
	));
	EXPECT_EQ(Header.RedirectDestination, TargetPath);
	EXPECT_EQ(Durin::Asset::CreateAssetRedirectorForTesting(MissingPath, MissingPath, Redirector).Error, Durin::Asset::EAssetError::InvalidPath);
	EXPECT_EQ(Durin::Asset::CreateAssetRedirectorForTesting(MissingPath, UnregisteredPath, Redirector).Error, Durin::Asset::EAssetError::NotFound);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(NormalizedAliasPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AliasPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetPath));
	ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
		Durin::Asset::EAssetRegistryScanMode::FullValidation
	));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(AliasPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(TargetPath), nullptr);
	Durin::Asset::FAssetCatalogEntry Exact =
		Durin::Asset::FindAssetExact(AliasPath);
	ASSERT_NE(Exact, nullptr);
	EXPECT_EQ(Exact->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Exact->RedirectDestination, TargetPath);
	const auto Reverse = Durin::Asset::FindRedirectorsTo(TargetPath);
	EXPECT_EQ(Reverse, (std::vector<Durin::FAssetPath>{NormalizedAliasPath, AliasPath}));
	const Durin::Asset::FAssetPathResolveResult Resolved =
		Durin::Asset::ResolveAssetPath(AliasPath);
	ASSERT_TRUE(Resolved);
	EXPECT_EQ(Resolved.CatalogRevision, Durin::Asset::GetAssetCatalogRevision());
	EXPECT_EQ(Resolved.RequestedPath, AliasPath);
	EXPECT_EQ(Resolved.FinalPath, TargetPath);
	EXPECT_EQ(Resolved.RedirectChain, (std::vector<Durin::FAssetPath>{AliasPath}));
	ASSERT_TRUE(Resolved.FinalAssetData.has_value());
	EXPECT_EQ(Resolved.FinalAssetData->PackagePath, TargetPath);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(AliasPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(TargetPath), nullptr);

	ShutdownAssetManagerForRestart();
	const Durin::Asset::FAssetCatalogRefreshResult RestartRefresh =
		Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(RestartRefresh);
	EXPECT_GE(RestartRefresh.CatalogStats.Reused, 2u);
	EXPECT_EQ(RestartRefresh.CatalogStats.Redirectors, 2u);
	Exact = Durin::Asset::FindAssetExact(AliasPath);
	ASSERT_NE(Exact, nullptr);
	EXPECT_EQ(Exact->RedirectDestination, TargetPath);
	const auto RegistryCache = std::filesystem::path(
								   Durin::FPaths::DerivedDataCacheDir()
							   )
							   / "AssetRegistry" / "Registry.bin";
	const std::array<std::byte, 3> CorruptCache = {std::byte{1}, std::byte{2}, std::byte{3}};
	WriteTestBytes(RegistryCache, CorruptCache);
	const Durin::Asset::FAssetCatalogRefreshResult CacheRecoveryRefresh =
		Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(CacheRecoveryRefresh);
	EXPECT_FALSE(CacheRecoveryRefresh.CatalogCacheWarning.empty());
	Exact = Durin::Asset::FindAssetExact(AliasPath);
	ASSERT_NE(Exact, nullptr);
	EXPECT_EQ(Exact->RedirectDestination, TargetPath);
	DPackageAssetForTest* RedirectedTarget = nullptr;
	Durin::Asset::FAssetLoadReport RedirectedReport;
	ASSERT_TRUE(Durin::Asset::LoadAsset(
		AliasPath, RedirectedTarget, &RedirectedReport));
	ASSERT_NE(RedirectedTarget, nullptr);
	EXPECT_EQ(RedirectedReport.RequestedPath, AliasPath);
	EXPECT_EQ(RedirectedReport.FinalPath, TargetPath);
	EXPECT_EQ(RedirectedReport.PackagePath, TargetPath);
	EXPECT_EQ(RedirectedReport.CatalogRevision, Durin::Asset::GetAssetCatalogRevision());
	EXPECT_EQ(RedirectedReport.RedirectChain,
		(std::vector<Durin::FAssetPath>{AliasPath}));
	EXPECT_EQ(RedirectedReport.FinalAssetClassName,
		DPackageAssetForTest::StaticClass()->GetQualifiedName().ToString());
	EXPECT_EQ(RedirectedReport.Error, Durin::Asset::EAssetError::None);
	EXPECT_EQ(RedirectedReport.PackageFileReadCount, 1u);
	EXPECT_EQ(RedirectedTarget->GetPackage()->GetPackagePath(), TargetPath.ToString());
	EXPECT_EQ(Durin::Asset::FindResidentPackage(AliasPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(TargetPath), RedirectedTarget->GetPackage());
	Redirector = nullptr;
	Durin::Asset::FAssetLoadReport WrongTypeReport;
	EXPECT_EQ(
		Durin::Asset::LoadAsset(AliasPath, Redirector, &WrongTypeReport).Error,
		Durin::Asset::EAssetError::TypeMismatch
	);
	EXPECT_EQ(Redirector, nullptr);
	EXPECT_EQ(WrongTypeReport.RequestedPath, AliasPath);
	EXPECT_EQ(WrongTypeReport.FinalPath, TargetPath);
	EXPECT_EQ(WrongTypeReport.Error, Durin::Asset::EAssetError::TypeMismatch);
	EXPECT_EQ(WrongTypeReport.PackageFileReadCount, 0u);

	EXPECT_EQ(
		Durin::Asset::DeleteAssetForTesting(AliasPath).Error,
		Durin::Asset::EAssetError::InUse
	);
	ASSERT_TRUE(DeleteAssetClosureForTest(
		{NormalizedAliasPath, AliasPath, TargetPath}
	));
}

TEST(FPackageAssetTests, AuthoredArchiveFreezesNativeFieldsReferencesAndFailures)
{
	InitializeAssetTests();
	Durin::FAssetPath SourcePath;
	Durin::FAssetPath TargetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AuthoredArchive", SourcePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AuthoredArchiveTarget", TargetPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetPath, Target));
	DAuthoredArchiveAssetForTest* Source = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Source));
	Source->NativeValue = 0x12345678;
	Source->HardReference = Target;
	ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate(
		"/TestAssets/AuthoredArchiveSoftOnly", Source->SoftReference));

	GAuthoredArchivePurposes.clear();
	GAuthoredArchiveFormatVersions.clear();
	std::vector<std::byte> FirstBytes;
	std::vector<std::byte> SecondBytes;
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(Source->GetPackage(), FirstBytes));
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(Source->GetPackage(), SecondBytes));
	EXPECT_EQ(FirstBytes, SecondBytes);
	EXPECT_EQ(std::ranges::count(GAuthoredArchivePurposes, Durin::EArchivePurpose::Discovery), 4);
	EXPECT_EQ(std::ranges::count(GAuthoredArchivePurposes, Durin::EArchivePurpose::AuthoredPackage), 4);
	EXPECT_TRUE(std::ranges::all_of(GAuthoredArchiveFormatVersions, [](uint32 Version) {
		return Version == Durin::Asset::AssetPackageObjectStreamVersion;
	}));

	const auto File = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "AuthoredArchiveInspection.dasset";
	WriteTestBytes(File, FirstBytes);
	const uint64 ConstructCountBeforeTools = GAuthoredConstructCount;
	const size_t SerializeCountBeforeTools = GAuthoredArchivePurposes.size();
	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(File.generic_string(), Inspection));
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
	Durin::Asset::FAssetPackageObjectReference HardReference;
	ASSERT_TRUE(HardField->TryReadObjectReference(HardReference));
	EXPECT_EQ(HardReference.Kind, Durin::Asset::EAssetPackageObjectReferenceKind::External);
	EXPECT_EQ(HardReference.ExternalPath, TargetPath);
	ASSERT_NE(Inspection.FindField("SoftReference"), nullptr);
	EXPECT_EQ(GAuthoredConstructCount, ConstructCountBeforeTools);
	EXPECT_EQ(GAuthoredArchivePurposes.size(), SerializeCountBeforeTools);

	auto ExpectAtomicFailure = [&](auto Configure, Durin::Asset::EAssetError ExpectedError) {
		Source->bSkipSuper = false;
		Source->bDuplicateField = false;
		Source->bLateField = false;
		Source->bUnsupportedCustomVersion = false;
		Configure();
		std::vector<std::byte> Sentinel{std::byte{9}, std::byte{8}, std::byte{7}};
		const Durin::Asset::FAssetResult Result =
			Durin::Asset::SerializeAssetPackageBytes(Source->GetPackage(), Sentinel);
		EXPECT_EQ(Result.Error, ExpectedError);
		EXPECT_EQ(Sentinel, (std::vector<std::byte>{std::byte{9}, std::byte{8}, std::byte{7}}));
	};
	ExpectAtomicFailure([&] { Source->bSkipSuper = true; },
		Durin::Asset::EAssetError::UnsupportedProperty);
	ExpectAtomicFailure([&] { Source->bDuplicateField = true; },
		Durin::Asset::EAssetError::UnsupportedProperty);
	ExpectAtomicFailure([&] { Source->bLateField = true; },
		Durin::Asset::EAssetError::UnsupportedProperty);
	ExpectAtomicFailure([&] { Source->bUnsupportedCustomVersion = true; },
		Durin::Asset::EAssetError::UnsupportedVersion);
}

TEST(FPackageAssetTests, PerSaveOverridesOwnValuesOmitFieldsAndPreserveLiveState)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SaveOverrides", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Value = 17;
	Asset->Label = "LiveLabel";
	const bool bDirtyBefore = Asset->GetPackage()->IsDirty();
	Durin::FAssetPath ForeignPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/ForeignSaveOverride", ForeignPath));
	DPackageAssetForTest* Foreign = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ForeignPath, Foreign));

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
	auto Overrides = std::make_shared<Durin::Asset::FObjectSaveOverrides>();
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
	Durin::Asset::FObjectSaveOverrides TypeMismatchOverrides;
	EXPECT_FALSE(TypeMismatchOverrides.AddPropertyValue(
		*Asset, *ValueProperty, WrongType, &Error));
	const uint32 SameSizeWrongType = 91;
	EXPECT_FALSE(TypeMismatchOverrides.AddPropertyValue(
		*Asset, *ValueProperty, SameSizeWrongType, &Error));

	Durin::Asset::FAssetPackageSerializationOptions Options;
	Options.SaveOverrides = Overrides;
	std::vector<std::byte> ObjectStream;
	std::vector<std::byte> FirstBytes;
	std::vector<std::byte> SecondBytes;
	const Durin::Asset::FAssetResult FirstResult = Durin::Asset::PackageObjectStream::WriteAssetPackage(
		Asset->GetPackage(), ObjectStream,
		{.DeltaMode = Durin::EDefaultDeltaMode::Enabled,
			.Serialization = Options,
			.bVerifyRepeatedEncoding = true});
	ASSERT_TRUE(FirstResult) << FirstResult.Message;
	ASSERT_TRUE(Durin::Asset::Private::DastV6::BuildPackageFromObjectStream(
		ObjectStream, FirstBytes));
	const Durin::Asset::FAssetResult SecondResult = Durin::Asset::SerializeAssetPackageBytes(
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
	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(File.generic_string(), Inspection));
	ASSERT_EQ(Inspection.Objects.size(), 1u);
	ASSERT_EQ(Inspection.Header.Dependencies.size(), 1u);
	EXPECT_EQ(Inspection.Header.Dependencies.front(), ForeignPath);
	const Durin::Asset::FAssetPackageField* ValueField = Inspection.FindField("Value");
	ASSERT_NE(ValueField, nullptr);
	int32 SavedValue = 0;
	ASSERT_TRUE(ValueField->TryReadScalar(SavedValue));
	EXPECT_EQ(SavedValue, Replacement);
	EXPECT_EQ(Inspection.FindField("Label"), nullptr);

	Durin::FProperty* ForeignValueProperty = Foreign->GetClass()->FindPropertyByName("Value");
	ASSERT_NE(ForeignValueProperty, nullptr);
	auto ForeignOverrides = std::make_shared<Durin::Asset::FObjectSaveOverrides>();
	ASSERT_TRUE(ForeignOverrides->AddPropertyValue(
		*Foreign, *ForeignValueProperty, Replacement, &Error));
	Durin::Asset::FAssetPackageSerializationOptions ForeignOptions;
	ForeignOptions.SaveOverrides = std::move(ForeignOverrides);
	std::vector<std::byte> Sentinel{std::byte{1}, std::byte{2}};
	const Durin::Asset::FAssetResult ForeignResult =
		Durin::Asset::SerializeAssetPackageBytes(Asset->GetPackage(), Sentinel, ForeignOptions);
	EXPECT_FALSE(ForeignResult);
	EXPECT_EQ(Sentinel, (std::vector<std::byte>{std::byte{1}, std::byte{2}}));
	EXPECT_EQ(Asset->Value, 17);
	EXPECT_EQ(Asset->Label, "LiveLabel");
	EXPECT_EQ(Asset->GetPackage()->IsDirty(), bDirtyBefore);

	Durin::FAssetPath SoftOwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftSaveOverride", SoftOwnerPath));
	DSoftPackageAssetForTest* SoftOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SoftOwnerPath, SoftOwner));
	Durin::FProperty* DirectProperty = SoftOwner->GetClass()->FindPropertyByName("Direct");
	ASSERT_NE(DirectProperty, nullptr);
	DSoftPackageAssetForTest::FSoftReference ReplacementSoft(ForeignPath);
	auto SoftOverrides = std::make_shared<Durin::Asset::FObjectSaveOverrides>();
	ASSERT_TRUE(SoftOverrides->AddPropertyValue(
		*SoftOwner, *DirectProperty, ReplacementSoft, &Error)) << Error;
	Durin::Asset::FAssetPackageSerializationOptions SoftOptions;
	SoftOptions.SaveOverrides = std::move(SoftOverrides);
	std::vector<std::byte> SoftBytes;
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(
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
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RoundTrip", Path));

	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Value = 42;
	Asset->Label = "RoundTrip";
	Asset->DisplayName = Durin::FName("RoundTripName");
	Asset->PersistentId = Durin::FGuid(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff);
	Asset->RelatedIds = {Durin::FGuid(1, 2, 3, 4), Durin::FGuid(5, 6, 7, 8)};
	Asset->Scores = {3, 5, 8};
	Asset->NamedScores = {{"Alpha", 11}, {"Beta", 17}};
	ASSERT_NE(Asset->DefaultChild.Get(), nullptr);
	EXPECT_EQ(Asset->DefaultChild->GetObjectPath(), "/TestAssets/RoundTrip:DefaultChild");

	Durin::DPackage* Package = Asset->GetPackage();
	ASSERT_NE(Package, nullptr);
	EXPECT_TRUE(Package->HasAnyObjectFlags(Durin::EObjectFlags::Standalone));
	EXPECT_FALSE(Package->HasAnyInternalFlags(
		Durin::EObjectInternalFlags::RootSet));
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), Package);
	ASSERT_TRUE(Durin::Asset::SavePackage(Package));
	ASSERT_TRUE(Durin::Asset::FindAssetExact(Path));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	DPackageAssetForTest* Loaded = nullptr;
	Durin::Asset::FAssetLoadReport LoadReport;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded, &LoadReport));
	EXPECT_EQ(LoadReport.PackageFileReadCount, 1u);
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->GetPackage()->HasAnyObjectFlags(
		Durin::EObjectFlags::Standalone));
	EXPECT_FALSE(Loaded->GetPackage()->HasAnyInternalFlags(
		Durin::EObjectInternalFlags::RootSet));
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), Loaded->GetPackage());
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
	EXPECT_EQ(Loaded->DefaultChild->GetObjectPath(), "/TestAssets/RoundTrip:DefaultChild");
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), Loaded->GetPackage());
	DPackageAssetForTest* Cached = nullptr;
	Durin::Asset::FAssetLoadReport CachedReport;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Cached, &CachedReport));
	EXPECT_EQ(Cached, Loaded);
	EXPECT_EQ(CachedReport.PackageFileReadCount, 0u);

	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, EditorOnlyInnerObjectPersistsInspectsAndPrunesForCook)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/EditorOnlyInnerObject", Path));
	DImportMetadataOwnerForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Owner));
	auto* ImportData = Durin::NewObject<DReplayImportMetadataForTest>(
		Owner, "AssetImportData");
	ImportData->SchemaVersion = 7;
	ImportData->SourcePath = "/TestSources/EditorOnlyInnerObject.png";
	ImportData->Translator = "Tests.EditorOnlyTranslator";
	ImportData->Fingerprint = 0x123456789abcdef0ull;
	Owner->AssetImportData = ImportData;
	Owner->RuntimeValue = 91;
	const std::string ExpectedObjectPath = ImportData->GetObjectPath();

	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	const Durin::Asset::FAssetCatalogEntry Data = Durin::Asset::FindAssetExact(Path);
	ASSERT_TRUE(Data);
	const uint64 ConstructionCountBeforeInspection =
		GImportMetadataConstructionCount;
	Durin::Asset::FAssetPackageInspection AuthoredInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		Data->PhysicalPath, AuthoredInspection));
	EXPECT_EQ(GImportMetadataConstructionCount, ConstructionCountBeforeInspection);
	ASSERT_EQ(AuthoredInspection.Objects.size(), 2u);
	const auto* ReferenceField = AuthoredInspection.FindField("AssetImportData");
	ASSERT_NE(ReferenceField, nullptr);
	Durin::Asset::FAssetPackageObjectReference Reference;
	ASSERT_TRUE(ReferenceField->TryReadObjectReference(Reference));
	ASSERT_EQ(Reference.Kind,
		Durin::Asset::EAssetPackageObjectReferenceKind::Internal);
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

	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	DImportMetadataOwnerForTest* LoadedOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, LoadedOwner));
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

	Durin::Asset::FAssetPackageSerializationOptions CookOptions;
	CookOptions.Domain = Durin::Asset::EAssetPackageSaveDomain::Cooked;
	CookOptions.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64;
	CookOptions.TargetProfile = Durin::Asset::ECookTargetProfile::Game;
	std::vector<std::byte> CookedBytes;
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(
		LoadedOwner->GetPackage(), CookedBytes, CookOptions));
	const auto CookedFile = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "EditorOnlyInnerObjectCooked.dasset";
	WriteTestBytes(CookedFile, CookedBytes);
	Durin::Asset::FAssetPackageInspection CookedInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
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
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, CoreRegisteredPackageIsResidentWithoutAssetLayerAdoption)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/CoreRegisteredPackage", Path));
	Durin::DPackage* Package = Durin::CreatePackage(Path);
	ASSERT_NE(Package, nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), Package);
	EXPECT_FALSE(Package->IsNewlyCreated());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		Package, Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
}

TEST(FPackageAssetTests, SoftObjectResolveAndLoadPreservePathAcrossResidencyChanges)
{
	InitializeAssetTests();
	Durin::FAssetPath Path, AliasPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftObjectTarget", Path));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftObjectAlias", AliasPath));

	const uint64 CatalogRevisionBeforeDraft =
		Durin::Asset::GetAssetCatalogRevision();
	DPackageAssetForTest* Created = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Created));
	EXPECT_EQ(Durin::Asset::GetAssetCatalogRevision(), CatalogRevisionBeforeDraft);
	EXPECT_FALSE(Durin::Asset::FindAssetExact(Path));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), Created->GetPackage());
	EXPECT_TRUE(Created->GetPackage()->IsNewlyCreated());
	DPackageAssetForTest* DraftLoad = nullptr;
	const Durin::Asset::FAssetResult DraftLoadResult =
		Durin::Asset::LoadAsset(Path, DraftLoad);
	EXPECT_TRUE(DraftLoadResult);
	EXPECT_EQ(DraftLoad, Created);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), Created->GetPackage());
	Durin::TSoftObjectPtr<DPackageAssetForTest> UnpublishedReference(Path);
	const auto UnpublishedResolve =
		Durin::Asset::ResolveSoftObject(UnpublishedReference);
	EXPECT_TRUE(UnpublishedResolve);
	EXPECT_EQ(UnpublishedResolve.State, Durin::Asset::ESoftObjectResolveState::Loaded);
	EXPECT_EQ(UnpublishedResolve.Object, Created);
	ASSERT_TRUE(Durin::Asset::SavePackage(Created->GetPackage()));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), Created->GetPackage());
	EXPECT_FALSE(Created->GetPackage()->IsNewlyCreated());
	EXPECT_TRUE(Durin::Asset::FindAssetExact(Path));
	EXPECT_GT(Durin::Asset::GetAssetCatalogRevision(), CatalogRevisionBeforeDraft);
	Durin::Asset::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirectorForTesting(AliasPath, Path, Redirector));
	ASSERT_TRUE(Durin::Asset::SavePackage(Redirector->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AliasPath));

	Durin::TSoftObjectPtr<DPackageAssetForTest> Reference(AliasPath);
	auto Resolved = Durin::Asset::ResolveSoftObject(Reference);
	ASSERT_TRUE(Resolved);
	EXPECT_EQ(Resolved.State, Durin::Asset::ESoftObjectResolveState::Loaded);
	EXPECT_EQ(Resolved.Object, Created);
	EXPECT_TRUE(Resolved.bRedirected);
	EXPECT_EQ(Resolved.ResolvedPath, Path);
	EXPECT_EQ(Reference.Get(), Created);
	EXPECT_EQ(Reference.GetSoftObjectPath().GetAssetPath(), AliasPath);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(AliasPath), nullptr);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	EXPECT_EQ(Reference.Get(), nullptr);
	EXPECT_EQ(Reference.GetSoftObjectPath().GetAssetPath(), AliasPath);
	Resolved = Durin::Asset::ResolveSoftObject(Reference);
	ASSERT_TRUE(Resolved);
	EXPECT_EQ(Resolved.State, Durin::Asset::ESoftObjectResolveState::NotLoaded);
	EXPECT_EQ(Resolved.Object, nullptr);
	EXPECT_TRUE(Resolved.bRedirected);
	EXPECT_EQ(Resolved.ResolvedPath, Path);

	DPackageAssetForTest* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadSoftObject(Reference, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Reference.Get(), Loaded);
	EXPECT_EQ(Reference.GetSoftObjectPath().GetAssetPath(), AliasPath);
	EXPECT_EQ(Loaded->GetPackage()->GetPackagePath(), Path.ToString());
	Durin::FAssetPath OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/SoftRedirectOwner", OwnerPath
	));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->Direct.SetPath(AliasPath);
	DPackageAssetForTest* CachedForOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadSoftObject(Owner->Direct, CachedForOwner));
	EXPECT_EQ(CachedForOwner, Loaded);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));

	Durin::TSoftObjectPtr<DCodecSourceAsset> WrongType(AliasPath);
	auto WrongResolve = Durin::Asset::ResolveSoftObject(WrongType);
	EXPECT_FALSE(WrongResolve);
	EXPECT_EQ(WrongResolve.Result.Error, Durin::Asset::EAssetError::TypeMismatch);
	DCodecSourceAsset* WrongLoaded = nullptr;
	EXPECT_EQ(
		Durin::Asset::LoadSoftObject(WrongType, WrongLoaded).Error,
		Durin::Asset::EAssetError::TypeMismatch
	);
	EXPECT_EQ(WrongLoaded, nullptr);
	EXPECT_EQ(WrongType.GetSoftObjectPath().GetAssetPath(), AliasPath);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	WrongResolve = Durin::Asset::ResolveSoftObject(WrongType);
	EXPECT_FALSE(WrongResolve);
	EXPECT_EQ(WrongResolve.Result.Error, Durin::Asset::EAssetError::TypeMismatch);
	EXPECT_EQ(Reference.Get(), nullptr);
	ASSERT_TRUE(Durin::Asset::LoadSoftObject(Reference, Loaded));
	EXPECT_EQ(Reference.Get(), Loaded);
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
	Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, Owner));
	ASSERT_NE(Owner, nullptr);
	EXPECT_EQ(Owner->Direct.GetSoftObjectPath().GetAssetPath(), AliasPath);
	EXPECT_FALSE(Owner->Direct.IsLoaded());
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(OwnerPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({AliasPath, Path}));
}

TEST(FPackageAssetTests, ResidentUnloadRequiresExplicitUnsavedDiscard)
{
	InitializeAssetTests();
	Durin::FAssetPath NewlyCreatedPath;
	Durin::FAssetPath PublishedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/ResidentNewlyCreated", NewlyCreatedPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/ResidentPublished", PublishedPath));

	DPackageAssetForTest* NewlyCreated = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(NewlyCreatedPath, NewlyCreated));
	EXPECT_EQ(
		Durin::Asset::UnloadPackage(NewlyCreatedPath).Error,
		Durin::Asset::EAssetError::InUse);
	NewlyCreated->GetPackage()->ClearDirty();
	EXPECT_EQ(
		Durin::Asset::UnloadPackage(NewlyCreatedPath).Error,
		Durin::Asset::EAssetError::InUse);
	EXPECT_EQ(
		Durin::Asset::FindResidentPackage(NewlyCreatedPath),
		NewlyCreated->GetPackage());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		NewlyCreatedPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(NewlyCreatedPath), nullptr);
	EXPECT_FALSE(Durin::Asset::FindAssetExact(NewlyCreatedPath));

	DPackageAssetForTest* Published = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(PublishedPath, Published));
	Published->Value = 17;
	ASSERT_TRUE(Durin::Asset::SavePackage(Published->GetPackage()));
	Published->Value = 29;
	Published->MarkPackageDirty();
	EXPECT_EQ(
		Durin::Asset::UnloadPackage(PublishedPath).Error,
		Durin::Asset::EAssetError::InUse);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		PublishedPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Durin::Asset::FindAssetExact(PublishedPath));

	Published = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(PublishedPath, Published));
	ASSERT_NE(Published, nullptr);
	EXPECT_EQ(Published->Value, 17);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(PublishedPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(PublishedPath));
}

TEST(FPackageAssetTests, SoftObjectNullAndMissingPoliciesReturnStableResults)
{
	InitializeAssetTests();
	Durin::TSoftObjectPtr<DPackageAssetForTest> NullReference;
	auto RejectedNull = Durin::Asset::ResolveSoftObject(NullReference);
	EXPECT_FALSE(RejectedNull);
	EXPECT_EQ(RejectedNull.Result.Error, Durin::Asset::EAssetError::InvalidPath);
	EXPECT_EQ(RejectedNull.State, Durin::Asset::ESoftObjectResolveState::Null);

	auto AllowedNull = Durin::Asset::ResolveSoftObject(
		NullReference, Durin::Asset::ESoftObjectNullPolicy::Allow
	);
	EXPECT_TRUE(AllowedNull);
	EXPECT_EQ(AllowedNull.State, Durin::Asset::ESoftObjectResolveState::Null);
	DPackageAssetForTest* NullObject = reinterpret_cast<DPackageAssetForTest*>(1);
	EXPECT_TRUE(Durin::Asset::LoadSoftObject(
		NullReference, NullObject, Durin::Asset::ESoftObjectNullPolicy::Allow
	));
	EXPECT_EQ(NullObject, nullptr);

	Durin::FSoftObjectPath MissingPath;
	ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate("/TestAssets/MissingSoftObject", MissingPath));
	Durin::TSoftObjectPtr<DPackageAssetForTest> MissingReference(MissingPath);
	const Durin::FSoftObjectPath OriginalPath = MissingReference.GetSoftObjectPath();
	auto MissingResolve = Durin::Asset::ResolveSoftObject(MissingReference);
	ASSERT_FALSE(MissingResolve);
	EXPECT_EQ(MissingResolve.Result.Error, Durin::Asset::EAssetError::NotFound);
	EXPECT_EQ(MissingResolve.State, Durin::Asset::ESoftObjectResolveState::NotLoaded);

	DPackageAssetForTest* MissingObject = nullptr;
	EXPECT_EQ(
		Durin::Asset::LoadSoftObject(MissingReference, MissingObject).Error,
		Durin::Asset::EAssetError::NotFound
	);
	EXPECT_EQ(MissingObject, nullptr);
	EXPECT_EQ(MissingReference.GetSoftObjectPath(), OriginalPath);
	EXPECT_FALSE(MissingReference.IsLoaded());
}

TEST(FPackageAssetTests, SoftArchiveUsesBoundedPathOnlyPayloadsTransactionally)
{
	InitializeAssetTests();
	Durin::FAssetPath OwnerPath;
	Durin::FAssetPath TargetPath;
	Durin::FAssetPath SentinelPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftArchiveOwner", OwnerPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftArchiveTarget", TargetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftArchiveSentinel", SentinelPath));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->Direct.SetPath(TargetPath);
	auto* Property = static_cast<Durin::FSoftObjectProperty*>(
		DSoftPackageAssetForTest::StaticClass()->FindPropertyByName(
			Durin::FName("Direct"), false
		)
	);
	ASSERT_NE(Property, nullptr);

	std::vector<std::byte> Bytes;
	Durin::FMemoryWriter Writer(Bytes);
	Durin::SerializeReflectedPropertyValue(Writer, *Property, Owner);
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
	ASSERT_EQ(Bytes.front(), std::byte{1});
	Owner->Direct.SetPath(SentinelPath);
	Durin::FMemoryReader Reader(Bytes);
	Durin::SerializeReflectedPropertyValue(Reader, *Property, Owner);
	ASSERT_FALSE(Reader.HasError()) << Reader.GetError();
	EXPECT_EQ(Owner->Direct.GetSoftObjectPath().GetAssetPath(), TargetPath);
	EXPECT_FALSE(Owner->Direct.IsLoaded());

	std::vector<std::byte> OversizedBytes;
	Durin::FMemoryWriter OversizedWriter(OversizedBytes);
	uint8 ReferenceKind = 1;
	uint64 OversizedPathSize = 1024 * 1024 + 1;
	OversizedWriter << ReferenceKind << OversizedPathSize;
	Owner->Direct.SetPath(SentinelPath);
	Durin::FMemoryReader OversizedReader(OversizedBytes);
	Durin::SerializeReflectedPropertyValue(OversizedReader, *Property, Owner);
	ASSERT_TRUE(OversizedReader.HasError());
	EXPECT_NE(OversizedReader.GetError().find("1 MiB"), std::string_view::npos);
	EXPECT_EQ(Owner->Direct.GetSoftObjectPath().GetAssetPath(), SentinelPath);
	OversizedReader.SetError("must remain sticky");
	EXPECT_NE(OversizedReader.GetError().find("1 MiB"), std::string_view::npos);

	std::vector<std::byte> NullBytes{std::byte{0}};
	Durin::FMemoryReader NullReader(NullBytes);
	Durin::SerializeReflectedPropertyValue(NullReader, *Property, Owner);
	ASSERT_FALSE(NullReader.HasError());
	EXPECT_TRUE(Owner->Direct.IsNull());
}

TEST(FPackageAssetTests, DastSoftFieldsRoundTripWithoutHardDependenciesOrTargetLoads)
{
	InitializeAssetTests();
	Durin::FAssetPath OwnerPath;
	Durin::FAssetPath TargetPath;
	Durin::FAssetPath MissingPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftStage3Owner", OwnerPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftStage3Target", TargetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftStage3Missing", MissingPath));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	ASSERT_TRUE(Owner->Direct.TrySetObject(Target));
	Owner->Fixed[0].SetPath(MissingPath);
	Owner->Array = {
		DSoftPackageAssetForTest::FSoftReference(TargetPath),
		DSoftPackageAssetForTest::FSoftReference(MissingPath)
	};
	Owner->Map.emplace(
		"hero", DSoftPackageAssetForTest::FSoftReference(TargetPath)
	);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));

	const Durin::Asset::FAssetCatalogEntry OwnerData =
		Durin::Asset::FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	EXPECT_TRUE(OwnerData->Dependencies.empty());
	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		OwnerData->PhysicalPath, Inspection
	));
	const Durin::Asset::FAssetPackageField* DirectField =
		Inspection.FindField("Direct");
	ASSERT_NE(DirectField, nullptr);
	EXPECT_EQ(
		DirectField->TypeSignature,
		"SoftObject:Tests::DPackageAssetForTest:v1"
	);
	const Durin::Asset::FReflectionCompatibilityCatalog Catalog =
		Durin::Asset::FReflectionCompatibilityCatalog::Capture();
	const auto* CatalogClass = Catalog.FindClass("Tests::DSoftPackageAssetForTest");
	ASSERT_NE(CatalogClass, nullptr);
	const auto* CatalogField = Catalog.FindField(
		*CatalogClass, "Tests::DSoftPackageAssetForTest", "Direct"
	);
	ASSERT_NE(CatalogField, nullptr);
	EXPECT_EQ(CatalogField->TypeSignature, DirectField->TypeSignature);
	ASSERT_GE(DirectField->Payload.size(), 1u + sizeof(uint64));
	EXPECT_EQ(DirectField->Payload.front(), std::byte{1});
	const Durin::Asset::FAssetPackageField* FixedField =
		Inspection.FindField("Fixed");
	ASSERT_NE(FixedField, nullptr);
	const size_t FirstFixedValueBytes =
		1 + sizeof(uint64) + MissingPath.GetView().size();
	ASSERT_EQ(FixedField->Payload.size(), FirstFixedValueBytes + 1);
	EXPECT_EQ(FixedField->Payload[FirstFixedValueBytes], std::byte{0});

	std::vector<Durin::Asset::FAssetReferenceEdge> Extracted;
	ASSERT_TRUE(Durin::Asset::ExtractAssetReferences(
		OwnerPath, Inspection, Extracted
	));
	ASSERT_EQ(Extracted.size(), 5u);
	EXPECT_EQ(std::ranges::count(Extracted, TargetPath, &Durin::Asset::FAssetReferenceEdge::TargetPath), 3);
	EXPECT_EQ(std::ranges::count(Extracted, MissingPath, &Durin::Asset::FAssetReferenceEdge::TargetPath), 2);
	EXPECT_TRUE(std::ranges::any_of(Extracted, [](const auto& Reference) {
		return Reference.DisplayRoute == "Fixed[fixed:0]"
			   && Reference.Route.size() == 1
			   && Reference.Route.front().Kind
					  == Durin::Asset::EAssetReferenceRouteKind::FixedArray;
	}));
	EXPECT_TRUE(std::ranges::any_of(Extracted, [](const auto& Reference) {
		return Reference.DisplayRoute.starts_with("Map[key:")
			   && Reference.Route.size() == 1
			   && Reference.Route.front().Kind
					  == Durin::Asset::EAssetReferenceRouteKind::MapValue;
	}));

	auto Referencers = Durin::Asset::CaptureAssetReferenceIndex().FindReferencers(TargetPath);
	EXPECT_EQ(Referencers.size(), 3u);
	auto Targets = Durin::Asset::CaptureAssetReferenceIndex().FindTargets(OwnerPath);
	ASSERT_EQ(Targets.size(), 2u);
	EXPECT_EQ(Targets[0], MissingPath);
	EXPECT_EQ(Targets[1], TargetPath);

	// The loaded owner and a populated weak cache do not block target unload.
	std::vector<std::byte> CachedBytes;
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(
		Owner->GetPackage(), CachedBytes
	));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetPath));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(TargetPath), nullptr);
	std::vector<std::byte> UnloadedBytes;
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(
		Owner->GetPackage(), UnloadedBytes
	));
	EXPECT_EQ(UnloadedBytes, CachedBytes);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	DSoftPackageAssetForTest* LoadedOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, LoadedOwner));
	ASSERT_NE(LoadedOwner, nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(TargetPath), nullptr);
	EXPECT_EQ(LoadedOwner->Direct.GetSoftObjectPath().GetAssetPath(), TargetPath);
	EXPECT_EQ(LoadedOwner->Fixed[0].GetSoftObjectPath().GetAssetPath(), MissingPath);
	EXPECT_EQ(LoadedOwner->Array[1].GetSoftObjectPath().GetAssetPath(), MissingPath);
	EXPECT_EQ(LoadedOwner->Map.at("hero").GetSoftObjectPath().GetAssetPath(), TargetPath);
	EXPECT_FALSE(LoadedOwner->Direct.IsLoaded());
	DPackageAssetForTest* Missing = nullptr;
	EXPECT_EQ(
		Durin::Asset::LoadSoftObject(LoadedOwner->Fixed[0], Missing).Error,
		Durin::Asset::EAssetError::NotFound
	);
	EXPECT_EQ(Missing, nullptr);
}

TEST(FPackageAssetTests, SoftInspectionRejectsMalformedPayloadsAndPreservesUnknownFields)
{
	InitializeAssetTests();
	Durin::FAssetPath OwnerPath;
	Durin::FAssetPath TargetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftInspectionOwner", OwnerPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftInspectionTarget", TargetPath));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->Direct.SetPath(TargetPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	const auto Data = Durin::Asset::FindAssetExact(OwnerPath);
	ASSERT_NE(Data, nullptr);
	Durin::Asset::FAssetPackageInspection Valid;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(Data->PhysicalPath, Valid));

	auto FindMutableField = [](Durin::Asset::FAssetPackageInspection& Inspection,
							   std::string_view Name) -> Durin::Asset::FAssetPackageField* {
		for (auto& Object : Inspection.Objects)
			for (auto& Field : Object.Fields)
				if (Field.Name == Name) return &Field;
		return nullptr;
	};
	auto MakePathPayload = [](uint8 Kind, std::string_view Path) {
		std::vector<std::byte> Payload{static_cast<std::byte>(Kind)};
		const uint64 Size = Path.size();
		const auto SizeBytes = std::as_bytes(std::span{&Size, 1});
		Payload.insert(Payload.end(), SizeBytes.begin(), SizeBytes.end());
		const auto PathBytes = std::as_bytes(std::span{Path});
		Payload.insert(Payload.end(), PathBytes.begin(), PathBytes.end());
		return Payload;
	};
	std::vector<Durin::Asset::FAssetReferenceEdge> References;

	auto Unknown = Valid;
	ASSERT_NE(FindMutableField(Unknown, "Direct"), nullptr);
	FindMutableField(Unknown, "Direct")->Name = "RetiredSoftField";
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(OwnerPath, Unknown, References).Error, Durin::Asset::EAssetError::TypeMismatch);

	auto WrongSignature = Valid;
	FindMutableField(WrongSignature, "Direct")->TypeSignature =
		"SoftObject:Tests::DCodecSourceAsset:v1";
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(OwnerPath, WrongSignature, References).Error, Durin::Asset::EAssetError::TypeMismatch);

	auto UnknownTag = Valid;
	FindMutableField(UnknownTag, "Direct")->Payload = {std::byte{2}};
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(OwnerPath, UnknownTag, References).Error, Durin::Asset::EAssetError::CorruptFile);

	auto Truncated = Valid;
	FindMutableField(Truncated, "Direct")->Payload = {std::byte{1}};
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(OwnerPath, Truncated, References).Error, Durin::Asset::EAssetError::CorruptFile);

	auto Overlong = Valid;
	FindMutableField(Overlong, "Direct")->Payload = {std::byte{1}};
	const uint64 OverlongSize = 1024 * 1024 + 1;
	const auto OverlongBytes = std::as_bytes(std::span{&OverlongSize, 1});
	FindMutableField(Overlong, "Direct")->Payload.insert(
		FindMutableField(Overlong, "Direct")->Payload.end(), OverlongBytes.begin(), OverlongBytes.end());
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(OwnerPath, Overlong, References).Error, Durin::Asset::EAssetError::CorruptFile);

	auto InvalidPath = Valid;
	FindMutableField(InvalidPath, "Direct")->Payload =
		MakePathPayload(1, "/TestAssets//Invalid");
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(OwnerPath, InvalidPath, References).Error, Durin::Asset::EAssetError::InvalidPath);

	auto TrailingNull = Valid;
	FindMutableField(TrailingNull, "Direct")->Payload = {std::byte{0}, std::byte{0}};
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(OwnerPath, TrailingNull, References).Error, Durin::Asset::EAssetError::CorruptFile);

	auto RuntimeMismatch = Valid;
	RuntimeMismatch.Header.AssetClassName = "Tests::DCodecSourceAsset";
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(OwnerPath, RuntimeMismatch, References).Error, Durin::Asset::EAssetError::TypeMismatch);

	Durin::FAssetPath OldPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftInspectionOld", OldPath));
	DPackageAssetForTest* OldAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, OldAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(OldAsset->GetPackage()));
	const auto OldData = Durin::Asset::FindAssetExact(OldPath);
	ASSERT_NE(OldData, nullptr);
	Durin::Asset::FAssetPackageInspection OldInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		OldData->PhysicalPath, OldInspection
	));
	ASSERT_TRUE(Durin::Asset::ExtractAssetReferences(
		OldPath, OldInspection, References
	));
	EXPECT_TRUE(References.empty());

	std::vector<std::byte> OmittedSoftBytes;
	Durin::Asset::FAssetPackageSerializationOptions OmitSoftFields;
	OmitSoftFields.PropertyFilter = [](const Durin::DObject*, const Durin::FProperty* Property) {
		return Property->NamePrivate.ToString() != "Direct"
			   && Property->NamePrivate.ToString() != "Fixed"
			   && Property->NamePrivate.ToString() != "Array"
			   && Property->NamePrivate.ToString() != "Map";
	};
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(
		Owner->GetPackage(), OmittedSoftBytes, OmitSoftFields
	));
	WriteTestBytes(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "SoftOmittedFields.dasset",
		OmittedSoftBytes
	);
	Durin::FAssetPath OmittedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/SoftOmittedFields", OmittedPath
	));
	DSoftPackageAssetForTest* Omitted = nullptr;
	EXPECT_EQ(
		Durin::Asset::LoadAsset(OmittedPath, Omitted).Error,
		Durin::Asset::EAssetError::NotFound);
	ASSERT_TRUE(Durin::Asset::AdmitAssetPackageToCatalog(OmittedPath));
	ASSERT_TRUE(Durin::Asset::LoadAsset(OmittedPath, Omitted));
	ASSERT_NE(Omitted, nullptr);
	EXPECT_TRUE(Omitted->Direct.IsNull());
	EXPECT_TRUE(Omitted->Fixed[0].IsNull());
	EXPECT_TRUE(Omitted->Array.empty());
	EXPECT_TRUE(Omitted->Map.empty());
}

TEST(FPackageAssetTests, SoftCookReachabilityAddsSoftTargetsButRejectsMissingAndWrongTypes)
{
	InitializeAssetTests();
	Durin::FAssetPath TargetPath;
	Durin::FAssetPath OwnerPath;
	Durin::FAssetPath MissingPath;
	Durin::FAssetPath WrongTypePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftCookTarget", TargetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftCookOwner", OwnerPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftCookMissing", MissingPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftCookWrongType", WrongTypePath));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->Direct.SetPath(TargetPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	std::vector<Durin::FAssetPath> Reachable;
	ASSERT_TRUE(Durin::Asset::BuildCookReachability(
		std::span<const Durin::FAssetPath>(&OwnerPath, 1), Reachable
	));
	EXPECT_EQ(Reachable, (std::vector<Durin::FAssetPath>{OwnerPath, TargetPath}));

	Owner->Direct.SetPath(MissingPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	EXPECT_EQ(Durin::Asset::BuildCookReachability(std::span<const Durin::FAssetPath>(&OwnerPath, 1), Reachable).Error, Durin::Asset::EAssetError::MissingDependency);

	DMathStructAssetForTest* WrongType = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(WrongTypePath, WrongType));
	ASSERT_TRUE(Durin::Asset::SavePackage(WrongType->GetPackage()));
	Owner->Direct.SetPath(WrongTypePath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	EXPECT_EQ(Durin::Asset::BuildCookReachability(std::span<const Durin::FAssetPath>(&OwnerPath, 1), Reachable).Error, Durin::Asset::EAssetError::TypeMismatch);

	Owner->Direct.SetPath(TargetPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	DSoftPackageAssetForTest* CycleTarget = nullptr;
	Durin::FAssetPath CyclePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftCookCycle", CyclePath));
	ASSERT_TRUE(Durin::Asset::CreateAsset(CyclePath, CycleTarget));
	CycleTarget->Direct.SetPath(OwnerPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(CycleTarget->GetPackage()));
	Owner->Direct.SetPath(CyclePath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::Asset::BuildCookReachability(
		std::span<const Durin::FAssetPath>(&OwnerPath, 1), Reachable
	));
	EXPECT_EQ(Reachable, (std::vector<Durin::FAssetPath>{CyclePath, OwnerPath}));
}

TEST(FPackageAssetTests, CookCanonicalizesRedirectedRootsReferencesAndPublishedBytes)
{
	InitializeAssetTests();
	Durin::FAssetPath OwnerPath;
	Durin::FAssetPath OldTargetPath;
	Durin::FAssetPath FinalTargetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/CookCanonicalOwner", OwnerPath
	));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/CookCanonicalTargetOld", OldTargetPath
	));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/CookCanonicalTargetFinal", FinalTargetPath
	));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldTargetPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Target;
	Owner->Direct.SetPath(OldTargetPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));

	const Durin::Asset::FAssetCatalogEntry OwnerData =
		Durin::Asset::FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	const std::string OwnerPhysicalPath = OwnerData->PhysicalPath;
	std::vector<std::byte> AuthoredBytes;
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
	std::vector<Durin::FAssetPath> Reachable;
	ASSERT_TRUE(Durin::Asset::BuildCookReachability(
		std::span{&OwnerPath, 1}, Reachable
	));
	std::vector ExpectedReachable{OwnerPath, FinalTargetPath};
	std::ranges::sort(ExpectedReachable, [](const Durin::FAssetPath& Left, const Durin::FAssetPath& Right) {
		return Left.GetView() < Right.GetView();
	});
	EXPECT_EQ(Reachable, ExpectedReachable);
	EXPECT_EQ(RuntimeRoot.Path, OldTargetPath);

	std::vector<std::byte> CanonicalBytes;
	const Durin::Asset::FAssetResult CanonicalResult =
		Durin::Asset::CanonicalizeAssetPackageForCook(
			AuthoredBytes, CanonicalBytes
		);
	ASSERT_TRUE(CanonicalResult) << CanonicalResult.Message;
	EXPECT_NE(CanonicalBytes, AuthoredBytes);
	const std::filesystem::path CanonicalFile =
		Durin::Testing::GetTestWorkDirectory() / "CookCanonicalized.dasset";
	WriteTestBytes(CanonicalFile, CanonicalBytes);
	Durin::Asset::FAssetPackageInspection CanonicalInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		CanonicalFile.generic_string(), CanonicalInspection
	));
	EXPECT_EQ(
		CanonicalInspection.Header.Dependencies,
		(std::vector<Durin::FAssetPath>{FinalTargetPath})
	);
	std::vector<Durin::Asset::FAssetReferenceEdge> CanonicalReferences;
	ASSERT_TRUE(Durin::Asset::ExtractAssetReferences(
		OwnerPath, CanonicalInspection, CanonicalReferences
	));
	ASSERT_EQ(CanonicalReferences.size(), 2u);
	for (const Durin::Asset::FAssetReferenceEdge& Reference :
		 CanonicalReferences)
		EXPECT_EQ(Reference.TargetPath, FinalTargetPath);

	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CanonicalCookOutput"
	);
	Durin::Testing::RemoveTestWorkDirectory(CookRoot);
	Durin::Asset::FCookContext Cook(
		CookRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game
	);
	ASSERT_TRUE(Cook.AddPackage(
		"/Game/CookCanonicalOwner", AuthoredBytes, {}
	));
	std::string CookError;
	ASSERT_TRUE(Cook.Publish(&CookError)) << CookError;
	Durin::Asset::FAssetPackageInspection PublishedInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		(CookRoot / "Game/CookCanonicalOwner.dasset").generic_string(),
		PublishedInspection
	));
	EXPECT_EQ(
		PublishedInspection.Header.Dependencies,
		(std::vector<Durin::FAssetPath>{FinalTargetPath})
	);

	std::vector<std::byte> AuthoredAfterCook;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AuthoredAfterCook, OwnerPhysicalPath
	));
	EXPECT_EQ(AuthoredAfterCook, AuthoredBytes);

	const Durin::Asset::FAssetCatalogEntry AliasData =
		Durin::Asset::FindAssetExact(OldTargetPath);
	ASSERT_NE(AliasData, nullptr);
	std::vector<std::byte> AliasBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AliasBytes, AliasData->PhysicalPath
	));

	const std::filesystem::path RedirectorRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "RedirectorCookOutput"
	);
	Durin::Testing::RemoveTestWorkDirectory(RedirectorRoot);
	Durin::Asset::FCookContext RedirectorCook(
		RedirectorRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game
	);
	ASSERT_TRUE(RedirectorCook.AddPackage(
		"/Game/RedirectorMustNotCook", AliasBytes, {}
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
	Durin::FAssetPath TargetPath;
	Durin::FAssetPath SourcePath;
	Durin::FAssetPath MovedSourcePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMutationTarget", TargetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMutationSource", SourcePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMutationMoved", MovedSourcePath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Source = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Source));
	Source->Direct.SetPath(TargetPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Source->GetPackage()));
	EXPECT_EQ(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(SourcePath), (std::vector<Durin::FAssetPath>{TargetPath}));

	Source->Direct.Reset();
	ASSERT_TRUE(Durin::Asset::SavePackage(Source->GetPackage()));
	EXPECT_TRUE(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(SourcePath).empty());
	Source->Direct.SetPath(TargetPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Source->GetPackage()));
	ASSERT_TRUE(RelocateAssetForTest(SourcePath, MovedSourcePath));
	EXPECT_EQ(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(SourcePath), (std::vector<Durin::FAssetPath>{MovedSourcePath}));
	EXPECT_EQ(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(MovedSourcePath), (std::vector<Durin::FAssetPath>{TargetPath}));
	ASSERT_TRUE(DeleteAssetClosureForTest({SourcePath, MovedSourcePath}));
	EXPECT_TRUE(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(MovedSourcePath).empty());
	EXPECT_TRUE(Durin::Asset::CaptureAssetReferenceIndex().FindReferencers(TargetPath).empty());
}

TEST(FPackageAssetTests, RelocationPreservesLoadedAndUnloadedSoftAuthoredPaths)
{
	InitializeAssetTests();
	Durin::FAssetPath OldPath;
	Durin::FAssetPath NewPath;
	Durin::FAssetPath LoadedOwnerPath;
	Durin::FAssetPath UnloadedOwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMoveTarget", OldPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMoveTargetRenamed", NewPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMoveLoadedOwner", LoadedOwnerPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMoveUnloadedOwner", UnloadedOwnerPath));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* LoadedOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(LoadedOwnerPath, LoadedOwner));
	LoadedOwner->Direct.SetPath(OldPath);
	LoadedOwner->Fixed[0].SetPath(OldPath);
	LoadedOwner->Array.emplace_back(OldPath);
	LoadedOwner->Map.emplace("loaded", DSoftPackageAssetForTest::FSoftReference(OldPath));
	ASSERT_TRUE(LoadedOwner->Direct.TrySetLoadedObject(Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(LoadedOwner->GetPackage()));

	DSoftPackageAssetForTest* UnloadedOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(UnloadedOwnerPath, UnloadedOwner));
	UnloadedOwner->Direct.SetPath(OldPath);
	UnloadedOwner->Fixed[1].SetPath(OldPath);
	UnloadedOwner->Array.emplace_back(OldPath);
	UnloadedOwner->Map.emplace("unloaded", DSoftPackageAssetForTest::FSoftReference(OldPath));
	ASSERT_TRUE(Durin::Asset::SavePackage(UnloadedOwner->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(UnloadedOwnerPath));
	const uint64 ConstructionsBeforeMove = GSoftPackageConstructionCount;

	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	EXPECT_EQ(GSoftPackageConstructionCount, ConstructionsBeforeMove);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(UnloadedOwnerPath), nullptr);
	EXPECT_EQ(LoadedOwner->Direct.GetSoftObjectPath().GetAssetPath(), OldPath);
	EXPECT_EQ(LoadedOwner->Fixed[0].GetSoftObjectPath().GetAssetPath(), OldPath);
	ASSERT_EQ(LoadedOwner->Array.size(), 1u);
	EXPECT_EQ(LoadedOwner->Array[0].GetSoftObjectPath().GetAssetPath(), OldPath);
	EXPECT_EQ(LoadedOwner->Map.at("loaded").GetSoftObjectPath().GetAssetPath(), OldPath);
	EXPECT_FALSE(LoadedOwner->Direct.IsLoaded());

	auto LoadedTargets = Durin::Asset::CaptureAssetReferenceIndex().FindTargets(LoadedOwnerPath);
	auto UnloadedTargets = Durin::Asset::CaptureAssetReferenceIndex().FindTargets(UnloadedOwnerPath);
	EXPECT_EQ(LoadedTargets, (std::vector<Durin::FAssetPath>{OldPath}));
	EXPECT_EQ(UnloadedTargets, (std::vector<Durin::FAssetPath>{OldPath}));
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry());
	EXPECT_EQ(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(UnloadedOwnerPath), (std::vector<Durin::FAssetPath>{OldPath}));
	DSoftPackageAssetForTest* ReloadedOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(UnloadedOwnerPath, ReloadedOwner));
	EXPECT_EQ(ReloadedOwner->Direct.GetSoftObjectPath().GetAssetPath(), OldPath);
	EXPECT_EQ(ReloadedOwner->Fixed[1].GetSoftObjectPath().GetAssetPath(), OldPath);
	ASSERT_EQ(ReloadedOwner->Array.size(), 1u);
	EXPECT_EQ(ReloadedOwner->Array[0].GetSoftObjectPath().GetAssetPath(), OldPath);
	EXPECT_EQ(ReloadedOwner->Map.at("unloaded").GetSoftObjectPath().GetAssetPath(), OldPath);
}

TEST(FPackageAssetTests, RelocationIgnoresStaleAndReadOnlyUnloadedSoftReferencers)
{
	InitializeAssetTests();
	auto RunCase = [](std::string_view Suffix, bool bMakeReadOnly) {
		Durin::FAssetPath OldPath;
		Durin::FAssetPath NewPath;
		Durin::FAssetPath OwnerPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/SoftMove{}Target", Suffix), OldPath
		));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/SoftMove{}TargetRenamed", Suffix), NewPath
		));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/SoftMove{}Owner", Suffix), OwnerPath
		));
		DPackageAssetForTest* Target = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
		ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
		DSoftPackageAssetForTest* Owner = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
		Owner->Direct.SetPath(OldPath);
		ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
		ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
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
		const Durin::Asset::FAssetResult Result =
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
		ASSERT_NE(Durin::Asset::FindAssetExact(OldPath), nullptr);
		EXPECT_EQ(Durin::Asset::FindAssetExact(OldPath)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
		EXPECT_NE(Durin::Asset::FindAssetExact(NewPath), nullptr);
		EXPECT_EQ(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(OwnerPath), (std::vector<Durin::FAssetPath>{OldPath}));
	};
	RunCase("Stale", false);
	RunCase("ReadOnly", true);
}

TEST(FPackageAssetTests, RelocationPublicationFailureRestoresAuthoredState)
{
	InitializeAssetTests();
	Durin::FAssetPath OldPath;
	Durin::FAssetPath NewPath;
	Durin::FAssetPath OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RelocationRollbackTarget", OldPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RelocationRollbackRenamed", NewPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RelocationRollbackOwner", OwnerPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->Direct.SetPath(OldPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));

	Durin::TSoftObjectPtr<DPackageAssetForTest> ExternalSetting(OldPath);
	Durin::Asset::FAssetMutationSummary Summary;
	Durin::Asset::FAssetMutationTransaction Transaction;
	const Durin::Asset::FAssetRelocationMapping Mapping{OldPath, NewPath};
	ASSERT_TRUE(Durin::Asset::PrepareAssetRelocationTransaction(
		std::span{&Mapping, 1}, Summary, Transaction));
	Durin::Asset::SetAssetRelocationFailurePointForTesting(
		Durin::Asset::EAssetRelocationFailurePoint::PublishRedirector
	);
	const Durin::Asset::FAssetResult Result = Transaction.Commit();
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::IoError);
	EXPECT_EQ(ExternalSetting.GetSoftObjectPath().GetAssetPath(), OldPath);
	ASSERT_NE(Durin::Asset::FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindAssetExact(OldPath)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::FindAssetExact(NewPath), nullptr);
	EXPECT_EQ(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(OwnerPath), (std::vector<Durin::FAssetPath>{OldPath}));
}

TEST(FPackageAssetTests, RelocationPreservesExternalAuthoredPathsAndRejectsRealCollision)
{
	InitializeAssetTests();
	Durin::FAssetPath OldPath;
	Durin::FAssetPath NewPath;
	Durin::FAssetPath CollisionPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMoveExternalTarget", OldPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMoveExternalTargetRenamed", NewPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMoveExternalCollision", CollisionPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	DPackageAssetForTest* Collision = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(CollisionPath, Collision));
	ASSERT_TRUE(Durin::Asset::SavePackage(Collision->GetPackage()));
	EXPECT_EQ(RelocateAssetForTest(OldPath, CollisionPath).Error, Durin::Asset::EAssetError::AlreadyExists);

	Durin::TSoftObjectPtr<DPackageAssetForTest> ExternalSetting(OldPath);
	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	EXPECT_EQ(ExternalSetting.GetSoftObjectPath().GetAssetPath(), OldPath);
	ASSERT_NE(Durin::Asset::FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindAssetExact(OldPath)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_NE(Durin::Asset::FindAssetExact(NewPath), nullptr);
}

TEST(FPackageAssetTests, RelocationRejectsReadOnlySourceWithoutStagingMutation)
{
	InitializeAssetTests();
	Durin::FAssetPath SourcePath;
	Durin::FAssetPath DestinationPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/ReadOnlyRelocationSource", SourcePath
	));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/ReadOnlyRelocationDestination", DestinationPath
	));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const std::filesystem::path SourceFile =
		Durin::Asset::FindAssetExact(SourcePath)->PhysicalPath;
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
	const Durin::Asset::FAssetResult Result =
		RelocateAssetForTest(SourcePath, DestinationPath);
	std::filesystem::permissions(
		SourceFile, Original,
		std::filesystem::perm_options::replace, ErrorCode
	);
	ASSERT_FALSE(ErrorCode);
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::ReadOnlyMode);
	EXPECT_EQ(Durin::Asset::FindAssetExact(SourcePath)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::FindAssetExact(DestinationPath), nullptr);
}

TEST(FPackageAssetTests, RelocationFailureSeamsPreserveEveryOrdinaryBoundary)
{
	InitializeAssetTests();
	const std::array Points = {
		Durin::Asset::EAssetRelocationFailurePoint::StageOriginal,
		Durin::Asset::EAssetRelocationFailurePoint::PublishRealAsset,
		Durin::Asset::EAssetRelocationFailurePoint::PublishRedirector,
		Durin::Asset::EAssetRelocationFailurePoint::UpdateLoadedPackage,
		Durin::Asset::EAssetRelocationFailurePoint::PublishRegistry
	};
	for (size_t Index = 0; Index < Points.size(); ++Index)
	{
		Durin::FAssetPath SourcePath;
		Durin::FAssetPath DestinationPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(std::format("/TestAssets/FailureBoundary{}Source", Index), SourcePath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(std::format("/TestAssets/FailureBoundary{}Destination", Index), DestinationPath));
		DPackageAssetForTest* Asset = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Asset));
		ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
		const Durin::Asset::FAssetRelocationMapping Mapping{
			SourcePath, DestinationPath
		};
		Durin::Asset::FAssetMutationSummary Summary;
		Durin::Asset::FAssetMutationTransaction Transaction;
		ASSERT_TRUE(Durin::Asset::PrepareAssetRelocationTransaction(
			std::span{&Mapping, 1}, Summary, Transaction));
		Durin::Asset::SetAssetRelocationFailurePointForTesting(Points[Index]);
		EXPECT_EQ(Transaction.Commit().Error, Durin::Asset::EAssetError::IoError);
		ASSERT_NE(Durin::Asset::FindAssetExact(SourcePath), nullptr);
		EXPECT_EQ(Durin::Asset::FindAssetExact(SourcePath)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);
		EXPECT_EQ(Durin::Asset::FindAssetExact(DestinationPath), nullptr);
	}

	Durin::FAssetPath PrepareSource;
	Durin::FAssetPath PrepareDestination;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/FailurePrepareSource", PrepareSource
	));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/FailurePrepareDestination", PrepareDestination
	));
	DPackageAssetForTest* PrepareAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(PrepareSource, PrepareAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(PrepareAsset->GetPackage()));
	const Durin::Asset::FAssetRelocationMapping PrepareMapping{
		PrepareSource, PrepareDestination
	};
	Durin::Asset::SetAssetRelocationFailurePointForTesting(
		Durin::Asset::EAssetRelocationFailurePoint::PrepareOutput
	);
	Durin::Asset::FAssetMutationSummary PrepareSummary;
	Durin::Asset::FAssetMutationTransaction PrepareTransaction;
	EXPECT_EQ(Durin::Asset::PrepareAssetRelocationTransaction(
		std::span{&PrepareMapping, 1}, PrepareSummary, PrepareTransaction).Error,
		Durin::Asset::EAssetError::IoError);
	EXPECT_EQ(Durin::Asset::FindAssetExact(PrepareSource)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::FindAssetExact(PrepareDestination), nullptr);
}

TEST(FPackageAssetTests, CompensationFailureRetainsDiagnosableRecoveryRoot)
{
	InitializeAssetTests();
	Durin::FAssetPath SourcePath;
	Durin::FAssetPath DestinationPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RecoverySource", SourcePath
	));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RecoveryDestination", DestinationPath
	));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const Durin::Asset::FAssetRelocationMapping Mapping{
		SourcePath, DestinationPath
	};
	Durin::Asset::FAssetMutationSummary Summary;
	Durin::Asset::FAssetMutationTransaction Transaction;
	ASSERT_TRUE(Durin::Asset::PrepareAssetRelocationTransaction(
		std::span{&Mapping, 1}, Summary, Transaction));
	Durin::Asset::SetAssetRelocationFailurePointForTesting(
		Durin::Asset::EAssetRelocationFailurePoint::PublishRedirector
	);
	Durin::Asset::SetAssetRelocationFailurePointForTesting(
		Durin::Asset::EAssetRelocationFailurePoint::CompensateFile
	);
	const Durin::Asset::FAssetResult Result = Transaction.Commit();
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::IoError);
	EXPECT_NE(Result.Message.find("AssetMutationRecoveryRequired"), std::string::npos);
	EXPECT_EQ(Transaction.GetState(),
		Durin::Asset::EAssetMutationTransactionState::RecoveryRequired);
	const Durin::Asset::FAssetMutationResultDetails Details =
		Transaction.GetLastResultDetails();
	EXPECT_FALSE(Details.bStateRestored);
	EXPECT_TRUE(Details.bRecoveryRequired);
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
	std::vector<std::byte> JournalBytes;
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
	EXPECT_NE(Journal.find("compensated=false"), std::string::npos);

	const std::string RecoveryBase = Durin::FPaths::ProjectDir().empty() ? Durin::FPaths::LaunchDir() : Durin::FPaths::ProjectDir();
	const std::filesystem::path LocatorRoot =
		std::filesystem::path(RecoveryBase)
		/ "Saved" / "AssetMutationRecovery";
	const std::filesystem::path Locator =
		LocatorRoot / OperationRoot.filename();
	std::vector<std::byte> LocatorBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		LocatorBytes, Locator
	));
	const std::string LocatorText(
		reinterpret_cast<const char*>(LocatorBytes.data()), LocatorBytes.size()
	);
	EXPECT_NE(LocatorText.find(OperationRoot.generic_string()), std::string::npos);
	Transaction = {};
	std::error_code ErrorCode;
	std::filesystem::remove(
		ContentRoot / "RecoveryDestination.dasset", ErrorCode
	);
	Durin::Testing::RemoveTestWorkDirectory(RecoveryRoot, ErrorCode);
	std::filesystem::remove(Locator, ErrorCode);
	std::filesystem::remove(LocatorRoot, ErrorCode);
	Durin::Asset::SetAssetRelocationFailurePointForTesting(
		Durin::Asset::EAssetRelocationFailurePoint::None
	);
}

namespace
{
	auto RunRedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletionTest()
		-> void
	{
		InitializeAssetTests();
		Durin::FAssetPath OldPath;
		Durin::FAssetPath NewPath;
		Durin::FAssetPath OwnerPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/FixupOld", OldPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/FixupNew", NewPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/FixupOwner", OwnerPath));
		DPackageAssetForTest* Target = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
		ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
		DSoftPackageAssetForTest* Owner = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
		Owner->Label = "unrelated-fixup-bytes";
		Owner->ExternalReference = Target;
		Owner->Direct.SetPath(OldPath);
		ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
		auto OwnerData = Durin::Asset::FindAssetExact(OwnerPath);
		ASSERT_NE(OwnerData, nullptr);
		Durin::Asset::FAssetPackageInspection BeforeInspection;
		ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
			OwnerData->PhysicalPath, BeforeInspection
		));
		const auto* BeforeLabel = BeforeInspection.FindField("Label");
		ASSERT_NE(BeforeLabel, nullptr);
		const std::vector<std::byte> UnrelatedBytes = BeforeLabel->Payload;
		ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
		ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));

		const auto Incoming = Durin::Asset::CaptureAssetReferenceIndex().FindReferencers(OldPath);
		ASSERT_EQ(Incoming.size(), 2u);
		EXPECT_TRUE(std::ranges::any_of(Incoming, [](const auto& Edge) {
			return Edge.Kind == Durin::Asset::EAssetReferenceKind::HardObject;
		}));
		EXPECT_TRUE(std::ranges::any_of(Incoming, [](const auto& Edge) {
			return Edge.Kind == Durin::Asset::EAssetReferenceKind::SoftObject;
		}));

		FMemoryAssetReferenceStore Store(OldPath);
		FScopedReferenceStoreRegistration StoreRegistration(&Store);
		Durin::Asset::FAssetRedirectorFixupSummary Summary;
		Durin::Asset::FAssetMutationTransaction Transaction;
		ASSERT_TRUE(Durin::Asset::PrepareRedirectorFixupTransaction(
			std::span{&OldPath, 1},
			Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
			Summary,
			Transaction
		));
		ASSERT_EQ(Summary.GetRedirectors().size(), 1u);
		EXPECT_EQ(Summary.GetRedirectors().front(), OldPath);
		EXPECT_EQ(Summary.GetPackageOccurrences().size(), 2u);
		EXPECT_EQ(Summary.GetStoreOccurrences().size(), 1u);
		EXPECT_EQ(Summary.GetDeletableRedirectors().size(), 1u);
		const uint64 ConstructionCount = GSoftPackageConstructionCount;
		ASSERT_TRUE(Transaction.Commit());
		const Durin::Asset::FAssetMutationResultDetails Details =
			Transaction.GetLastResultDetails();
		EXPECT_EQ(Details.DeletedPaths, std::vector{OldPath});
		EXPECT_EQ(Details.RewrittenPaths, std::vector{OwnerPath});
		EXPECT_EQ(GSoftPackageConstructionCount, ConstructionCount);
		EXPECT_EQ(Store.Path, NewPath);
		EXPECT_EQ(Durin::Asset::FindAssetExact(OldPath), nullptr);
		EXPECT_TRUE(Durin::Asset::CaptureAssetReferenceIndex().FindReferencers(OldPath).empty());
		EXPECT_TRUE(Durin::Asset::CaptureAssetReferenceIndex().IsComplete());

		OwnerData = Durin::Asset::FindAssetExact(OwnerPath);
		ASSERT_NE(OwnerData, nullptr);
		Durin::Asset::FAssetPackageInspection AfterInspection;
		ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
			OwnerData->PhysicalPath, AfterInspection
		));
		const auto* AfterLabel = AfterInspection.FindField("Label");
		ASSERT_NE(AfterLabel, nullptr);
		EXPECT_EQ(AfterLabel->Payload, UnrelatedBytes);
		const auto NewIncoming = Durin::Asset::CaptureAssetReferenceIndex().FindReferencers(NewPath);
		EXPECT_GE(NewIncoming.size(), 2u);
		DSoftPackageAssetForTest* ReloadedOwner = nullptr;
		ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, ReloadedOwner));
		EXPECT_EQ(ReloadedOwner->Direct.GetSoftObjectPath().GetAssetPath(), NewPath);
		EXPECT_EQ(ReloadedOwner->ExternalReference.Get(), Target);
	}

	auto RunRedirectorFixupVerificationFailureRestoresPackagesStoresAndAliasTest()
		-> void
	{
		InitializeAssetTests();
		Durin::FAssetPath OldPath;
		Durin::FAssetPath NewPath;
		Durin::FAssetPath OwnerPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/FixupFailureOld", OldPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/FixupFailureNew", NewPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/FixupFailureOwner", OwnerPath));
		DPackageAssetForTest* Target = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
		ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
		DSoftPackageAssetForTest* Owner = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
		Owner->ExternalReference = Target;
		Owner->Direct.SetPath(OldPath);
		ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
		const std::string OwnerFile = Durin::Asset::FindAssetExact(OwnerPath)
										  ->PhysicalPath;
		ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
		ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
		ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
		std::vector<std::byte> BeforeBytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeBytes, OwnerFile));

		FMemoryAssetReferenceStore Store(OldPath);
		FScopedReferenceStoreRegistration StoreRegistration(&Store);
		Durin::Asset::SetAssetRedirectorFixupFailurePointForTesting(
			Durin::Asset::EAssetRedirectorFixupFailurePoint::Verify
		);
		const Durin::Asset::FAssetResult FixupResult =
			FixUpRedirectorsForTest(std::span{&OldPath, 1});
		EXPECT_EQ(FixupResult.Error, Durin::Asset::EAssetError::IoError)
			<< FixupResult.Message;
		Durin::Asset::SetAssetRedirectorFixupFailurePointForTesting(
			Durin::Asset::EAssetRedirectorFixupFailurePoint::None
		);
		std::vector<std::byte> AfterBytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterBytes, OwnerFile));
		EXPECT_EQ(AfterBytes, BeforeBytes);
		EXPECT_EQ(Store.Path, OldPath);
		const auto Alias = Durin::Asset::FindAssetExact(OldPath);
		ASSERT_NE(Alias, nullptr);
		EXPECT_EQ(Alias->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
		EXPECT_EQ(Durin::Asset::CaptureAssetReferenceIndex().FindReferencers(OldPath).size(), 2u);
		ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(OwnerPath));
		ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
	}
} // namespace

namespace
{
	auto RunRedirectorFixupRejectsUnavailableProviderWithoutMutationTest() -> void
	{
		InitializeAssetTests();
		Durin::FAssetPath OldPath;
		Durin::FAssetPath NewPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/TestAssets/FixupProviderGoneOld", OldPath
		));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/TestAssets/FixupProviderGoneNew", NewPath
		));
		DPackageAssetForTest* Target = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
		ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
		ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
		ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
			Durin::Asset::EAssetRegistryScanMode::FullValidation
		));
		FMemoryAssetReferenceStore Store(OldPath);
		const Durin::Asset::FAssetReferenceStoreHandle Handle =
			Durin::Asset::RegisterAssetReferenceStore(&Store);
		Durin::Asset::FAssetRedirectorFixupSummary Summary;
		Durin::Asset::FAssetMutationTransaction Transaction;
		ASSERT_TRUE(Durin::Asset::PrepareRedirectorFixupTransaction(
			std::span{&OldPath, 1},
			Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
			Summary,
			Transaction
		));
		Durin::Asset::UnregisterAssetReferenceStore(Handle);
		const Durin::Asset::FAssetResult Result = Transaction.Commit();
		EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::StaleData);
		EXPECT_EQ(Transaction.GetLastResultDetails().FailedPaths,
			std::vector{OldPath});
		EXPECT_EQ(Store.Path, OldPath);
		const auto Alias = Durin::Asset::FindAssetExact(OldPath);
		ASSERT_NE(Alias, nullptr);
		EXPECT_EQ(Alias->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
		ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
	}

	auto RunRedirectorFixupRejectsReadOnlyAndChangedPackageInputsTest() -> void
	{
		InitializeAssetTests();
		auto RunCase = [](std::string_view Suffix, bool bReadOnly) {
			Durin::FAssetPath OldPath;
			Durin::FAssetPath NewPath;
			Durin::FAssetPath OwnerPath;
			ASSERT_TRUE(Durin::FAssetPath::TryCreate(
				std::format("/TestAssets/Fixup{}Old", Suffix), OldPath
			));
			ASSERT_TRUE(Durin::FAssetPath::TryCreate(
				std::format("/TestAssets/Fixup{}New", Suffix), NewPath
			));
			ASSERT_TRUE(Durin::FAssetPath::TryCreate(
				std::format("/TestAssets/Fixup{}Owner", Suffix), OwnerPath
			));
			DPackageAssetForTest* Target = nullptr;
			ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
			ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
			DSoftPackageAssetForTest* Owner = nullptr;
			ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
			Owner->Direct.SetPath(OldPath);
			ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
			const std::string OwnerFile = Durin::Asset::FindAssetExact(OwnerPath)
											  ->PhysicalPath;
			ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
			ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
			ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
				Durin::Asset::EAssetRegistryScanMode::FullValidation
			));
			std::vector<std::byte> PreBytes;
			ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(PreBytes, OwnerFile));
			std::filesystem::perms OriginalPermissions =
				std::filesystem::status(OwnerFile).permissions();
			Durin::Asset::FAssetRedirectorFixupSummary Summary;
			Durin::Asset::FAssetMutationTransaction Transaction;
			Durin::Asset::FAssetResult Result;
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
				Result = Durin::Asset::PrepareRedirectorFixupTransaction(
					std::span{&OldPath, 1},
					Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
					Summary,
					Transaction
				);
				std::filesystem::permissions(
					OwnerFile, OriginalPermissions,
					std::filesystem::perm_options::replace,
					PermissionError
				);
				ASSERT_FALSE(PermissionError) << PermissionError.message();
				EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::ReadOnlyMode)
					<< Result.Message;
			}
			else
			{
				ASSERT_TRUE(Durin::Asset::PrepareRedirectorFixupTransaction(
					std::span{&OldPath, 1},
					Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
					Summary,
					Transaction
				));
				std::vector<std::byte> ChangedBytes = PreBytes;
				ASSERT_TRUE(RenameSerializedString(
					ChangedBytes, "Label", "Ghost"
				));
				WriteTestBytes(OwnerFile, ChangedBytes);
				Result = Transaction.Commit();
				EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::StaleData)
					<< Result.Message;
				WriteTestBytes(OwnerFile, PreBytes);
			}
			const auto Alias = Durin::Asset::FindAssetExact(OldPath);
			ASSERT_NE(Alias, nullptr);
			EXPECT_EQ(Alias->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
			ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(OwnerPath));
			ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
		};
		RunCase("ReadOnly", true);
		RunCase("Changed", false);
	}

	auto RunRedirectorFixupPublicationFailuresRestoreAllParticipantsTest() -> void
	{
		InitializeAssetTests();
		constexpr std::array FailurePoints{
			Durin::Asset::EAssetRedirectorFixupFailurePoint::PublishPackage,
			Durin::Asset::EAssetRedirectorFixupFailurePoint::ApplyStore,
			Durin::Asset::EAssetRedirectorFixupFailurePoint::DeleteRedirector,
			Durin::Asset::EAssetRedirectorFixupFailurePoint::PublishRegistry
		};
		for (size_t Index = 0; Index < FailurePoints.size(); ++Index)
		{
			Durin::FAssetPath OldPath;
			Durin::FAssetPath NewPath;
			Durin::FAssetPath OwnerPath;
			ASSERT_TRUE(Durin::FAssetPath::TryCreate(
				std::format("/TestAssets/FixupMatrix{}Old", Index), OldPath
			));
			ASSERT_TRUE(Durin::FAssetPath::TryCreate(
				std::format("/TestAssets/FixupMatrix{}New", Index), NewPath
			));
			ASSERT_TRUE(Durin::FAssetPath::TryCreate(
				std::format("/TestAssets/FixupMatrix{}Owner", Index), OwnerPath
			));
			DPackageAssetForTest* Target = nullptr;
			ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
			ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
			DSoftPackageAssetForTest* Owner = nullptr;
			ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
			Owner->Direct.SetPath(OldPath);
			ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
			const std::string OwnerFile = Durin::Asset::FindAssetExact(OwnerPath)
											  ->PhysicalPath;
			ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
			std::vector<std::byte> BeforeBytes;
			ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeBytes, OwnerFile));
			ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
			ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
				Durin::Asset::EAssetRegistryScanMode::FullValidation
			));
			FMemoryAssetReferenceStore Store(OldPath);
			FScopedReferenceStoreRegistration StoreRegistration(&Store);
			Durin::Asset::SetAssetRedirectorFixupFailurePointForTesting(
				FailurePoints[Index]
			);
			const Durin::Asset::FAssetResult Result =
				FixUpRedirectorsForTest(std::span{&OldPath, 1});
			Durin::Asset::SetAssetRedirectorFixupFailurePointForTesting(
				Durin::Asset::EAssetRedirectorFixupFailurePoint::None
			);
			EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::IoError)
				<< Result.Message;
			std::vector<std::byte> AfterBytes;
			ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterBytes, OwnerFile));
			EXPECT_EQ(AfterBytes, BeforeBytes);
			EXPECT_EQ(Store.Path, OldPath);
			const auto Alias = Durin::Asset::FindAssetExact(OldPath);
			ASSERT_NE(Alias, nullptr);
			EXPECT_EQ(Alias->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
			ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(OwnerPath));
			ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
		}
	}
} // namespace

TEST(FPackageAssetTests, SoftReferencedTargetDeletionLeavesDanglingPathWithoutBlockingTransaction)
{
	InitializeAssetTests();
	Durin::FAssetPath TargetPath;
	Durin::FAssetPath OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftDeleteDanglingTarget", TargetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftDeleteDanglingOwner", OwnerPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->Direct.SetPath(TargetPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(TargetPath, Analysis));
	EXPECT_TRUE(Analysis.DirectReferencers.empty());
	EXPECT_TRUE(Analysis.CanDelete());
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(TargetPath));
	EXPECT_EQ(Durin::Asset::FindAssetExact(TargetPath), nullptr);
	auto Referencers =
		Durin::Asset::CaptureAssetReferenceIndex().FindReferencers(TargetPath);
	ASSERT_EQ(Referencers.size(), 1u);
	EXPECT_EQ(Referencers.front().SourcePackage, OwnerPath);

	std::vector<Durin::FAssetPath> Reachable;
	EXPECT_EQ(Durin::Asset::BuildCookReachability(std::span<const Durin::FAssetPath>(&OwnerPath, 1), Reachable).Error, Durin::Asset::EAssetError::MissingDependency);
}

TEST(FPackageAssetTests, DastMapBytesAreCanonicalAcrossInsertionAndBucketHistory)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/MapOrderingBaseline", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	const std::array<std::pair<std::string, int32>, 8> Entries = {{{"alpha", 1}, {"bravo", 2}, {"charlie", 3}, {"delta", 4}, {"echo", 5}, {"foxtrot", 6}, {"golf", 7}, {"hotel", 8}}};

	Asset->NamedScores.clear();
	Asset->NamedScores.rehash(37);
	for (const auto& [Key, Value] : Entries)
		Asset->NamedScores.emplace(Key, Value);
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const auto File = Durin::Testing::GetTestWorkDirectory()
					  / "Assets" / "MapOrderingBaseline.dasset";
	std::vector<std::byte> ForwardBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(ForwardBytes, File));

	Asset->NamedScores.clear();
	Asset->NamedScores.rehash(2);
	for (auto It = Entries.rbegin(); It != Entries.rend(); ++It)
		Asset->NamedScores.emplace(It->first, It->second);
	Asset->GetPackage()->MarkDirty();
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	std::vector<std::byte> ReverseBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(ReverseBytes, File));

	EXPECT_EQ(ForwardBytes, ReverseBytes);
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
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

TEST(FPackageAssetTests, PrecisionSpecificMathStructsRoundTripThroughPackageObjectStream)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/PrecisionMath", Path));
	DMathStructAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->FloatQuat = Durin::FQuatf(0.5f, 0.25f, -0.5f, 0.75f);
	Asset->FloatMatrix = Durin::FMatrix4f(0.0f);
	Asset->FloatMatrix[0] = Durin::FVector4f(1.0f, 2.0f, 3.0f, 4.0f);
	Asset->FloatMatrix[1] = Durin::FVector4f(5.0f, 6.0f, 7.0f, 8.0f);
	Asset->FloatMatrix[2] = Durin::FVector4f(9.0f, 10.0f, 11.0f, 12.0f);
	Asset->FloatMatrix[3] = Durin::FVector4f(13.0f, 14.0f, 15.0f, 16.0f);
	const Durin::FMatrix4f ExpectedMatrix = Asset->FloatMatrix;
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	DMathStructAssetForTest* Loaded = nullptr;
	const Durin::Asset::FAssetResult LoadResult = Durin::Asset::LoadAsset(Path, Loaded);
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->FloatQuat, Durin::FQuatf(0.5f, 0.25f, -0.5f, 0.75f));
	EXPECT_EQ(Loaded->FloatMatrix, ExpectedMatrix);
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, WriterUsesVersionedWireSignaturesForLogicalEncodings)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/WireSignatures", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "WireSignatures.dasset";
	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(File.generic_string(), Inspection));
	ASSERT_FALSE(Inspection.Objects.empty());
	const Durin::Asset::FAssetPackageObjectInspection& Object = Inspection.Objects.front();
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
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/CompleteInspection", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Value = 17;
	Asset->DefaultChild->Rename("InspectedChild");
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));

	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "CompleteInspection.dasset";
	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(File.generic_string(), Inspection));
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
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

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
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Snapshot/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.Root = AutoRoot,
			.ContentPath = ".",
			.bAutoScan = true},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Manual/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.Root = ManualRoot,
			.ContentPath = ".",
			.bAutoScan = false}};
	Durin::PathUtilities::FScopedMountRegistryFixture Mounts(Definitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	std::vector<std::byte> FirstBefore;
	std::vector<std::byte> SecondBefore;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstBefore, First));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondBefore, Second));

	const auto Snapshot = Durin::Asset::CaptureMountedAssetPackageSnapshot();
	ASSERT_EQ(Snapshot.Status, Durin::Asset::EAssetPackageSnapshotStatus::Completed);
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
	std::vector<std::byte> FirstAfter;
	std::vector<std::byte> SecondAfter;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstAfter, First));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondAfter, Second));
	EXPECT_EQ(FirstAfter, FirstBefore);
	EXPECT_EQ(SecondAfter, SecondBefore);

	const auto Cancelled = Durin::Asset::CaptureMountedAssetPackageSnapshot([] { return true; });
	EXPECT_EQ(Cancelled.Status, Durin::Asset::EAssetPackageSnapshotStatus::Cancelled);
	EXPECT_TRUE(Cancelled.Packages.empty());
}

TEST(FPackageAssetTests, PackageSavesRejectReadOnlyContentMounts)
{
	InitializeAssetTests();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "ReadOnlyPackageSave";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(Root);
	const std::array Definitions{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/ReadOnly/",
			.Owner = Durin::PathUtilities::EMountOwner::Extension,
			.Root = Root,
			.ContentPath = ".",
			.bAutoScan = true,
			.bContentWritable = false}};
	Durin::PathUtilities::FScopedMountRegistryFixture Mounts(Definitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();

	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/ReadOnly/Blocked", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Durin::DPackage* Package = Asset->GetPackage();
	const Durin::Asset::FAssetResult BundleSaved =
		Durin::Asset::SavePackagesAtomically(
			std::span<Durin::DPackage* const>(&Package, 1), {});
	EXPECT_EQ(BundleSaved.Error, Durin::Asset::EAssetError::ReadOnlyMode);
	EXPECT_EQ(BundleSaved.Message, "Content mount /ReadOnly/ is read-only.");
	const Durin::Asset::FAssetResult Saved =
		Durin::Asset::SavePackage(Package);
	EXPECT_EQ(Saved.Error, Durin::Asset::EAssetError::ReadOnlyMode);
	EXPECT_EQ(Saved.Message, "Content mount /ReadOnly/ is read-only.");
	EXPECT_FALSE(std::filesystem::exists(Root / "Blocked.dasset"));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(
		Package,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FPackageAssetTests, V4NoDeltaWriterAssociatesObjectPlansIndependentOfDiscoveryOrder)
{
	InitializeAssetTests();
	using namespace Durin::Asset;
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/V4ObjectPlanOrder", AssetPath));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(AssetPath, Asset));
	auto* ZetaChild = Durin::NewObject<Durin::DObject>(Asset, "ZetaChild");
	ASSERT_NE(ZetaChild, nullptr);
	ASSERT_NE(Durin::NewObject<Durin::DObject>(Asset, "AlphaChild"), nullptr);
	Asset->DefaultChild = ZetaChild;

	std::vector<std::byte> Bytes;
	PackageObjectStream::FAssetPackageWriteOptions Options;
	Options.DeltaMode = Durin::EDefaultDeltaMode::NoDelta;
	PackageObjectStream::FWriterDiagnostic Diagnostic;
	EXPECT_TRUE(PackageObjectStream::WriteAssetPackage(Asset->GetPackage(), Bytes, Options, &Diagnostic))
		<< Diagnostic.Message;
	EXPECT_FALSE(Bytes.empty());
	PackageObjectStream::FDecodedPackage Decoded;
	PackageObjectStream::FReaderDiagnostic ReaderDiagnostic;
	ASSERT_TRUE(PackageObjectStream::DecodePackage(Bytes, Decoded, {}, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	ASSERT_FALSE(Decoded.ObjectValues.empty());
	const auto& RootValues = Decoded.ObjectValues.front();
	const auto ReferenceOverride = std::ranges::find_if(RootValues.Overrides, [&](const auto& Override) {
		if (Override.SchemaId == 0 || Override.SchemaId > Decoded.Schemas.size()) return false;
		const auto& Schema = Decoded.Schemas[Override.SchemaId - 1];
		return Override.FieldId > 0 && Override.FieldId <= Schema.Fields.size()
			&& Schema.Fields[Override.FieldId - 1].Name == "DefaultChild";
	});
	ASSERT_NE(ReferenceOverride, RootValues.Overrides.end());
	ASSERT_EQ(ReferenceOverride->Value.ReferenceTag, 1);
	ASSERT_GT(ReferenceOverride->Value.ReferenceId, 0u);
	ASSERT_LE(ReferenceOverride->Value.ReferenceId, Decoded.Objects.size());
	EXPECT_EQ(Decoded.Objects[ReferenceOverride->Value.ReferenceId - 1].ObjectName, "ZetaChild");
}

TEST(FPackageAssetTests, V4PropertyLegacyNameLoadsAndResavesCanonically)
{
	InitializeAssetTests();
	using namespace Durin::Asset;
	Durin::FAssetPath SourcePath;
	Durin::FAssetPath LoadPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LegacyPropertySource", SourcePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LegacyPropertyLoaded", LoadPath));
	DPackageAssetForTest* Source = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Source));
	Source->Value = 417;

	PackageObjectStream::FAssetPackageWriteOptions Options;
	Options.DeltaMode = Durin::EDefaultDeltaMode::NoDelta;
	PackageObjectStream::FWriterDiagnostic WriterDiagnostic;
	std::vector<std::byte> CurrentBytes;
	ASSERT_TRUE(PackageObjectStream::WriteAssetPackage(
		Source->GetPackage(), CurrentBytes, Options, &WriterDiagnostic)) << WriterDiagnostic.Message;

	PackageObjectStream::FDecodedPackage LegacyPackage;
	PackageObjectStream::FReaderDiagnostic ReaderDiagnostic;
	ASSERT_TRUE(PackageObjectStream::DecodePackage(CurrentBytes, LegacyPackage, {}, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	auto Schema = std::ranges::find(
		LegacyPackage.Schemas, std::string("Tests::DPackageAssetForTest"),
		&PackageObjectStream::FDecodedSchema::QualifiedName);
	ASSERT_NE(Schema, LegacyPackage.Schemas.end());
	auto ValueField = std::ranges::find(
		Schema->Fields, std::string("Value"), &PackageObjectStream::FDecodedField::Name);
	ASSERT_NE(ValueField, Schema->Fields.end());
	ValueField->Name = "LegacyValue";

	std::vector<std::byte> LegacyBytes;
	ASSERT_TRUE(PackageObjectStream::ReencodePackage(LegacyPackage, LegacyBytes, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	const FReflectionCompatibilityCatalog Catalog = FReflectionCompatibilityCatalog::Capture();
	const FReflectionSerializedPropertyAlias* Alias =
		Catalog.FindSerializedPropertyAlias("Tests::DPackageAssetForTest", "LegacyValue");
	ASSERT_NE(Alias, nullptr);
	EXPECT_EQ(Alias->CurrentName, "Value");

	FAssetPackageCompatibilityRecord Compatibility;
	ASSERT_TRUE(PackageObjectStream::ProbeCompatibility(
		LegacyBytes, LoadPath, Catalog, Compatibility, nullptr, {}, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	EXPECT_EQ(Compatibility.Compatibility, EAssetPackageCompatibility::Compatible);
	EXPECT_TRUE(std::ranges::any_of(Compatibility.CanonicalizationEvidence, [](const auto& Evidence) {
		return Evidence.Kind == EAssetReflectedIdentityKind::Property
			&& Evidence.StoredIdentity == "LegacyValue"
			&& Evidence.CurrentIdentity == "Value";
	}));

	PackageObjectStream::FLoadedAssetPackage Loaded;
	FAssetLoadReport LoadReport;
	const FAssetResult Load = PackageObjectStream::LoadAssetPackage(
		LegacyBytes, LoadPath, Loaded, &LoadReport, {}, {}, &ReaderDiagnostic);
	ASSERT_TRUE(Load) << Load.Message << ": " << ReaderDiagnostic.Message;
	ASSERT_NE(Loaded.GetPackage(), nullptr);
	auto* LoadedAsset = static_cast<DPackageAssetForTest*>(Loaded.GetPackage()->GetAsset());
	ASSERT_NE(LoadedAsset, nullptr);
	EXPECT_EQ(LoadedAsset->Value, 417);
	EXPECT_TRUE(Loaded.GetPackage()->IsCanonicalResaveRecommended());
	EXPECT_TRUE(std::ranges::any_of(LoadReport.CanonicalizationEvidence, [](const auto& Evidence) {
		return Evidence.Kind == EAssetReflectedIdentityKind::Property;
	}));

	std::vector<std::byte> CanonicalBytes;
	ASSERT_TRUE(PackageObjectStream::WriteAssetPackage(
		Loaded.GetPackage(), CanonicalBytes, Options, &WriterDiagnostic)) << WriterDiagnostic.Message;
	PackageObjectStream::FDecodedPackage CanonicalPackage;
	ASSERT_TRUE(PackageObjectStream::DecodePackage(CanonicalBytes, CanonicalPackage, {}, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	const auto CanonicalSchema = std::ranges::find(
		CanonicalPackage.Schemas, std::string("Tests::DPackageAssetForTest"),
		&PackageObjectStream::FDecodedSchema::QualifiedName);
	ASSERT_NE(CanonicalSchema, CanonicalPackage.Schemas.end());
	EXPECT_NE(std::ranges::find(
		CanonicalSchema->Fields, std::string("Value"), &PackageObjectStream::FDecodedField::Name),
		CanonicalSchema->Fields.end());
	EXPECT_EQ(std::ranges::find(
		CanonicalSchema->Fields, std::string("LegacyValue"), &PackageObjectStream::FDecodedField::Name),
		CanonicalSchema->Fields.end());

	PackageObjectStream::FDecodedPackage CollisionPackage = LegacyPackage;
	auto CollisionSchema = std::ranges::find(
		CollisionPackage.Schemas, std::string("Tests::DPackageAssetForTest"),
		&PackageObjectStream::FDecodedSchema::QualifiedName);
	ASSERT_NE(CollisionSchema, CollisionPackage.Schemas.end());
	auto LabelField = std::ranges::find(
		CollisionSchema->Fields, std::string("Label"), &PackageObjectStream::FDecodedField::Name);
	ASSERT_NE(LabelField, CollisionSchema->Fields.end());
	LabelField->Name = "Value";
	std::vector<std::byte> CollisionBytes;
	ASSERT_TRUE(PackageObjectStream::ReencodePackage(CollisionPackage, CollisionBytes, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	FAssetPackageCompatibilityRecord CollisionCompatibility;
	const FAssetResult CollisionProbe = PackageObjectStream::ProbeCompatibility(
		CollisionBytes, LoadPath, Catalog, CollisionCompatibility, nullptr, {}, &ReaderDiagnostic);
	EXPECT_EQ(CollisionProbe.Error, EAssetError::CorruptFile);
	EXPECT_EQ(ReaderDiagnostic.Failure, PackageObjectStream::EReaderFailure::InvalidTable);
	Loaded.Reset();
}

TEST(FPackageAssetTests, V4ContainerNestedDeprecatedRoutesEmitAndValidateCustomVersions)
{
	InitializeAssetTests();
	using namespace Durin;
	using namespace Durin::Asset;
	FAssetPath SourcePath;
	FAssetPath FuturePath;
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/ContainerMigrationSource", SourcePath));
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/ContainerMigrationFuture", FuturePath));
	DContainerMigrationAssetForTest* Source = nullptr;
	ASSERT_TRUE(CreateAsset(SourcePath, Source));
	Source->Values.push_back({.Value = 12.0f});

	PackageObjectStream::FAssetPackageWriteOptions Options;
	Options.DeltaMode = EDefaultDeltaMode::NoDelta;
	PackageObjectStream::FWriterDiagnostic WriterDiagnostic;
	std::vector<std::byte> Bytes;
	ASSERT_TRUE(PackageObjectStream::WriteAssetPackage(
		Source->GetPackage(), Bytes, Options, &WriterDiagnostic)) << WriterDiagnostic.Message;
	PackageObjectStream::FDecodedPackage Decoded;
	PackageObjectStream::FReaderDiagnostic ReaderDiagnostic;
	ASSERT_TRUE(PackageObjectStream::DecodePackage(Bytes, Decoded, {}, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	auto Version = std::ranges::find(
		Decoded.CustomVersions, AssetStructTest::FStructMigrationVersion::Guid,
		&PackageObjectStream::FCustomVersion::Guid);
	ASSERT_NE(Version, Decoded.CustomVersions.end());
	EXPECT_EQ(Version->Value, AssetStructTest::FStructMigrationVersion::LatestVersion);

	Version->Value = AssetStructTest::FStructMigrationVersion::LatestVersion + 1;
	std::vector<std::byte> FutureBytes;
	ASSERT_TRUE(PackageObjectStream::ReencodePackage(Decoded, FutureBytes, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	PackageObjectStream::FLoadedAssetPackage Loaded;
	EXPECT_FALSE(PackageObjectStream::LoadAssetPackage(
		FutureBytes, FuturePath, Loaded, nullptr, {}, {}, &ReaderDiagnostic));
	EXPECT_EQ(FindResidentPackage(FuturePath), nullptr);
}

TEST(FPackageAssetTests, DeprecatedRoutesMigrateVersionedFieldsAndAuthoredIntent)
{
	InitializeAssetTests();
	using namespace Durin;
	using namespace Durin::Asset;
	FAssetPath SourcePath;
	FAssetPath LegacyLoadPath;
	FAssetPath CurrentLoadPath;
	FAssetPath FailedLoadPath;
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/SchemaMigrationSource", SourcePath));
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/SchemaMigrationLegacy", LegacyLoadPath));
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/SchemaMigrationCurrent", CurrentLoadPath));
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/SchemaMigrationFailed", FailedLoadPath));
	DSchemaMigrationAssetForTest* Source = nullptr;
	ASSERT_TRUE(CreateAsset(SourcePath, Source));
	Source->A = 21.0f;
	Source->B = 22.0f;
	Source->Merged = 23;
	Source->Distance = 24.0f;
	Source->Anchor = 25;
	Source->StructData.Value = 26.0f;

	PackageObjectStream::FAssetPackageWriteOptions Options;
	Options.DeltaMode = EDefaultDeltaMode::NoDelta;
	PackageObjectStream::FWriterDiagnostic WriterDiagnostic;
	std::vector<std::byte> CurrentBytes;
	ASSERT_TRUE(PackageObjectStream::WriteAssetPackage(
		Source->GetPackage(), CurrentBytes, Options, &WriterDiagnostic)) << WriterDiagnostic.Message;
	PackageObjectStream::FDecodedPackage CurrentPackage;
	PackageObjectStream::FReaderDiagnostic ReaderDiagnostic;
	ASSERT_TRUE(PackageObjectStream::DecodePackage(CurrentBytes, CurrentPackage, {}, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	const auto CurrentVersion = std::ranges::find(
		CurrentPackage.CustomVersions, FSchemaMigrationVersion::Guid,
		&PackageObjectStream::FCustomVersion::Guid);
	ASSERT_NE(CurrentVersion, CurrentPackage.CustomVersions.end());
	EXPECT_EQ(CurrentVersion->Value, FSchemaMigrationVersion::LatestVersion);

	auto Schema = std::ranges::find(
		CurrentPackage.Schemas, std::string("Tests::DSchemaMigrationAssetForTest"),
		&PackageObjectStream::FDecodedSchema::QualifiedName);
	ASSERT_NE(Schema, CurrentPackage.Schemas.end());
	const auto FindField = [&](std::string_view Name) {
		return std::ranges::find(Schema->Fields, std::string(Name), &PackageObjectStream::FDecodedField::Name);
	};
	auto AField = FindField("A");
	auto BField = FindField("B");
	auto MergedField = FindField("Merged");
	auto DistanceField = FindField("Distance");
	auto AnchorField = FindField("Anchor");
	auto StructDataField = FindField("StructData");
	ASSERT_NE(AField, Schema->Fields.end());
	ASSERT_NE(BField, Schema->Fields.end());
	ASSERT_NE(MergedField, Schema->Fields.end());
	ASSERT_NE(DistanceField, Schema->Fields.end());
	ASSERT_NE(AnchorField, Schema->Fields.end());
	ASSERT_NE(StructDataField, Schema->Fields.end());
	const uint64 SchemaId = static_cast<uint64>(std::distance(CurrentPackage.Schemas.begin(), Schema)) + 1;
	const uint64 AFieldId = static_cast<uint64>(std::distance(Schema->Fields.begin(), AField)) + 1;
	const uint64 BFieldId = static_cast<uint64>(std::distance(Schema->Fields.begin(), BField)) + 1;
	const uint64 MergedFieldId = static_cast<uint64>(std::distance(Schema->Fields.begin(), MergedField)) + 1;
	const uint64 DistanceFieldId = static_cast<uint64>(std::distance(Schema->Fields.begin(), DistanceField)) + 1;
	const uint64 StructDataFieldId = static_cast<uint64>(
		std::distance(Schema->Fields.begin(), StructDataField)) + 1;
	const uint64 Int32TypeId = AnchorField->TypeId;
	const auto FindOverride = [&](PackageObjectStream::FDecodedPackage& Package, uint64 FieldId) {
		return std::ranges::find_if(Package.ObjectValues.front().Overrides,
			[&](const PackageObjectStream::FDecodedOverride& Override) {
				return Override.SchemaId == SchemaId && Override.FieldId == FieldId;
			});
	};

	PackageObjectStream::FDecodedPackage LegacyPackage = CurrentPackage;
	LegacyPackage.CustomVersions.clear();
	auto LegacySchema = LegacyPackage.Schemas.begin() + static_cast<ptrdiff_t>(SchemaId - 1);
	LegacySchema->Fields[AFieldId - 1].TypeId = Int32TypeId;
	LegacySchema->Fields[BFieldId - 1].Name = "Left";
	LegacySchema->Fields[BFieldId - 1].TypeId = Int32TypeId;
	LegacySchema->Fields[MergedFieldId - 1].Name = "Right";
	auto LegacyStructSchema = std::ranges::find(
		LegacyPackage.Schemas, std::string("Tests::FMigratingValue"),
		&PackageObjectStream::FDecodedSchema::QualifiedName);
	ASSERT_NE(LegacyStructSchema, LegacyPackage.Schemas.end());
	auto LegacyStructValueField = std::ranges::find(
		LegacyStructSchema->Fields, std::string("Value"), &PackageObjectStream::FDecodedField::Name);
	ASSERT_NE(LegacyStructValueField, LegacyStructSchema->Fields.end());
	LegacyStructValueField->TypeId = Int32TypeId;
	auto AOverride = FindOverride(LegacyPackage, AFieldId);
	auto LeftOverride = FindOverride(LegacyPackage, BFieldId);
	auto RightOverride = FindOverride(LegacyPackage, MergedFieldId);
	auto DistanceOverride = FindOverride(LegacyPackage, DistanceFieldId);
	auto StructDataOverride = FindOverride(LegacyPackage, StructDataFieldId);
	ASSERT_NE(AOverride, LegacyPackage.ObjectValues.front().Overrides.end());
	ASSERT_NE(LeftOverride, LegacyPackage.ObjectValues.front().Overrides.end());
	ASSERT_NE(RightOverride, LegacyPackage.ObjectValues.front().Overrides.end());
	ASSERT_NE(DistanceOverride, LegacyPackage.ObjectValues.front().Overrides.end());
	ASSERT_NE(StructDataOverride, LegacyPackage.ObjectValues.front().Overrides.end());
	ASSERT_EQ(StructDataOverride->Value.Elements.size(), 1u);
	AOverride->Value.Signed = 10;
	AOverride->Provenance = 1;
	LeftOverride->Value.Signed = 3;
	LeftOverride->Provenance = 0;
	RightOverride->Value.Signed = 4;
	RightOverride->Provenance = 1;
	DistanceOverride->Value.FloatingBits = std::bit_cast<uint32>(1.5f);
	StructDataOverride->Value.Elements.front().Signed = 8;

	std::vector<std::byte> LegacyBytes;
	ASSERT_TRUE(PackageObjectStream::ReencodePackage(LegacyPackage, LegacyBytes, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	const FReflectionCompatibilityCatalog Catalog = FReflectionCompatibilityCatalog::Capture();
	FAssetPackageCompatibilityRecord Compatibility;
	ASSERT_TRUE(PackageObjectStream::ProbeCompatibility(
		LegacyBytes, LegacyLoadPath, Catalog, Compatibility, nullptr, {}, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	EXPECT_EQ(Compatibility.Compatibility, EAssetPackageCompatibility::Compatible);
	EXPECT_EQ(Compatibility.DeprecatedRouteEvidence.size(), 5u);
	EXPECT_EQ(std::ranges::count(Compatibility.Findings,
		EAssetCompatibilityFindingCode::DeprecatedRouteUsed,
		&FAssetCompatibilityFinding::Code), 5);
	PackageObjectStream::FLoadedAssetPackage Loaded;
	FAssetLoadReport LoadReport;
	const FAssetResult Load = PackageObjectStream::LoadAssetPackage(
		LegacyBytes, LegacyLoadPath, Loaded, &LoadReport, {}, {}, &ReaderDiagnostic);
	ASSERT_TRUE(Load) << Load.Message << ": " << ReaderDiagnostic.Message;
	auto* Migrated = static_cast<DSchemaMigrationAssetForTest*>(Loaded.GetPackage()->GetAsset());
	ASSERT_NE(Migrated, nullptr);
	EXPECT_TRUE(Loaded.GetPackage()->IsCanonicalResaveRecommended());
	EXPECT_EQ(LoadReport.DeprecatedRouteEvidence.size(), 5u);
	EXPECT_FLOAT_EQ(Migrated->A, 5.0f);
	EXPECT_FLOAT_EQ(Migrated->B, 20.0f);
	EXPECT_EQ(Migrated->Merged, 7);
	EXPECT_FLOAT_EQ(Migrated->Distance, 150.0f);
	EXPECT_FLOAT_EQ(Migrated->StructData.Value, 80.0f);
	EXPECT_EQ(Migrated->StructData.Value_DEPRECATED, 8);
	EXPECT_GT(AssetStructTest::MigrationPostDeserializeCount, 0u);
	const auto Ledger = Migrated->GetAuthoredOverrideEntries();
	const auto FindLedger = [&](FName FieldName) {
		return std::ranges::find_if(Ledger, [&](const FAuthoredOverrideEntry& Entry) {
			return Entry.Path.size() == 1 && Entry.Path.front().FieldName == FieldName;
		});
	};
	ASSERT_NE(FindLedger(FName("A")), Ledger.end());
	ASSERT_NE(FindLedger(FName("B")), Ledger.end());
	ASSERT_NE(FindLedger(FName("Merged")), Ledger.end());
	ASSERT_NE(FindLedger(FName("Distance")), Ledger.end());
	EXPECT_EQ(FindLedger(FName("A"))->Provenance, EAuthoredOverrideProvenance::Forced);
	EXPECT_EQ(FindLedger(FName("B"))->Provenance, EAuthoredOverrideProvenance::Forced);
	EXPECT_EQ(FindLedger(FName("Merged"))->Provenance, EAuthoredOverrideProvenance::Forced);
	EXPECT_EQ(FindLedger(FName("Distance"))->Provenance,
		EAuthoredOverrideProvenance::Forced);

	std::vector<std::byte> CanonicalBytes;
	ASSERT_TRUE(PackageObjectStream::WriteAssetPackage(
		Loaded.GetPackage(), CanonicalBytes, Options, &WriterDiagnostic)) << WriterDiagnostic.Message;
	PackageObjectStream::FDecodedPackage CanonicalPackage;
	ASSERT_TRUE(PackageObjectStream::DecodePackage(CanonicalBytes, CanonicalPackage, {}, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	const auto CanonicalSchema = std::ranges::find(
		CanonicalPackage.Schemas, std::string("Tests::DSchemaMigrationAssetForTest"),
		&PackageObjectStream::FDecodedSchema::QualifiedName);
	ASSERT_NE(CanonicalSchema, CanonicalPackage.Schemas.end());
	for (std::string_view OldName : {"A_DEPRECATED", "Left", "Left_DEPRECATED", "Right",
		"Right_DEPRECATED", "Distance_DEPRECATED"})
		EXPECT_EQ(std::ranges::find(CanonicalSchema->Fields, std::string(OldName),
			&PackageObjectStream::FDecodedField::Name), CanonicalSchema->Fields.end());

	PackageObjectStream::FLoadedAssetPackage CurrentLoaded;
	ASSERT_TRUE(PackageObjectStream::LoadAssetPackage(
		CurrentBytes, CurrentLoadPath, CurrentLoaded, nullptr, {}, {}, &ReaderDiagnostic));
	auto* Current = static_cast<DSchemaMigrationAssetForTest*>(CurrentLoaded.GetPackage()->GetAsset());
	ASSERT_NE(Current, nullptr);
	EXPECT_FLOAT_EQ(Current->A, 21.0f);
	EXPECT_FLOAT_EQ(Current->B, 22.0f);
	EXPECT_EQ(Current->Merged, 23);
	EXPECT_FLOAT_EQ(Current->Distance, 24.0f);
	EXPECT_FLOAT_EQ(Current->StructData.Value, 26.0f);

	GRejectSchemaMigrationPostLoad = true;
	PackageObjectStream::FLoadedAssetPackage Rejected;
	const FAssetResult RejectedResult = PackageObjectStream::LoadAssetPackage(
		LegacyBytes, FailedLoadPath, Rejected, nullptr, {}, {}, &ReaderDiagnostic);
	GRejectSchemaMigrationPostLoad = false;
	EXPECT_FALSE(RejectedResult);
	EXPECT_EQ(FindResidentPackage(FailedLoadPath), nullptr);
	FAssetPath StructFailedPath;
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/SchemaMigrationStructFailed", StructFailedPath));
	AssetStructTest::RejectMigrationPostDeserialize = true;
	PackageObjectStream::FLoadedAssetPackage StructRejected;
	const FAssetResult StructRejectedResult = PackageObjectStream::LoadAssetPackage(
		LegacyBytes, StructFailedPath, StructRejected, nullptr, {}, {}, &ReaderDiagnostic);
	AssetStructTest::RejectMigrationPostDeserialize = false;
	EXPECT_FALSE(StructRejectedResult);
	EXPECT_EQ(FindResidentPackage(StructFailedPath), nullptr);

	PackageObjectStream::FDecodedPackage NewerPackage = CurrentPackage;
	auto NewerVersion = std::ranges::find(
		NewerPackage.CustomVersions, FSchemaMigrationVersion::Guid,
		&PackageObjectStream::FCustomVersion::Guid);
	ASSERT_NE(NewerVersion, NewerPackage.CustomVersions.end());
	NewerVersion->Value = FSchemaMigrationVersion::LatestVersion + 1;
	std::vector<std::byte> NewerBytes;
	ASSERT_TRUE(PackageObjectStream::ReencodePackage(NewerPackage, NewerBytes, &ReaderDiagnostic));
	FAssetPath NewerPath;
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/SchemaMigrationNewer", NewerPath));
	PackageObjectStream::FLoadedAssetPackage NewerLoaded;
	EXPECT_FALSE(PackageObjectStream::LoadAssetPackage(
		NewerBytes, NewerPath, NewerLoaded, nullptr, {}, {}, &ReaderDiagnostic));
	EXPECT_EQ(FindResidentPackage(NewerPath), nullptr);

	PackageObjectStream::FDecodedPackage OverflowPackage = CurrentPackage;
	auto OverflowVersion = std::ranges::find(
		OverflowPackage.CustomVersions, FSchemaMigrationVersion::Guid,
		&PackageObjectStream::FCustomVersion::Guid);
	ASSERT_NE(OverflowVersion, OverflowPackage.CustomVersions.end());
	OverflowVersion->Value = static_cast<uint32>(std::numeric_limits<int32>::max()) + 1u;
	std::vector<std::byte> OverflowBytes;
	ASSERT_TRUE(PackageObjectStream::ReencodePackage(OverflowPackage, OverflowBytes, &ReaderDiagnostic));
	PackageObjectStream::FDecodedPackage RejectedOverflowPackage;
	EXPECT_FALSE(PackageObjectStream::DecodePackage(
		OverflowBytes, RejectedOverflowPackage, {}, &ReaderDiagnostic));
	EXPECT_EQ(ReaderDiagnostic.Failure, PackageObjectStream::EReaderFailure::InvalidTable);

	PackageObjectStream::FDecodedPackage IncompatiblePackage = LegacyPackage;
	PackageObjectStream::FDecodedType IncompatibleType = IncompatiblePackage.Types[Int32TypeId - 1];
	IncompatibleType.Opcode = PackageObjectStream::ETypeOpcode::I64;
	IncompatiblePackage.Types.push_back(std::move(IncompatibleType));
	IncompatiblePackage.Schemas[SchemaId - 1].Fields[AFieldId - 1].TypeId =
		IncompatiblePackage.Types.size();
	std::vector<std::byte> IncompatibleBytes;
	ASSERT_TRUE(PackageObjectStream::ReencodePackage(
		IncompatiblePackage, IncompatibleBytes, &ReaderDiagnostic)) << ReaderDiagnostic.Message;
	FAssetPath IncompatiblePath;
	ASSERT_TRUE(FAssetPath::TryCreate("/TestAssets/SchemaMigrationIncompatible", IncompatiblePath));
	PackageObjectStream::FLoadedAssetPackage IncompatibleLoaded;
	EXPECT_FALSE(PackageObjectStream::LoadAssetPackage(
		IncompatibleBytes, IncompatiblePath, IncompatibleLoaded, nullptr, {}, {}, &ReaderDiagnostic));
	EXPECT_EQ(FindResidentPackage(IncompatiblePath), nullptr);

	Loaded.Reset();
	CurrentLoaded.Reset();
}

TEST(FPackageAssetTests, PackageLoadSnapshotReleasesOnlyPackagesIntroducedAfterCapture)
{
	InitializeAssetTests();
	Durin::FAssetPath ExistingPath;
	Durin::FAssetPath IntroducedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/ExistingOwnership", ExistingPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/IntroducedOwnership", IntroducedPath));

	DPackageAssetForTest* Existing = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ExistingPath, Existing));
	ASSERT_TRUE(Durin::Asset::SavePackage(Existing->GetPackage()));
	const Durin::Asset::FAssetPackageLoadSnapshot Snapshot =
		Durin::Asset::CapturePackageLoadSnapshot();

	DPackageAssetForTest* Introduced = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(IntroducedPath, Introduced));
	ASSERT_TRUE(Durin::Asset::SavePackage(Introduced->GetPackage()));
	ASSERT_TRUE(Durin::Asset::ReleasePackagesLoadedSince(Snapshot));

	EXPECT_NE(Durin::Asset::FindResidentPackage(ExistingPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(IntroducedPath), nullptr);
	EXPECT_TRUE(Durin::Asset::UnloadPackage(ExistingPath));
}

TEST(FPackageAssetTests, RejectsInvalidPaths)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("TestAssets/Relative", Path));
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("/TestAssets/../Escape", Path));
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("/Unknown/Asset", Path));
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("/TestAssets/With.dasset", Path));
}

TEST(FPackageAssetTests, RejectsSavingCppPackages)
{
	InitializeAssetTests();
	Durin::DPackage* Package = Durin::FindOrCreateCppPackage("AssetCoreTests");
	ASSERT_NE(Package, nullptr);
	EXPECT_EQ(Durin::Asset::SavePackage(Package).Error, Durin::Asset::EAssetError::InvalidPackageType);
}

TEST(FPackageAssetTests, PackageEditRevisionAdvancesForRepeatedDirtyEdits)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/EditRevision", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
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
	Durin::FAssetPath FirstPath;
	Durin::FAssetPath BlockedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Stage0First", FirstPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Stage0Blocked/Second", BlockedPath));

	DPackageAssetForTest* First = nullptr;
	DPackageAssetForTest* Second = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(FirstPath, First));
	ASSERT_TRUE(Durin::Asset::CreateAsset(BlockedPath, Second));
	First->Value = 1;
	Second->Value = 2;

	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "Assets";
	const std::filesystem::path Blocker = Root / "Stage0Blocked";
	{
		std::ofstream Stream(Blocker);
		ASSERT_TRUE(Stream.is_open());
		Stream << "blocks destination directory creation";
	}

	ASSERT_TRUE(Durin::Asset::SavePackage(First->GetPackage()));
	const Durin::Asset::FAssetResult SecondResult = Durin::Asset::SavePackage(Second->GetPackage());
	EXPECT_EQ(SecondResult.Error, Durin::Asset::EAssetError::IoError);

	EXPECT_TRUE(std::filesystem::is_regular_file(Root / "Stage0First.dasset"));
	EXPECT_NE(Durin::Asset::FindAssetExact(FirstPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(FirstPath), First->GetPackage());
	EXPECT_FALSE(First->GetPackage()->IsNewlyCreated());
	EXPECT_FALSE(First->GetPackage()->IsDirty());

	EXPECT_FALSE(std::filesystem::exists(Root / "Stage0Blocked" / "Second.dasset"));
	EXPECT_EQ(Durin::Asset::FindAssetExact(BlockedPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(BlockedPath), Second->GetPackage());
	EXPECT_TRUE(Second->GetPackage()->IsNewlyCreated());
	EXPECT_TRUE(Second->GetPackage()->IsDirty());

	ASSERT_TRUE(std::filesystem::remove(Blocker));
	ASSERT_TRUE(Durin::Asset::SavePackage(Second->GetPackage()));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(BlockedPath), Second->GetPackage());
	EXPECT_FALSE(Second->GetPackage()->IsNewlyCreated());
	EXPECT_TRUE(Durin::Asset::FindAssetExact(BlockedPath));
}

TEST(FPackageAssetTests, AtomicBundleSaveRestoresFilesRegistryAndDirtyStateOnFailure)
{
	InitializeAssetTests();
	Durin::FAssetPath ExistingPath;
	Durin::FAssetPath NewPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AtomicBundleExisting", ExistingPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AtomicBundleNew", NewPath));

	DPackageAssetForTest* Existing = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ExistingPath, Existing));
	Existing->Value = 11;
	ASSERT_TRUE(Durin::Asset::SavePackage(Existing->GetPackage()));
	const Durin::Asset::FAssetData ExistingRegistry =
		*Durin::Asset::FindAssetExact(ExistingPath);
	std::vector<std::byte> ExistingBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		ExistingBytes, ExistingRegistry.PhysicalPath
	));

	DPackageAssetForTest* Added = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(NewPath, Added));
	Added->Value = 22;
	Existing->Value = 33;
	Existing->MarkPackageDirty();
	const std::array Packages = {
		Existing->GetPackage(),
		Added->GetPackage()
	};
	const Durin::Asset::FAssetResult Result = Durin::Asset::SavePackagesAtomically(
		Packages,
		{.RootPackage = Added->GetPackage(),
		 .ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
			 return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRegistry;
		 }}
	);
	EXPECT_FALSE(Result);

	std::vector<std::byte> RestoredBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		RestoredBytes, ExistingRegistry.PhysicalPath
	));
	EXPECT_EQ(RestoredBytes, ExistingBytes);
	EXPECT_EQ(*Durin::Asset::FindAssetExact(ExistingPath), ExistingRegistry);
	EXPECT_EQ(Durin::Asset::FindAssetExact(NewPath), nullptr);
	EXPECT_FALSE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "AtomicBundleNew.dasset"
	));
	EXPECT_TRUE(Existing->GetPackage()->IsDirty());
	EXPECT_TRUE(Added->GetPackage()->IsDirty());
	EXPECT_EQ(Durin::Asset::FindResidentPackage(NewPath), Added->GetPackage());
	EXPECT_TRUE(Added->GetPackage()->IsNewlyCreated());
	const uint64 RevisionBeforeCommit = Durin::Asset::GetAssetCatalogRevision();
	ASSERT_TRUE(Durin::Asset::SavePackagesAtomically(
		Packages, {.RootPackage = Added->GetPackage()}));
	EXPECT_EQ(Durin::Asset::GetAssetCatalogRevision(), RevisionBeforeCommit + 1);
	EXPECT_TRUE(Durin::Asset::FindAssetExact(ExistingPath));
	EXPECT_TRUE(Durin::Asset::FindAssetExact(NewPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Added->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FPackageAssetTests, OrdinaryV6SavesAreDeterministic)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/OrdinaryV4Policy", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Value = 41;
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));

	const Durin::Asset::FAssetData Current = *Durin::Asset::FindAssetExact(Path);
	ASSERT_EQ(Current.FormatVersion, Durin::Asset::AssetPackageV6FormatVersion);
	std::vector<std::byte> FirstBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstBytes, Current.PhysicalPath));
	std::vector<std::byte> FirstSerialization;
	std::vector<std::byte> SecondSerialization;
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(
		Asset->GetPackage(), FirstSerialization));
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(
		Asset->GetPackage(), SecondSerialization));
	EXPECT_EQ(FirstSerialization, SecondSerialization);
	EXPECT_EQ(FirstSerialization, FirstBytes);
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	std::vector<std::byte> RepeatedBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(RepeatedBytes, Current.PhysicalPath));
	EXPECT_EQ(RepeatedBytes, FirstBytes);

}


TEST(FPackageAssetTests, LoadsExternalDependenciesAndPreventsPrematureUnload)
{
	InitializeAssetTests();
	Durin::FAssetPath DependencyPath;
	Durin::FAssetPath OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Dependency", DependencyPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Owner", OwnerPath));

	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(DependencyPath, Dependency));
	Dependency->Label = "Dependency";
	ASSERT_TRUE(Durin::Asset::SavePackage(Dependency->GetPackage()));

	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	ASSERT_EQ(Durin::Asset::FindAssetExact(OwnerPath)->Dependencies.size(), 1u);
	EXPECT_EQ(Durin::Asset::UnloadPackage(DependencyPath).Error, Durin::Asset::EAssetError::InUse);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(DependencyPath));

	DPackageAssetForTest* LoadedOwner = nullptr;
	Durin::Asset::FAssetLoadReport LoadReport;
	ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, LoadedOwner, &LoadReport));
	EXPECT_EQ(LoadReport.PackageFileReadCount, 2u);
	ASSERT_NE(LoadedOwner->ExternalReference.Get(), nullptr);
	EXPECT_EQ(LoadedOwner->ExternalReference->GetObjectPath(), "/TestAssets/Dependency");
	EXPECT_EQ(Durin::Asset::FindResidentPackage(DependencyPath)->GetAsset(), LoadedOwner->ExternalReference.Get());

	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(DependencyPath));
	ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry());
	EXPECT_TRUE(Durin::Asset::FindAssetExact(OwnerPath));
	EXPECT_TRUE(Durin::Asset::FindAssetExact(DependencyPath));
}

TEST(FPackageAssetTests, RuntimeStrongReferencePreventsUnloadAndRestoresResidency)
{
	InitializeAssetTests();
	Durin::FAssetPath TargetPath, OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RuntimeUnloadTarget", TargetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RuntimeUnloadOwner", OwnerPath));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	Durin::DPackage* TargetPackage = Target->GetPackage();

	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Target;
	const Durin::Asset::FAssetResult Result =
		Durin::Asset::UnloadPackage(TargetPath);
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::InUse);
	EXPECT_EQ(Result.Message, "Package remains referenced by live objects.");
	EXPECT_EQ(Durin::Asset::FindResidentPackage(TargetPath), TargetPackage);
	EXPECT_TRUE(TargetPackage->HasAnyObjectFlags(Durin::EObjectFlags::Standalone));
	EXPECT_FALSE(TargetPackage->IsGarbage());
	EXPECT_EQ(Owner->ExternalReference.Get(), Target);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		OwnerPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(TargetPath));
}

TEST(FPackageAssetTests, RejectsTruncatedPackagesWithoutCachingPartialObjects)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Corrupt", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	const std::filesystem::path File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "Corrupt.dasset";
	std::filesystem::resize_file(File, 12);
	Durin::DObject* Loaded = nullptr;
	const Durin::Asset::FAssetResult Result = Durin::Asset::LoadAsset(Path, Loaded);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::CorruptFile);
	EXPECT_EQ(Loaded, nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), nullptr);
}

TEST(FPackageAssetTests, RelocationTransactionOwnsStateMachineAndExactUndoRedo)
{
	InitializeAssetTests();
	Durin::FAssetPath First;
	Durin::FAssetPath FirstMoved;
	Durin::FAssetPath Second;
	Durin::FAssetPath SecondMoved;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/BatchFirst", First));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/BatchFirstMoved", FirstMoved));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/BatchSecond", Second));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/BatchSecondMoved", SecondMoved));
	DPackageAssetForTest* FirstAsset = nullptr;
	DPackageAssetForTest* SecondAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(First, FirstAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(FirstAsset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::CreateAsset(Second, SecondAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(SecondAsset->GetPackage()));

	const std::array Mappings = {
		Durin::Asset::FAssetRelocationMapping{First, FirstMoved},
		Durin::Asset::FAssetRelocationMapping{Second, SecondMoved}
	};
	Durin::Asset::FAssetMutationSummary Summary;
	Durin::Asset::FAssetMutationTransaction Transaction;
	const Durin::Asset::FAssetResult Analysis =
		Durin::Asset::PrepareAssetRelocationTransaction(
			Mappings, Summary, Transaction);
	ASSERT_TRUE(Analysis) << Analysis.Message;
	EXPECT_EQ(Summary.GetOperationKind(),
		Durin::Asset::EAssetMutationOperationKind::Relocation);
	EXPECT_EQ(Summary.GetScope().size(), 4u);
	EXPECT_EQ(Transaction.GetState(),
		Durin::Asset::EAssetMutationTransactionState::Prepared);
	EXPECT_EQ(Transaction.Undo().Error, Durin::Asset::EAssetError::StaleData);
	EXPECT_EQ(Transaction.Redo().Error, Durin::Asset::EAssetError::StaleData);
	const uint64 BeforeRevision =
		Durin::Asset::GetAssetCatalogRevision();
	ASSERT_TRUE(Transaction.Commit());
	EXPECT_EQ(Transaction.GetState(),
		Durin::Asset::EAssetMutationTransactionState::Committed);
	EXPECT_EQ(Transaction.Commit().Error, Durin::Asset::EAssetError::StaleData);
	EXPECT_EQ(Durin::Asset::GetAssetCatalogRevision(), BeforeRevision + 1);
	EXPECT_EQ(Durin::Asset::FindAssetExact(First)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Durin::Asset::FindAssetExact(Second)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Durin::Asset::FindAssetExact(FirstMoved)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::FindAssetExact(SecondMoved)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);

	ASSERT_TRUE(Transaction.Undo());
	EXPECT_EQ(Transaction.GetState(),
		Durin::Asset::EAssetMutationTransactionState::Undone);
	EXPECT_EQ(Transaction.Undo().Error, Durin::Asset::EAssetError::StaleData);
	EXPECT_EQ(Durin::Asset::GetAssetCatalogRevision(), BeforeRevision + 2);
	EXPECT_EQ(Durin::Asset::FindAssetExact(First)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::FindAssetExact(Second)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::FindAssetExact(FirstMoved), nullptr);
	EXPECT_EQ(Durin::Asset::FindAssetExact(SecondMoved), nullptr);

	ASSERT_TRUE(Transaction.Redo());
	EXPECT_EQ(Durin::Asset::GetAssetCatalogRevision(), BeforeRevision + 3);
	EXPECT_EQ(Durin::Asset::ResolveAssetPath(First).FinalPath, FirstMoved);
	EXPECT_EQ(Durin::Asset::ResolveAssetPath(Second).FinalPath, SecondMoved);
}

TEST(FPackageAssetTests, RelocationTransactionRejectsStaleCommitWithoutMutatingState)
{
	InitializeAssetTests();
	Durin::FAssetPath Source;
	Durin::FAssetPath Destination;
	Durin::FAssetPath Unrelated;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/StaleTransactionSource", Source));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/StaleTransactionDestination", Destination));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/StaleTransactionUnrelated", Unrelated));
	DPackageAssetForTest* SourceAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Source, SourceAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(SourceAsset->GetPackage()));
	const Durin::Asset::FAssetRelocationMapping Mapping{Source, Destination};
	Durin::Asset::FAssetMutationSummary Summary;
	Durin::Asset::FAssetMutationTransaction Transaction;
	ASSERT_TRUE(Durin::Asset::PrepareAssetRelocationTransaction(
		std::span{&Mapping, 1}, Summary, Transaction));

	DPackageAssetForTest* UnrelatedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Unrelated, UnrelatedAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(UnrelatedAsset->GetPackage()));
	EXPECT_EQ(Transaction.Commit().Error, Durin::Asset::EAssetError::StaleData);
	EXPECT_EQ(Transaction.GetState(),
		Durin::Asset::EAssetMutationTransactionState::Prepared);
	const Durin::Asset::FAssetMutationResultDetails Details =
		Transaction.GetLastResultDetails();
	EXPECT_TRUE(Details.bStateRestored);
	EXPECT_FALSE(Details.bRecoveryRequired);
	EXPECT_NE(Durin::Asset::FindAssetExact(Source), nullptr);
	EXPECT_EQ(Durin::Asset::FindAssetExact(Destination), nullptr);
}

TEST(FPackageAssetTests, RepeatedRelocationCompressesAliasesAndMoveBackReclaimsSameObject)
{
	InitializeAssetTests();
	Durin::FAssetPath First;
	Durin::FAssetPath Second;
	Durin::FAssetPath Third;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RepeatedA", First));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RepeatedB", Second));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RepeatedC", Third));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(First, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));

	ASSERT_TRUE(RelocateAssetForTest(First, Second));
	ASSERT_TRUE(RelocateAssetForTest(Second, Third));
	const auto FirstAlias =
		Durin::Asset::FindAssetExact(First);
	const auto SecondAlias =
		Durin::Asset::FindAssetExact(Second);
	ASSERT_NE(FirstAlias, nullptr);
	ASSERT_NE(SecondAlias, nullptr);
	EXPECT_EQ(FirstAlias->RedirectDestination, Third);
	EXPECT_EQ(SecondAlias->RedirectDestination, Third);

	ASSERT_TRUE(RelocateAssetForTest(Third, First));
	EXPECT_EQ(Durin::Asset::FindAssetExact(First)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::FindAssetExact(Second)->RedirectDestination, First);
	EXPECT_EQ(Durin::Asset::FindAssetExact(Third)->RedirectDestination, First);
	EXPECT_EQ(Durin::Asset::ResolveAssetPath(Second).FinalPath, First);

	Durin::FAssetPath Unrelated;
	Durin::FAssetPath UnrelatedAlias;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RepeatedUnrelated", Unrelated
	));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RepeatedUnrelatedAlias", UnrelatedAlias
	));
	DPackageAssetForTest* UnrelatedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Unrelated, UnrelatedAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(UnrelatedAsset->GetPackage()));
	Durin::Asset::DAssetRedirector* Alias = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirectorForTesting(
		UnrelatedAlias, Unrelated, Alias
	));
	ASSERT_TRUE(Durin::Asset::SavePackage(Alias->GetPackage()));
	EXPECT_EQ(RelocateAssetForTest(First, UnrelatedAlias).Error, Durin::Asset::EAssetError::AlreadyExists);
	EXPECT_EQ(Durin::Asset::ResolveAssetPath(UnrelatedAlias).FinalPath, Unrelated);
}

TEST(FPackageAssetTests, PackageDoesNotStoreItsOwnPathAndDirectoryMoveIsByteStable)
{
	InitializeAssetTests();
	Durin::FAssetPath OldPath, NewPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/MoveSource", OldPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Sub/MoveSource", NewPath));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Asset));
	Asset->Label = "movable";
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const auto OldFile = Durin::Testing::GetTestWorkDirectory() / "Assets" / "MoveSource.dasset";
	std::vector<std::byte> Before;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Before, OldFile));
	uint32 FormatVersion = 0;
	ASSERT_TRUE(Durin::ReadLittleEndianAt(Before, 24, FormatVersion));
	EXPECT_EQ(FormatVersion, Durin::Asset::AssetPackageV6FormatVersion);
	const std::string_view OldPathView = OldPath.GetView();
	const std::span<const std::byte> OldPathBytes =
		std::as_bytes(std::span{OldPathView.data(), OldPathView.size()});
	EXPECT_EQ(std::search(Before.begin(), Before.end(),
		OldPathBytes.begin(), OldPathBytes.end()), Before.end());

	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	const auto NewFile = Durin::Testing::GetTestWorkDirectory() / "Assets" / "Sub" / "MoveSource.dasset";
	std::vector<std::byte> After;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(After, NewFile));
	EXPECT_EQ(Before, After);
	EXPECT_TRUE(std::filesystem::exists(OldFile));
	ASSERT_NE(Durin::Asset::FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindAssetExact(OldPath)->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(OldPath), nullptr);
	EXPECT_NE(Durin::Asset::FindResidentPackage(NewPath), nullptr);
}

TEST(FPackageAssetTests, RelocationLeavesMountedReferrersAuthoredToAlias)
{
	InitializeAssetTests();
	Durin::FAssetPath OldPath, NewPath, OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/MoveDependency", OldPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RenamedDependency", NewPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/MoveOwner", OwnerPath));
	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Dependency));
	ASSERT_TRUE(Durin::Asset::SavePackage(Dependency->GetPackage()));
	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));

	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	const Durin::Asset::FAssetCatalogEntry OwnerData = Durin::Asset::FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	EXPECT_EQ(std::ranges::find(OwnerData->Dependencies, NewPath), OwnerData->Dependencies.end());
	EXPECT_NE(std::ranges::find(OwnerData->Dependencies, OldPath), OwnerData->Dependencies.end());
	EXPECT_EQ(Dependency->GetName(), NewPath.GetAssetName());
}

TEST(FPackageAssetTests, DeletesUnreferencedAssetAndRegistryEntry)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DeleteMe", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const std::filesystem::path File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteMe.dasset";
	ASSERT_TRUE(std::filesystem::exists(File));

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_FALSE(Analysis.bLoaded);
	EXPECT_TRUE(Analysis.CanDelete());
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(Path));
	EXPECT_FALSE(std::filesystem::exists(File));
	EXPECT_EQ(Durin::Asset::FindAssetExact(Path), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), nullptr);
}

TEST(FPackageAssetTests, UnloadsAndDeletesLoadedUnreferencedAsset)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LoadedDelete", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_TRUE(Analysis.bLoaded);
	EXPECT_TRUE(Analysis.CanDelete());

	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(Path));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), nullptr);
	EXPECT_EQ(Durin::Asset::FindAssetExact(Path), nullptr);
	EXPECT_FALSE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "LoadedDelete.dasset"
	));
}

TEST(FPackageAssetTests, RejectsDeletingReferencedAssetWithoutChangingDisk)
{
	InitializeAssetTests();
	Durin::FAssetPath DependencyPath, OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DeleteDependency", DependencyPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DeleteOwner", OwnerPath));
	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(DependencyPath, Dependency));
	ASSERT_TRUE(Durin::Asset::SavePackage(Dependency->GetPackage()));
	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(DependencyPath, Analysis));
	ASSERT_EQ(Analysis.DirectReferencers.size(), 1u);
	EXPECT_EQ(Analysis.DirectReferencers.front(), OwnerPath);
	EXPECT_FALSE(Analysis.CanDelete());
	EXPECT_EQ(Durin::Asset::DeleteAssetForTesting(DependencyPath).Error, Durin::Asset::EAssetError::InUse);
	EXPECT_NE(Durin::Asset::FindResidentPackage(DependencyPath), nullptr);
	EXPECT_NE(Durin::Asset::FindAssetExact(DependencyPath), nullptr);
	EXPECT_TRUE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteDependency.dasset"
	));
}

TEST(FPackageAssetTests, DeletesRegisteredCompanionFile)
{
	InitializeAssetTests();
	RegisterTestDeleteContributor();

	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DeleteWithCompanion", Path));
	const std::filesystem::path Companion =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteWithCompanion.source";
	{
		std::ofstream Stream(Companion);
		Stream << "source";
	}
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Label = "companion:" + Companion.generic_string();
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), nullptr);
	ASSERT_EQ(Analysis.CompanionFiles.size(), 1u);
	EXPECT_EQ(Analysis.CompanionFiles.front(), Companion);
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(Path));
	EXPECT_FALSE(std::filesystem::exists(Companion));
}

TEST(FPackageAssetTests, DeletesMainAssetWhenCompanionInspectionFails)
{
	InitializeAssetTests();
	RegisterTestDeleteContributor();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DeleteCorruptPackage", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const std::filesystem::path File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteCorruptPackage.dasset";
	std::vector<std::byte> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File));
	ASSERT_GT(Bytes.size(), 16u);
	WriteTestBytes(File, std::span<const std::byte>(Bytes).first(16));

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_FALSE(Analysis.Warning.empty());
	EXPECT_TRUE(Analysis.CompanionFiles.empty());
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(Path));
	EXPECT_FALSE(std::filesystem::exists(File));
}

TEST(FPackageAssetTests, DeleteAnalysisDoesNotLeaveTemporaryDependenciesLoaded)
{
	InitializeAssetTests();
	Durin::FAssetPath DependencyPath, OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AnalysisDependency", DependencyPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/AnalysisOwner", OwnerPath));
	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(DependencyPath, Dependency));
	ASSERT_TRUE(Durin::Asset::SavePackage(Dependency->GetPackage()));
	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(DependencyPath));

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(OwnerPath, Analysis));
	EXPECT_FALSE(Analysis.bLoaded);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(OwnerPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(DependencyPath), nullptr);
}

TEST(FPackageAssetTests, LegacyPackageIsExplicitlyUnsupportedWithoutCatalogMutation)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LegacyVersion", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const Durin::Asset::FAssetCatalogEntry BeforeRefresh =
		Durin::Asset::FindAssetExact(Path);
	ASSERT_TRUE(BeforeRefresh);
	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "LegacyVersion.dasset";
	const std::array<std::byte, 8> LegacyPackage{
		std::byte{0x44}, std::byte{0x41}, std::byte{0x53}, std::byte{0x54},
		std::byte{0x05}, std::byte{}, std::byte{}, std::byte{}};
	WriteTestBytes(File, LegacyPackage);
	Durin::DObject* Loaded = nullptr;
	EXPECT_EQ(Durin::Asset::LoadAsset(Path, Loaded).Error, Durin::Asset::EAssetError::UnsupportedVersion);
	EXPECT_EQ(Loaded, nullptr);
	const Durin::Asset::FAssetCatalogRefreshResult Refresh =
		Durin::Asset::RefreshAssetRegistry();
	EXPECT_FALSE(Refresh);
	EXPECT_FALSE(Refresh.bCatalogComplete);
	EXPECT_TRUE(Refresh.bRetainedPriorRevision);
	EXPECT_EQ(Refresh.PriorRevision, BeforeRefresh.Revision);
	EXPECT_EQ(Refresh.ResultingRevision, BeforeRefresh.Revision);
	const Durin::Asset::FAssetCatalogEntry AfterRefresh =
		Durin::Asset::FindAssetExact(Path);
	ASSERT_TRUE(AfterRefresh);
	EXPECT_EQ(AfterRefresh.Revision, BeforeRefresh.Revision);
	EXPECT_EQ(AfterRefresh.Data, BeforeRefresh.Data);
	EXPECT_TRUE(std::ranges::any_of(
		Refresh.Errors,
		[](const Durin::Asset::FAssetResult& Error) {
			return Error.Error == Durin::Asset::EAssetError::UnsupportedVersion;
		}
	));
}

TEST(FPackageAssetTests, ManualScanMountsRequireExplicitAdmissionBeforeLoading)
{
	InitializeAssetTests();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "Assets";
	const std::array Definitions{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/TestAssets/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.Root = Root,
			.ContentPath = ".",
			.bAutoScan = false,
			.bContentWritable = true
		}
	};
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/ManualScanAsset", Path
	));
	{
		Durin::PathUtilities::FScopedMountRegistryFixture Mounts(Definitions);
		ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
		DPackageAssetForTest* Asset = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
		ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
		ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

		const Durin::Asset::FAssetCatalogRefreshResult ManualRefresh =
			Durin::Asset::RefreshAssetRegistry(
				Durin::Asset::EAssetRegistryScanMode::FullValidation);
		ASSERT_TRUE(ManualRefresh);
		EXPECT_EQ(ManualRefresh.CatalogStats.Enumerated, 0u);
		EXPECT_FALSE(Durin::Asset::FindAssetExact(Path));

		Durin::DObject* Loaded = nullptr;
		Durin::Asset::FAssetLoadReport MissingReport;
		const Durin::Asset::FAssetResult Missing =
			Durin::Asset::LoadAsset(Path, Loaded, &MissingReport);
		EXPECT_EQ(Missing.Error, Durin::Asset::EAssetError::NotFound);
		EXPECT_EQ(MissingReport.Error, Durin::Asset::EAssetError::NotFound);
		EXPECT_EQ(MissingReport.RequestedPath, Path);
		EXPECT_EQ(MissingReport.CatalogRevision,
			Durin::Asset::GetAssetCatalogRevision());
		EXPECT_EQ(MissingReport.PackageFileReadCount, 0u);
		EXPECT_EQ(Loaded, nullptr);

		ASSERT_TRUE(Durin::Asset::AdmitAssetPackageToCatalog(Path));
		ASSERT_TRUE(Durin::Asset::FindAssetExact(Path));
		Durin::Asset::FAssetLoadReport LoadReport;
		ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded, &LoadReport));
		EXPECT_NE(Loaded, nullptr);
		EXPECT_EQ(LoadReport.RequestedPath, Path);
		EXPECT_EQ(LoadReport.FinalPath, Path);
		EXPECT_EQ(LoadReport.CatalogRevision,
			Durin::Asset::GetAssetCatalogRevision());
		EXPECT_EQ(LoadReport.Error, Durin::Asset::EAssetError::None);
		EXPECT_EQ(LoadReport.PackageFileReadCount, 1u);
		ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	}

	ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(Path));
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
	Durin::FAssetPath SeedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RegistryReconciliationSeed", SeedPath));
	DPackageAssetForTest* SeedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SeedPath, SeedAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(SeedAsset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SeedPath));
	const auto ValidSource = OriginalAssets / "RegistryReconciliationSeed.dasset";
	std::filesystem::copy_file(ValidSource, ContentA / "Alpha.dasset");
	std::filesystem::copy_file(ValidSource, ContentA / "Beta.dasset");
	Durin::PathUtilities::RegisterMountPointForTests("/TestAssets/", ContentA.generic_string() + "/");
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	const uint64 RevisionBeforeInitialScan =
		Durin::Asset::GetAssetCatalogRevision();
	const auto InitialRefresh = Durin::Asset::RefreshAssetRegistry(
		Durin::Asset::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(InitialRefresh);
	EXPECT_GT(InitialRefresh.ResultingRevision, RevisionBeforeInitialScan);
	EXPECT_EQ(InitialRefresh.CatalogStats.Enumerated, 2u);
	EXPECT_EQ(InitialRefresh.CatalogStats.Reparsed, 2u);
	EXPECT_EQ(InitialRefresh.CatalogStats.HeaderReadAttempts, 2u);
	EXPECT_GT(InitialRefresh.CatalogStats.HeaderBytesRead, 0u);
	EXPECT_EQ(InitialRefresh.ReferenceStats.PayloadReadAttempts, 2u);
	EXPECT_GT(InitialRefresh.ReferenceStats.PayloadBytesRead, 0u);
	EXPECT_GE(InitialRefresh.CatalogStats.DurationMilliseconds, 0.0);
	EXPECT_EQ(Durin::Asset::CaptureAssetCatalogSnapshot().Assets.size(), 2u);
	const auto CacheFile = CacheRoot / "AssetRegistry" / "Registry.bin";
	std::vector<std::byte> FirstCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstCache, CacheFile));

	const uint64 StableRevision = Durin::Asset::GetAssetCatalogRevision();
	const auto StableRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(StableRefresh);
	EXPECT_EQ(StableRefresh.ResultingRevision, StableRevision);
	EXPECT_EQ(StableRefresh.CatalogStats.Reused, 2u);
	EXPECT_EQ(StableRefresh.CatalogStats.Reparsed, 0u);
	EXPECT_EQ(StableRefresh.CatalogStats.HeaderReadAttempts, 0u);
	EXPECT_EQ(StableRefresh.CatalogStats.HeaderBytesRead, 0u);
	EXPECT_EQ(StableRefresh.ReferenceStats.PayloadReadAttempts, 0u);
	EXPECT_EQ(StableRefresh.ReferenceStats.PayloadBytesRead, 0u);
	EXPECT_GE(StableRefresh.CatalogStats.DurationMilliseconds, 0.0);
	std::vector<std::byte> SecondCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondCache, CacheFile));
	EXPECT_EQ(SecondCache, FirstCache);

	const auto Alpha = ContentA / "Alpha.dasset";
	std::filesystem::last_write_time(Alpha, std::filesystem::last_write_time(Alpha) + std::chrono::seconds(2));
	const auto ChangedRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(ChangedRefresh);
	EXPECT_GT(ChangedRefresh.ResultingRevision, StableRevision);
	EXPECT_EQ(ChangedRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(ChangedRefresh.CatalogStats.Reparsed, 1u);
	EXPECT_EQ(ChangedRefresh.CatalogStats.HeaderReadAttempts, 1u);
	EXPECT_GT(ChangedRefresh.CatalogStats.HeaderBytesRead, 0u);
	EXPECT_EQ(ChangedRefresh.ReferenceStats.PayloadReadAttempts, 1u);
	EXPECT_GE(ChangedRefresh.CatalogStats.DurationMilliseconds, 0.0);

	std::filesystem::copy_file(ValidSource, ContentA / "Gamma.dasset");
	std::filesystem::remove(ContentA / "Beta.dasset");
	const auto AddedRemovedRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(AddedRemovedRefresh);
	EXPECT_EQ(AddedRemovedRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(AddedRemovedRefresh.CatalogStats.Reparsed, 1u);
	EXPECT_EQ(AddedRemovedRefresh.CatalogStats.Removed, 1u);
	EXPECT_EQ(Durin::Asset::CaptureAssetCatalogSnapshot().Assets.size(), 2u);
	EXPECT_EQ(AddedRemovedRefresh.ReferenceStats.PayloadReadAttempts, 1u);

	std::filesystem::rename(ContentA / "Gamma.dasset", ContentA / "Delta.dasset");
	const auto RenamedRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(RenamedRefresh);
	EXPECT_EQ(RenamedRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(RenamedRefresh.CatalogStats.Reparsed, 1u);
	EXPECT_EQ(RenamedRefresh.CatalogStats.Removed, 1u);
	EXPECT_EQ(RenamedRefresh.ReferenceStats.PayloadReadAttempts, 1u);

	const auto FullRefresh = Durin::Asset::RefreshAssetRegistry(
		Durin::Asset::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(FullRefresh);
	EXPECT_EQ(FullRefresh.CatalogStats.Reused, 0u);
	EXPECT_EQ(FullRefresh.CatalogStats.Reparsed, 2u);
	EXPECT_EQ(FullRefresh.CatalogStats.HeaderReadAttempts, 2u);
	EXPECT_GT(FullRefresh.CatalogStats.HeaderBytesRead, 0u);
	EXPECT_EQ(FullRefresh.ReferenceStats.PayloadReadAttempts, 2u);
	EXPECT_GE(FullRefresh.CatalogStats.DurationMilliseconds, 0.0);
	EXPECT_EQ(Durin::Asset::CaptureAssetCatalogSnapshot().Assets.size(), 2u);

	const std::array<std::byte, 3> CorruptCache = {std::byte{1}, std::byte{2}, std::byte{3}};
	WriteTestBytes(CacheFile, CorruptCache);
	const auto CorruptCacheRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(CorruptCacheRefresh);
	EXPECT_EQ(CorruptCacheRefresh.CatalogStats.Reparsed, 2u);
	EXPECT_FALSE(CorruptCacheRefresh.CatalogCacheWarning.empty());
	EXPECT_EQ(CorruptCacheRefresh.ReferenceStats.PayloadReadAttempts, 0u);

	std::vector<std::byte> IncompatibleCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(IncompatibleCache, CacheFile));
	const uint32 IncompatibleSchema = 99;
	std::memcpy(IncompatibleCache.data() + sizeof(uint32), &IncompatibleSchema, sizeof(IncompatibleSchema));
	WriteTestBytes(CacheFile, IncompatibleCache);
	const auto IncompatibleCacheRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(IncompatibleCacheRefresh);
	EXPECT_EQ(IncompatibleCacheRefresh.CatalogStats.Reparsed, 2u);
	EXPECT_FALSE(IncompatibleCacheRefresh.CatalogCacheWarning.empty());
	EXPECT_EQ(IncompatibleCacheRefresh.ReferenceStats.PayloadReadAttempts, 0u);

	std::filesystem::create_directories(ContentB);
	for (const auto& Source : {Alpha, ContentA / "Delta.dasset"})
	{
		const auto Destination = ContentB / Source.filename();
		std::filesystem::copy_file(Source, Destination);
		std::filesystem::last_write_time(Destination, std::filesystem::last_write_time(Source));
	}
	Durin::PathUtilities::RegisterMountPointForTests("/TestAssets/", ContentB.generic_string() + "/");
	const auto RelocatedMountRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(RelocatedMountRefresh);
	EXPECT_EQ(RelocatedMountRefresh.CatalogStats.Reused, 2u);
	EXPECT_EQ(RelocatedMountRefresh.CatalogStats.Reparsed, 0u);
	EXPECT_EQ(RelocatedMountRefresh.ReferenceStats.PayloadReadAttempts, 0u);

	const auto AdditionalContent = WorkRoot / "AdditionalContent";
	std::filesystem::create_directories(AdditionalContent);
	Durin::PathUtilities::RegisterMountPointForTests("/Additional/", AdditionalContent.generic_string() + "/");
	const auto ManifestRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(ManifestRefresh);
	EXPECT_EQ(ManifestRefresh.CatalogStats.Reparsed, 2u);
	EXPECT_NE(ManifestRefresh.CatalogCacheWarning.find("mount manifest changed"), std::string::npos);
	EXPECT_EQ(ManifestRefresh.ReferenceStats.PayloadReadAttempts, 0u);

	const auto BlockedCacheRoot = WorkRoot / "BlockedCacheRoot";
	WriteTestBytes(BlockedCacheRoot, CorruptCache);
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	const auto BlockedCacheRefresh = Durin::Asset::RefreshAssetRegistry(
		Durin::Asset::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(BlockedCacheRefresh);
	EXPECT_EQ(Durin::Asset::CaptureAssetCatalogSnapshot().Assets.size(), 2u);
	EXPECT_FALSE(BlockedCacheRefresh.CatalogCacheWarning.empty());
	EXPECT_EQ(BlockedCacheRefresh.ReferenceStats.PayloadReadAttempts, 2u);
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

	Durin::FAssetPath SeedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/DuplicateSeed", SeedPath));
	DPackageAssetForTest* SeedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SeedPath, SeedAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(SeedAsset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SeedPath));
	const auto SeedData = Durin::Asset::FindAssetExact(SeedPath);
	ASSERT_NE(SeedData, nullptr);
	const std::filesystem::path SeedFile = SeedData->PhysicalPath;
	std::filesystem::copy_file(SeedFile, RootA / "Nested" / "Duplicate.dasset");
	std::filesystem::copy_file(SeedFile, RootB / "Duplicate.dasset");

	const std::array Definitions{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/TestAssets/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.Root = RootA,
			.ContentPath = ".",
			.bAutoScan = true,
			.bContentWritable = true},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/TestAssets/Nested/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.Root = RootB,
			.ContentPath = ".",
			.bAutoScan = true,
			.bContentWritable = true}};
	Durin::PathUtilities::FScopedMountRegistryFixture Mounts(Definitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	const Durin::Asset::FAssetCatalogSnapshot BeforeRefresh =
		Durin::Asset::CaptureAssetCatalogSnapshot();
	const Durin::Asset::FAssetCatalogRefreshResult Refresh =
		Durin::Asset::RefreshAssetRegistry(
			Durin::Asset::EAssetRegistryScanMode::FullValidation);
	EXPECT_FALSE(Refresh);
	EXPECT_FALSE(Refresh.bCatalogComplete);
	EXPECT_TRUE(Refresh.bRetainedPriorRevision);
	EXPECT_EQ(Refresh.CatalogStats.Enumerated, 2u);
	EXPECT_EQ(Refresh.CatalogStats.Failed, 1u);
	EXPECT_EQ(Refresh.ReferenceStats.PayloadReadAttempts, 1u);
	const Durin::Asset::FAssetCatalogSnapshot AfterRefresh =
		Durin::Asset::CaptureAssetCatalogSnapshot();
	EXPECT_EQ(AfterRefresh.Revision, BeforeRefresh.Revision);
	EXPECT_EQ(AfterRefresh.Assets, BeforeRefresh.Assets);
	EXPECT_TRUE(std::ranges::any_of(
		Refresh.Errors,
		[](const Durin::Asset::FAssetResult& Error) {
			return Error.Error == Durin::Asset::EAssetError::AlreadyExists;
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
	Durin::PathUtilities::RegisterMountPointForTests("/TestAssets/", ContentRoot.generic_string() + "/");
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	const uint64 EmptyRegistryRevision = Durin::Asset::GetAssetCatalogRevision();

	Durin::FAssetPath FirstPath;
	Durin::FAssetPath MovedPath;
	Durin::FAssetPath ImportedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LifecycleFirst", FirstPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LifecycleMoved", MovedPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LifecycleImported", ImportedPath));

	DPackageAssetForTest* FirstAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(FirstPath, FirstAsset));
	EXPECT_EQ(Durin::Asset::GetAssetCatalogRevision(), EmptyRegistryRevision);
	EXPECT_FALSE(Durin::Asset::IsAssetCatalogSnapshotDirtyForTesting());
	EXPECT_EQ(Durin::Asset::FindResidentPackage(FirstPath), FirstAsset->GetPackage());
	ASSERT_TRUE(Durin::Asset::SavePackage(FirstAsset->GetPackage()));
	EXPECT_GT(Durin::Asset::GetAssetCatalogRevision(), EmptyRegistryRevision);
	EXPECT_TRUE(Durin::Asset::IsAssetCatalogSnapshotDirtyForTesting());
	ShutdownAssetManagerForRestart();
	EXPECT_FALSE(Durin::Asset::IsAssetCatalogSnapshotDirtyForTesting());
	const auto FirstWarmRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(FirstWarmRefresh);
	EXPECT_EQ(FirstWarmRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(FirstWarmRefresh.CatalogStats.Reparsed, 0u);

	Durin::DObject* Reloaded = nullptr;
	const uint64 RevisionBeforeLoad = Durin::Asset::GetAssetCatalogRevision();
	ASSERT_TRUE(Durin::Asset::LoadAsset(FirstPath, Reloaded));
	EXPECT_EQ(Durin::Asset::GetAssetCatalogRevision(), RevisionBeforeLoad);
	EXPECT_FALSE(Durin::Asset::IsAssetCatalogSnapshotDirtyForTesting());
	ShutdownAssetManagerForRestart();
	const auto SecondWarmRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(SecondWarmRefresh);
	EXPECT_EQ(SecondWarmRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(SecondWarmRefresh.CatalogStats.Reparsed, 0u);

	ASSERT_TRUE(RelocateAssetForTest(FirstPath, MovedPath));
	EXPECT_TRUE(Durin::Asset::IsAssetCatalogSnapshotDirtyForTesting());
	ShutdownAssetManagerForRestart();
	const auto RelocatedRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(RelocatedRefresh);
	EXPECT_EQ(RelocatedRefresh.CatalogStats.Reused, 2u);
	EXPECT_EQ(RelocatedRefresh.CatalogStats.Reparsed, 0u);
	ASSERT_NE(Durin::Asset::FindAssetExact(FirstPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindAssetExact(FirstPath)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_NE(Durin::Asset::FindAssetExact(MovedPath), nullptr);

	DPackageAssetForTest* ImportedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ImportedPath, ImportedAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(ImportedAsset->GetPackage()));
	ShutdownAssetManagerForRestart();
	const auto ImportedRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(ImportedRefresh);
	EXPECT_EQ(ImportedRefresh.CatalogStats.Reused, 3u);
	EXPECT_EQ(ImportedRefresh.CatalogStats.Reparsed, 0u);

	ASSERT_TRUE(DeleteAssetClosureForTest({FirstPath, MovedPath}));
	EXPECT_TRUE(Durin::Asset::IsAssetCatalogSnapshotDirtyForTesting());
	ShutdownAssetManagerForRestart();
	const auto DeletedRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(DeletedRefresh);
	EXPECT_EQ(DeletedRefresh.CatalogStats.Reused, 1u);
	EXPECT_EQ(DeletedRefresh.CatalogStats.Reparsed, 0u);
	EXPECT_EQ(Durin::Asset::FindAssetExact(MovedPath), nullptr);

	const auto BlockedCacheRoot = WorkRoot / "BlockedCacheRoot";
	const std::array<std::byte, 3> Blocker = {std::byte{1}, std::byte{2}, std::byte{3}};
	WriteTestBytes(BlockedCacheRoot, Blocker);
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	ImportedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(ImportedPath, ImportedAsset));
	ImportedAsset->Value = 42;
	ASSERT_TRUE(Durin::Asset::SavePackage(ImportedAsset->GetPackage()));
	const auto AuthoredFile = ContentRoot / "LifecycleImported.dasset";
	std::vector<std::byte> BeforeFailedFlush;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeFailedFlush, AuthoredFile));
	ShutdownAssetManagerForRestart();
	EXPECT_TRUE(Durin::Asset::IsAssetCatalogSnapshotDirtyForTesting());
	EXPECT_FALSE(Durin::Asset::GetAssetCatalogCacheWarningForTesting().empty());
	std::vector<std::byte> AfterFailedFlush;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterFailedFlush, AuthoredFile));
	EXPECT_EQ(AfterFailedFlush, BeforeFailedFlush);

	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	ShutdownAssetManagerForRestart();
	EXPECT_FALSE(Durin::Asset::IsAssetCatalogSnapshotDirtyForTesting());
}

TEST(FPackageAssetTests, SoftReferenceCacheUsesCheapMetadataAndFullValidationWithoutLoadingTargets)
{
	InitializeAssetTests();
	const auto CacheRoot =
		Durin::Testing::GetTestWorkDirectory() / "SoftReferenceDerivedDataCache";
	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());

	Durin::FAssetPath OwnerPath;
	Durin::FAssetPath TargetAPath;
	Durin::FAssetPath TargetBPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftCacheOwner", OwnerPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftCacheTargetA", TargetAPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftCacheTargetB", TargetBPath));
	DPackageAssetForTest* TargetA = nullptr;
	DPackageAssetForTest* TargetB = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetAPath, TargetA));
	ASSERT_TRUE(Durin::Asset::SavePackage(TargetA->GetPackage()));
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetBPath, TargetB));
	ASSERT_TRUE(Durin::Asset::SavePackage(TargetB->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->Direct.SetPath(TargetAPath);
	Owner->Label.assign(512u * 1024u, 'x');
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetAPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetBPath));

	const auto InitialRefresh = Durin::Asset::RefreshAssetRegistry(
		Durin::Asset::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(InitialRefresh);
	EXPECT_GT(InitialRefresh.ReferenceStats.PayloadReadAttempts, 0u);
	EXPECT_GT(InitialRefresh.ReferenceStats.PayloadBytesRead, 512u * 1024u);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(TargetAPath), nullptr);
	EXPECT_EQ(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FAssetPath>{TargetAPath}));
	const auto CacheFile = CacheRoot / "AssetRegistry" / "References.bin";
	ASSERT_TRUE(std::filesystem::is_regular_file(CacheFile));
	std::vector<std::byte> FirstCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstCache, CacheFile
	));

	const auto WarmRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(WarmRefresh);
	EXPECT_GT(WarmRefresh.ReferenceStats.ReusedSources, 0u);
	EXPECT_EQ(WarmRefresh.ReferenceStats.ExtractedSources, 0u);
	EXPECT_EQ(WarmRefresh.ReferenceStats.PayloadReadAttempts, 0u);
	EXPECT_EQ(WarmRefresh.ReferenceStats.PayloadBytesRead, 0u);
	std::vector<std::byte> SecondCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondCache, CacheFile
	));
	EXPECT_EQ(SecondCache, FirstCache);

	const auto OwnerData = Durin::Asset::FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	const std::filesystem::path OwnerFile = OwnerData->PhysicalPath;
	const auto PreservedTime = std::filesystem::last_write_time(OwnerFile);
	ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, Owner));
	Owner->Direct.SetPath(TargetBPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	std::filesystem::last_write_time(OwnerFile, PreservedTime);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	const auto PreservedTimestampRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(PreservedTimestampRefresh);
	EXPECT_GT(PreservedTimestampRefresh.ReferenceStats.ReusedSources, 0u);
	EXPECT_EQ(PreservedTimestampRefresh.ReferenceStats.PayloadReadAttempts, 0u);
	EXPECT_EQ(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FAssetPath>{TargetAPath}));
	const auto FullRefresh = Durin::Asset::RefreshAssetRegistry(
		Durin::Asset::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(FullRefresh);
	EXPECT_GT(FullRefresh.ReferenceStats.PayloadReadAttempts, 0u);
	EXPECT_EQ(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FAssetPath>{TargetBPath}));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(TargetBPath), nullptr);
	std::filesystem::last_write_time(
		OwnerFile, std::filesystem::last_write_time(OwnerFile) + std::chrono::seconds(2));
	const auto ModifiedTimestampRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(ModifiedTimestampRefresh);
	EXPECT_EQ(ModifiedTimestampRefresh.ReferenceStats.PayloadReadAttempts, 1u);
	EXPECT_EQ(ModifiedTimestampRefresh.ReferenceStats.ExtractedSources, 1u);
	EXPECT_GT(ModifiedTimestampRefresh.ReferenceStats.ReusedSources, 0u);
	EXPECT_GT(ModifiedTimestampRefresh.ReferenceStats.PayloadBytesRead, 512u * 1024u);

	const std::array<std::byte, 3> CorruptCache = {std::byte{1}, std::byte{2}, std::byte{3}};
	WriteTestBytes(CacheFile, CorruptCache);
	const auto CorruptCacheRefresh = Durin::Asset::RefreshAssetRegistry();
	ASSERT_TRUE(CorruptCacheRefresh);
	EXPECT_FALSE(CorruptCacheRefresh.ReferenceCacheWarning.empty());
	EXPECT_GT(CorruptCacheRefresh.ReferenceStats.ExtractedSources, 0u);
	EXPECT_GT(CorruptCacheRefresh.ReferenceStats.PayloadReadAttempts, 0u);
	EXPECT_EQ(Durin::Asset::CaptureAssetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FAssetPath>{TargetBPath}));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(TargetBPath), nullptr);
	std::vector<std::byte> RecoveredCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		RecoveredCache, CacheFile
	));
	EXPECT_NE(RecoveredCache, std::vector<std::byte>(CorruptCache.begin(), CorruptCache.end()));
}
