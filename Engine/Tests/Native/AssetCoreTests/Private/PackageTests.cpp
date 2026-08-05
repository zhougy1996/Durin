#include <gtest/gtest.h>

#include "Asset/SourcePath.h"
#include "AssetCompatibility.h"
#include "AssetSystem.h"
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
	template<typename T>
	auto VectorNum(const void* Container) -> Durin::uint64 { return static_cast<const std::vector<T>*>(Container)->size(); }
	template<typename T>
	auto VectorGet(const void* Container, Durin::uint64 Index) -> const void* { return &(*static_cast<const std::vector<T>*>(Container))[Index]; }
	template<typename T>
	auto VectorGetMutable(void* Container, Durin::uint64 Index) -> void* { return &(*static_cast<std::vector<T>*>(Container))[Index]; }
	template<typename T>
	auto VectorResize(void* Container, Durin::uint64 Num) -> bool { static_cast<std::vector<T>*>(Container)->resize(Num); return true; }

	const Durin::DurinCodeGen::FArrayPropertyHelper GIntVectorHelper = {
		&VectorNum<Durin::int32>, &VectorGet<Durin::int32>, &VectorGetMutable<Durin::int32>, &VectorResize<Durin::int32>
	};
	const Durin::DurinCodeGen::FArrayPropertyHelper GGuidVectorHelper = {
		&VectorNum<Durin::FGuid>, &VectorGet<Durin::FGuid>, &VectorGetMutable<Durin::FGuid>, &VectorResize<Durin::FGuid>
	};
	const Durin::DurinCodeGen::FArrayPropertyHelper GVector3VectorHelper = {
		&VectorNum<Durin::FVector3>, &VectorGet<Durin::FVector3>,
		&VectorGetMutable<Durin::FVector3>, &VectorResize<Durin::FVector3>
	};

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
	const Durin::DurinCodeGen::FMapPropertyHelper GScoreMapHelper = {
		&MapNum, &MapKey, &MapValue, &MapMutableValue, &MapClear, &CreateString, &CopyString, &DestroyString,
		&CreateInt, &DestroyInt, &MapInsert, &MapContains, &MapRenameKey, &MapRemove
	};

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
	const Durin::DurinCodeGen::FMapPropertyHelper GVectorMapHelper = {
		&VectorMapNum, &VectorMapKey, &VectorMapValue, &VectorMapMutableValue,
		&VectorMapClear, &CreateString, &CopyString, &DestroyString,
		&CreateVector3, &DestroyVector3, &VectorMapInsert, &VectorMapContains,
		&VectorMapRenameKey, &VectorMapRemove
	};
	bool GReportNonUpgradeMutationOnPostLoad = false;

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
			static const Durin::DurinCodeGen::FArrayPropertyParams GuidsProp = {"RelatedIds", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, RelatedIds)), sizeof(RelatedIds), Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, nullptr, &GuidInner, nullptr, nullptr, false, &GGuidVectorHelper};
			static const Durin::DurinCodeGen::FPropertyParamsBase ScoreInner = {"Scores_Inner", Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FPropertyParamsBase ScoresProp = {"Scores", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, Scores)), sizeof(Scores), Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, nullptr, &ScoreInner, nullptr, nullptr, false, &GIntVectorHelper};
			static const Durin::DurinCodeGen::FPropertyParamsBase MapKeyProp = {"NamedScores_Key", Durin::EPropertyFlags::None, 1, 0, sizeof(std::string), Durin::DurinCodeGen::EPropertyGenFlags::String};
			static const Durin::DurinCodeGen::FPropertyParamsBase MapValueProp = {"NamedScores_Value", Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FPropertyParamsBase NamedScoresProp = {"NamedScores", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, NamedScores)), sizeof(NamedScores), Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, nullptr, nullptr, &MapKeyProp, &MapValueProp, false, nullptr, &GScoreMapHelper};
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
				sizeof(Vectors), Durin::DurinCodeGen::EPropertyGenFlags::Array,
				nullptr, nullptr, &VectorInner, nullptr, nullptr, false,
				&GVector3VectorHelper};
			static const Durin::DurinCodeGen::FStringPropertyParams VectorMapKey = {
				"VectorMap_Key", Durin::EPropertyFlags::None, 1, 0, sizeof(std::string),
				Durin::DurinCodeGen::EPropertyGenFlags::String};
			static const Durin::DurinCodeGen::FStructPropertyParams VectorMapValue = {
				"VectorMap_Value", Durin::EPropertyFlags::None, 1, 0,
				&Durin::Z_Construct_DStruct_Durin_FVector3};
			static const Durin::DurinCodeGen::FMapPropertyParams VectorMapProp = {
				"VectorMap", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DMathStructAssetForTest, VectorMap)),
				sizeof(VectorMap), Durin::DurinCodeGen::EPropertyGenFlags::Map,
				nullptr, nullptr, nullptr, &VectorMapKey, &VectorMapValue, false,
				nullptr, &GVectorMapHelper};
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
	EXPECT_EQ(Header.FormatVersion, 2u);
	EXPECT_EQ(Header.ObjectCount, 2u);
	EXPECT_LT(Header.BytesRead, 1024u);
}

