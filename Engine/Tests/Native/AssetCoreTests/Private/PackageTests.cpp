#include <gtest/gtest.h>

#include "Asset/SourcePath.h"
#include "AssetCompatibility.h"
#include "AssetRedirector.h"
#include "AssetSystem.h"
#include "CookedAsset.h"
#include "CoreGlobals.h"
#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "NativeTestSupport.h"
#include "Threading/RunnableThread.h"

#include <chrono>
#include <bit>
#include <iostream>
#include <limits>

namespace AssetStructTest
{
	struct FCodecSource
	{
		Durin::int32 Value = 0;
	};

	struct FCodecTarget
	{
		Durin::int32 Value = 0;
	};
}

namespace Durin
{
	template<>
	struct TDStructOpsTraits<AssetStructTest::FCodecTarget>
		: TDStructOpsTraitsBase<AssetStructTest::FCodecTarget>
	{
		static constexpr bool bHasCompleteAuthoredFields = false;
	};
}

namespace
{
	auto RelocateAssetsForTest(
		std::span<const Durin::Asset::FAssetRelocationMapping> Mappings,
		Durin::Asset::FAssetRelocationBatchToken* OutToken = nullptr)
		-> Durin::Asset::FAssetResult
	{
		Durin::Asset::FAssetRelocationBatchToken Token;
		Durin::Asset::FAssetResult Result =
			Durin::Asset::AnalyzeAssetRelocationBatch(Mappings, Token);
		if (Result) Result = Durin::Asset::RevalidateAssetRelocationBatch(Token);
		if (Result) Result = Durin::Asset::ApplyAssetRelocationBatch(Token);
		if (Result && OutToken) *OutToken = std::move(Token);
		return Result;
	}

	auto RelocateAssetForTest(
		const Durin::FAssetPath& Source,
		const Durin::FAssetPath& Destination,
		Durin::Asset::FAssetRelocationBatchToken* OutToken = nullptr)
		-> Durin::Asset::FAssetResult
	{
		const Durin::Asset::FAssetRelocationMapping Mapping{
			Source, Destination};
		return RelocateAssetsForTest(std::span{&Mapping, 1}, OutToken);
	}

	// Test cleanup follows the production target-plus-alias closure contract while
	// avoiding a second editor-level filesystem transaction in AssetCore tests.
	auto DeleteAssetClosureForTest(
		std::initializer_list<Durin::FAssetPath> Paths)
		-> Durin::Asset::FAssetResult
	{
		const std::vector<Durin::FAssetPath> DeletionPaths(Paths);
		Durin::Asset::FAssetDeletionBatchToken Token;
		std::vector<Durin::Asset::FAssetDeletionBatchBlocker> Blockers;
		Durin::Asset::FAssetResult Result =
			Durin::Asset::AnalyzeAssetDeletionBatch(
				DeletionPaths, {}, Token, Blockers);
		if (!Result) return Result;
		if (!Blockers.empty())
			return {
				Durin::Asset::EAssetError::InUse,
				Blockers.front().Details};
		Result = Durin::Asset::UnloadAssetDeletionBatch(Token);
		if (!Result) return Result;
		for (const Durin::Asset::FAssetDeletionBatchEntry& Entry :
			 Token.GetEntries())
		{
			std::error_code Error;
			if (!std::filesystem::remove(Entry.RegistryEntry.PhysicalPath, Error)
				|| Error)
				return {
					Durin::Asset::EAssetError::IoError,
					std::format(
						"Could not remove test asset {}: {}",
						Entry.RegistryEntry.PackagePath.ToString(),
						Error.message())};
			for (const std::filesystem::path& Companion : Entry.CompanionFiles)
			{
				Error.clear();
				if (!std::filesystem::remove(Companion, Error) || Error)
					return {
						Durin::Asset::EAssetError::IoError,
						std::format(
							"Could not remove test companion {}: {}",
							Companion.generic_string(), Error.message())};
			}
		}
		return Durin::Asset::RemoveAssetDeletionBatchRegistryProjection(Token);
	}

	template<typename T>
	auto VectorNum(const void* Container) -> Durin::uint64 { return static_cast<const std::vector<T>*>(Container)->size(); }
	template<typename T>
	auto VectorGet(const void* Container, Durin::uint64 Index) -> const void* { return &(*static_cast<const std::vector<T>*>(Container))[Index]; }
	template<typename T>
	auto VectorGetMutable(void* Container, Durin::uint64 Index) -> void* { return &(*static_cast<std::vector<T>*>(Container))[Index]; }
	template<typename T>
	auto VectorResize(void* Container, Durin::uint64 Num) -> bool { static_cast<std::vector<T>*>(Container)->resize(Num); return true; }

	auto GIntVectorHelper() -> const Durin::FArrayOps* { return Durin::ResolveArrayOps<std::vector<Durin::int32>>(); }
	auto GGuidVectorHelper() -> const Durin::FArrayOps* { return Durin::ResolveArrayOps<std::vector<Durin::FGuid>>(); }
	auto GVector3VectorHelper() -> const Durin::FArrayOps* { return Durin::ResolveArrayOps<std::vector<Durin::FVector3>>(); }

	using FScoreMap = std::unordered_map<std::string, Durin::int32>;
	auto MapNum(const void* Container) -> Durin::uint64 { return static_cast<const FScoreMap*>(Container)->size(); }
	auto MapKey(const void* Container, Durin::uint64 Index) -> const void*
	{
		auto It = static_cast<const FScoreMap*>(Container)->begin(); std::advance(It, Index); return &It->first;
	}
	auto MapValue(const void* Container, Durin::uint64 Index) -> const void*
	{
		auto It = static_cast<const FScoreMap*>(Container)->begin(); std::advance(It, Index); return &It->second;
	}
	auto MapMutableValue(void* Container, Durin::uint64 Index) -> void*
	{
		auto It = static_cast<FScoreMap*>(Container)->begin(); std::advance(It, Index); return &It->second;
	}
	auto MapClear(void* Container) -> void { static_cast<FScoreMap*>(Container)->clear(); }
	auto CreateString() -> void* { return new std::string(); }
	auto CopyString(const void* Value) -> void* { return new std::string(*static_cast<const std::string*>(Value)); }
	auto DestroyString(void* Value) -> void { delete static_cast<std::string*>(Value); }
	auto CreateInt() -> void* { return new Durin::int32(); }
	auto DestroyInt(void* Value) -> void { delete static_cast<Durin::int32*>(Value); }
	auto MapInsert(void* Container, const void* Key, const void* Value) -> bool
	{
		static_cast<FScoreMap*>(Container)->insert_or_assign(*static_cast<const std::string*>(Key), *static_cast<const Durin::int32*>(Value));
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
	auto VectorMapNum(const void* Container) -> Durin::uint64 { return static_cast<const FVectorMap*>(Container)->size(); }
	auto VectorMapKey(const void* Container, Durin::uint64 Index) -> const void*
	{
		auto It = static_cast<const FVectorMap*>(Container)->begin(); std::advance(It, Index); return &It->first;
	}
	auto VectorMapValue(const void* Container, Durin::uint64 Index) -> const void*
	{
		auto It = static_cast<const FVectorMap*>(Container)->begin(); std::advance(It, Index); return &It->second;
	}
	auto VectorMapMutableValue(void* Container, Durin::uint64 Index) -> void*
	{
		auto It = static_cast<FVectorMap*>(Container)->begin(); std::advance(It, Index); return &It->second;
	}
	auto VectorMapClear(void* Container) -> void { static_cast<FVectorMap*>(Container)->clear(); }
	auto CreateVector3() -> void* { return new Durin::FVector3(0.0); }
	auto DestroyVector3(void* Value) -> void { delete static_cast<Durin::FVector3*>(Value); }
	auto VectorMapInsert(void* Container, const void* Key, const void* Value) -> bool
	{
		static_cast<FVectorMap*>(Container)->insert_or_assign(
			*static_cast<const std::string*>(Key), *static_cast<const Durin::FVector3*>(Value));
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
	bool GReportNonUpgradeMutationOnPostLoad = false;
	Durin::uint64 GSoftPackageConstructionCount = 0;

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
			if (!DObject::PostLoad(OutError)) return false;
			if (GReportNonUpgradeMutationOnPostLoad)
				Durin::Asset::ReportAssetLoadMutation(
					this,
					"Tests.NonUpgradePostLoadMutation",
					"Test-only non-upgrade mutation.");
			return true;
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
			static const Durin::DurinCodeGen::FPropertyParamsBase ValueProp = {"Value", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, Value)), sizeof(Value), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FPropertyParamsBase LabelProp = {"Label", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, Label)), sizeof(Label), Durin::DurinCodeGen::EPropertyGenFlags::String};
			static const Durin::DurinCodeGen::FNamePropertyParams DisplayNameProp = {"DisplayName", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, DisplayName)), sizeof(DisplayName), Durin::DurinCodeGen::EPropertyGenFlags::Name};
			static const Durin::DurinCodeGen::FGuidPropertyParams GuidProp = {"PersistentId", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, PersistentId)), sizeof(PersistentId), Durin::DurinCodeGen::EPropertyGenFlags::Guid};
			static const Durin::DurinCodeGen::FGuidPropertyParams GuidInner = {"RelatedIds_Inner", Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::FGuid), Durin::DurinCodeGen::EPropertyGenFlags::Guid};
			static const Durin::DurinCodeGen::FArrayPropertyParams GuidsProp = {"RelatedIds", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, RelatedIds)), &GuidInner, &GGuidVectorHelper};
			static const Durin::DurinCodeGen::FPropertyParamsBase ScoreInner = {"Scores_Inner", Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FArrayPropertyParams ScoresProp = {"Scores", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, Scores)), &ScoreInner, &GIntVectorHelper};
			static const Durin::DurinCodeGen::FPropertyParamsBase MapKeyProp = {"NamedScores_Key", Durin::EPropertyFlags::None, 1, 0, sizeof(std::string), Durin::DurinCodeGen::EPropertyGenFlags::String};
			static const Durin::DurinCodeGen::FPropertyParamsBase MapValueProp = {"NamedScores_Value", Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FMapPropertyParams NamedScoresProp = {"NamedScores", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, NamedScores)), &MapKeyProp, &MapValueProp, &GScoreMapHelper};
			static const Durin::DurinCodeGen::FStructPropertyParams SourcePathProp = {
				"SourcePath", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, SourcePath)),
				&Durin::FSourcePath::StaticStruct};
			static const Durin::DurinCodeGen::FPropertyParamsBase ChildProp = {"DefaultChild", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, DefaultChild)), sizeof(DefaultChild), Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass, nullptr, nullptr, nullptr, nullptr, true};
			static const Durin::DurinCodeGen::FPropertyParamsBase ExternalProp = {"ExternalReference", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, ExternalReference)), sizeof(ExternalReference), Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass, nullptr, nullptr, nullptr, nullptr, true};
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&ValueProp, &LabelProp, &DisplayNameProp, &GuidProp, &GuidsProp, &ScoresProp,
				&NamedScoresProp, &SourcePathProp, &ChildProp, &ExternalProp};
			static const Durin::DurinCodeGen::FClassParams Params = {&StaticClassNoRegister, "Tests::DPackageAssetForTest", "DPackageAssetForTest", Properties, std::size(Properties)};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		Durin::int32 Value = 0;
		std::string Label;
		Durin::FName DisplayName;
		Durin::FGuid PersistentId;
		std::vector<Durin::FGuid> RelatedIds;
		std::vector<Durin::int32> Scores;
		FScoreMap NamedScores;
		Durin::FSourcePath SourcePath;
		Durin::TObjectPtr<Durin::DObject> DefaultChild;
		Durin::TObjectPtr<Durin::DObject> ExternalReference;
	};

	class DSoftPackageAssetForTest : public DPackageAssetForTest
	{
	public:
		using FSoftReference = Durin::TSoftObjectPtr<DPackageAssetForTest>;
		using FSoftReferenceArray = std::vector<FSoftReference>;
		using FSoftReferenceMap = std::unordered_map<std::string, FSoftReference>;

		explicit DSoftPackageAssetForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
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
						Durin::InternalConstructor<DSoftPackageAssetForTest>);
				Class->SetSuperStructBase(DPackageAssetForTest::StaticClass());
				Class->Register(
					Durin::DClass::StaticClass, "", "DSoftPackageAssetForTest");
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
					static_cast<Durin::uint16>(offsetof(DSoftPackageAssetForTest, Direct)),
					&DPackageAssetForTest::StaticClass);
			static const auto FixedProp =
				Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftReference>(
					"Fixed", Durin::EPropertyFlags::None, 2,
					static_cast<Durin::uint16>(offsetof(DSoftPackageAssetForTest, Fixed)),
					&DPackageAssetForTest::StaticClass);
			static const auto ArrayInner =
				Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftReference>(
					"Array_Inner", Durin::EPropertyFlags::None, 1, 0,
					&DPackageAssetForTest::StaticClass);
			static const Durin::DurinCodeGen::FArrayPropertyParams ArrayProp = {
				"Array", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DSoftPackageAssetForTest, Array)),
				&ArrayInner, &ResolveSoftArrayOps};
			static const Durin::DurinCodeGen::FStringPropertyParams MapKey = {
				"Map_Key", Durin::EPropertyFlags::None, 1, 0, sizeof(std::string),
				Durin::DurinCodeGen::EPropertyGenFlags::String};
			static const auto MapValue =
				Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftReference>(
					"Map_Value", Durin::EPropertyFlags::None, 1, 0,
					&DPackageAssetForTest::StaticClass);
			static const Durin::DurinCodeGen::FMapPropertyParams MapProp = {
				"Map", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DSoftPackageAssetForTest, Map)),
				&MapKey, &MapValue, &ResolveSoftMapOps};
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&DirectProp, &FixedProp, &ArrayProp, &MapProp};
			static const Durin::DurinCodeGen::FClassParams Params = {
				&StaticClassNoRegister, "Tests::DSoftPackageAssetForTest",
				"DSoftPackageAssetForTest", Properties, std::size(Properties)};
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
			alignof(AssetStructTest::FCodecSource), Durin::EObjectFlags::Transient);
		return Struct;
	}

	auto GetCodecSourceStruct() -> Durin::DStruct*
	{
		static const Durin::DurinCodeGen::FInt32PropertyParams Value = {
			"Value", Durin::EPropertyFlags::None, 1,
			static_cast<Durin::uint16>(offsetof(AssetStructTest::FCodecSource, Value)),
			sizeof(Durin::int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
		static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {&Value};
		static const Durin::DurinCodeGen::FStructParams Params = {
			&GetCodecSourceStructNoRegister, "Tests::FCodecSource", "FCodecSource",
			sizeof(AssetStructTest::FCodecSource), alignof(AssetStructTest::FCodecSource),
			Properties, std::size(Properties),
			&Durin::GetDStructOps<AssetStructTest::FCodecSource>()};
		return Durin::DurinCodeGen::ConstructDStruct(Params);
	}

	auto GetCodecTargetStructNoRegister() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = new Durin::DStruct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FCodecTarget"),
			Durin::FName("FCodecTarget"), sizeof(AssetStructTest::FCodecTarget),
			alignof(AssetStructTest::FCodecTarget), Durin::EObjectFlags::Transient);
		return Struct;
	}

	auto GetCodecTargetStruct() -> Durin::DStruct*
	{
		static const Durin::DurinCodeGen::FInt32PropertyParams Value = {
			"Value", Durin::EPropertyFlags::None, 1,
			static_cast<Durin::uint16>(offsetof(AssetStructTest::FCodecTarget, Value)),
			sizeof(Durin::int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
		static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {&Value};
		static const Durin::DurinCodeGen::FStructParams Params = {
			&GetCodecTargetStructNoRegister, "Tests::FCodecTarget", "FCodecTarget",
			sizeof(AssetStructTest::FCodecTarget), alignof(AssetStructTest::FCodecTarget),
			Properties, std::size(Properties),
			&Durin::GetDStructOps<AssetStructTest::FCodecTarget>()};
		return Durin::DurinCodeGen::ConstructDStruct(Params);
	}

	template<typename TValue>
	class TCodecAssetForTest : public Durin::DObject
	{
	public:
		explicit TCodecAssetForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer) {}

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
						Durin::InternalConstructor<TCodecAssetForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(
					Durin::DClass::StaticClass, "",
					bSource ? "DCodecSourceAsset" : "DCodecTargetAsset");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			constexpr bool bSource = std::is_same_v<TValue, AssetStructTest::FCodecSource>;
			static const Durin::DurinCodeGen::FStructPropertyParams ValueProp = {
				"Value", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(TCodecAssetForTest, Value)),
				bSource ? &GetCodecSourceStruct : &GetCodecTargetStruct};
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&ValueProp};
			static const Durin::DurinCodeGen::FClassParams Params = {
				&StaticClassNoRegister,
				bSource ? "Tests::DCodecSourceAsset" : "Tests::DCodecTargetAsset",
				bSource ? "DCodecSourceAsset" : "DCodecTargetAsset",
				Properties, std::size(Properties)};
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
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer) {}

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
						Durin::InternalConstructor<DMathStructAssetForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(
					Durin::DClass::StaticClass, "", "DMathStructAssetForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static const Durin::DurinCodeGen::FStructPropertyParams VectorProp = {
				"Vector", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DMathStructAssetForTest, Vector)),
				&Durin::Z_Construct_DStruct_Durin_FVector3};
			static const Durin::DurinCodeGen::FStructPropertyParams TransformProp = {
				"Transform", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DMathStructAssetForTest, Transform)),
				&Durin::Z_Construct_DStruct_Durin_FTransform};
			static const Durin::DurinCodeGen::FStructPropertyParams VectorInner = {
				"Vectors_Inner", Durin::EPropertyFlags::None, 1, 0,
				&Durin::Z_Construct_DStruct_Durin_FVector3};
			static const Durin::DurinCodeGen::FArrayPropertyParams VectorsProp = {
				"Vectors", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DMathStructAssetForTest, Vectors)),
				&VectorInner, &GVector3VectorHelper};
			static const Durin::DurinCodeGen::FStringPropertyParams VectorMapKey = {
				"VectorMap_Key", Durin::EPropertyFlags::None, 1, 0, sizeof(std::string),
				Durin::DurinCodeGen::EPropertyGenFlags::String};
			static const Durin::DurinCodeGen::FStructPropertyParams VectorMapValue = {
				"VectorMap_Value", Durin::EPropertyFlags::None, 1, 0,
				&Durin::Z_Construct_DStruct_Durin_FVector3};
			static const Durin::DurinCodeGen::FMapPropertyParams VectorMapProp = {
				"VectorMap", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DMathStructAssetForTest, VectorMap)),
				&VectorMapKey, &VectorMapValue, &GVectorMapHelper};
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&VectorProp, &TransformProp, &VectorsProp, &VectorMapProp};
			static const Durin::DurinCodeGen::FClassParams Params = {
				&StaticClassNoRegister, "Tests::DMathStructAssetForTest",
				"DMathStructAssetForTest", Properties, std::size(Properties)};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		Durin::FVector3 Vector{0.0};
		Durin::FTransform Transform;
		std::vector<Durin::FVector3> Vectors;
		FVectorMap VectorMap;
	};

	auto RegisterTestDeleteContributor() -> void
	{
		static const bool Registered = [] {
			Durin::Asset::RegisterAssetDeleteContributor(DPackageAssetForTest::StaticClass(), [](const Durin::Asset::FAssetData&,
				const Durin::Asset::FAssetPackageInspection& Inspection,
				Durin::Asset::FAssetDeleteContribution& Out) -> Durin::Asset::FAssetResult {
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
			Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
			Durin::GIsGameThreadIdInitialized = true;
			Durin::FNameInit();
			Durin::DObjectInit();
			(void)DPackageAssetForTest::StaticClass();
			(void)DSoftPackageAssetForTest::StaticClass();
			(void)DCodecSourceAsset::StaticClass();
			(void)DCodecTargetAsset::StaticClass();
			(void)DMathStructAssetForTest::StaticClass();
			const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "Assets";
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::FPaths::SetDerivedDataCacheDirForTests(
				(Durin::Testing::GetTestWorkDirectory() / "DerivedDataCache").generic_string());
			Durin::PathUtilities::RegisterMountPointForTests("/TestAssets/", Root.generic_string() + "/");
			return true;
		}();
		(void)Initialized;
	}

	class FMemoryAssetReferenceStore final
		: public Durin::Asset::IAssetReferenceStore
	{
	public:
		explicit FMemoryAssetReferenceStore(
			Durin::FAssetPath InPath,
			bool bInCookRoot = false,
			std::string InExpectedClass = {})
			: Path(std::move(InPath))
			, ExpectedClass(std::move(InExpectedClass))
			, bCookRoot(bInCookRoot)
		{}

		auto CaptureSnapshot(
			Durin::Asset::FAssetReferenceStoreSnapshot& OutSnapshot)
			-> Durin::Asset::FAssetResult override
		{
			OutSnapshot = {
				.ProviderId = "Tests.MemoryReferenceStore",
				.ProviderVersion = 1,
				.Fingerprint = Path.ToString(),
				.Occurrences = {{
					.ProviderId = "Tests.MemoryReferenceStore",
					.StableId = "root",
					.TargetPath = Path,
					.DisplayRoute = "Root",
					.ExpectedClass = ExpectedClass,
					.bCookRoot = bCookRoot}}};
			return {};
		}

		auto PrepareRewrite(
			std::span<const Durin::Asset::FAssetReferenceRewrite> Rewrites,
			std::string_view ExpectedFingerprint,
			Durin::Asset::FAssetReferenceStoreRewriteContribution& OutContribution)
			-> Durin::Asset::FAssetResult override
		{
			if (ExpectedFingerprint != Path.ToString() || Rewrites.size() != 1
				|| Rewrites.front().StableId != "root"
				|| Rewrites.front().SourcePath != Path)
				return {Durin::Asset::EAssetError::StaleData,
					"Memory reference store preparation is stale."};
			const Durin::FAssetPath PrePath = Path;
			const Durin::FAssetPath PostPath = Rewrites.front().DestinationPath;
			OutContribution = {
				.Fingerprint = std::string(ExpectedFingerprint),
				.Rewrites = {Rewrites.front()},
				.Revalidate = [this, PrePath] {
					return Path == PrePath
						? Durin::Asset::FAssetResult{}
						: Durin::Asset::FAssetResult{
							Durin::Asset::EAssetError::StaleData,
							"Memory reference store changed."};
				},
				.Apply = [this, PostPath] {
					Path = PostPath;
					return Durin::Asset::FAssetResult{};
				},
				.Restore = [this, PrePath] {
					Path = PrePath;
					return Durin::Asset::FAssetResult{};
				},
				.Verify = [this, PostPath] {
					return Path == PostPath
						? Durin::Asset::FAssetResult{}
						: Durin::Asset::FAssetResult{
							Durin::Asset::EAssetError::StaleData,
							"Memory reference store verification failed."};
				}};
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
			Durin::Asset::IAssetReferenceStore* Store)
			: Handle(Durin::Asset::RegisterAssetReferenceStore(Store))
		{}

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
		Durin::Asset::FAssetManager::Get().Initialize();
	}

	auto WriteTestBytes(const std::filesystem::path& Path, std::span<const Durin::uint8> Bytes) -> void
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(Stream.is_open());
		Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		ASSERT_TRUE(Stream.good());
	}

	auto RenameSerializedString(
		std::vector<Durin::uint8>& Bytes,
		std::string_view OldValue,
		std::string_view NewValue
	) -> bool
	{
		if (OldValue.size() != NewValue.size()) return false;
		std::vector<Durin::uint8> Pattern(sizeof(Durin::uint64) + OldValue.size());
		const Durin::uint64 Length = OldValue.size();
		std::memcpy(Pattern.data(), &Length, sizeof(Length));
		std::memcpy(Pattern.data() + sizeof(Length), OldValue.data(), OldValue.size());
		const auto It = std::search(Bytes.begin(), Bytes.end(), Pattern.begin(), Pattern.end());
		if (It == Bytes.end()) return false;
		std::copy(NewValue.begin(), NewValue.end(), It + sizeof(Length));
		return true;
	}

	auto RenameAllSerializedStrings(
		std::vector<Durin::uint8>& Bytes,
		std::string_view OldValue,
		std::string_view NewValue) -> Durin::uint64
	{
		Durin::uint64 Count = 0;
		while (RenameSerializedString(Bytes, OldValue, NewValue)) ++Count;
		return Count;
	}

	auto RenameSerializedStringOccurrence(
		std::vector<Durin::uint8>& Bytes,
		std::string_view OldValue,
		std::string_view NewValue,
		size_t Occurrence) -> bool
	{
		if (OldValue.size() != NewValue.size()) return false;
		std::vector<Durin::uint8> Pattern(sizeof(Durin::uint64) + OldValue.size());
		const Durin::uint64 Length = OldValue.size();
		std::memcpy(Pattern.data(), &Length, sizeof(Length));
		std::memcpy(Pattern.data() + sizeof(Length), OldValue.data(), OldValue.size());
		auto SearchStart = Bytes.begin();
		for (size_t Index = 0; Index <= Occurrence; ++Index)
		{
			const auto It = std::search(
				SearchStart, Bytes.end(), Pattern.begin(), Pattern.end());
			if (It == Bytes.end()) return false;
			if (Index == Occurrence)
			{
				std::copy(NewValue.begin(), NewValue.end(), It + sizeof(Length));
				return true;
			}
			SearchStart = It + static_cast<std::ptrdiff_t>(Pattern.size());
		}
		return false;
	}

	auto SetVersionThreeEntryKind(
		std::vector<Durin::uint8>& Bytes,
		Durin::uint8 EntryKind) -> bool
	{
		if (Bytes.size() < sizeof(Durin::uint32) * 2 + sizeof(Durin::uint64))
			return false;
		size_t Offset = sizeof(Durin::uint32) * 2;
		Durin::uint64 ClassLength = 0;
		std::memcpy(&ClassLength, Bytes.data() + Offset, sizeof(ClassLength));
		Offset += sizeof(ClassLength);
		if (ClassLength >= Bytes.size() - Offset) return false;
		Offset += static_cast<size_t>(ClassLength);
		Bytes[Offset] = EntryKind;
		return true;
	}

	auto ConvertVersionThreeAssetToVersionTwo(
		std::vector<Durin::uint8>& Bytes) -> bool
	{
		if (Bytes.size() < sizeof(Durin::uint32) * 2 + sizeof(Durin::uint64))
			return false;
		Durin::uint32 Version = 0;
		std::memcpy(&Version, Bytes.data() + sizeof(Durin::uint32), sizeof(Version));
		if (Version != 3) return false;
		size_t Offset = sizeof(Durin::uint32) * 2;
		Durin::uint64 ClassLength = 0;
		std::memcpy(&ClassLength, Bytes.data() + Offset, sizeof(ClassLength));
		Offset += sizeof(ClassLength);
		if (ClassLength > Bytes.size() - Offset) return false;
		Offset += static_cast<size_t>(ClassLength);
		if (Offset + sizeof(Durin::uint8) + sizeof(Durin::uint64) > Bytes.size())
			return false;
		Durin::uint64 DestinationLength = 0;
		std::memcpy(
			&DestinationLength,
			Bytes.data() + Offset + sizeof(Durin::uint8),
			sizeof(DestinationLength));
		const size_t SummarySize = sizeof(Durin::uint8)
			+ sizeof(Durin::uint64) + static_cast<size_t>(DestinationLength);
		if (DestinationLength > Bytes.size() - Offset - sizeof(Durin::uint8)
			- sizeof(Durin::uint64))
			return false;
		Bytes.erase(Bytes.begin() + static_cast<std::ptrdiff_t>(Offset),
			Bytes.begin() + static_cast<std::ptrdiff_t>(Offset + SummarySize));
		Version = 2;
		std::memcpy(Bytes.data() + sizeof(Durin::uint32), &Version, sizeof(Version));
		return true;
	}

	auto MakeCompatibilityProbeInput(
		const Durin::FAssetPath& PackagePath,
		const std::filesystem::path& PhysicalPath)
		-> Durin::Asset::FAssetPackageCompatibilityProbeInput
	{
		std::error_code Error;
		const auto LastWriteTime = std::filesystem::last_write_time(PhysicalPath, Error);
		EXPECT_FALSE(Error);
		return {
			.PackagePath = PackagePath,
			.PhysicalPath = PhysicalPath.generic_string(),
			.ExpectedFileSize = std::filesystem::file_size(PhysicalPath, Error),
			.ExpectedLastWriteTimeTicks = Durin::DerivedDataCache::FileTimeToStableTicks(LastWriteTime)};
	}

	auto HexDigit(char Character) -> Durin::uint8
	{
		if (Character >= '0' && Character <= '9') return static_cast<Durin::uint8>(Character - '0');
		if (Character >= 'A' && Character <= 'F') return static_cast<Durin::uint8>(Character - 'A' + 10);
		if (Character >= 'a' && Character <= 'f') return static_cast<Durin::uint8>(Character - 'a' + 10);
		ADD_FAILURE() << "Invalid hexadecimal fixture digit.";
		return 0;
	}

	auto WriteCompatibilityFixture(std::string_view Name, const std::filesystem::path& Destination) -> void
	{
		std::ifstream Stream(std::filesystem::path(DURIN_TEST_DATA_DIR) / std::format("{}.dasset.hex", Name));
		ASSERT_TRUE(Stream.is_open());
		std::string Hex;
		Stream >> Hex;
		ASSERT_FALSE(Hex.empty());
		ASSERT_EQ(Hex.size() % 2, 0u);
		std::vector<Durin::uint8> Bytes(Hex.size() / 2);
		for (size_t Index = 0; Index < Bytes.size(); ++Index)
			Bytes[Index] = static_cast<Durin::uint8>((HexDigit(Hex[Index * 2]) << 4) | HexDigit(Hex[Index * 2 + 1]));
		WriteTestBytes(Destination, Bytes);
	}

	auto RunRedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletionTest()
		-> void;
	auto RunRedirectorFixupVerificationFailureRestoresPackagesStoresAndAliasTest()
		-> void;
	auto RunRedirectorFixupRejectsUnavailableProviderWithoutMutationTest()
		-> void;
	auto RunRedirectorFixupRejectsDirtyAndCompatibilityRiskPackagesTest()
		-> void;
	auto RunRedirectorFixupRejectsReadOnlyAndChangedPackageInputsTest()
		-> void;
	auto RunRedirectorFixupPublicationFailuresRestoreAllParticipantsTest()
		-> void;
}

TEST(FPackageAssetTests, RedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletion)
{
	RunRedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletionTest();
}

TEST(FPackageAssetTests, RedirectorFixupVerificationFailureRestoresPackagesStoresAndAlias)
{
	RunRedirectorFixupVerificationFailureRestoresPackagesStoresAndAliasTest();
}

TEST(FPackageAssetTests, RedirectorFixupRejectsUnavailableProviderWithoutMutation)
{
	RunRedirectorFixupRejectsUnavailableProviderWithoutMutationTest();
}

TEST(FPackageAssetTests, RedirectorFixupRejectsDirtyAndCompatibilityRiskPackages)
{
	RunRedirectorFixupRejectsDirtyAndCompatibilityRiskPackagesTest();
}

TEST(FPackageAssetTests, RedirectorFixupRejectsReadOnlyAndChangedPackageInputs)
{
	RunRedirectorFixupRejectsReadOnlyAndChangedPackageInputsTest();
}

TEST(FPackageAssetTests, RedirectorFixupPublicationFailuresRestoreAllParticipants)
{
	RunRedirectorFixupPublicationFailuresRestoreAllParticipantsTest();
}

TEST(FPackageAssetTests, HeaderReaderStopsBeforeLargeObjectPayload)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LargeHeaderOnly", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Scores.resize(1024 * 1024, 7);
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "LargeHeaderOnly.dasset";
	ASSERT_GT(std::filesystem::file_size(File), 4u * 1024u * 1024u);

	Durin::Asset::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::Asset::ReadAssetPackageHeader(File.generic_string(), Header));
	EXPECT_EQ(Header.AssetClassName, "Tests::DPackageAssetForTest");
	EXPECT_EQ(Header.FormatVersion, 3u);
	EXPECT_EQ(Header.EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_FALSE(Header.RedirectDestination.IsValid());
	EXPECT_EQ(Header.ObjectCount, 2u);
	EXPECT_LT(Header.BytesRead, 1024u);
}

TEST(FPackageAssetTests, WriterEmitsVersionThreeRedirectSummary)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/VersionThreePrefix", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));

	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "VersionThreePrefix.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	constexpr std::array<Durin::uint8, 8> ExpectedPrefix = {
		0x44, 0x41, 0x53, 0x54,
		0x03, 0x00, 0x00, 0x00};
	ASSERT_GE(Bytes.size(), ExpectedPrefix.size());
	EXPECT_TRUE(std::ranges::equal(
		ExpectedPrefix,
		std::span<const Durin::uint8>(Bytes).first(ExpectedPrefix.size())));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
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
	std::vector<Durin::uint8> Valid;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Valid, Source.generic_string()));
	ASSERT_GT(Valid.size(), 16u);
	Durin::Asset::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::Asset::ReadAssetPackageHeader(Source.generic_string(), Header));

	auto Truncated = std::span<const Durin::uint8>(Valid).first(4);
	const auto TruncatedFile = Root / "HeaderTruncated.dasset";
	WriteTestBytes(TruncatedFile, Truncated);
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(TruncatedFile.generic_string(), Header).Error, Durin::Asset::EAssetError::CorruptFile);

	auto Corrupt = Valid;
	Corrupt[0] ^= 0xff;
	const auto CorruptFile = Root / "HeaderCorrupt.dasset";
	WriteTestBytes(CorruptFile, Corrupt);
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(CorruptFile.generic_string(), Header).Error, Durin::Asset::EAssetError::CorruptFile);

	auto Unsupported = Valid;
	const Durin::uint32 OldVersion = 1;
	std::memcpy(Unsupported.data() + sizeof(Durin::uint32), &OldVersion, sizeof(OldVersion));
	const auto UnsupportedFile = Root / "HeaderUnsupported.dasset";
	WriteTestBytes(UnsupportedFile, Unsupported);
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(UnsupportedFile.generic_string(), Header).Error, Durin::Asset::EAssetError::UnsupportedVersion);

	auto Oversized = Valid;
	const Durin::uint64 OversizedString = 1024 * 1024 + 1;
	std::memcpy(Oversized.data() + sizeof(Durin::uint32) * 2, &OversizedString, sizeof(OversizedString));
	const auto OversizedFile = Root / "HeaderOversized.dasset";
	WriteTestBytes(OversizedFile, Oversized);
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(OversizedFile.generic_string(), Header).Error, Durin::Asset::EAssetError::CorruptFile);
	EXPECT_LE(Header.BytesRead, 16u);
}

TEST(FPackageAssetTests, VersionTwoOrdinaryPackagesRemainReadableWithoutMigration)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/VersionTwoCompatibility", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Value = 73;
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const auto File = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "VersionTwoCompatibility.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_TRUE(ConvertVersionThreeAssetToVersionTwo(Bytes));
	WriteTestBytes(File, Bytes);

	Durin::Asset::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::Asset::ReadAssetPackageHeader(File.generic_string(), Header));
	EXPECT_EQ(Header.FormatVersion, 2u);
	EXPECT_EQ(Header.EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_FALSE(Header.RedirectDestination.IsValid());
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	const Durin::Asset::FAssetData* Data =
		Durin::Asset::GetAssetRegistry().FindAssetExact(Path);
	ASSERT_NE(Data, nullptr);
	EXPECT_EQ(Data->FormatVersion, 2u);
	EXPECT_EQ(Data->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Asset);

	Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Asset));
	ASSERT_NE(Asset, nullptr);
	EXPECT_EQ(Asset->Value, 73);
	EXPECT_FALSE(Asset->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
}

TEST(FPackageAssetTests, RedirectorsRoundTripAndResolveWithoutLoading)
{
	InitializeAssetTests();
	Durin::FAssetPath TargetPath, AliasPath, NormalizedAliasPath, MissingPath,
		UnregisteredPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectRoundTripTarget", TargetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectRoundTripAlias", AliasPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectNormalizedAlias", NormalizedAliasPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectMissingTarget", MissingPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/DoesNotExist", UnregisteredPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetPath, Target));
	Target->Value = 99;
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));

	Durin::Asset::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirector(
		AliasPath, TargetPath, Redirector));
	ASSERT_NE(Redirector, nullptr);
	EXPECT_EQ(Redirector->GetDestinationObject(), Target);
	ASSERT_TRUE(Durin::Asset::SavePackage(Redirector->GetPackage()));
	const auto AliasFile = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "RedirectRoundTripAlias.dasset";
	Durin::Asset::FAssetPackageHeader Header;
	ASSERT_TRUE(Durin::Asset::ReadAssetPackageHeader(
		AliasFile.generic_string(), Header));
	EXPECT_EQ(Header.FormatVersion, 3u);
	EXPECT_EQ(Header.AssetClassName, "Durin::Asset::DAssetRedirector");
	EXPECT_EQ(Header.EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Header.RedirectDestination, TargetPath);
	EXPECT_EQ(Header.Dependencies, (std::vector<Durin::FAssetPath>{TargetPath}));
	EXPECT_EQ(Header.ObjectCount, 1u);
	EXPECT_LT(Header.BytesRead, std::filesystem::file_size(AliasFile));

	Durin::Asset::DAssetRedirector* Normalized = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirector(
		NormalizedAliasPath, AliasPath, Normalized));
	ASSERT_NE(Normalized, nullptr);
	EXPECT_EQ(Normalized->GetDestinationObject(), Target);
	ASSERT_TRUE(Durin::Asset::SavePackage(Normalized->GetPackage()));
	const auto NormalizedFile = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "RedirectNormalizedAlias.dasset";
	ASSERT_TRUE(Durin::Asset::ReadAssetPackageHeader(
		NormalizedFile.generic_string(), Header));
	EXPECT_EQ(Header.RedirectDestination, TargetPath);
	EXPECT_EQ(Durin::Asset::CreateAssetRedirector(
		MissingPath, MissingPath, Redirector).Error,
		Durin::Asset::EAssetError::InvalidPath);
	EXPECT_EQ(Durin::Asset::CreateAssetRedirector(
		MissingPath, UnregisteredPath,
		Redirector).Error,
		Durin::Asset::EAssetError::NotFound);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(NormalizedAliasPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AliasPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetPath));
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(AliasPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(TargetPath), nullptr);
	const Durin::Asset::FAssetData* Exact =
		Durin::Asset::GetAssetRegistry().FindAssetExact(AliasPath);
	ASSERT_NE(Exact, nullptr);
	EXPECT_EQ(Exact->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Exact->RedirectDestination, TargetPath);
	const auto Reverse = Durin::Asset::GetAssetRegistry().FindRedirectorsTo(TargetPath);
	EXPECT_EQ(Reverse,
		(std::vector<Durin::FAssetPath>{NormalizedAliasPath, AliasPath}));
	const Durin::Asset::FAssetPathResolveResult Resolved =
		Durin::Asset::GetAssetRegistry().ResolveAssetPath(AliasPath);
	ASSERT_TRUE(Resolved);
	EXPECT_EQ(Resolved.RequestedPath, AliasPath);
	EXPECT_EQ(Resolved.FinalPath, TargetPath);
	EXPECT_EQ(Resolved.RedirectChain,
		(std::vector<Durin::FAssetPath>{AliasPath}));
	ASSERT_TRUE(Resolved.FinalAssetData.has_value());
	EXPECT_EQ(Resolved.FinalAssetData->PackagePath, TargetPath);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(AliasPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(TargetPath), nullptr);

	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent());
	EXPECT_GE(Durin::Asset::GetAssetRegistry().GetLastScanStats().Reused, 2u);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetLastScanStats().Redirectors, 2u);
	Exact = Durin::Asset::GetAssetRegistry().FindAssetExact(AliasPath);
	ASSERT_NE(Exact, nullptr);
	EXPECT_EQ(Exact->RedirectDestination, TargetPath);
	const auto RegistryCache = std::filesystem::path(
		Durin::FPaths::DerivedDataCacheDir()) / "AssetRegistry" / "Registry.bin";
	const std::array<Durin::uint8, 3> CorruptCache = {1, 2, 3};
	WriteTestBytes(RegistryCache, CorruptCache);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent());
	EXPECT_FALSE(Durin::Asset::GetAssetRegistry().GetCacheWarning().empty());
	Exact = Durin::Asset::GetAssetRegistry().FindAssetExact(AliasPath);
	ASSERT_NE(Exact, nullptr);
	EXPECT_EQ(Exact->RedirectDestination, TargetPath);
	DPackageAssetForTest* RedirectedTarget = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AliasPath, RedirectedTarget));
	ASSERT_NE(RedirectedTarget, nullptr);
	EXPECT_EQ(RedirectedTarget->GetPackage()->GetPackagePath(), TargetPath.ToString());
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(AliasPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(TargetPath), RedirectedTarget->GetPackage());
	Redirector = nullptr;
	EXPECT_EQ(
		Durin::Asset::LoadAsset(AliasPath, Redirector).Error,
		Durin::Asset::EAssetError::TypeMismatch);
	EXPECT_EQ(Redirector, nullptr);

	EXPECT_EQ(
		Durin::Asset::DeleteAsset(AliasPath).Error,
		Durin::Asset::EAssetError::InUse);
	ASSERT_TRUE(DeleteAssetClosureForTest(
		{NormalizedAliasPath, AliasPath, TargetPath}));
}