TEST(FPackageAssetTests, WriterEmitsFrozenVersionTwoPrefix)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/VersionTwoPrefix", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));

	const auto File =
		Durin::Testing::GetTestWorkDirectory() / "Assets" / "VersionTwoPrefix.dasset";
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, File.generic_string()));
	constexpr std::array<Durin::uint8, 8> ExpectedPrefix = {
		0x44, 0x41, 0x53, 0x54,
		0x02, 0x00, 0x00, 0x00};
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
	ASSERT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(Path), nullptr);
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
	WriteCompatibilityFixture("newer_format", FormatFile);
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
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAsset(FirstPath), nullptr);
	EXPECT_FALSE(First->GetPackage()->IsDirty());

	EXPECT_FALSE(std::filesystem::exists(Root / "Stage0Blocked" / "Second.dasset"));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(BlockedPath), nullptr);
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
		*Durin::Asset::GetAssetRegistry().FindAsset(ExistingPath);
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
	EXPECT_EQ(*Durin::Asset::GetAssetRegistry().FindAsset(ExistingPath), ExistingRegistry);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(NewPath), nullptr);
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
	ASSERT_EQ(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(OwnerPath)->Dependencies.size(), 1u);
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
	EXPECT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(OwnerPath), nullptr);
	EXPECT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(DependencyPath), nullptr);
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

TEST(FPackageAssetTests, VersionTwoDoesNotStoreItsOwnPathAndDirectoryMoveIsByteStable)
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
	EXPECT_EQ(*reinterpret_cast<const Durin::uint32*>(Before.data() + sizeof(Durin::uint32)), 2u);
	EXPECT_EQ(std::search(Before.begin(), Before.end(), OldPath.GetView().begin(), OldPath.GetView().end()), Before.end());

	ASSERT_TRUE(Durin::Asset::MoveAsset(OldPath, NewPath));
	const auto NewFile = Durin::Testing::GetTestWorkDirectory() / "Assets" / "Sub" / "MoveSource.dasset";
	std::vector<Durin::uint8> After;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(After, NewFile.generic_string()));
	EXPECT_EQ(Before, After);
	EXPECT_FALSE(std::filesystem::exists(OldFile));
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(OldPath), nullptr);
	EXPECT_NE(Durin::Asset::FindLoadedPackage(NewPath), nullptr);
}

TEST(FPackageAssetTests, MoveRewritesMountedReferrers)
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

	ASSERT_TRUE(Durin::Asset::MoveAsset(OldPath, NewPath));
	const Durin::Asset::FAssetData* OwnerData = Durin::Asset::GetAssetRegistry().FindAsset(OwnerPath);
	ASSERT_NE(OwnerData, nullptr);
	EXPECT_NE(std::ranges::find(OwnerData->Dependencies, NewPath), OwnerData->Dependencies.end());
	EXPECT_EQ(std::ranges::find(OwnerData->Dependencies, OldPath), OwnerData->Dependencies.end());
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
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(Path), nullptr);
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
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(Path), nullptr);
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
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAsset(DependencyPath), nullptr);
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
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(Path), nullptr);
	ASSERT_FALSE(Durin::Asset::GetAssetRegistry().GetScanErrors().empty());
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().GetScanErrors().back().Error, Durin::Asset::EAssetError::UnsupportedVersion);
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
		EXPECT_EQ(Registry.FindAsset(Path), nullptr);

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

	ASSERT_TRUE(Durin::Asset::MoveAsset(FirstPath, MovedPath));
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);
	EXPECT_EQ(Registry.FindAsset(FirstPath), nullptr);
	EXPECT_NE(Registry.FindAsset(MovedPath), nullptr);

	DPackageAssetForTest* ImportedAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ImportedPath, ImportedAsset));
	ASSERT_TRUE(Durin::Asset::SavePackage(ImportedAsset->GetPackage()));
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 2u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);

	ASSERT_TRUE(Durin::Asset::DeleteAsset(MovedPath));
	EXPECT_TRUE(Registry.IsPersistentSnapshotDirty());
	ShutdownAssetManagerForRestart();
	ASSERT_TRUE(Registry.ScanMountedContent());
	EXPECT_EQ(Registry.GetLastScanStats().Reused, 1u);
	EXPECT_EQ(Registry.GetLastScanStats().Reparsed, 0u);
	EXPECT_EQ(Registry.FindAsset(MovedPath), nullptr);

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