TEST(FPackageAssetTests, RedirectorValidationRejectsMalformedSummaryAndBody)
{
	InitializeAssetTests();
	Durin::FAssetPath TargetPath, AlternatePath, AliasPath, InvalidPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectValidTargetA", TargetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectValidTargetB", AlternatePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectValidationAlias", AliasPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectInvalidObject", InvalidPath));
	ASSERT_EQ(TargetPath.GetView().size(), AlternatePath.GetView().size());
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	Durin::Asset::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirector(
		AliasPath, TargetPath, Redirector));
	ASSERT_TRUE(Durin::Asset::SavePackage(Redirector->GetPackage()));
	const auto File = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "RedirectValidationAlias.dasset";
	std::vector<Durin::uint8> Valid;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Valid, File.generic_string()));
	ASSERT_TRUE(Durin::Asset::ValidateAssetPackageBytes(Valid));

	auto UnknownKind = Valid;
	ASSERT_TRUE(SetVersionThreeEntryKind(UnknownKind, 2));
	const auto UnknownKindFile = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "RedirectUnknownKind.invalid";
	WriteTestBytes(UnknownKindFile, UnknownKind);
	Durin::Asset::FAssetPackageHeader Header;
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(
		UnknownKindFile.generic_string(), Header).Error,
		Durin::Asset::EAssetError::CorruptFile);

	auto DependencyMismatch = Valid;
	ASSERT_TRUE(RenameSerializedStringOccurrence(
		DependencyMismatch, TargetPath.GetView(), AlternatePath.GetView(), 1));
	EXPECT_EQ(Durin::Asset::ValidateAssetPackageBytes(DependencyMismatch).Error,
		Durin::Asset::EAssetError::CorruptFile);

	auto BodyMismatch = Valid;
	ASSERT_TRUE(RenameSerializedStringOccurrence(
		BodyMismatch, TargetPath.GetView(), AlternatePath.GetView(), 2));
	EXPECT_EQ(Durin::Asset::ValidateAssetPackageBytes(BodyMismatch).Error,
		Durin::Asset::EAssetError::CorruptFile);

	auto LegacyRedirector = Valid;
	ASSERT_TRUE(ConvertVersionThreeAssetToVersionTwo(LegacyRedirector));
	const auto LegacyFile = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "RedirectLegacy.invalid";
	WriteTestBytes(LegacyFile, LegacyRedirector);
	EXPECT_EQ(Durin::Asset::ReadAssetPackageHeader(
		LegacyFile.generic_string(), Header).Error,
		Durin::Asset::EAssetError::CorruptFile);

	Durin::Asset::DAssetRedirector* InvalidRedirector = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(InvalidPath, InvalidRedirector));
	EXPECT_EQ(Durin::Asset::SavePackage(InvalidRedirector->GetPackage()).Error,
		Durin::Asset::EAssetError::CorruptFile);
	ASSERT_TRUE(Durin::Asset::DiscardUnpublishedPackage(
		InvalidRedirector->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AliasPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetPath));
	WriteTestBytes(File, BodyMismatch);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(AliasPath), nullptr);
	EXPECT_TRUE(std::ranges::any_of(
		Durin::Asset::GetAssetRegistry().GetScanErrors(),
		[](const Durin::Asset::FAssetResult& Error) {
			return Error.Message.starts_with("CorruptRedirector:");
		}));
	WriteTestBytes(File, Valid);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAssetExact(AliasPath), nullptr);

	std::filesystem::remove(UnknownKindFile);
	std::filesystem::remove(LegacyFile);
	ASSERT_TRUE(DeleteAssetClosureForTest({AliasPath, TargetPath}));
}

TEST(FPackageAssetTests, RedirectResolutionHandlesBoundsCyclesAndStableErrors)
{
	InitializeAssetTests();
	Durin::FAssetPath TargetPath, MissingPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RChainZZ", TargetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RChainMM", MissingPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));

	std::vector<Durin::FAssetPath> Aliases;
	Aliases.reserve(33);
	for (Durin::uint32 Index = 0; Index < 33; ++Index)
	{
		Durin::FAssetPath AliasPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/RChain{:02}", Index), AliasPath));
		Durin::Asset::DAssetRedirector* Redirector = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAssetRedirector(
			AliasPath, TargetPath, Redirector));
		ASSERT_TRUE(Durin::Asset::SavePackage(Redirector->GetPackage()));
		Aliases.push_back(AliasPath);
	}
	for (const Durin::FAssetPath& Alias : Aliases)
		ASSERT_TRUE(Durin::Asset::UnloadPackage(Alias));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetPath));

	const auto Root = Durin::Testing::GetTestWorkDirectory() / "Assets";
	for (size_t Index = 0; Index + 1 < Aliases.size(); ++Index)
	{
		const auto File = Root / std::format("RChain{:02}.dasset", Index);
		std::vector<Durin::uint8> Bytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
		EXPECT_EQ(RenameAllSerializedStrings(
			Bytes, TargetPath.GetView(), Aliases[Index + 1].GetView()), 3u);
		WriteTestBytes(File, Bytes);
	}
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	const auto MaximumValid =
		Durin::Asset::GetAssetRegistry().ResolveAssetPath(Aliases[1]);
	ASSERT_TRUE(MaximumValid);
	EXPECT_EQ(MaximumValid.FinalPath, TargetPath);
	EXPECT_EQ(MaximumValid.RedirectChain.size(), 32u);
	DPackageAssetForTest* LoadedThroughAlias = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Aliases[1], LoadedThroughAlias));
	ASSERT_NE(LoadedThroughAlias, nullptr);
	EXPECT_EQ(LoadedThroughAlias->GetPackage()->GetPackagePath(), TargetPath.ToString());
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Aliases[1]), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetPath));
	Durin::TSoftObjectPtr<DPackageAssetForTest> SoftAlias(Aliases[1]);
	ASSERT_TRUE(Durin::Asset::LoadSoftObject(SoftAlias, LoadedThroughAlias));
	EXPECT_EQ(SoftAlias.GetSoftObjectPath().GetAssetPath(), Aliases[1]);
	EXPECT_EQ(SoftAlias.Get(), LoadedThroughAlias);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetPath));
	const auto TooDeep =
		Durin::Asset::GetAssetRegistry().ResolveAssetPath(Aliases[0]);
	EXPECT_EQ(TooDeep.State,
		Durin::Asset::EAssetPathResolveState::RedirectDepthExceeded);
	EXPECT_EQ(TooDeep.RedirectChain.size(), 32u);
	LoadedThroughAlias = nullptr;
	EXPECT_EQ(
		Durin::Asset::LoadAsset(Aliases[0], LoadedThroughAlias).Error,
		Durin::Asset::EAssetError::CircularDependency);
	EXPECT_EQ(LoadedThroughAlias, nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(TargetPath), nullptr);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindRedirectorsTo(Aliases[1]),
		(std::vector<Durin::FAssetPath>{Aliases[0]}));

	const auto TypeMismatch = Durin::Asset::GetAssetRegistry().ResolveAssetPath(
		Aliases[1], {.ExpectedClass = DSoftPackageAssetForTest::StaticClass()});
	EXPECT_EQ(TypeMismatch.State,
		Durin::Asset::EAssetPathResolveState::RedirectTypeMismatch);
	const auto NotFound =
		Durin::Asset::GetAssetRegistry().ResolveAssetPath(MissingPath);
	EXPECT_EQ(NotFound.State, Durin::Asset::EAssetPathResolveState::NotFound);

	const auto TailFile = Root / "RChain32.dasset";
	std::vector<Durin::uint8> TailBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		TailBytes, TailFile.generic_string()));
	EXPECT_EQ(RenameAllSerializedStrings(
		TailBytes, TargetPath.GetView(), MissingPath.GetView()), 3u);
	WriteTestBytes(TailFile, TailBytes);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	const auto Missing =
		Durin::Asset::GetAssetRegistry().ResolveAssetPath(Aliases[1]);
	EXPECT_EQ(Missing.State,
		Durin::Asset::EAssetPathResolveState::MissingRedirectTarget);
	EXPECT_EQ(Missing.RedirectChain.size(), 32u);
	EXPECT_EQ(
		Durin::Asset::LoadAsset(Aliases[1], LoadedThroughAlias).Error,
		Durin::Asset::EAssetError::NotFound);
	EXPECT_EQ(LoadedThroughAlias, nullptr);

	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		TailBytes, TailFile.generic_string()));
	EXPECT_EQ(RenameAllSerializedStrings(
		TailBytes, MissingPath.GetView(), Aliases[1].GetView()), 3u);
	WriteTestBytes(TailFile, TailBytes);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	const auto Cycle =
		Durin::Asset::GetAssetRegistry().ResolveAssetPath(Aliases[1]);
	EXPECT_EQ(Cycle.State, Durin::Asset::EAssetPathResolveState::RedirectCycle);
	EXPECT_EQ(Cycle.RedirectChain.size(), 32u);
	EXPECT_EQ(
		Durin::Asset::LoadAsset(Aliases[1], LoadedThroughAlias).Error,
		Durin::Asset::EAssetError::CircularDependency);
	EXPECT_EQ(LoadedThroughAlias, nullptr);

	for (const Durin::FAssetPath& Alias : Aliases)
		std::filesystem::remove(Root / std::format("{}.dasset", Alias.GetAssetName()));
	std::filesystem::remove(Root / "RChainZZ.dasset");
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
}

TEST(FPackageAssetTests, RedirectResolutionRejectsUnknownFinalClass)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RedirectUnknownFinalClass", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const auto File = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "RedirectUnknownFinalClass.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	const std::string_view KnownClass = "Tests::DPackageAssetForTest";
	const std::string UnknownClass(KnownClass.size(), 'Z');
	ASSERT_TRUE(RenameSerializedStringOccurrence(
		Bytes, KnownClass, UnknownClass, 0));
	WriteTestBytes(File, Bytes);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	const auto Result = Durin::Asset::GetAssetRegistry().ResolveAssetPath(Path);
	EXPECT_EQ(Result.State,
		Durin::Asset::EAssetPathResolveState::UnknownTargetClass);
	DPackageAssetForTest* Loaded = nullptr;
	EXPECT_EQ(
		Durin::Asset::LoadAsset(Path, Loaded).Error,
		Durin::Asset::EAssetError::UnknownClass);
	EXPECT_EQ(Loaded, nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
	std::filesystem::remove(File);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
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
	Asset->SourcePath.Path = "/TestAssets/Sources/RoundTrip.txt";
	ASSERT_NE(Asset->DefaultChild.Get(), nullptr);
	EXPECT_EQ(Asset->DefaultChild->GetObjectPath(), "/TestAssets/RoundTrip:DefaultChild");

	Durin::DPackage* Package = Asset->GetPackage();
	ASSERT_NE(Package, nullptr);
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().SavePackage(Package));
	ASSERT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAssetExact(Path), nullptr);
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(Path));

	DPackageAssetForTest* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->Value, 42);
	EXPECT_EQ(Loaded->Label, "RoundTrip");
	EXPECT_EQ(Loaded->DisplayName, Durin::FName("RoundTripName"));
	EXPECT_EQ(Loaded->PersistentId, (Durin::FGuid(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff)));
	EXPECT_EQ(Loaded->RelatedIds, (std::vector<Durin::FGuid>{Durin::FGuid(1, 2, 3, 4), Durin::FGuid(5, 6, 7, 8)}));
	EXPECT_EQ(Loaded->Scores, (std::vector<Durin::int32>{3, 5, 8}));
	EXPECT_EQ(Loaded->NamedScores.at("Alpha"), 11);
	EXPECT_EQ(Loaded->NamedScores.at("Beta"), 17);
	EXPECT_EQ(Loaded->SourcePath.Path, "/TestAssets/Sources/RoundTrip.txt");
	ASSERT_NE(Loaded->DefaultChild.Get(), nullptr);
	EXPECT_EQ(Durin::GDObjectArray.GetObjectsWithOuter(Loaded).size(), 1u);
	EXPECT_EQ(Loaded->DefaultChild->GetObjectPath(), "/TestAssets/RoundTrip:DefaultChild");
	EXPECT_EQ(Durin::Asset::FAssetManager::Get().FindLoadedPackage(Path), Loaded->GetPackage());

	EXPECT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(Path));
}

TEST(FPackageAssetTests, SoftObjectResolveAndLoadPreservePathAcrossResidencyChanges)
{
	InitializeAssetTests();
	Durin::FAssetPath Path, AliasPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftObjectTarget", Path));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftObjectAlias", AliasPath));

	DPackageAssetForTest* Created = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Created));
	Durin::TSoftObjectPtr<DPackageAssetForTest> UnpublishedReference(Path);
	const auto UnpublishedResolve =
		Durin::Asset::ResolveSoftObject(UnpublishedReference);
	ASSERT_TRUE(UnpublishedResolve);
	EXPECT_EQ(UnpublishedResolve.State, Durin::Asset::ESoftObjectResolveState::Loaded);
	EXPECT_EQ(UnpublishedResolve.Object, Created);
	EXPECT_EQ(UnpublishedResolve.ResolvedPath, Path);
	ASSERT_TRUE(Durin::Asset::SavePackage(Created->GetPackage()));
	Durin::Asset::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirector(AliasPath, Path, Redirector));
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
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(AliasPath), nullptr);

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
		"/TestAssets/SoftRedirectOwner", OwnerPath));
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
		Durin::Asset::EAssetError::TypeMismatch);
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
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(OwnerPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({AliasPath, Path}));
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
		NullReference, Durin::Asset::ESoftObjectNullPolicy::Allow);
	EXPECT_TRUE(AllowedNull);
	EXPECT_EQ(AllowedNull.State, Durin::Asset::ESoftObjectResolveState::Null);
	DPackageAssetForTest* NullObject = reinterpret_cast<DPackageAssetForTest*>(1);
	EXPECT_TRUE(Durin::Asset::LoadSoftObject(
		NullReference, NullObject, Durin::Asset::ESoftObjectNullPolicy::Allow));
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
		Durin::Asset::EAssetError::NotFound);
	EXPECT_EQ(MissingObject, nullptr);
	EXPECT_EQ(MissingReference.GetSoftObjectPath(), OriginalPath);
	EXPECT_FALSE(MissingReference.IsLoaded());
}

TEST(FPackageAssetTests, RedirectedSoftCacheRefreshesAfterDestinationChangesAndRestart)
{
	InitializeAssetTests();
	Durin::FAssetPath FirstPath, SecondPath, AliasPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/SoftCacheTargetA", FirstPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/SoftCacheTargetB", SecondPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/SoftCacheAliasXX", AliasPath));
	DPackageAssetForTest* First = nullptr;
	DPackageAssetForTest* Second = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(FirstPath, First));
	ASSERT_TRUE(Durin::Asset::SavePackage(First->GetPackage()));
	ASSERT_TRUE(Durin::Asset::CreateAsset(SecondPath, Second));
	ASSERT_TRUE(Durin::Asset::SavePackage(Second->GetPackage()));
	Durin::Asset::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirector(AliasPath, FirstPath, Redirector));
	ASSERT_TRUE(Durin::Asset::SavePackage(Redirector->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AliasPath));

	Durin::TSoftObjectPtr<DPackageAssetForTest> Reference(AliasPath);
	auto Resolved = Durin::Asset::ResolveSoftObject(Reference);
	ASSERT_TRUE(Resolved);
	EXPECT_EQ(Resolved.Object, First);
	EXPECT_EQ(Reference.Get(), First);

	const auto AliasFile = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "SoftCacheAliasXX.dasset";
	std::vector<Durin::uint8> AliasBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AliasBytes, AliasFile.generic_string()));
	EXPECT_EQ(RenameAllSerializedStrings(
		AliasBytes, FirstPath.GetView(), SecondPath.GetView()), 3u);
	WriteTestBytes(AliasFile, AliasBytes);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));

	Resolved = Durin::Asset::ResolveSoftObject(Reference);
	ASSERT_TRUE(Resolved);
	EXPECT_EQ(Resolved.ResolvedPath, SecondPath);
	EXPECT_EQ(Resolved.Object, Second);
	EXPECT_EQ(Reference.Get(), Second);
	EXPECT_EQ(Reference.GetSoftObjectPath().GetAssetPath(), AliasPath);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SecondPath));
	EXPECT_EQ(Reference.Get(), nullptr);
	Resolved = Durin::Asset::ResolveSoftObject(Reference);
	ASSERT_TRUE(Resolved);
	EXPECT_EQ(Resolved.State, Durin::Asset::ESoftObjectResolveState::NotLoaded);
	EXPECT_EQ(Reference.Get(), nullptr);

	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent());
	Second = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadSoftObject(Reference, Second));
	ASSERT_NE(Second, nullptr);
	EXPECT_EQ(Second->GetPackage()->GetPackagePath(), SecondPath.ToString());
	EXPECT_EQ(Reference.GetSoftObjectPath().GetAssetPath(), AliasPath);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(AliasPath), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SecondPath));
	ASSERT_TRUE(DeleteAssetClosureForTest(
		{AliasPath, FirstPath, SecondPath}));
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
			Durin::FName("Direct"), false));
	ASSERT_NE(Property, nullptr);

	std::vector<Durin::uint8> Bytes;
	Durin::FMemoryWriter Writer(Bytes);
	Durin::SerializeReflectedPropertyValue(Writer, Property, Owner);
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
	ASSERT_EQ(Bytes.front(), 1u);
	Owner->Direct.SetPath(SentinelPath);
	Durin::FMemoryReader Reader(Bytes);
	Durin::SerializeReflectedPropertyValue(Reader, Property, Owner);
	ASSERT_FALSE(Reader.HasError()) << Reader.GetError();
	EXPECT_EQ(Owner->Direct.GetSoftObjectPath().GetAssetPath(), TargetPath);
	EXPECT_FALSE(Owner->Direct.IsLoaded());

	std::vector<Durin::uint8> OversizedBytes;
	Durin::FMemoryWriter OversizedWriter(OversizedBytes);
	Durin::uint8 ReferenceKind = 1;
	Durin::uint64 OversizedPathSize = 1024 * 1024 + 1;
	OversizedWriter << ReferenceKind << OversizedPathSize;
	Owner->Direct.SetPath(SentinelPath);
	Durin::FMemoryReader OversizedReader(OversizedBytes);
	Durin::SerializeReflectedPropertyValue(OversizedReader, Property, Owner);
	ASSERT_TRUE(OversizedReader.HasError());
	EXPECT_NE(OversizedReader.GetError().find("1 MiB"), std::string_view::npos);
	EXPECT_EQ(Owner->Direct.GetSoftObjectPath().GetAssetPath(), SentinelPath);
	OversizedReader.SetError("must remain sticky");
	EXPECT_NE(OversizedReader.GetError().find("1 MiB"), std::string_view::npos);

	std::vector<Durin::uint8> NullBytes{0};
	Durin::FMemoryReader NullReader(NullBytes);
	Durin::SerializeReflectedPropertyValue(NullReader, Property, Owner);
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
		DSoftPackageAssetForTest::FSoftReference(MissingPath)};
	Owner->Map.emplace(
		"hero", DSoftPackageAssetForTest::FSoftReference(TargetPath));
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));

	const Durin::Asset::FAssetData* OwnerData =
		Durin::Asset::GetAssetRegistry().FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	EXPECT_TRUE(OwnerData->Dependencies.empty());
	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		OwnerData->PhysicalPath, Inspection));
	const Durin::Asset::FAssetPackageField* DirectField =
		Inspection.FindField("Direct");
	ASSERT_NE(DirectField, nullptr);
	EXPECT_EQ(
		DirectField->TypeSignature,
		"SoftObject:Tests::DPackageAssetForTest:v1");
	const Durin::Asset::FReflectionCompatibilityCatalog Catalog =
		Durin::Asset::FReflectionCompatibilityCatalog::Capture();
	const auto* CatalogClass = Catalog.FindClass("Tests::DSoftPackageAssetForTest");
	ASSERT_NE(CatalogClass, nullptr);
	const auto* CatalogField = Catalog.FindField(
		*CatalogClass, "Tests::DSoftPackageAssetForTest", "Direct");
	ASSERT_NE(CatalogField, nullptr);
	EXPECT_EQ(CatalogField->TypeSignature, DirectField->TypeSignature);
	ASSERT_GE(DirectField->Payload.size(), 1u + sizeof(Durin::uint64));
	EXPECT_EQ(DirectField->Payload.front(), 1u);
	const Durin::Asset::FAssetPackageField* FixedField =
		Inspection.FindField("Fixed");
	ASSERT_NE(FixedField, nullptr);
	const size_t FirstFixedValueBytes =
		1 + sizeof(Durin::uint64) + MissingPath.GetView().size();
	ASSERT_EQ(FixedField->Payload.size(), FirstFixedValueBytes + 1);
	EXPECT_EQ(FixedField->Payload[FirstFixedValueBytes], 0u);

	std::vector<Durin::Asset::FAssetReferenceEdge> Extracted;
	ASSERT_TRUE(Durin::Asset::ExtractAssetReferences(
		OwnerPath, Inspection, Extracted));
	ASSERT_EQ(Extracted.size(), 5u);
	EXPECT_EQ(std::ranges::count(Extracted, TargetPath,
		&Durin::Asset::FAssetReferenceEdge::TargetPath), 3);
	EXPECT_EQ(std::ranges::count(Extracted, MissingPath,
		&Durin::Asset::FAssetReferenceEdge::TargetPath), 2);
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

	auto Referencers = Durin::Asset::GetAssetRegistry().GetReferenceIndex()
		.FindReferencers(TargetPath);
	EXPECT_EQ(Referencers.size(), 3u);
	auto Targets = Durin::Asset::GetAssetRegistry().GetReferenceIndex()
		.FindTargets(OwnerPath);
	ASSERT_EQ(Targets.size(), 2u);
	EXPECT_EQ(Targets[0], MissingPath);
	EXPECT_EQ(Targets[1], TargetPath);

	// The loaded owner and a populated weak cache do not block target unload.
	std::vector<Durin::uint8> CachedBytes;
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(
		Owner->GetPackage(), CachedBytes));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetPath));
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(TargetPath), nullptr);
	std::vector<Durin::uint8> UnloadedBytes;
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(
		Owner->GetPackage(), UnloadedBytes));
	EXPECT_EQ(UnloadedBytes, CachedBytes);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	DSoftPackageAssetForTest* LoadedOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, LoadedOwner));
	ASSERT_NE(LoadedOwner, nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(TargetPath), nullptr);
	EXPECT_EQ(LoadedOwner->Direct.GetSoftObjectPath().GetAssetPath(), TargetPath);
	EXPECT_EQ(LoadedOwner->Fixed[0].GetSoftObjectPath().GetAssetPath(), MissingPath);
	EXPECT_EQ(LoadedOwner->Array[1].GetSoftObjectPath().GetAssetPath(), MissingPath);
	EXPECT_EQ(LoadedOwner->Map.at("hero").GetSoftObjectPath().GetAssetPath(), TargetPath);
	EXPECT_FALSE(LoadedOwner->Direct.IsLoaded());
	DPackageAssetForTest* Missing = nullptr;
	EXPECT_EQ(
		Durin::Asset::LoadSoftObject(LoadedOwner->Fixed[0], Missing).Error,
		Durin::Asset::EAssetError::NotFound);
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
	const auto* Data = Durin::Asset::GetAssetRegistry().FindAssetExact(OwnerPath);
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
	auto MakePathPayload = [](Durin::uint8 Kind, std::string_view Path) {
		std::vector<Durin::uint8> Payload{Kind};
		const Durin::uint64 Size = Path.size();
		const auto* SizeBytes = reinterpret_cast<const Durin::uint8*>(&Size);
		Payload.insert(Payload.end(), SizeBytes, SizeBytes + sizeof(Size));
		Payload.insert(Payload.end(), Path.begin(), Path.end());
		return Payload;
	};
	std::vector<Durin::Asset::FAssetReferenceEdge> References;

	auto Unknown = Valid;
	ASSERT_NE(FindMutableField(Unknown, "Direct"), nullptr);
	FindMutableField(Unknown, "Direct")->Name = "RetiredSoftField";
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(
		OwnerPath, Unknown, References).Error,
		Durin::Asset::EAssetError::TypeMismatch);

	auto WrongSignature = Valid;
	FindMutableField(WrongSignature, "Direct")->TypeSignature =
		"SoftObject:Tests::DCodecSourceAsset:v1";
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(
		OwnerPath, WrongSignature, References).Error,
		Durin::Asset::EAssetError::TypeMismatch);

	auto UnknownTag = Valid;
	FindMutableField(UnknownTag, "Direct")->Payload = {2};
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(
		OwnerPath, UnknownTag, References).Error,
		Durin::Asset::EAssetError::CorruptFile);

	auto Truncated = Valid;
	FindMutableField(Truncated, "Direct")->Payload = {1};
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(
		OwnerPath, Truncated, References).Error,
		Durin::Asset::EAssetError::CorruptFile);

	auto Overlong = Valid;
	FindMutableField(Overlong, "Direct")->Payload = {1};
	const Durin::uint64 OverlongSize = 1024 * 1024 + 1;
	const auto* OverlongBytes = reinterpret_cast<const Durin::uint8*>(&OverlongSize);
	FindMutableField(Overlong, "Direct")->Payload.insert(
		FindMutableField(Overlong, "Direct")->Payload.end(),
		OverlongBytes, OverlongBytes + sizeof(OverlongSize));
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(
		OwnerPath, Overlong, References).Error,
		Durin::Asset::EAssetError::CorruptFile);

	auto InvalidPath = Valid;
	FindMutableField(InvalidPath, "Direct")->Payload =
		MakePathPayload(1, "/TestAssets//Invalid");
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(
		OwnerPath, InvalidPath, References).Error,
		Durin::Asset::EAssetError::InvalidPath);

	auto TrailingNull = Valid;
	FindMutableField(TrailingNull, "Direct")->Payload = {0, 0};
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(
		OwnerPath, TrailingNull, References).Error,
		Durin::Asset::EAssetError::CorruptFile);

	auto RuntimeMismatch = Valid;
	RuntimeMismatch.Header.AssetClassName = "Tests::DCodecSourceAsset";
	EXPECT_EQ(Durin::Asset::ExtractAssetReferences(
		OwnerPath, RuntimeMismatch, References).Error,
		Durin::Asset::EAssetError::TypeMismatch);

	Durin::FAssetPath OldPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftInspectionOld", OldPath));
	DPackageAssetForTest* OldAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, OldAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(OldAsset->GetPackage()));
	const auto* OldData = Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath);
	ASSERT_NE(OldData, nullptr);
	Durin::Asset::FAssetPackageInspection OldInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		OldData->PhysicalPath, OldInspection));
	ASSERT_TRUE(Durin::Asset::ExtractAssetReferences(
		OldPath, OldInspection, References));
	EXPECT_TRUE(References.empty());

	std::vector<Durin::uint8> OmittedSoftBytes;
	Durin::Asset::FAssetPackageSerializationOptions OmitSoftFields;
	OmitSoftFields.PropertyFilter = [](const Durin::DObject*, const Durin::FProperty* Property) {
		return Property->NamePrivate.ToString() != "Direct"
			&& Property->NamePrivate.ToString() != "Fixed"
			&& Property->NamePrivate.ToString() != "Array"
			&& Property->NamePrivate.ToString() != "Map";
	};
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(
		Owner->GetPackage(), OmittedSoftBytes, OmitSoftFields));
	WriteTestBytes(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "SoftOmittedFields.dasset",
		OmittedSoftBytes);
	Durin::FAssetPath OmittedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/SoftOmittedFields", OmittedPath));
	DSoftPackageAssetForTest* Omitted = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(OmittedPath, Omitted));
	ASSERT_NE(Omitted, nullptr);
	EXPECT_TRUE(Omitted->Direct.IsNull());
	EXPECT_TRUE(Omitted->Fixed[0].IsNull());
	EXPECT_TRUE(Omitted->Array.empty());
	EXPECT_TRUE(Omitted->Map.empty());
}

TEST(FPackageAssetTests, DastSoftReaderRejectsMalformedTagsBoundsAndPaths)
{
	InitializeAssetTests();
	Durin::FAssetPath SourcePath;
	Durin::FAssetPath TargetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMalformedSource", SourcePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMalformedTarget", TargetPath));
	DSoftPackageAssetForTest* Source = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Source));
	Source->Direct.SetPath(TargetPath);
	std::vector<Durin::uint8> ValidBytes;
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(
		Source->GetPackage(), ValidBytes));

	std::vector<Durin::uint8> Pattern{1};
	const Durin::uint64 PathSize = TargetPath.GetView().size();
	const auto* PathSizeBytes = reinterpret_cast<const Durin::uint8*>(&PathSize);
	Pattern.insert(Pattern.end(), PathSizeBytes, PathSizeBytes + sizeof(PathSize));
	Pattern.insert(Pattern.end(), TargetPath.GetView().begin(), TargetPath.GetView().end());
	const auto PatternIt = std::search(
		ValidBytes.begin(), ValidBytes.end(), Pattern.begin(), Pattern.end());
	ASSERT_NE(PatternIt, ValidBytes.end());
	const size_t PayloadOffset = static_cast<size_t>(PatternIt - ValidBytes.begin());
	const auto Root = Durin::Testing::GetTestWorkDirectory() / "Assets";

	auto UnknownTag = ValidBytes;
	UnknownTag[PayloadOffset] = 2;
	WriteTestBytes(Root / "SoftMalformedTag.dasset", UnknownTag);
	Durin::FAssetPath UnknownTagPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SoftMalformedTag", UnknownTagPath));
	DSoftPackageAssetForTest* Loaded = nullptr;
	EXPECT_EQ(Durin::Asset::LoadAsset(UnknownTagPath, Loaded).Error,
		Durin::Asset::EAssetError::CorruptFile);
	EXPECT_EQ(Loaded, nullptr);

	auto Overlong = ValidBytes;
	const Durin::uint64 OverlongPathSize = 1024 * 1024 + 1;
	std::memcpy(
		Overlong.data() + PayloadOffset + 1,
		&OverlongPathSize, sizeof(OverlongPathSize));
	WriteTestBytes(Root / "SoftMalformedOverlong.dasset", Overlong);
	Durin::FAssetPath OverlongPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/SoftMalformedOverlong", OverlongPath));
	EXPECT_EQ(Durin::Asset::LoadAsset(OverlongPath, Loaded).Error,
		Durin::Asset::EAssetError::CorruptFile);
	EXPECT_EQ(Loaded, nullptr);

	auto InvalidPath = ValidBytes;
	std::string InvalidPathString(TargetPath.GetView());
	ASSERT_GT(InvalidPathString.size(), 13u);
	InvalidPathString[12] = '/';
	std::memcpy(
		InvalidPath.data() + PayloadOffset + 1 + sizeof(Durin::uint64),
		InvalidPathString.data(), InvalidPathString.size());
	WriteTestBytes(Root / "SoftMalformedPath.dasset", InvalidPath);
	Durin::FAssetPath InvalidPackagePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/SoftMalformedPath", InvalidPackagePath));
	EXPECT_EQ(Durin::Asset::LoadAsset(InvalidPackagePath, Loaded).Error,
		Durin::Asset::EAssetError::InvalidPath);
	EXPECT_EQ(Loaded, nullptr);
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
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().BuildCookReachability(
		std::span<const Durin::FAssetPath>(&OwnerPath, 1), Reachable));
	EXPECT_EQ(Reachable, (std::vector<Durin::FAssetPath>{OwnerPath, TargetPath}));

	Owner->Direct.SetPath(MissingPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().BuildCookReachability(
		std::span<const Durin::FAssetPath>(&OwnerPath, 1), Reachable).Error,
		Durin::Asset::EAssetError::MissingDependency);

	DCodecSourceAsset* WrongType = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(WrongTypePath, WrongType));
	ASSERT_TRUE(Durin::Asset::SavePackage(WrongType->GetPackage()));
	Owner->Direct.SetPath(WrongTypePath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().BuildCookReachability(
		std::span<const Durin::FAssetPath>(&OwnerPath, 1), Reachable).Error,
		Durin::Asset::EAssetError::TypeMismatch);

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
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().BuildCookReachability(
		std::span<const Durin::FAssetPath>(&OwnerPath, 1), Reachable));
	EXPECT_EQ(Reachable, (std::vector<Durin::FAssetPath>{CyclePath, OwnerPath}));
}

TEST(FPackageAssetTests, CookCanonicalizesRedirectedRootsReferencesAndPublishedBytes)
{
	InitializeAssetTests();
	Durin::FAssetPath OwnerPath;
	Durin::FAssetPath OldTargetPath;
	Durin::FAssetPath FinalTargetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/CookCanonicalOwner", OwnerPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/CookCanonicalTargetOld", OldTargetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/CookCanonicalTargetFinal", FinalTargetPath));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldTargetPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	DSoftPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Target;
	Owner->Direct.SetPath(OldTargetPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));

	const Durin::Asset::FAssetData* OwnerData =
		Durin::Asset::GetAssetRegistry().FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	const std::string OwnerPhysicalPath = OwnerData->PhysicalPath;
	std::vector<Durin::uint8> AuthoredBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AuthoredBytes, OwnerPhysicalPath));
	ASSERT_TRUE(RelocateAssetForTest(OldTargetPath, FinalTargetPath));

	FMemoryAssetReferenceStore RuntimeRoot(
		OldTargetPath,
		true,
		DPackageAssetForTest::StaticClass()->GetQualifiedName().ToString());
	FScopedReferenceStoreRegistration RuntimeRootRegistration(&RuntimeRoot);
	std::vector<Durin::FAssetPath> Reachable;
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().BuildCookReachability(
		std::span{&OwnerPath, 1}, Reachable));
	std::vector ExpectedReachable{OwnerPath, FinalTargetPath};
	std::ranges::sort(ExpectedReachable, [](const Durin::FAssetPath& Left,
		const Durin::FAssetPath& Right) {
		return Left.GetView() < Right.GetView();
	});
	EXPECT_EQ(Reachable, ExpectedReachable);
	EXPECT_EQ(RuntimeRoot.Path, OldTargetPath);

	std::vector<Durin::uint8> CanonicalBytes;
	const Durin::Asset::FAssetResult CanonicalResult =
		Durin::Asset::CanonicalizeAssetPackageForCook(
			AuthoredBytes, CanonicalBytes);
	ASSERT_TRUE(CanonicalResult) << CanonicalResult.Message;
	EXPECT_NE(CanonicalBytes, AuthoredBytes);
	const std::filesystem::path CanonicalFile =
		Durin::Testing::GetTestWorkDirectory() / "CookCanonicalized.dasset";
	WriteTestBytes(CanonicalFile, CanonicalBytes);
	Durin::Asset::FAssetPackageInspection CanonicalInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		CanonicalFile.generic_string(), CanonicalInspection));
	EXPECT_EQ(
		CanonicalInspection.Header.Dependencies,
		(std::vector<Durin::FAssetPath>{FinalTargetPath}));
	std::vector<Durin::Asset::FAssetReferenceEdge> CanonicalReferences;
	ASSERT_TRUE(Durin::Asset::ExtractAssetReferences(
		OwnerPath, CanonicalInspection, CanonicalReferences));
	ASSERT_EQ(CanonicalReferences.size(), 2u);
	for (const Durin::Asset::FAssetReferenceEdge& Reference :
		CanonicalReferences)
		EXPECT_EQ(Reference.TargetPath, FinalTargetPath);

	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CanonicalCookOutput");
	Durin::Testing::RemoveTestWorkDirectory(CookRoot);
	Durin::Asset::FCookContext Cook(
		CookRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Cook.AddPackage(
		"/Game/CookCanonicalOwner", AuthoredBytes, {}));
	std::string CookError;
	ASSERT_TRUE(Cook.Publish(&CookError)) << CookError;
	Durin::Asset::FAssetPackageInspection PublishedInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		(CookRoot / "Game/CookCanonicalOwner.dasset").generic_string(),
		PublishedInspection));
	EXPECT_EQ(
		PublishedInspection.Header.Dependencies,
		(std::vector<Durin::FAssetPath>{FinalTargetPath}));

	std::vector<Durin::uint8> AuthoredAfterCook;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AuthoredAfterCook, OwnerPhysicalPath));
	EXPECT_EQ(AuthoredAfterCook, AuthoredBytes);

	const Durin::Asset::FAssetData* AliasData =
		Durin::Asset::GetAssetRegistry().FindAssetExact(OldTargetPath);
	ASSERT_NE(AliasData, nullptr);
	std::vector<Durin::uint8> AliasBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		AliasBytes, AliasData->PhysicalPath));

	const std::filesystem::path RedirectorRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "RedirectorCookOutput");
	Durin::Testing::RemoveTestWorkDirectory(RedirectorRoot);
	Durin::Asset::FCookContext RedirectorCook(
		RedirectorRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(RedirectorCook.AddPackage(
		"/Game/RedirectorMustNotCook", AliasBytes, {}));
	EXPECT_FALSE(RedirectorCook.Publish(&CookError));
	EXPECT_NE(CookError.find("redirector packages are authoring-only"),
		std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(
		RedirectorRoot / "CookManifest.bin"));
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
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindTargets(SourcePath),
		(std::vector<Durin::FAssetPath>{TargetPath}));

	Source->Direct.Reset();
	ASSERT_TRUE(Durin::Asset::SavePackage(Source->GetPackage()));
	EXPECT_TRUE(Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindTargets(SourcePath).empty());
	Source->Direct.SetPath(TargetPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Source->GetPackage()));
	ASSERT_TRUE(RelocateAssetForTest(SourcePath, MovedSourcePath));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindTargets(SourcePath),
		(std::vector<Durin::FAssetPath>{MovedSourcePath}));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindTargets(MovedSourcePath),
		(std::vector<Durin::FAssetPath>{TargetPath}));
	ASSERT_TRUE(DeleteAssetClosureForTest({SourcePath, MovedSourcePath}));
	EXPECT_TRUE(Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindTargets(MovedSourcePath).empty());
	EXPECT_TRUE(Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindReferencers(TargetPath).empty());
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
	const Durin::uint64 ConstructionsBeforeMove = GSoftPackageConstructionCount;

	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	EXPECT_EQ(GSoftPackageConstructionCount, ConstructionsBeforeMove);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(UnloadedOwnerPath), nullptr);
	EXPECT_EQ(LoadedOwner->Direct.GetSoftObjectPath().GetAssetPath(), OldPath);
	EXPECT_EQ(LoadedOwner->Fixed[0].GetSoftObjectPath().GetAssetPath(), OldPath);
	ASSERT_EQ(LoadedOwner->Array.size(), 1u);
	EXPECT_EQ(LoadedOwner->Array[0].GetSoftObjectPath().GetAssetPath(), OldPath);
	EXPECT_EQ(LoadedOwner->Map.at("loaded").GetSoftObjectPath().GetAssetPath(), OldPath);
	EXPECT_FALSE(LoadedOwner->Direct.IsLoaded());

	auto LoadedTargets = Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindTargets(LoadedOwnerPath);
	auto UnloadedTargets = Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindTargets(UnloadedOwnerPath);
	EXPECT_EQ(LoadedTargets, (std::vector<Durin::FAssetPath>{OldPath}));
	EXPECT_EQ(UnloadedTargets, (std::vector<Durin::FAssetPath>{OldPath}));
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent());
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindTargets(UnloadedOwnerPath),
		(std::vector<Durin::FAssetPath>{OldPath}));
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
			std::format("/TestAssets/SoftMove{}Target", Suffix), OldPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/SoftMove{}TargetRenamed", Suffix), NewPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/SoftMove{}Owner", Suffix), OwnerPath));
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
				Ec);
		else
			std::filesystem::last_write_time(
				OwnerFile,
				std::filesystem::last_write_time(OwnerFile)
					+ std::chrono::seconds(2),
				Ec);
		ASSERT_FALSE(Ec);
		const Durin::Asset::FAssetResult Result =
			RelocateAssetForTest(OldPath, NewPath);
		if (bMakeReadOnly)
		{
			std::filesystem::permissions(
				OwnerFile, OriginalPermissions,
				std::filesystem::perm_options::replace, Ec);
			ASSERT_FALSE(Ec);
		}
		EXPECT_TRUE(Result) << Result.Message;
		ASSERT_NE(Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath), nullptr);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath)->EntryKind,
			Durin::Asset::EAssetRegistryEntryKind::Redirector);
		EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAssetExact(NewPath), nullptr);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindTargets(OwnerPath),
			(std::vector<Durin::FAssetPath>{OldPath}));
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
	Durin::Asset::FAssetRelocationBatchToken Token;
	const Durin::Asset::FAssetRelocationMapping Mapping{OldPath, NewPath};
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetRelocationBatch(
		std::span{&Mapping, 1}, Token));
	Durin::Asset::SetAssetRelocationFailurePointForTesting(
		Durin::Asset::EAssetRelocationFailurePoint::PublishRedirector);
	const Durin::Asset::FAssetResult Result =
		Durin::Asset::ApplyAssetRelocationBatch(Token);
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::IoError);
	EXPECT_EQ(ExternalSetting.GetSoftObjectPath().GetAssetPath(), OldPath);
	ASSERT_NE(Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(NewPath), nullptr);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FAssetPath>{OldPath}));
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
	EXPECT_EQ(RelocateAssetForTest(OldPath, CollisionPath).Error,
		Durin::Asset::EAssetError::AlreadyExists);

	Durin::TSoftObjectPtr<DPackageAssetForTest> ExternalSetting(OldPath);
	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	EXPECT_EQ(ExternalSetting.GetSoftObjectPath().GetAssetPath(), OldPath);
	ASSERT_NE(Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAssetExact(NewPath), nullptr);
}

TEST(FPackageAssetTests, RelocationRejectsReadOnlySourceWithoutStagingMutation)
{
	InitializeAssetTests();
	Durin::FAssetPath SourcePath;
	Durin::FAssetPath DestinationPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/ReadOnlyRelocationSource", SourcePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/ReadOnlyRelocationDestination", DestinationPath));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const std::filesystem::path SourceFile =
		Durin::Asset::GetAssetRegistry().FindAssetExact(SourcePath)->PhysicalPath;
	const std::filesystem::perms Original =
		std::filesystem::status(SourceFile).permissions();
	std::error_code ErrorCode;
	std::filesystem::permissions(
		SourceFile,
		std::filesystem::perms::owner_read
			| std::filesystem::perms::group_read
			| std::filesystem::perms::others_read,
		std::filesystem::perm_options::replace,
		ErrorCode);
	ASSERT_FALSE(ErrorCode);
	const Durin::Asset::FAssetResult Result =
		RelocateAssetForTest(SourcePath, DestinationPath);
	std::filesystem::permissions(
		SourceFile, Original,
		std::filesystem::perm_options::replace, ErrorCode);
	ASSERT_FALSE(ErrorCode);
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::ReadOnlyMode);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(SourcePath)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(DestinationPath), nullptr);
}

TEST(FPackageAssetTests, RelocationFailureSeamsPreserveEveryOrdinaryBoundary)
{
	InitializeAssetTests();
	const std::array Points = {
		Durin::Asset::EAssetRelocationFailurePoint::StageOriginal,
		Durin::Asset::EAssetRelocationFailurePoint::PublishRealAsset,
		Durin::Asset::EAssetRelocationFailurePoint::PublishRedirector,
		Durin::Asset::EAssetRelocationFailurePoint::UpdateLoadedPackage,
		Durin::Asset::EAssetRelocationFailurePoint::PublishRegistry};
	for (size_t Index = 0; Index < Points.size(); ++Index)
	{
		Durin::FAssetPath SourcePath;
		Durin::FAssetPath DestinationPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(std::format(
			"/TestAssets/FailureBoundary{}Source", Index), SourcePath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(std::format(
			"/TestAssets/FailureBoundary{}Destination", Index), DestinationPath));
		DPackageAssetForTest* Asset = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Asset));
		ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
		const Durin::Asset::FAssetRelocationMapping Mapping{
			SourcePath, DestinationPath};
		Durin::Asset::FAssetRelocationBatchToken Token;
		ASSERT_TRUE(Durin::Asset::AnalyzeAssetRelocationBatch(
			std::span{&Mapping, 1}, Token));
		Durin::Asset::SetAssetRelocationFailurePointForTesting(Points[Index]);
		EXPECT_EQ(Durin::Asset::ApplyAssetRelocationBatch(Token).Error,
			Durin::Asset::EAssetError::IoError);
		ASSERT_NE(Durin::Asset::GetAssetRegistry().FindAssetExact(SourcePath), nullptr);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(SourcePath)->EntryKind,
			Durin::Asset::EAssetRegistryEntryKind::Asset);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(DestinationPath), nullptr);
	}

	Durin::FAssetPath PrepareSource;
	Durin::FAssetPath PrepareDestination;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/FailurePrepareSource", PrepareSource));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/FailurePrepareDestination", PrepareDestination));
	DPackageAssetForTest* PrepareAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(PrepareSource, PrepareAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(PrepareAsset->GetPackage()));
	const Durin::Asset::FAssetRelocationMapping PrepareMapping{
		PrepareSource, PrepareDestination};
	Durin::Asset::SetAssetRelocationFailurePointForTesting(
		Durin::Asset::EAssetRelocationFailurePoint::PrepareOutput);
	Durin::Asset::FAssetRelocationBatchToken PrepareToken;
	EXPECT_EQ(Durin::Asset::AnalyzeAssetRelocationBatch(
		std::span{&PrepareMapping, 1}, PrepareToken).Error,
		Durin::Asset::EAssetError::IoError);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(PrepareSource)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(PrepareDestination), nullptr);
}

TEST(FPackageAssetTests, CompensationFailureRetainsDiagnosableRecoveryRoot)
{
	InitializeAssetTests();
	Durin::FAssetPath SourcePath;
	Durin::FAssetPath DestinationPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RecoverySource", SourcePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RecoveryDestination", DestinationPath));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SourcePath, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const Durin::Asset::FAssetRelocationMapping Mapping{
		SourcePath, DestinationPath};
	Durin::Asset::FAssetRelocationBatchToken Token;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetRelocationBatch(
		std::span{&Mapping, 1}, Token));
	Durin::Asset::SetAssetRelocationFailurePointForTesting(
		Durin::Asset::EAssetRelocationFailurePoint::PublishRedirector);
	Durin::Asset::SetAssetRelocationFailurePointForTesting(
		Durin::Asset::EAssetRelocationFailurePoint::CompensateFile);
	const Durin::Asset::FAssetResult Result =
		Durin::Asset::ApplyAssetRelocationBatch(Token);
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::IoError);
	EXPECT_NE(Result.Message.find("AssetMutationRecoveryRequired"),
		std::string::npos);
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
	std::vector<Durin::uint8> JournalBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		JournalBytes, (OperationRoot / "journal").generic_string()));
	const std::string Journal(
		reinterpret_cast<const char*>(JournalBytes.data()), JournalBytes.size());
	EXPECT_NE(Journal.find("type=relocation"), std::string::npos);
	EXPECT_NE(Journal.find("original="), std::string::npos);
	EXPECT_NE(Journal.find("staged_pre="), std::string::npos);
	EXPECT_NE(Journal.find("pre_fingerprint="), std::string::npos);
	EXPECT_NE(Journal.find("completed=true"), std::string::npos);
	EXPECT_NE(Journal.find("compensated=false"), std::string::npos);

	const std::string RecoveryBase = Durin::FPaths::ProjectDir().empty()
		? Durin::FPaths::LaunchDir() : Durin::FPaths::ProjectDir();
	const std::filesystem::path LocatorRoot =
		std::filesystem::path(RecoveryBase)
		/ "Saved" / "AssetMutationRecovery";
	const std::filesystem::path Locator =
		LocatorRoot / OperationRoot.filename();
	std::vector<Durin::uint8> LocatorBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		LocatorBytes, Locator.generic_string()));
	const std::string LocatorText(
		reinterpret_cast<const char*>(LocatorBytes.data()), LocatorBytes.size());
	EXPECT_NE(LocatorText.find(OperationRoot.generic_string()), std::string::npos);
	Token = {};
	std::error_code ErrorCode;
	std::filesystem::remove(
		ContentRoot / "RecoveryDestination.dasset", ErrorCode);
	Durin::Testing::RemoveTestWorkDirectory(RecoveryRoot, ErrorCode);
	std::filesystem::remove(Locator, ErrorCode);
	std::filesystem::remove(LocatorRoot, ErrorCode);
	Durin::Asset::SetAssetRelocationFailurePointForTesting(
		Durin::Asset::EAssetRelocationFailurePoint::None);
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
	const auto* OwnerData = Durin::Asset::GetAssetRegistry().FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	Durin::Asset::FAssetPackageInspection BeforeInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		OwnerData->PhysicalPath, BeforeInspection));
	const auto* BeforeLabel = BeforeInspection.FindField("Label");
	ASSERT_NE(BeforeLabel, nullptr);
	const std::vector<Durin::uint8> UnrelatedBytes = BeforeLabel->Payload;
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));

	const auto Incoming = Durin::Asset::GetAssetRegistry().GetReferenceIndex()
		.FindReferencers(OldPath);
	ASSERT_EQ(Incoming.size(), 2u);
	EXPECT_TRUE(std::ranges::any_of(Incoming, [](const auto& Edge) {
		return Edge.Kind == Durin::Asset::EAssetReferenceKind::HardObject;
	}));
	EXPECT_TRUE(std::ranges::any_of(Incoming, [](const auto& Edge) {
		return Edge.Kind == Durin::Asset::EAssetReferenceKind::SoftObject;
	}));

	FMemoryAssetReferenceStore Store(OldPath);
	FScopedReferenceStoreRegistration StoreRegistration(&Store);
	Durin::Asset::FAssetRedirectorFixupPlan Plan;
	ASSERT_TRUE(Durin::Asset::AnalyzeRedirectorFixup(
		std::span{&OldPath, 1},
		Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
		Plan));
	ASSERT_EQ(Plan.GetRedirectors().size(), 1u);
	EXPECT_EQ(Plan.GetRedirectors().front(), OldPath);
	EXPECT_EQ(Plan.GetPackageOccurrences().size(), 2u);
	EXPECT_EQ(Plan.GetStoreOccurrences().size(), 1u);
	EXPECT_EQ(Plan.GetDeletableRedirectors().size(), 1u);
	const Durin::uint64 ConstructionCount = GSoftPackageConstructionCount;
	ASSERT_TRUE(Durin::Asset::ApplyRedirectorFixup(Plan));
	EXPECT_EQ(GSoftPackageConstructionCount, ConstructionCount);
	EXPECT_EQ(Store.Path, NewPath);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath), nullptr);
	EXPECT_TRUE(Durin::Asset::GetAssetRegistry().GetReferenceIndex()
		.FindReferencers(OldPath).empty());
	EXPECT_TRUE(Durin::Asset::GetAssetRegistry().GetReferenceIndex().IsComplete());

	OwnerData = Durin::Asset::GetAssetRegistry().FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	Durin::Asset::FAssetPackageInspection AfterInspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		OwnerData->PhysicalPath, AfterInspection));
	const auto* AfterLabel = AfterInspection.FindField("Label");
	ASSERT_NE(AfterLabel, nullptr);
	EXPECT_EQ(AfterLabel->Payload, UnrelatedBytes);
	const auto NewIncoming = Durin::Asset::GetAssetRegistry().GetReferenceIndex()
		.FindReferencers(NewPath);
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
	const std::string OwnerFile = Durin::Asset::GetAssetRegistry()
		.FindAssetExact(OwnerPath)->PhysicalPath;
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	std::vector<Durin::uint8> BeforeBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeBytes, OwnerFile));

	FMemoryAssetReferenceStore Store(OldPath);
	FScopedReferenceStoreRegistration StoreRegistration(&Store);
	Durin::Asset::SetAssetRedirectorFixupFailurePointForTesting(
		Durin::Asset::EAssetRedirectorFixupFailurePoint::Verify);
	const Durin::Asset::FAssetResult FixupResult =
		Durin::Asset::FixUpRedirectors(std::span{&OldPath, 1});
	EXPECT_EQ(FixupResult.Error, Durin::Asset::EAssetError::IoError)
		<< FixupResult.Message;
	Durin::Asset::SetAssetRedirectorFixupFailurePointForTesting(
		Durin::Asset::EAssetRedirectorFixupFailurePoint::None);
	std::vector<Durin::uint8> AfterBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterBytes, OwnerFile));
	EXPECT_EQ(AfterBytes, BeforeBytes);
	EXPECT_EQ(Store.Path, OldPath);
	const auto* Alias = Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath);
	ASSERT_NE(Alias, nullptr);
	EXPECT_EQ(Alias->EntryKind, Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetReferenceIndex()
		.FindReferencers(OldPath).size(), 2u);
	ASSERT_TRUE(Durin::Asset::DeleteAsset(OwnerPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
}
}

namespace
{
auto RunRedirectorFixupRejectsUnavailableProviderWithoutMutationTest() -> void
{
	InitializeAssetTests();
	Durin::FAssetPath OldPath;
	Durin::FAssetPath NewPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/FixupProviderGoneOld", OldPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/FixupProviderGoneNew", NewPath));
	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	FMemoryAssetReferenceStore Store(OldPath);
	const Durin::Asset::FAssetReferenceStoreHandle Handle =
		Durin::Asset::RegisterAssetReferenceStore(&Store);
	Durin::Asset::FAssetRedirectorFixupPlan Plan;
	ASSERT_TRUE(Durin::Asset::AnalyzeRedirectorFixup(
		std::span{&OldPath, 1},
		Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
		Plan));
	Durin::Asset::UnregisterAssetReferenceStore(Handle);
	const Durin::Asset::FAssetResult Result =
		Durin::Asset::ApplyRedirectorFixup(Plan);
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::StaleData);
	EXPECT_EQ(Store.Path, OldPath);
	const auto* Alias = Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath);
	ASSERT_NE(Alias, nullptr);
	EXPECT_EQ(Alias->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Redirector);
	ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
}

auto RunRedirectorFixupRejectsDirtyAndCompatibilityRiskPackagesTest() -> void
{
	InitializeAssetTests();
	auto RunCase = [](std::string_view Suffix, bool bCompatibilityRisk) {
		Durin::FAssetPath OldPath;
		Durin::FAssetPath NewPath;
		Durin::FAssetPath OwnerPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/Fixup{}Old", Suffix), OldPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/Fixup{}New", Suffix), NewPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/Fixup{}Owner", Suffix), OwnerPath));
		DPackageAssetForTest* Target = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
		ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
		DSoftPackageAssetForTest* Owner = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
		Owner->Direct.SetPath(OldPath);
		ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
		if (bCompatibilityRisk)
		{
			const std::string OwnerFile = Durin::Asset::GetAssetRegistry()
				.FindAssetExact(OwnerPath)->PhysicalPath;
			ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
			std::vector<Durin::uint8> Bytes;
			ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, OwnerFile));
			ASSERT_TRUE(RenameSerializedString(Bytes, "Label", "Ghost"));
			WriteTestBytes(OwnerFile, Bytes);
			ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
				Durin::Asset::EAssetRegistryScanMode::FullValidation));
			Durin::Asset::FAssetLoadReport Report;
			ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, Owner, &Report));
			ASSERT_TRUE(Report.HasRiskItems());
		}
		else
		{
			Owner->MarkPackageDirty();
		}
		ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
		ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
		Durin::Asset::FAssetRedirectorFixupPlan Plan;
		const Durin::Asset::FAssetResult Result =
			Durin::Asset::AnalyzeRedirectorFixup(
				std::span{&OldPath, 1},
				Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
				Plan);
		EXPECT_EQ(Result.Error, bCompatibilityRisk
			? Durin::Asset::EAssetError::UnsupportedProperty
			: Durin::Asset::EAssetError::InUse) << Result.Message;
		const auto* Alias = Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath);
		ASSERT_NE(Alias, nullptr);
		EXPECT_EQ(Alias->EntryKind,
			Durin::Asset::EAssetRegistryEntryKind::Redirector);
		Owner->GetPackage()->ClearDirty();
		ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
		ASSERT_TRUE(Durin::Asset::DeleteAsset(OwnerPath));
		ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
	};
	RunCase("Dirty", false);
	RunCase("Risk", true);
}

auto RunRedirectorFixupRejectsReadOnlyAndChangedPackageInputsTest() -> void
{
	InitializeAssetTests();
	auto RunCase = [](std::string_view Suffix, bool bReadOnly) {
		Durin::FAssetPath OldPath;
		Durin::FAssetPath NewPath;
		Durin::FAssetPath OwnerPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/Fixup{}Old", Suffix), OldPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/Fixup{}New", Suffix), NewPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/Fixup{}Owner", Suffix), OwnerPath));
		DPackageAssetForTest* Target = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
		ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
		DSoftPackageAssetForTest* Owner = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
		Owner->Direct.SetPath(OldPath);
		ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
		const std::string OwnerFile = Durin::Asset::GetAssetRegistry()
			.FindAssetExact(OwnerPath)->PhysicalPath;
		ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
		ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
		ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
		std::vector<Durin::uint8> PreBytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(PreBytes, OwnerFile));
		std::filesystem::perms OriginalPermissions =
			std::filesystem::status(OwnerFile).permissions();
		Durin::Asset::FAssetRedirectorFixupPlan Plan;
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
				PermissionError);
			ASSERT_FALSE(PermissionError) << PermissionError.message();
			Result = Durin::Asset::AnalyzeRedirectorFixup(
				std::span{&OldPath, 1},
				Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
				Plan);
			std::filesystem::permissions(
				OwnerFile, OriginalPermissions,
				std::filesystem::perm_options::replace,
				PermissionError);
			ASSERT_FALSE(PermissionError) << PermissionError.message();
			EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::ReadOnlyMode)
				<< Result.Message;
		}
		else
		{
			ASSERT_TRUE(Durin::Asset::AnalyzeRedirectorFixup(
				std::span{&OldPath, 1},
				Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
				Plan));
			std::vector<Durin::uint8> ChangedBytes = PreBytes;
			ASSERT_TRUE(RenameSerializedString(
				ChangedBytes, "Label", "Ghost"));
			WriteTestBytes(OwnerFile, ChangedBytes);
			Result = Durin::Asset::ApplyRedirectorFixup(Plan);
			EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::StaleData)
				<< Result.Message;
			WriteTestBytes(OwnerFile, PreBytes);
		}
		const auto* Alias = Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath);
		ASSERT_NE(Alias, nullptr);
		EXPECT_EQ(Alias->EntryKind,
			Durin::Asset::EAssetRegistryEntryKind::Redirector);
		ASSERT_TRUE(Durin::Asset::DeleteAsset(OwnerPath));
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
		Durin::Asset::EAssetRedirectorFixupFailurePoint::PublishRegistry};
	for (size_t Index = 0; Index < FailurePoints.size(); ++Index)
	{
		Durin::FAssetPath OldPath;
		Durin::FAssetPath NewPath;
		Durin::FAssetPath OwnerPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/FixupMatrix{}Old", Index), OldPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/FixupMatrix{}New", Index), NewPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			std::format("/TestAssets/FixupMatrix{}Owner", Index), OwnerPath));
		DPackageAssetForTest* Target = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OldPath, Target));
		ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
		DSoftPackageAssetForTest* Owner = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
		Owner->Direct.SetPath(OldPath);
		ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
		const std::string OwnerFile = Durin::Asset::GetAssetRegistry()
			.FindAssetExact(OwnerPath)->PhysicalPath;
		ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
		std::vector<Durin::uint8> BeforeBytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeBytes, OwnerFile));
		ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
		ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
		FMemoryAssetReferenceStore Store(OldPath);
		FScopedReferenceStoreRegistration StoreRegistration(&Store);
		Durin::Asset::SetAssetRedirectorFixupFailurePointForTesting(
			FailurePoints[Index]);
		const Durin::Asset::FAssetResult Result =
			Durin::Asset::FixUpRedirectors(std::span{&OldPath, 1});
		Durin::Asset::SetAssetRedirectorFixupFailurePointForTesting(
			Durin::Asset::EAssetRedirectorFixupFailurePoint::None);
		EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::IoError)
			<< Result.Message;
		std::vector<Durin::uint8> AfterBytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterBytes, OwnerFile));
		EXPECT_EQ(AfterBytes, BeforeBytes);
		EXPECT_EQ(Store.Path, OldPath);
		const auto* Alias = Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath);
		ASSERT_NE(Alias, nullptr);
		EXPECT_EQ(Alias->EntryKind,
			Durin::Asset::EAssetRegistryEntryKind::Redirector);
		ASSERT_TRUE(Durin::Asset::DeleteAsset(OwnerPath));
		ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
	}
}
}

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
	ASSERT_TRUE(Durin::Asset::DeleteAsset(TargetPath));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(TargetPath), nullptr);
	auto Referencers =
		Durin::Asset::GetAssetRegistry().GetReferenceIndex().FindReferencers(TargetPath);
	ASSERT_EQ(Referencers.size(), 1u);
	EXPECT_EQ(Referencers.front().SourcePackage, OwnerPath);

	std::vector<Durin::FAssetPath> Reachable;
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().BuildCookReachability(
		std::span<const Durin::FAssetPath>(&OwnerPath, 1), Reachable).Error,
		Durin::Asset::EAssetError::MissingDependency);
}

TEST(FPackageAssetTests, DastV2MapBytesAreCanonicalAcrossInsertionAndBucketHistory)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/MapOrderingBaseline", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	const std::array<std::pair<std::string, Durin::int32>, 8> Entries = {{
		{"alpha", 1}, {"bravo", 2}, {"charlie", 3}, {"delta", 4},
		{"echo", 5}, {"foxtrot", 6}, {"golf", 7}, {"hotel", 8}}};

	Asset->NamedScores.clear();
	Asset->NamedScores.rehash(37);
	for (const auto& [Key, Value] : Entries) Asset->NamedScores.emplace(Key, Value);
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	const auto File = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "MapOrderingBaseline.dasset";
	std::vector<Durin::uint8> ForwardBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(ForwardBytes, File.generic_string()));

	Asset->NamedScores.clear();
	Asset->NamedScores.rehash(2);
	for (auto It = Entries.rbegin(); It != Entries.rend(); ++It)
		Asset->NamedScores.emplace(It->first, It->second);
	Asset->GetPackage()->MarkDirty();
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	std::vector<Durin::uint8> ReverseBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(ReverseBytes, File.generic_string()));

	EXPECT_EQ(ForwardBytes, ReverseBytes);
	EXPECT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(Path));
}

TEST(FPackageAssetTests, DastV2ReadsHistoricalMapOrderAndRejectsDuplicateDecodedKeys)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/MapDecodeContract", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->NamedScores = {{"alpha", 1}, {"bravo", 2}};
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "MapDecodeContract.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	auto FindSerializedString = [&](std::string_view Value) {
		std::vector<Durin::uint8> Pattern(sizeof(Durin::uint64) + Value.size());
		const Durin::uint64 Length = Value.size();
		std::memcpy(Pattern.data(), &Length, sizeof(Length));
		std::memcpy(Pattern.data() + sizeof(Length), Value.data(), Value.size());
		return std::search(Bytes.begin(), Bytes.end(), Pattern.begin(), Pattern.end());
	};
	const auto Alpha = FindSerializedString("alpha");
	const auto Bravo = FindSerializedString("bravo");
	ASSERT_NE(Alpha, Bytes.end());
	ASSERT_NE(Bravo, Bytes.end());
	constexpr size_t EntrySize = sizeof(Durin::uint64) + 5 + sizeof(Durin::int32);
	ASSERT_LE(static_cast<size_t>(std::distance(Bytes.begin(), Alpha)) + EntrySize, Bytes.size());
	ASSERT_LE(static_cast<size_t>(std::distance(Bytes.begin(), Bravo)) + EntrySize, Bytes.size());
	for (size_t Index = 0; Index < EntrySize; ++Index) std::iter_swap(Alpha + Index, Bravo + Index);
	WriteTestBytes(File, Bytes);

	DPackageAssetForTest* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->NamedScores.at("alpha"), 1);
	EXPECT_EQ(Loaded->NamedScores.at("bravo"), 2);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	ASSERT_TRUE(RenameSerializedString(Bytes, "bravo", "alpha"));
	WriteTestBytes(File, Bytes);
	Durin::DObject* DuplicateLoad = nullptr;
	const Durin::Asset::FAssetResult DuplicateResult = Durin::Asset::LoadAsset(Path, DuplicateLoad);
	EXPECT_FALSE(DuplicateResult);
	EXPECT_EQ(DuplicateResult.Error, Durin::Asset::EAssetError::CorruptFile);
	EXPECT_NE(DuplicateResult.Message.find("duplicate decoded key"), std::string::npos);
	EXPECT_EQ(DuplicateLoad, nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
}

TEST(FPackageAssetTests, MathStructRegistrationPreservesDirectAndNestedSchemaIdentity)
{
	InitializeAssetTests();
	Durin::DClass* Class = DMathStructAssetForTest::StaticClass();
	Durin::DStruct* VectorStruct = Durin::Z_Construct_DStruct_Durin_FVector3();
	Durin::DStruct* TransformStruct = Durin::Z_Construct_DStruct_Durin_FTransform();
	ASSERT_NE(Class, nullptr);
	ASSERT_NE(VectorStruct, nullptr);
	ASSERT_NE(TransformStruct, nullptr);
	auto* Vector = static_cast<Durin::FStructProperty*>(Class->FindPropertyByName("Vector", false));
	auto* Transform = static_cast<Durin::FStructProperty*>(Class->FindPropertyByName("Transform", false));
	auto* Vectors = static_cast<Durin::FArrayProperty*>(Class->FindPropertyByName("Vectors", false));
	auto* VectorMap = static_cast<Durin::FMapProperty*>(Class->FindPropertyByName("VectorMap", false));
	ASSERT_NE(Vector, nullptr);
	ASSERT_NE(Transform, nullptr);
	ASSERT_NE(Vectors, nullptr);
	ASSERT_NE(VectorMap, nullptr);
	ASSERT_EQ(Class->ChildProperties, Vector);
	EXPECT_EQ(Vector->Next, Transform);
	EXPECT_EQ(Transform->Next, Vectors);
	EXPECT_EQ(Vectors->Next, VectorMap);
	EXPECT_EQ(VectorMap->Next, nullptr);

	EXPECT_EQ(Vector->GetStruct(), VectorStruct);
	EXPECT_EQ(Transform->GetStruct(), TransformStruct);
	EXPECT_EQ(Vector->GetPropertyFlags(), Durin::EPropertyFlags::None);
	EXPECT_EQ(Transform->GetPropertyFlags(), Durin::EPropertyFlags::None);
	EXPECT_EQ(Vector->GetArrayDim(), 1);
	EXPECT_EQ(Transform->GetArrayDim(), 1);
	EXPECT_EQ(Vector->GetOffset(), static_cast<Durin::uint16>(offsetof(DMathStructAssetForTest, Vector)));
	EXPECT_EQ(Transform->GetOffset(), static_cast<Durin::uint16>(offsetof(DMathStructAssetForTest, Transform)));
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
}

TEST(FPackageAssetTests, PreservesMathStructBitsAcrossDastV2AndObjectGraphs)
{
	InitializeAssetTests();
	const double PayloadNaN = std::bit_cast<double>(Durin::uint64{0x7ff8000000000042ull});
	const double PositiveInfinity = std::numeric_limits<double>::infinity();
	const double NegativeInfinity = -std::numeric_limits<double>::infinity();
	auto ExpectVectorBits = [](const Durin::FVector3& Actual, const Durin::FVector3& Expected) {
		EXPECT_EQ(std::bit_cast<Durin::uint64>(Actual.x), std::bit_cast<Durin::uint64>(Expected.x));
		EXPECT_EQ(std::bit_cast<Durin::uint64>(Actual.y), std::bit_cast<Durin::uint64>(Expected.y));
		EXPECT_EQ(std::bit_cast<Durin::uint64>(Actual.z), std::bit_cast<Durin::uint64>(Expected.z));
	};
	auto Populate = [&](DMathStructAssetForTest& Asset) {
		Asset.Vector = Durin::FVector3(0.0, -0.0, PositiveInfinity);
		Asset.Transform.Translation = Durin::FVector3(PayloadNaN, NegativeInfinity, -0.0);
		Asset.Transform.Scale3D = Durin::FVector3(1.0, 0.0, PositiveInfinity);
		Asset.Vectors = {
			Durin::FVector3(0.0),
			Durin::FVector3(PayloadNaN, -0.0, NegativeInfinity)};
		Asset.VectorMap = {
			{"zero", Durin::FVector3(0.0, -0.0, 0.0)},
			{"special", Durin::FVector3(PositiveInfinity, PayloadNaN, NegativeInfinity)}};
	};
	auto ExpectValues = [&](const DMathStructAssetForTest& Asset) {
		ExpectVectorBits(Asset.Vector, Durin::FVector3(0.0, -0.0, PositiveInfinity));
		ExpectVectorBits(
			Asset.Transform.Translation,
			Durin::FVector3(PayloadNaN, NegativeInfinity, -0.0));
		ExpectVectorBits(
			Asset.Transform.Scale3D,
			Durin::FVector3(1.0, 0.0, PositiveInfinity));
		ASSERT_EQ(Asset.Vectors.size(), 2u);
		ExpectVectorBits(Asset.Vectors[0], Durin::FVector3(0.0));
		ExpectVectorBits(
			Asset.Vectors[1], Durin::FVector3(PayloadNaN, -0.0, NegativeInfinity));
		ASSERT_EQ(Asset.VectorMap.size(), 2u);
		ExpectVectorBits(Asset.VectorMap.at("zero"), Durin::FVector3(0.0, -0.0, 0.0));
		ExpectVectorBits(
			Asset.VectorMap.at("special"),
			Durin::FVector3(PositiveInfinity, PayloadNaN, NegativeInfinity));
	};

	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/MathStructBits", Path));
	DMathStructAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Populate(*Asset);
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const auto MathFile = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "MathStructBits.dasset";
	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(MathFile.generic_string(), Inspection));
	ASSERT_FALSE(Inspection.Objects.empty());
	const Durin::Asset::FAssetPackageField* VectorField =
		Inspection.Objects.front().FindField("Vector");
	ASSERT_NE(VectorField, nullptr);
	Durin::FVector3 InspectedVector(9.0);
	ASSERT_TRUE(VectorField->TryReadStruct(
		Durin::Z_Construct_DStruct_Durin_FVector3(), &InspectedVector));
	ExpectVectorBits(InspectedVector, Durin::FVector3(0.0, -0.0, PositiveInfinity));
	DMathStructAssetForTest* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded));
	ASSERT_NE(Loaded, nullptr);
	ExpectValues(*Loaded);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	DMathStructAssetForTest* GraphSource =
		Durin::NewObject<DMathStructAssetForTest>(nullptr, "MathStructGraphSource");
	Populate(*GraphSource);
	std::vector<Durin::uint8> GraphBytes;
	ASSERT_TRUE(Durin::SaveObjectGraphToMemory(GraphSource, GraphBytes));
	auto* GraphLoaded = Durin::Cast<DMathStructAssetForTest>(
		Durin::LoadObjectGraphFromMemory(GraphBytes));
	ASSERT_NE(GraphLoaded, nullptr);
	ExpectValues(*GraphLoaded);
	GraphBytes.pop_back();
	EXPECT_EQ(Durin::LoadObjectGraphFromMemory(GraphBytes), nullptr);
}

TEST(FPackageAssetTests, RequiresExplicitCodecForIncompleteAuthoredStructs)
{
	InitializeAssetTests();
	Durin::FAssetPath SavePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/CodecSaveDenied", SavePath));
	DCodecTargetAsset* SaveTarget = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SavePath, SaveTarget));
	SaveTarget->Value.Value = 17;
	const Durin::Asset::FAssetResult SaveResult =
		Durin::Asset::SavePackage(SaveTarget->GetPackage());
	EXPECT_EQ(SaveResult.Error, Durin::Asset::EAssetError::UnsupportedProperty);
	EXPECT_NE(SaveResult.Message.find("CustomStructCodecRequired"), std::string::npos);
	ASSERT_TRUE(Durin::Asset::DiscardUnpublishedPackage(SaveTarget->GetPackage()));

	Durin::FAssetPath LoadPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/CodecLoadDenied", LoadPath));
	DCodecSourceAsset* Source = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(LoadPath, Source));
	Source->Value.Value = 31;
	ASSERT_TRUE(Durin::Asset::SavePackage(Source->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(LoadPath));

	const auto File = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "CodecLoadDenied.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_EQ(RenameAllSerializedStrings(
		Bytes, "Tests::DCodecSourceAsset", "Tests::DCodecTargetAsset"), 3u);
	ASSERT_TRUE(RenameSerializedString(
		Bytes, "Struct<Tests::FCodecSource>", "Struct<Tests::FCodecTarget>"));
	ASSERT_EQ(RenameAllSerializedStrings(
		Bytes, "Tests::FCodecSource", "Tests::FCodecTarget"), 2u);
	WriteTestBytes(File, Bytes);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent());

	DCodecTargetAsset* LoadTarget = nullptr;
	const Durin::Asset::FAssetResult LoadResult =
		Durin::Asset::LoadAsset(LoadPath, LoadTarget);
	EXPECT_EQ(LoadResult.Error, Durin::Asset::EAssetError::UnsupportedProperty);
	EXPECT_NE(LoadResult.Message.find("CustomStructCodecRequired"), std::string::npos);
	EXPECT_EQ(LoadTarget, nullptr);
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

TEST(FPackageAssetTests, LoadsAbiSizedLogicalSignaturesWithoutSchemaMigration)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LegacyLogicalSignatures", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Label = "LegacyString";
	Asset->DisplayName = Durin::FName("LegacyName");
	Asset->PersistentId = Durin::FGuid(1, 2, 3, 4);
	Asset->RelatedIds = {Durin::FGuid(5, 6, 7, 8)};
	Asset->NamedScores = {{"Legacy", 42}};
	Asset->SourcePath.Path = "/TestAssets/Sources/Legacy.txt";
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	const auto File = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "LegacyLogicalSignatures.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	EXPECT_EQ(RenameAllSerializedStrings(Bytes, "12:v1", "12:32"), 2u);
	EXPECT_EQ(RenameAllSerializedStrings(Bytes, "18:v1", "18:12"), 1u);
	EXPECT_EQ(RenameAllSerializedStrings(Bytes, "19:v1", "19:16"), 1u);
	EXPECT_EQ(RenameAllSerializedStrings(Bytes, "Array<19:v1>", "Array<19:16>"), 1u);
	EXPECT_EQ(RenameAllSerializedStrings(Bytes, "Map<12:v1,4:4>", "Map<12:32,4:4>"), 1u);
	WriteTestBytes(File, Bytes);

	Durin::Asset::FAssetLoadReport Report;
	DPackageAssetForTest* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded, &Report));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_FALSE(Report.HasCompatibilityIssues());
	EXPECT_EQ(Loaded->Label, "LegacyString");
	EXPECT_EQ(Loaded->DisplayName, Durin::FName("LegacyName"));
	EXPECT_EQ(Loaded->PersistentId, Durin::FGuid(1, 2, 3, 4));
	EXPECT_EQ(Loaded->RelatedIds, (std::vector<Durin::FGuid>{Durin::FGuid(5, 6, 7, 8)}));
	EXPECT_EQ(Loaded->NamedScores.at("Legacy"), 42);
	EXPECT_EQ(Loaded->SourcePath.Path, "/TestAssets/Sources/Legacy.txt");
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, KeepsRawScalarWidthInSerializedSchema)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RawScalarWidth", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Value = 42;
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "RawScalarWidth.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_TRUE(RenameSerializedString(Bytes, "4:4", "4:8"));
	WriteTestBytes(File, Bytes);

	Durin::Asset::FAssetLoadReport Report;
	DPackageAssetForTest* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded, &Report));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->Value, 0);
	EXPECT_TRUE(Report.HasRiskItems());
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
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

TEST(FPackageAssetTests, CompatibilityProbeClassifiesCurrentFieldsAndSkipsPayloadBytes)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/CompatibilityCurrent", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Scores.resize(1024 * 1024, 7);
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "CompatibilityCurrent.dasset";
	const auto Catalog = Durin::Asset::FReflectionCompatibilityCatalog::Capture();
	const auto RepeatedCatalog = Durin::Asset::FReflectionCompatibilityCatalog::Capture();
	EXPECT_TRUE(std::ranges::equal(Catalog.GetClasses(), RepeatedCatalog.GetClasses()));
	const auto First = Durin::Asset::ProbeAssetPackageCompatibility(MakeCompatibilityProbeInput(Path, File), Catalog);
	const auto Second = Durin::Asset::ProbeAssetPackageCompatibility(MakeCompatibilityProbeInput(Path, File), Catalog);
	ASSERT_EQ(First.Status, Durin::Asset::EAssetCompatibilityProbeStatus::Completed);
	ASSERT_TRUE(First.Record.has_value());
	EXPECT_EQ(First.Record->Inspection, Durin::Asset::EAssetCompatibilityInspection::Ready);
	EXPECT_EQ(First.Record->Compatibility, Durin::Asset::EAssetPackageCompatibility::Compatible);
	EXPECT_EQ(First.Record->Freshness, Durin::Asset::EAssetCompatibilityFreshness::Current);
	EXPECT_TRUE(First.Record->Findings.empty());
	EXPECT_GT(First.Stats.PayloadBytesSkipped, 4u * 1024u * 1024u);
	EXPECT_LT(First.Stats.MetadataBytesRead, 64u * 1024u);
	EXPECT_LT(First.Stats.PeakMetadataBytes, 64u * 1024u);
	ASSERT_TRUE(Second.Record.has_value());
	EXPECT_EQ(Durin::Asset::SerializeAssetCompatibilityReportV1({&*First.Record, 1}),
		Durin::Asset::SerializeAssetCompatibilityReportV1({&*Second.Record, 1}));
	Durin::uint32 CancellationChecks = 0;
	const auto Cancelled = Durin::Asset::ProbeAssetPackageCompatibility(
		MakeCompatibilityProbeInput(Path, File), Catalog, [&] { return ++CancellationChecks >= 3; });
	EXPECT_EQ(Cancelled.Status, Durin::Asset::EAssetCompatibilityProbeStatus::Cancelled);
	EXPECT_FALSE(Cancelled.Record.has_value());

	Durin::FAssetPath FixturePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/CompatibilityCurrentFixture", FixturePath));
	const auto FixtureFile = Durin::Testing::GetTestWorkDirectory() / "Assets" / "CompatibilityCurrentFixture.dasset";
	WriteCompatibilityFixture("current", FixtureFile);
	const auto Fixture = Durin::Asset::ProbeAssetPackageCompatibility(MakeCompatibilityProbeInput(FixturePath, FixtureFile), Catalog);
	ASSERT_TRUE(Fixture.Record.has_value());
	EXPECT_EQ(Fixture.Record->Compatibility, Durin::Asset::EAssetPackageCompatibility::Compatible);
}

TEST(FPackageAssetTests, CompatibilityProbeQualificationMeasuresRepresentativeCorpusCosts)
{
	InitializeAssetTests();
	const auto Catalog = Durin::Asset::FReflectionCompatibilityCatalog::Capture();
	constexpr Durin::uint32 PackageCount = 16;
	Durin::uint64 CurrentMetadataBytes = 0;
	Durin::uint64 IncompatibleMetadataBytes = 0;
	Durin::uint64 PeakMetadataBytes = 0;

	const auto Started = std::chrono::steady_clock::now();
	for (Durin::uint32 Index = 0; Index < PackageCount; ++Index)
	{
		const bool bIncompatible = (Index % 2) != 0;
		const std::string Name = std::format("Qualification{:02}", Index);
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(std::format("/TestAssets/{}", Name), Path));
		const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / std::format("{}.dasset", Name);
		WriteCompatibilityFixture(bIncompatible ? "unknown_field" : "current", File);

		const auto Result = Durin::Asset::ProbeAssetPackageCompatibility(
			MakeCompatibilityProbeInput(Path, File), Catalog);
		ASSERT_EQ(Result.Status, Durin::Asset::EAssetCompatibilityProbeStatus::Completed);
		ASSERT_TRUE(Result.Record.has_value());
		EXPECT_EQ(Result.Record->Compatibility, bIncompatible
			? Durin::Asset::EAssetPackageCompatibility::Incompatible
			: Durin::Asset::EAssetPackageCompatibility::Compatible);
		(bIncompatible ? IncompatibleMetadataBytes : CurrentMetadataBytes) +=
			Result.Stats.MetadataBytesRead;
		PeakMetadataBytes = std::max(PeakMetadataBytes, Result.Stats.PeakMetadataBytes);
	}
	const auto ScanDuration = std::chrono::steady_clock::now() - Started;
	const auto ScanMicroseconds =
		std::chrono::duration_cast<std::chrono::microseconds>(ScanDuration).count();

	EXPECT_LT(CurrentMetadataBytes / (PackageCount / 2), 64u * 1024u);
	EXPECT_LT(IncompatibleMetadataBytes / (PackageCount / 2), 64u * 1024u);
	EXPECT_LT(PeakMetadataBytes, 64u * 1024u);
	EXPECT_LT(ScanDuration, std::chrono::seconds(5));
	std::cout << "[ QUALIFICATION ] asset_compatibility packages=" << PackageCount
		<< " current_metadata_bytes_avg=" << CurrentMetadataBytes / (PackageCount / 2)
		<< " incompatible_metadata_bytes_avg=" << IncompatibleMetadataBytes / (PackageCount / 2)
		<< " peak_metadata_bytes=" << PeakMetadataBytes
		<< " scan_us=" << ScanMicroseconds << '\n';
}

TEST(FPackageAssetTests, CompatibilityProbeUsesStableFindingsForAuthoredSchemaMismatch)
{
	InitializeAssetTests();
	auto MakeFixture = [](std::string_view Name) {
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(std::format("/TestAssets/{}", Name), Path));
		DPackageAssetForTest* Asset = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
		EXPECT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
		EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
		return std::pair(Path, Durin::Testing::GetTestWorkDirectory() / "Assets" / std::format("{}.dasset", Name));
	};
	const auto Catalog = Durin::Asset::FReflectionCompatibilityCatalog::Capture();

	auto [UnknownFieldPath, UnknownFieldFile] = MakeFixture("ProbeUnknownField");
	WriteCompatibilityFixture("unknown_field", UnknownFieldFile);
	auto UnknownField = Durin::Asset::ProbeAssetPackageCompatibility(MakeCompatibilityProbeInput(UnknownFieldPath, UnknownFieldFile), Catalog);
	ASSERT_TRUE(UnknownField.Record.has_value());
	ASSERT_EQ(UnknownField.Record->Findings.size(), 1u);
	EXPECT_EQ(UnknownField.Record->Compatibility, Durin::Asset::EAssetPackageCompatibility::Incompatible);
	EXPECT_EQ(UnknownField.Record->Findings.front().Code, Durin::Asset::EAssetCompatibilityFindingCode::UnknownField);

	auto [SignaturePath, SignatureFile] = MakeFixture("ProbeBadSignature");
	WriteCompatibilityFixture("incompatible_signature", SignatureFile);
	auto Signature = Durin::Asset::ProbeAssetPackageCompatibility(MakeCompatibilityProbeInput(SignaturePath, SignatureFile), Catalog);
	ASSERT_TRUE(Signature.Record.has_value());
	ASSERT_EQ(Signature.Record->Findings.size(), 1u);
	EXPECT_EQ(Signature.Record->Findings.front().Code, Durin::Asset::EAssetCompatibilityFindingCode::IncompatibleFieldSignature);
	EXPECT_EQ(Signature.Record->Findings.front().ExpectedTypeSignature, "4:4");
	const std::string SignatureJson = Durin::Asset::SerializeAssetCompatibilityReportV1({&*Signature.Record, 1});
	EXPECT_NE(SignatureJson.find("\"storedKind\":\"Int32\""), std::string::npos);
	EXPECT_NE(SignatureJson.find("\"expectedKind\":\"Int32\""), std::string::npos);

	auto [ClassPath, ClassFile] = MakeFixture("ProbeUnknownClass");
	WriteCompatibilityFixture("unknown_class", ClassFile);
	auto UnknownClass = Durin::Asset::ProbeAssetPackageCompatibility(MakeCompatibilityProbeInput(ClassPath, ClassFile), Catalog);
	ASSERT_TRUE(UnknownClass.Record.has_value());
	EXPECT_EQ(UnknownClass.Record->Compatibility, Durin::Asset::EAssetPackageCompatibility::Unsupported);
	ASSERT_FALSE(UnknownClass.Record->Findings.empty());
	EXPECT_EQ(UnknownClass.Record->Findings.front().Code, Durin::Asset::EAssetCompatibilityFindingCode::UnavailableClass);
}

TEST(FPackageAssetTests, CompatibilityProbeSeparatesFormatGraphCorruptionIoAndCancellation)
{
	InitializeAssetTests();
	auto MakeFixture = [](std::string_view Name) {
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(std::format("/TestAssets/{}", Name), Path));
		DPackageAssetForTest* Asset = nullptr;
		EXPECT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
		EXPECT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
		EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
		return std::pair(Path, Durin::Testing::GetTestWorkDirectory() / "Assets" / std::format("{}.dasset", Name));
	};
	const auto Catalog = Durin::Asset::FReflectionCompatibilityCatalog::Capture();
	auto [FormatPath, FormatFile] = MakeFixture("ProbeNewerFormat");
	std::vector<Durin::uint8> NewerFormatBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		NewerFormatBytes, FormatFile.generic_string()));
	const Durin::uint32 NewerVersion = 4;
	std::memcpy(
		NewerFormatBytes.data() + sizeof(Durin::uint32),
		&NewerVersion,
		sizeof(NewerVersion));
	WriteTestBytes(FormatFile, NewerFormatBytes);
	auto Format = Durin::Asset::ProbeAssetPackageCompatibility(MakeCompatibilityProbeInput(FormatPath, FormatFile), Catalog);
	ASSERT_TRUE(Format.Record.has_value());
	EXPECT_EQ(Format.Record->Findings.front().Code, Durin::Asset::EAssetCompatibilityFindingCode::UnsupportedPackageFormat);

	auto [GraphPath, GraphFile] = MakeFixture("ProbeInvalidGraph");
	WriteCompatibilityFixture("invalid_object_graph", GraphFile);
	auto Graph = Durin::Asset::ProbeAssetPackageCompatibility(MakeCompatibilityProbeInput(GraphPath, GraphFile), Catalog);
	ASSERT_TRUE(Graph.Record.has_value());
	EXPECT_EQ(Graph.Record->Findings.front().Code, Durin::Asset::EAssetCompatibilityFindingCode::InvalidObjectGraph);

	auto [TruncatedPath, TruncatedFile] = MakeFixture("ProbeTruncated");
	WriteCompatibilityFixture("truncated", TruncatedFile);
	auto Truncated = Durin::Asset::ProbeAssetPackageCompatibility(MakeCompatibilityProbeInput(TruncatedPath, TruncatedFile), Catalog);
	ASSERT_TRUE(Truncated.Record.has_value());
	EXPECT_EQ(Truncated.Record->Inspection, Durin::Asset::EAssetCompatibilityInspection::Failed);
	EXPECT_EQ(Truncated.Record->Findings.back().Code, Durin::Asset::EAssetCompatibilityFindingCode::CorruptPackage);

	auto [CorruptPath, CorruptFile] = MakeFixture("ProbeCorrupt");
	WriteCompatibilityFixture("corrupt", CorruptFile);
	auto Corrupt = Durin::Asset::ProbeAssetPackageCompatibility(MakeCompatibilityProbeInput(CorruptPath, CorruptFile), Catalog);
	ASSERT_TRUE(Corrupt.Record.has_value());
	EXPECT_EQ(Corrupt.Record->Findings.front().Code, Durin::Asset::EAssetCompatibilityFindingCode::CorruptPackage);

	Durin::FAssetPath MissingPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/ProbeMissing", MissingPath));
	auto MissingInput = Durin::Asset::FAssetPackageCompatibilityProbeInput{
		.PackagePath = MissingPath,
		.PhysicalPath = (Durin::Testing::GetTestWorkDirectory() / "Assets" / "Missing.dasset").generic_string()};
	auto Missing = Durin::Asset::ProbeAssetPackageCompatibility(MissingInput, Catalog);
	ASSERT_TRUE(Missing.Record.has_value());
	EXPECT_EQ(Missing.Record->Findings.front().Code, Durin::Asset::EAssetCompatibilityFindingCode::IoFailure);

	auto Cancelled = Durin::Asset::ProbeAssetPackageCompatibility(MissingInput, Catalog, [] { return true; });
	EXPECT_EQ(Cancelled.Status, Durin::Asset::EAssetCompatibilityProbeStatus::Cancelled);
	EXPECT_FALSE(Cancelled.Record.has_value());
}

TEST(FPackageAssetTests, CompatibilityProbeMarksSnapshotMismatchStaleAndSerializesPathOrder)
{
	InitializeAssetTests();
	Durin::FAssetPath FirstPath, SecondPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/ProbeSerializeA", FirstPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/ProbeSerializeB", SecondPath));
	DPackageAssetForTest* FirstAsset = nullptr;
	DPackageAssetForTest* SecondAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(FirstPath, FirstAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(FirstAsset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(FirstPath));
	ASSERT_TRUE(Durin::Asset::CreateAsset(SecondPath, SecondAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(SecondAsset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SecondPath));
	const auto Root = Durin::Testing::GetTestWorkDirectory() / "Assets";
	const auto Catalog = Durin::Asset::FReflectionCompatibilityCatalog::Capture();
	auto FirstInput = MakeCompatibilityProbeInput(FirstPath, Root / "ProbeSerializeA.dasset");
	auto SecondInput = MakeCompatibilityProbeInput(SecondPath, Root / "ProbeSerializeB.dasset");
	++FirstInput.ExpectedFileSize;
	auto First = Durin::Asset::ProbeAssetPackageCompatibility(FirstInput, Catalog);
	auto Second = Durin::Asset::ProbeAssetPackageCompatibility(SecondInput, Catalog);
	ASSERT_TRUE(First.Record.has_value() && Second.Record.has_value());
	EXPECT_EQ(First.Record->Freshness, Durin::Asset::EAssetCompatibilityFreshness::Stale);
	EXPECT_FALSE(Durin::Asset::IsAssetPackageCompatibilityRecordCurrent(
		*First.Record, First.Record->Fingerprint.FileSize, First.Record->Fingerprint.LastWriteTimeTicks));

	const std::array Records = {*Second.Record, *First.Record};
	const std::string Json = Durin::Asset::SerializeAssetCompatibilityReportV1(Records);
	EXPECT_NE(Json.find("\"schemaVersion\":1"), std::string::npos);
	EXPECT_LT(Json.find("/TestAssets/ProbeSerializeA"), Json.find("/TestAssets/ProbeSerializeB"));
	EXPECT_NE(Json.find("\"inspection\":\"Ready\""), std::string::npos);
	EXPECT_NE(Json.find("\"freshness\":\"Stale\""), std::string::npos);
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

	EXPECT_NE(Durin::Asset::FindLoadedPackage(ExistingPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(IntroducedPath), nullptr);
	EXPECT_TRUE(Durin::Asset::UnloadPackage(ExistingPath));
}

TEST(FPackageAssetTests, ReportsUnknownFieldsWithoutMarkingPackageDirty)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/ObsoleteField", Path));

	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Value = 42;
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "ObsoleteField.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_TRUE(RenameSerializedString(Bytes, "Value", "Stale"));
	WriteTestBytes(File, Bytes);

	testing::internal::CaptureStderr();
	DPackageAssetForTest* Loaded = nullptr;
	Durin::Asset::FAssetLoadReport Report;
	const Durin::Asset::FAssetResult LoadResult = Durin::Asset::LoadAsset(Path, Loaded, &Report);
	const std::string Warning = testing::internal::GetCapturedStderr();
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->Value, 0);
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_EQ(Report.CompatibilityIssues.size(), 1u);
	EXPECT_EQ(Report.PackagePath, Path);
	EXPECT_EQ(Report.GetAffectedObjectCount(), 1u);
	EXPECT_EQ(Report.GetLegacyFieldCount(), 1u);
	EXPECT_EQ(Report.GetMigratedDataCount(), 0u);
	EXPECT_EQ(Report.GetRiskItemCount(), 1u);
	EXPECT_TRUE(Report.HasRiskItems());
	const Durin::Asset::FAssetCompatibilityIssue& Issue = Report.CompatibilityIssues.front();
	EXPECT_EQ(Issue.ObjectPath, "/TestAssets/ObsoleteField");
	EXPECT_EQ(Issue.DeclaringClass, "Tests::DPackageAssetForTest");
	EXPECT_EQ(Issue.Classification, Durin::Asset::EAssetCompatibilityClassification::UnknownIncompatible);
	EXPECT_EQ(Issue.Risk, Durin::Asset::EAssetCompatibilityRisk::UnknownNewerSchema);
	ASSERT_EQ(Issue.LegacyFields.size(), 1u);
	EXPECT_EQ(Issue.LegacyFields.front().Name, "Stale");
	EXPECT_NE(Warning.find("on object '/TestAssets/ObsoleteField'"), std::string::npos);
	EXPECT_EQ(Warning.find("Resave the package"), std::string::npos);
	EXPECT_EQ(
		Durin::Asset::SavePackage(Loaded->GetPackage()).Error,
		Durin::Asset::EAssetError::UnsupportedProperty);
	EXPECT_TRUE(Durin::Asset::SavePackage(
		Loaded->GetPackage(),
		{.bAllowCompatibilityDataLoss = true}));

	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FPackageAssetTests, RegisteredSafeCleanupProducesStructuredReportAndDirtyPackage)
{
	InitializeAssetTests();
	Durin::Asset::RegisterAssetStructureUpgrader(
		DPackageAssetForTest::StaticClass(),
		"Tests.SafeScalarCleanup",
		[](Durin::DObject*, std::span<const Durin::Asset::FAssetLegacyField> Fields,
			const Durin::Asset::FAssetMigrationContext&,
			std::vector<Durin::Asset::FAssetCompatibilityIssue>& OutIssues) -> Durin::Asset::FAssetResult
		{
			const auto It = std::ranges::find(Fields, "Clean", &Durin::Asset::FAssetLegacyField::Name);
			if (It == Fields.end()) return {};
			Durin::int32 Value = 1;
			if (It->Payload.size() != sizeof(Value)) return {Durin::Asset::EAssetError::CorruptFile, "Invalid legacy scalar."};
			std::memcpy(&Value, It->Payload.data(), sizeof(Value));
			if (Value != 0) return {};
			OutIssues.push_back({
				.DeclaringClass = It->DeclaringClass,
				.LegacyFields = {*It},
				.Classification = Durin::Asset::EAssetCompatibilityClassification::SafeCleanup,
				.MigrationSummary = "Removed an empty legacy scalar.",
				.Risk = Durin::Asset::EAssetCompatibilityRisk::None});
			return {};
		});

	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/SafeCleanup", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "SafeCleanup.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_TRUE(RenameSerializedString(Bytes, "Value", "Clean"));
	WriteTestBytes(File, Bytes);

	DPackageAssetForTest* Loaded = nullptr;
	Durin::Asset::FAssetLoadReport Report;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded, &Report));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->GetPackage()->IsDirty());
	ASSERT_EQ(Report.CompatibilityIssues.size(), 1u);
	EXPECT_FALSE(Report.HasRiskItems());
	EXPECT_EQ(Report.GetLegacyFieldCount(), 1u);
	EXPECT_EQ(Report.CompatibilityIssues.front().HandlerId, "Tests.SafeScalarCleanup");
	ASSERT_EQ(Report.Mutations.size(), 1u);
	EXPECT_EQ(Report.Mutations.front().PackagePath, Path);
	EXPECT_EQ(Report.Mutations.front().HandlerId, "Tests.SafeScalarCleanup");
	EXPECT_EQ(
		Report.Mutations.front().Kind,
		Durin::Asset::EAssetLoadMutationKind::Upgrade);
	EXPECT_FALSE(Report.HasNonUpgradeMutations());
	EXPECT_EQ(
		Report.CompatibilityIssues.front().Classification,
		Durin::Asset::EAssetCompatibilityClassification::SafeCleanup);
	ASSERT_TRUE(Durin::Asset::SavePackage(Loaded->GetPackage()));
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
	Report = {};
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded, &Report));
	EXPECT_FALSE(Report.HasCompatibilityIssues());
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
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

	const Durin::uint64 CreatedRevision = Package->GetEditRevision();
	Package->ClearDirty();
	EXPECT_EQ(Package->GetEditRevision(), CreatedRevision);

	Package->MarkDirty();
	const Durin::uint64 FirstEditRevision = Package->GetEditRevision();
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
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAssetExact(FirstPath), nullptr);
	EXPECT_FALSE(First->GetPackage()->IsDirty());

	EXPECT_FALSE(std::filesystem::exists(Root / "Stage0Blocked" / "Second.dasset"));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(BlockedPath), nullptr);
	EXPECT_TRUE(Second->GetPackage()->IsDirty());
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
		*Durin::Asset::GetAssetRegistry().FindAssetExact(ExistingPath);
	std::vector<Durin::uint8> ExistingBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		ExistingBytes, ExistingRegistry.PhysicalPath));

	DPackageAssetForTest* Added = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(NewPath, Added));
	Added->Value = 22;
	Existing->Value = 33;
	Existing->MarkPackageDirty();
	const std::array Packages = {
		Existing->GetPackage(),
		Added->GetPackage()};
	const Durin::Asset::FAssetResult Result = Durin::Asset::SavePackagesAtomically(
		Packages,
		{
			.RootPackage = Added->GetPackage(),
			.ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
				return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRegistry;
			}});
	EXPECT_FALSE(Result);

	std::vector<Durin::uint8> RestoredBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		RestoredBytes, ExistingRegistry.PhysicalPath));
	EXPECT_EQ(RestoredBytes, ExistingBytes);
	EXPECT_EQ(*Durin::Asset::GetAssetRegistry().FindAssetExact(ExistingPath), ExistingRegistry);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(NewPath), nullptr);
	EXPECT_FALSE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "AtomicBundleNew.dasset"));
	EXPECT_TRUE(Existing->GetPackage()->IsDirty());
	EXPECT_TRUE(Added->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Asset::DiscardUnpublishedPackage(Added->GetPackage()));
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
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().SavePackage(Dependency->GetPackage()));

	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().SavePackage(Owner->GetPackage()));
	ASSERT_EQ(Durin::Asset::FAssetManager::Get().GetRegistry().FindAssetExact(OwnerPath)->Dependencies.size(), 1u);
	EXPECT_EQ(Durin::Asset::FAssetManager::Get().UnloadPackage(DependencyPath).Error, Durin::Asset::EAssetError::InUse);
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(DependencyPath));

	DPackageAssetForTest* LoadedOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, LoadedOwner));
	ASSERT_NE(LoadedOwner->ExternalReference.Get(), nullptr);
	EXPECT_EQ(LoadedOwner->ExternalReference->GetObjectPath(), "/TestAssets/Dependency");
	EXPECT_EQ(Durin::Asset::FAssetManager::Get().FindLoadedPackage(DependencyPath)->GetAsset(), LoadedOwner->ExternalReference.Get());

	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(DependencyPath));
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().GetRegistry().ScanMountedContent());
	EXPECT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAssetExact(OwnerPath), nullptr);
	EXPECT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAssetExact(DependencyPath), nullptr);
}

TEST(FPackageAssetTests, RedirectedHardReferencesLoadOnlyTheFinalPackage)
{
	InitializeAssetTests();
	Durin::FAssetPath TargetPath, AliasPath, OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/HardFinal", TargetPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/HardAlias", AliasPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/HardOwner", OwnerPath));

	DPackageAssetForTest* Target = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TargetPath, Target));
	Target->Label = "redirected hard target";
	ASSERT_TRUE(Durin::Asset::SavePackage(Target->GetPackage()));
	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Target;
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetPath));

	Durin::Asset::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirector(AliasPath, TargetPath, Redirector));
	ASSERT_TRUE(Durin::Asset::SavePackage(Redirector->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AliasPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetPath));

	const auto OwnerFile = Durin::Testing::GetTestWorkDirectory()
		/ "Assets" / "HardOwner.dasset";
	std::vector<Durin::uint8> OwnerBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		OwnerBytes, OwnerFile.generic_string()));
	EXPECT_EQ(RenameAllSerializedStrings(
		OwnerBytes, TargetPath.GetView(), AliasPath.GetView()), 2u);
	WriteTestBytes(OwnerFile, OwnerBytes);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	const Durin::Asset::FAssetData* OwnerData =
		Durin::Asset::GetAssetRegistry().FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	EXPECT_EQ(OwnerData->Dependencies, (std::vector<Durin::FAssetPath>{AliasPath}));

	Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, Owner));
	ASSERT_NE(Owner, nullptr);
	ASSERT_NE(Owner->ExternalReference.Get(), nullptr);
	EXPECT_EQ(Owner->ExternalReference->GetPackage()->GetPackagePath(),
		TargetPath.ToString());
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(AliasPath), nullptr);
	EXPECT_NE(Durin::Asset::FindLoadedPackage(TargetPath), nullptr);
	EXPECT_EQ(
		Durin::Asset::UnloadPackage(TargetPath).Error,
		Durin::Asset::EAssetError::InUse);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(OwnerPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({AliasPath, TargetPath}));
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
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
}

TEST(FPackageAssetTests, RelocationBatchPublishesOneRevisionAndSupportsExactUndoRedo)
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
		Durin::Asset::FAssetRelocationMapping{Second, SecondMoved}};
	Durin::Asset::FAssetRelocationBatchToken Token;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetRelocationBatch(Mappings, Token));
	const Durin::uint64 BeforeRevision =
		Durin::Asset::GetAssetRegistry().GetRevision();
	ASSERT_TRUE(Durin::Asset::ApplyAssetRelocationBatch(Token));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetRevision(), BeforeRevision + 1);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(First)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(Second)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(FirstMoved)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(SecondMoved)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Asset);

	ASSERT_TRUE(Durin::Asset::RestoreAssetRelocationBatch(Token));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetRevision(), BeforeRevision + 2);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(First)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(Second)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(FirstMoved), nullptr);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(SecondMoved), nullptr);

	ASSERT_TRUE(Durin::Asset::ApplyAssetRelocationBatch(Token));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetRevision(), BeforeRevision + 3);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().ResolveAssetPath(First).FinalPath,
		FirstMoved);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().ResolveAssetPath(Second).FinalPath,
		SecondMoved);
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
	const auto* FirstAlias =
		Durin::Asset::GetAssetRegistry().FindAssetExact(First);
	const auto* SecondAlias =
		Durin::Asset::GetAssetRegistry().FindAssetExact(Second);
	ASSERT_NE(FirstAlias, nullptr);
	ASSERT_NE(SecondAlias, nullptr);
	EXPECT_EQ(FirstAlias->RedirectDestination, Third);
	EXPECT_EQ(SecondAlias->RedirectDestination, Third);

	ASSERT_TRUE(RelocateAssetForTest(Third, First));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(First)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(Second)->RedirectDestination,
		First);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(Third)->RedirectDestination,
		First);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().ResolveAssetPath(Second).FinalPath,
		First);

	Durin::FAssetPath Unrelated;
	Durin::FAssetPath UnrelatedAlias;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RepeatedUnrelated", Unrelated));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/RepeatedUnrelatedAlias", UnrelatedAlias));
	DPackageAssetForTest* UnrelatedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Unrelated, UnrelatedAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(UnrelatedAsset->GetPackage()));
	Durin::Asset::DAssetRedirector* Alias = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirector(
		UnrelatedAlias, Unrelated, Alias));
	ASSERT_TRUE(Durin::Asset::SavePackage(Alias->GetPackage()));
	EXPECT_EQ(RelocateAssetForTest(First, UnrelatedAlias).Error,
		Durin::Asset::EAssetError::AlreadyExists);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().ResolveAssetPath(UnrelatedAlias).FinalPath,
		Unrelated);
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
	std::vector<Durin::uint8> Before;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Before, OldFile.generic_string()));
	EXPECT_EQ(*reinterpret_cast<const Durin::uint32*>(Before.data() + sizeof(Durin::uint32)), 3u);
	EXPECT_EQ(std::search(Before.begin(), Before.end(), OldPath.GetView().begin(), OldPath.GetView().end()), Before.end());

	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	const auto NewFile = Durin::Testing::GetTestWorkDirectory() / "Assets" / "Sub" / "MoveSource.dasset";
	std::vector<Durin::uint8> After;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(After, NewFile.generic_string()));
	EXPECT_EQ(Before, After);
	EXPECT_TRUE(std::filesystem::exists(OldFile));
	ASSERT_NE(Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(OldPath)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(OldPath), nullptr);
	EXPECT_NE(Durin::Asset::FindLoadedPackage(NewPath), nullptr);
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
	const Durin::Asset::FAssetData* OwnerData = Durin::Asset::GetAssetRegistry().FindAssetExact(OwnerPath);
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
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
	EXPECT_FALSE(std::filesystem::exists(File));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(Path), nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
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

	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(Path), nullptr);
	EXPECT_FALSE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "LoadedDelete.dasset"));
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
	EXPECT_EQ(Durin::Asset::DeleteAsset(DependencyPath).Error, Durin::Asset::EAssetError::InUse);
	EXPECT_NE(Durin::Asset::FindLoadedPackage(DependencyPath), nullptr);
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAssetExact(DependencyPath), nullptr);
	EXPECT_TRUE(std::filesystem::exists(
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "DeleteDependency.dasset"));
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
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
	ASSERT_EQ(Analysis.CompanionFiles.size(), 1u);
	EXPECT_EQ(Analysis.CompanionFiles.front(), Companion);
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
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
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	ASSERT_GT(Bytes.size(), 16u);
	WriteTestBytes(File, std::span<const Durin::uint8>(Bytes).first(16));

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(Path, Analysis));
	EXPECT_FALSE(Analysis.Warning.empty());
	EXPECT_TRUE(Analysis.CompanionFiles.empty());
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
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
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(OwnerPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(DependencyPath), nullptr);
}

TEST(FPackageAssetTests, VersionOneIsExplicitlyUnsupported)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LegacyVersion", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	const auto File = Durin::Testing::GetTestWorkDirectory() / "Assets" / "LegacyVersion.dasset";
	std::fstream Stream(File, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(Stream.is_open());
	const Durin::uint32 Version = 1;
	Stream.seekp(sizeof(Durin::uint32));
	Stream.write(reinterpret_cast<const char*>(&Version), sizeof(Version));
	Stream.close();
	Durin::DObject* Loaded = nullptr;
	EXPECT_EQ(Durin::Asset::LoadAsset(Path, Loaded).Error, Durin::Asset::EAssetError::UnsupportedVersion);
	EXPECT_EQ(Loaded, nullptr);
	ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent());
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(Path), nullptr);
	EXPECT_TRUE(std::ranges::any_of(
		Durin::Asset::GetAssetRegistry().GetScanErrors(),
		[](const Durin::Asset::FAssetResult& Error) {
			return Error.Error == Durin::Asset::EAssetError::UnsupportedVersion;
		}));
}

TEST(FPackageAssetTests, ManualScanMountsRetainPackageIdentityAndDirectLoading)
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
			.bAuthoringWritable = true}};
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TestAssets/ManualScanAsset", Path));
	{
		Durin::PathUtilities::FScopedMountRegistryFixture Mounts(Definitions);
		ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
		DPackageAssetForTest* Asset = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
		ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
		ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(Path));

		auto& Registry = Durin::Asset::GetAssetRegistry();
		ASSERT_TRUE(Registry.ScanMountedContent(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
		EXPECT_EQ(Registry.GetLastScanStats().Enumerated, 0u);
		EXPECT_EQ(Registry.FindAssetExact(Path), nullptr);

		Durin::DObject* Loaded = nullptr;
		ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded));
		EXPECT_NE(Loaded, nullptr);
		ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(Path));
	}

	auto& Registry = Durin::Asset::GetAssetRegistry();
	ASSERT_TRUE(Registry.ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
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
	auto& Registry = Durin::Asset::FAssetManager::Get().GetRegistry();

	const Durin::uint64 RevisionBeforeInitialScan = Registry.GetRevision();
	ASSERT_TRUE(Registry.ScanMountedContent(Durin::Asset::EAssetRegistryScanMode::FullValidation));
	EXPECT_GT(Registry.GetRevision(), RevisionBeforeInitialScan);
	EXPECT_EQ(Registry.GetLastScanStats().Enumerated, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().HeaderReadAttempts, 2u);
	EXPECT_GT(Registry.GetLastScanStats().HeaderBytesRead, 0u);
	EXPECT_GE(Registry.GetLastScanStats().DurationMilliseconds, 0.0);
	EXPECT_EQ(Registry.GetAssets().size(), 2u);
	const auto CacheFile = CacheRoot / "AssetRegistry" / "Registry.bin";
	std::vector<Durin::uint8> FirstCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstCache, CacheFile.generic_string()));

	const Durin::uint64 StableRevision = Registry.GetRevision();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetRevision(), StableRevision);
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);
	EXPECT_EQ(Registry.GetLastScanStats().HeaderReadAttempts, 0u);
	EXPECT_EQ(Registry.GetLastScanStats().HeaderBytesRead, 0u);
	EXPECT_GE(Registry.GetLastScanStats().DurationMilliseconds, 0.0);
	std::vector<Durin::uint8> SecondCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondCache, CacheFile.generic_string()));
	EXPECT_EQ(SecondCache, FirstCache);

	const auto Alpha = ContentA / "Alpha.dasset";
	std::filesystem::last_write_time(Alpha, std::filesystem::last_write_time(Alpha) + std::chrono::seconds(2));
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_GT(Registry.GetRevision(), StableRevision);
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().HeaderReadAttempts, 1u);
	EXPECT_GT(Registry.GetLastScanStats().HeaderBytesRead, 0u);
	EXPECT_GE(Registry.GetLastScanStats().DurationMilliseconds, 0.0);

	std::filesystem::copy_file(ValidSource, ContentA / "Gamma.dasset");
	std::filesystem::remove(ContentA / "Beta.dasset");
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Removed, 1u);
	EXPECT_EQ(Registry.GetAssets().size(), 2u);

	std::filesystem::rename(ContentA / "Gamma.dasset", ContentA / "Delta.dasset");
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Removed, 1u);

	ASSERT_TRUE(Registry.ScanMountedContent(Durin::Asset::EAssetRegistryScanMode::FullValidation));
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 0u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().HeaderReadAttempts, 2u);
	EXPECT_GT(Registry.GetLastScanStats().HeaderBytesRead, 0u);
	EXPECT_GE(Registry.GetLastScanStats().DurationMilliseconds, 0.0);
	EXPECT_EQ(Registry.GetAssets().size(), 2u);

	const std::array<Durin::uint8, 3> CorruptCache = {1, 2, 3};
	WriteTestBytes(CacheFile, CorruptCache);
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 2u);
	EXPECT_FALSE(Registry.GetCacheWarning().empty());

	std::vector<Durin::uint8> IncompatibleCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(IncompatibleCache, CacheFile.generic_string()));
	const Durin::uint32 IncompatibleSchema = 99;
	std::memcpy(IncompatibleCache.data() + sizeof(Durin::uint32), &IncompatibleSchema, sizeof(IncompatibleSchema));
	WriteTestBytes(CacheFile, IncompatibleCache);
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 2u);
	EXPECT_FALSE(Registry.GetCacheWarning().empty());

	std::filesystem::create_directories(ContentB);
	for (const auto& Source : {Alpha, ContentA / "Delta.dasset"})
	{
		const auto Destination = ContentB / Source.filename();
		std::filesystem::copy_file(Source, Destination);
		std::filesystem::last_write_time(Destination, std::filesystem::last_write_time(Source));
	}
	Durin::PathUtilities::RegisterMountPointForTests("/TestAssets/", ContentB.generic_string() + "/");
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);

	const auto AdditionalContent = WorkRoot / "AdditionalContent";
	std::filesystem::create_directories(AdditionalContent);
	Durin::PathUtilities::RegisterMountPointForTests("/Additional/", AdditionalContent.generic_string() + "/");
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 2u);
	EXPECT_NE(Registry.GetCacheWarning().find("mount manifest changed"), std::string::npos);

	const auto BlockedCacheRoot = WorkRoot / "BlockedCacheRoot";
	WriteTestBytes(BlockedCacheRoot, CorruptCache);
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	ASSERT_TRUE(Registry.ScanMountedContent(Durin::Asset::EAssetRegistryScanMode::FullValidation));
	EXPECT_EQ(Registry.GetAssets().size(), 2u);
	EXPECT_FALSE(Registry.GetCacheWarning().empty());
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
	auto& Registry = Durin::Asset::GetAssetRegistry();
	ASSERT_TRUE(Registry.ScanMountedContent(Durin::Asset::EAssetRegistryScanMode::FullValidation));
	const Durin::uint64 EmptyRegistryRevision = Registry.GetRevision();

	Durin::FAssetPath FirstPath;
	Durin::FAssetPath MovedPath;
	Durin::FAssetPath ImportedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LifecycleFirst", FirstPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LifecycleMoved", MovedPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/LifecycleImported", ImportedPath));

	DPackageAssetForTest* FirstAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(FirstPath, FirstAsset));
	EXPECT_EQ(Registry.GetRevision(), EmptyRegistryRevision);
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ASSERT_TRUE(Durin::Asset::SavePackage(FirstAsset->GetPackage()));
	EXPECT_GT(Registry.GetRevision(), EmptyRegistryRevision);
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ShutdownAssetManagerForRestart();
	EXPECT_FALSE(Registry.IsPersistentSnapshotDirty());
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);

	Durin::DObject* Reloaded = nullptr;
	const Durin::uint64 RevisionBeforeLoad = Registry.GetRevision();
	ASSERT_TRUE(Durin::Asset::LoadAsset(FirstPath, Reloaded));
	EXPECT_EQ(Registry.GetRevision(), RevisionBeforeLoad);
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);

	ASSERT_TRUE(RelocateAssetForTest(FirstPath, MovedPath));
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);
	ASSERT_NE(Registry.FindAssetExact(FirstPath), nullptr);
	EXPECT_EQ(Registry.FindAssetExact(FirstPath)->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_NE(Registry.FindAssetExact(MovedPath), nullptr);

	DPackageAssetForTest* ImportedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ImportedPath, ImportedAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(ImportedAsset->GetPackage()));
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 3u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);

	ASSERT_TRUE(DeleteAssetClosureForTest({FirstPath, MovedPath}));
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);
	EXPECT_EQ(Registry.FindAssetExact(MovedPath), nullptr);

	const auto BlockedCacheRoot = WorkRoot / "BlockedCacheRoot";
	const std::array<Durin::uint8, 3> Blocker = {1, 2, 3};
	WriteTestBytes(BlockedCacheRoot, Blocker);
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	ImportedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(ImportedPath, ImportedAsset));
	ImportedAsset->Value = 42;
	ASSERT_TRUE(Durin::Asset::SavePackage(ImportedAsset->GetPackage()));
	const auto AuthoredFile = ContentRoot / "LifecycleImported.dasset";
	std::vector<Durin::uint8> BeforeFailedFlush;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeFailedFlush, AuthoredFile.generic_string()));
	ShutdownAssetManagerForRestart();
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	EXPECT_FALSE(Registry.GetCacheWarning().empty());
	std::vector<Durin::uint8> AfterFailedFlush;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterFailedFlush, AuthoredFile.generic_string()));
	EXPECT_EQ(AfterFailedFlush, BeforeFailedFlush);

	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	ShutdownAssetManagerForRestart();
	EXPECT_FALSE(Registry.IsPersistentSnapshotDirty());
}

TEST(FPackageAssetTests, SoftReferenceCacheUsesContentFingerprintsAndRecoversFromCorruptionWithoutLoadingTargets)
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
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetAPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TargetBPath));

	auto& Registry = Durin::Asset::GetAssetRegistry();
	ASSERT_TRUE(Registry.ScanMountedContent(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(TargetAPath), nullptr);
	EXPECT_EQ(Registry.GetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FAssetPath>{TargetAPath}));
	const auto CacheFile = CacheRoot / "AssetRegistry" / "References.bin";
	ASSERT_TRUE(std::filesystem::is_regular_file(CacheFile));
	std::vector<Durin::uint8> FirstCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstCache, CacheFile.generic_string()));

	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_GT(Registry.GetReferenceIndex().GetStats().ReusedSources, 0u);
	EXPECT_EQ(Registry.GetReferenceIndex().GetStats().ExtractedSources, 0u);
	std::vector<Durin::uint8> SecondCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondCache, CacheFile.generic_string()));
	EXPECT_EQ(SecondCache, FirstCache);

	const auto* OwnerData = Registry.FindAssetExact(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	const std::filesystem::path OwnerFile = OwnerData->PhysicalPath;
	const auto PreservedTime = std::filesystem::last_write_time(OwnerFile);
	ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, Owner));
	Owner->Direct.SetPath(TargetBPath);
	ASSERT_TRUE(Durin::Asset::SavePackage(Owner->GetPackage()));
	std::filesystem::last_write_time(OwnerFile, PreservedTime);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(OwnerPath));
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_GT(Registry.GetReferenceIndex().GetStats().ExtractedSources, 0u);
	EXPECT_EQ(Registry.GetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FAssetPath>{TargetBPath}));
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(TargetBPath), nullptr);

	const std::array<Durin::uint8, 3> CorruptCache = {1, 2, 3};
	WriteTestBytes(CacheFile, CorruptCache);
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_FALSE(Registry.GetReferenceIndex().GetCacheWarning().empty());
	EXPECT_GT(Registry.GetReferenceIndex().GetStats().ExtractedSources, 0u);
	EXPECT_EQ(Registry.GetReferenceIndex().FindTargets(OwnerPath),
		(std::vector<Durin::FAssetPath>{TargetBPath}));
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(TargetBPath), nullptr);
	std::vector<Durin::uint8> RecoveredCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		RecoveredCache, CacheFile.generic_string()));
	EXPECT_NE(RecoveredCache, std::vector<Durin::uint8>(CorruptCache.begin(), CorruptCache.end()));
}
