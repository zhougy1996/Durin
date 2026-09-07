#include "DObject/ObjectGraphReplacement.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using Ptr = TObjectPtr<DObject>;
	using List = std::vector<Ptr>;
	using Nested = std::vector<List>;
	using Map = std::unordered_map<DObject*, List>;
	struct FNestedReference { Ptr Reference; };
	auto NestedReferenceStruct() -> DStruct*
	{
		static DStruct* Type = [] {
			using namespace DurinCodeGen;
			static const auto Reference = FObjectPropertyParams::ObjectPtr<DObject>("Reference", EPropertyFlags::None,
				1, STRUCT_OFFSET_UINT16(FNestedReference, Reference), &DObject::StaticClass);
			static const FPropertyParamsBase* Properties[]{&Reference};
			static DStruct* Raw = nullptr;
			auto NoRegister = []() -> DStruct* {
				if (!Raw)
				{
					Raw = new DStruct(EC_StaticConstructor, FName("FNestedReplacementReference"), FName("FNestedReplacementReference"),
						sizeof(FNestedReference), alignof(FNestedReference), EObjectFlags::NoFlags);
					Raw->Register(DStruct::StaticClass, "", "FNestedReplacementReference");
				}
				return Raw;
			};
			static const FStructParams Params{NoRegister, "FNestedReplacementReference", "FNestedReplacementReference",
				sizeof(FNestedReference), alignof(FNestedReference), Properties, std::size(Properties)};
			return ConstructDStruct(Params);
		}();
		return Type;
	}

	// Explicit reflection metadata keeps these disk-independent tests in CoreDObject.
	class DReplacementOwner : public DObject
	{
	public:
		explicit DReplacementOwner(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : DObject(Initializer) {}
		static auto __DefaultConstructor(const FObjectInitializer& X) -> void { new (X.GetObj()) DReplacementOwner(X); }
		static auto StaticClassNoRegister() -> DClass*
		{
			static DClass* Class = [] {
				auto* C = new DClass(EC_StaticConstructor, "DReplacementOwner", sizeof(DReplacementOwner), alignof(DReplacementOwner),
					EObjectFlags::NoFlags, EClassFlags::None, EClassCastFlags::DClass, &InternalConstructor<DReplacementOwner>);
				C->SetSuperStructBase(DObject::StaticClass());
				C->Register(DClass::StaticClass, "", "DReplacementOwner");
				return C;
			}();
			return Class;
		}
		static auto StaticClass() -> DClass*
		{
			static DClass* Class = [] {
				using namespace DurinCodeGen;
				static const auto Ref = FObjectPropertyParams::ObjectPtr<DObject>("Reference", EPropertyFlags::None,
					1, STRUCT_OFFSET_UINT16(DReplacementOwner, Reference), &DObject::StaticClass);
				static const auto Fixed = FObjectPropertyParams::ObjectPtr<DObject>("Fixed", EPropertyFlags::None,
					2, STRUCT_OFFSET_UINT16(DReplacementOwner, FixedReferences), &DObject::StaticClass);
				static const auto Inner = FObjectPropertyParams::ObjectPtr<DObject>("Inner", EPropertyFlags::None, 1, 0, &DObject::StaticClass);
				static const FArrayPropertyParams ListValue{"ListValue", EPropertyFlags::None, 1, 0, &Inner, &TArrayOpsResolver<List>::Get};
				static const FArrayPropertyParams NestedValue{"Nested", EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DReplacementOwner, NestedReferences), &ListValue, &TArrayOpsResolver<Nested>::Get};
				static const auto Key = FObjectPropertyParams::Raw<DObject>("Key", EPropertyFlags::None, 1, 0, &DObject::StaticClass);
				static const FMapPropertyParams MapValue{"Map", EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DReplacementOwner, MapReferences), &Key, &ListValue, &TMapOpsResolver<Map>::Get};
				static const FStructPropertyParams StructValue{"Struct", EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DReplacementOwner, StructReference), &NestedReferenceStruct};
				static const FPropertyParamsBase* Properties[]{&Ref, &Fixed, &NestedValue, &MapValue, &StructValue};
				static const FClassParams Params{&StaticClassNoRegister, "DReplacementOwner", "DReplacementOwner", Properties, std::size(Properties)};
				return ConstructDClass(Params);
			}();
			return Class;
		}
		auto AddReferencedObjects(FReferenceCollector& Collector) -> void override
		{
			DObject::AddReferencedObjects(Collector);
			Collector.AddReferencedObject(NativeReference);
		}
		Ptr Reference;
		Ptr FixedReferences[2];
		Nested NestedReferences;
		Map MapReferences;
		FNestedReference StructReference;
		DObject* NativeReference = nullptr;
	};

	// Owns a native cache and its optional strong handle through the retirement receipt.
	class FNativeParticipant final : public IObjectReplacementParticipant
	{
	public:
		DReplacementOwner* Owner = nullptr;
		DObject* Before = nullptr;
		DObject* After = nullptr;
		bool bReady = true;
		bool bReject = false;
		bool bAborted = false;
		FStrongObjectPtr Strong;
		FStrongObjectPtr PreparedStrong;
		auto Prepare(const FObjectReplacementMap& Mapping) -> FObjectReplacementResult override
		{
			Before = Owner->NativeReference;
			const auto* Entry = Mapping.Find(Before);
			if (bReject || !Entry || !Entry->Replacement) return {EObjectReplacementError::ParticipantRejected, "test rejection"};
			After = Entry->Replacement;
			if (Strong) PreparedStrong = FStrongObjectPtr(After);
			return {};
		}
		auto Validate() const -> bool override { return Owner->NativeReference == Before; }
		auto CoversNativeReferences(const DObject& Object) const -> bool override { return &Object == Owner; }
		auto GetStrongReferenceCount(const DObject& Object) const -> uint32 override { return Strong.Get() == &Object ? 1 : 0; }
		auto Commit() noexcept -> void override { Owner->NativeReference = After; Strong = std::move(PreparedStrong); }
		auto Abort() noexcept -> void override { bAborted = true; PreparedStrong.Reset(); }
		auto CanRetire() const -> bool override { return bReady; }
	};

	class FObjectGraphReplacementTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			(void)DReplacementOwner::StaticClass();
			Testing::RegisterMountPointForTests("/ReplacementTests/", Testing::GetTestWorkDirectory().generic_string() + "/");
			ASSERT_TRUE(FPackagePath::TryCreate("/ReplacementTests/Package", Path));
			Current = CreatePackage(Path);
			ASSERT_NE(Current, nullptr);
			Prepared = NewObject<DPackage>(nullptr, "Prepared");
			ASSERT_TRUE(Prepared->InitializePreparedAssetPackage(Path));
			Old = NewObject<DReplacementOwner>(Current, "Asset", EObjectFlags::Public);
			New = NewObject<DReplacementOwner>(Prepared, "Asset", EObjectFlags::Public);
			Owner = NewObject<DReplacementOwner>(nullptr, "Outside");
			Owner->Reference = Old;
			Pair = {Current, Prepared};
			Operation = std::make_unique<FObjectGraphReplacement>();
		}
		auto TearDown() -> void override
		{
			if (Operation)
			{
				Operation->Abort();
				(void)Operation->Retire();
				Operation.reset();
			}
			Owner.Reset();
			MarkObjectHierarchyAsGarbage(Current);
			MarkObjectHierarchyAsGarbage(Prepared);
			CollectGarbage();
		}
		auto Prepare() -> FObjectReplacementResult { return Operation->Prepare(std::span(&Pair, 1)); }
		Testing::FScopedMountRegistryFixture Mounts;
		FPackagePath Path;
		DPackage* Current = nullptr;
		DPackage* Prepared = nullptr;
		DReplacementOwner* Old = nullptr;
		DReplacementOwner* New = nullptr;
		TStrongObjectPtr<DReplacementOwner> Owner;
		FObjectReplacementPackagePair Pair;
		std::unique_ptr<FObjectGraphReplacement> Operation;
	};
}

TEST_F(FObjectGraphReplacementTests, IsolatesCandidateAndAtomicallyRebindsReferences)
{
	Owner->FixedReferences[0] = Old;
	Owner->FixedReferences[1] = Old;
	Owner->NestedReferences = {{Old}, {Old, Old}};
	Owner->MapReferences.emplace(Old, List{Old});
	Owner->StructReference.Reference = Old;
	New->Reference = Old;
	Current->MarkDirty();
	const auto OldHandle = MakeObjectHandle(Old);
	TWeakObjectPtr<DObject> Weak(Old);
	const auto Epoch = GetSoftObjectCacheEpoch();
	EXPECT_EQ(FindPackage(Path.GetView()), Current);
	const auto Visible = GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates);
	EXPECT_EQ(std::ranges::find(Visible, New), Visible.end());
	const auto Result = Prepare();
	ASSERT_TRUE(Result) << Result.Message;
	CollectGarbage();
	EXPECT_TRUE(IsValid(New));
	// GC with no membership changes preserves a prepared graph.
	ASSERT_TRUE(Operation->TryCommit());
	EXPECT_EQ(FindPackage(Path.GetView()), Prepared);
	EXPECT_EQ(Owner->Reference.Get(), New);
	EXPECT_EQ(Owner->FixedReferences[1].Get(), New);
	EXPECT_EQ(Owner->NestedReferences[1][1].Get(), New);
	EXPECT_EQ(Owner->MapReferences.count(Old), 0u);
	ASSERT_EQ(Owner->MapReferences.count(New), 1u);
	EXPECT_EQ(Owner->MapReferences.at(New)[0].Get(), New);
	EXPECT_EQ(Owner->StructReference.Reference.Get(), New);
	EXPECT_EQ(New->Reference.Get(), New);
	EXPECT_TRUE(Current->IsDirty());
	EXPECT_GT(GetSoftObjectCacheEpoch(), Epoch);
	EXPECT_EQ(ResolveObjectHandle(OldHandle), Old);
	EXPECT_TRUE(Operation->Retire());
	CollectGarbage();
	EXPECT_FALSE(Weak.IsValid());
	EXPECT_EQ(ResolveObjectHandle(OldHandle), nullptr);
}

TEST_F(FObjectGraphReplacementTests, AbortPreservesRegistrationReferencesAndDirty)
{
	Current->MarkDirty();
	ASSERT_TRUE(Prepare());
	Operation->Abort();
	EXPECT_EQ(FindPackage(Path.GetView()), Current);
	EXPECT_EQ(Owner->Reference.Get(), Old);
	EXPECT_TRUE(Current->IsDirty());
	EXPECT_TRUE(New->IsGarbage());
}

TEST_F(FObjectGraphReplacementTests, RejectsUnmappedExternalStrongReference)
{
	auto* Removed = NewObject<DObject>(Old, "Removed");
	Owner->Reference = Removed;
	EXPECT_EQ(Prepare().Error, EObjectReplacementError::UnmappedReference);
	EXPECT_EQ(Owner->Reference.Get(), Removed);
	EXPECT_EQ(FindPackage(Path.GetView()), Current);
}

TEST_F(FObjectGraphReplacementTests, RetiresUnreferencedRemovedChildAndMapsOuterPaths)
{
	auto* Child = NewObject<DObject>(Old, "Child");
	auto* ReplacementChild = NewObject<DObject>(New, "Child");
	auto* Removed = NewObject<DObject>(Old, "Removed");
	Owner->Reference = Child;
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Operation->TryCommit());
	EXPECT_EQ(Owner->Reference.Get(), ReplacementChild);
	EXPECT_TRUE(Operation->Retire());
	EXPECT_TRUE(Removed->IsGarbage());
}

TEST_F(FObjectGraphReplacementTests, RejectsMapKeyCollisionWithoutChangingIndex)
{
	Owner->MapReferences.emplace(Old, List{Old});
	Owner->MapReferences.emplace(New, List{New});
	EXPECT_EQ(Prepare().Error, EObjectReplacementError::MapCollision);
	EXPECT_EQ(Owner->MapReferences.size(), 2u);
	EXPECT_EQ(Owner->MapReferences.at(Old)[0].Get(), Old);
}

TEST_F(FObjectGraphReplacementTests, RejectsNewReferenceAndContainerChangesAfterPreparation)
{
	Owner->NestedReferences = {{Old}};
	ASSERT_TRUE(Prepare());
	Owner->NestedReferences[0].push_back(Old);
	EXPECT_EQ(Operation->TryCommit().Error, EObjectReplacementError::Stale);
	EXPECT_EQ(FindPackage(Path.GetView()), Current);
	EXPECT_EQ(Owner->Reference.Get(), Old);
}

TEST_F(FObjectGraphReplacementTests, RejectsNewOwnerAfterPreparation)
{
	ASSERT_TRUE(Prepare());
	auto* Late = NewObject<DReplacementOwner>(nullptr, "LateOwner");
	Late->Reference = Old;
	EXPECT_EQ(Operation->TryCommit().Error, EObjectReplacementError::Stale);
	EXPECT_EQ(Late->Reference.Get(), Old);
}

TEST_F(FObjectGraphReplacementTests, RejectsUnsupportedNativeAndStrongOwners)
{
	Owner->NativeReference = Old;
	EXPECT_EQ(Prepare().Error, EObjectReplacementError::Unsupported);
	Operation = std::make_unique<FObjectGraphReplacement>();
	Owner->NativeReference = nullptr;
	FStrongObjectPtr Unknown(Old);
	EXPECT_EQ(Prepare().Error, EObjectReplacementError::Unsupported);
}

TEST_F(FObjectGraphReplacementTests, NativeParticipantRebindsAndDefersRetirement)
{
	Owner->NativeReference = Old;
	auto Participant = std::make_shared<FNativeParticipant>();
	Participant->Owner = Owner.Get();
	Participant->bReady = false;
	const std::array<std::shared_ptr<IObjectReplacementParticipant>, 1> Participants{Participant};
	ASSERT_TRUE(Operation->Prepare(std::span(&Pair, 1), Participants));
	ASSERT_TRUE(Operation->TryCommit());
	EXPECT_EQ(Owner->NativeReference, New);
	EXPECT_FALSE(Operation->Retire());
	EXPECT_FALSE(Old->IsGarbage());
	Participant->bReady = true;
	EXPECT_TRUE(Operation->Retire());
}

TEST_F(FObjectGraphReplacementTests, ParticipantFailureAbortsBeforePublishing)
{
	Owner->NativeReference = Old;
	auto Participant = std::make_shared<FNativeParticipant>();
	Participant->Owner = Owner.Get();
	Participant->bReject = true;
	const std::array<std::shared_ptr<IObjectReplacementParticipant>, 1> Participants{Participant};
	EXPECT_EQ(Operation->Prepare(std::span(&Pair, 1), Participants).Error, EObjectReplacementError::ParticipantRejected);
	EXPECT_TRUE(Participant->bAborted);
	EXPECT_EQ(Owner->NativeReference, Old);
	EXPECT_EQ(FindPackage(Path.GetView()), Current);
}

TEST_F(FObjectGraphReplacementTests, RejectsBudgetBeforePublication)
{
	EXPECT_EQ(Operation->Prepare(std::span(&Pair, 1), {}, {.MaximumObjects = 1}).Error,
		EObjectReplacementError::BudgetExceeded);
	EXPECT_EQ(FindPackage(Path.GetView()), Current);
}

TEST_F(FObjectGraphReplacementTests, RejectsIncompatibleChildTypeBeforeAnyWrite)
{
	NewObject<DReplacementOwner>(Old, "Child");
	NewObject<DObject>(New, "Child");
	EXPECT_EQ(Prepare().Error, EObjectReplacementError::IncompatibleType);
	EXPECT_EQ(FindPackage(Path.GetView()), Current);
	EXPECT_EQ(Owner->Reference.Get(), Old);
}

TEST_F(FObjectGraphReplacementTests, RejectsChangedExistingOwnerAndDirtyRevision)
{
	ASSERT_TRUE(Prepare());
	Owner->Reference = nullptr;
	EXPECT_EQ(Operation->TryCommit().Error, EObjectReplacementError::Stale);
	Owner->Reference = Old;
	Current->MarkDirty();
	EXPECT_EQ(Operation->TryCommit().Error, EObjectReplacementError::Stale);
	EXPECT_EQ(FindPackage(Path.GetView()), Current);
}

TEST_F(FObjectGraphReplacementTests, ClaimedStrongOwnerMovesPreparedHandleWithoutAllocation)
{
	Owner->NativeReference = Old;
	auto Participant = std::make_shared<FNativeParticipant>();
	Participant->Owner = Owner.Get();
	Participant->Strong = FStrongObjectPtr(Old);
	const std::array<std::shared_ptr<IObjectReplacementParticipant>, 1> Participants{Participant};
	ASSERT_TRUE(Operation->Prepare(std::span(&Pair, 1), Participants));
	ASSERT_TRUE(Operation->TryCommit());
	EXPECT_EQ(Participant->Strong.Get(), New);
	EXPECT_TRUE(Operation->Retire());
}

TEST_F(FObjectGraphReplacementTests, LateManualRootPreventsCommitAndRetirement)
{
	ASSERT_TRUE(Prepare());
	AddToRoot(Old);
	EXPECT_EQ(Operation->TryCommit().Error, EObjectReplacementError::Stale);
	RemoveFromRoot(Old);
	ASSERT_TRUE(Operation->TryCommit());
	AddToRoot(Old);
	EXPECT_FALSE(Operation->Retire());
	RemoveFromRoot(Old);
	EXPECT_TRUE(Operation->Retire());
}

TEST_F(FObjectGraphReplacementTests, ReusedSlotsNeverRedirectOldWeakHandles)
{
	const auto OldHandle = MakeObjectHandle(Old);
	TWeakObjectPtr<DObject> Weak(Old);
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Operation->TryCommit());
	ASSERT_TRUE(Operation->Retire());
	CollectGarbage();
	bool Reused = false;
	for (uint32 I = 0; I < 16; ++I)
	{
		auto* Added = NewObject<DObject>(Prepared, FName(std::format("Reused{}", I).c_str()));
		const auto NewHandle = MakeObjectHandle(Added);
		if (NewHandle.Index == OldHandle.Index)
		{
			Reused = true;
			EXPECT_NE(NewHandle.Generation, OldHandle.Generation);
		}
	}
	EXPECT_TRUE(Reused);
	EXPECT_FALSE(Weak.IsValid());
	EXPECT_EQ(ResolveObjectHandle(OldHandle), nullptr);
}

TEST_F(FObjectGraphReplacementTests, RetirementWaitsForLateReflectedAndNativeReferences)
{
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Operation->TryCommit());
	Owner->Reference = Old;
	EXPECT_FALSE(Operation->Retire());
	Owner->Reference = New;
	Owner->NativeReference = Old;
	EXPECT_FALSE(Operation->Retire());
	Owner->NativeReference = nullptr;
	EXPECT_TRUE(Operation->Retire());
}

TEST_F(FObjectGraphReplacementTests, CrossPackageCyclesPublishTogetherAndRejectStaleBatch)
{
	FPackagePath OtherPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ReplacementTests/Other", OtherPath));
	auto* OtherCurrent = CreatePackage(OtherPath);
	auto* OtherPrepared = NewObject<DPackage>(nullptr, "OtherPrepared");
	ASSERT_TRUE(OtherPrepared->InitializePreparedAssetPackage(OtherPath));
	auto* OtherOld = NewObject<DReplacementOwner>(OtherCurrent, "Asset");
	auto* OtherNew = NewObject<DReplacementOwner>(OtherPrepared, "Asset");
	Old->Reference = OtherOld;
	OtherOld->Reference = Old;
	New->Reference = OtherOld;
	OtherNew->Reference = Old;
	const std::array Pairs{Pair, FObjectReplacementPackagePair{OtherCurrent, OtherPrepared}};
	ASSERT_TRUE(Operation->Prepare(Pairs));
	OtherCurrent->MarkDirty();
	EXPECT_EQ(Operation->TryCommit().Error, EObjectReplacementError::Stale);
	EXPECT_EQ(FindPackage(Path.GetView()), Current);
	EXPECT_EQ(FindPackage(OtherPath.GetView()), OtherCurrent);
	Operation->Abort();
	// Abort owns both candidates. Recreate them for a fresh batch.
	Prepared = NewObject<DPackage>(nullptr, "PreparedAgain");
	ASSERT_TRUE(Prepared->InitializePreparedAssetPackage(Path));
	OtherPrepared = NewObject<DPackage>(nullptr, "OtherPreparedAgain");
	ASSERT_TRUE(OtherPrepared->InitializePreparedAssetPackage(OtherPath));
	New = NewObject<DReplacementOwner>(Prepared, "Asset");
	OtherNew = NewObject<DReplacementOwner>(OtherPrepared, "Asset");
	New->Reference = OtherOld;
	OtherNew->Reference = Old;
	Operation = std::make_unique<FObjectGraphReplacement>();
	const std::array FreshPairs{FObjectReplacementPackagePair{Current, Prepared}, FObjectReplacementPackagePair{OtherCurrent, OtherPrepared}};
	ASSERT_TRUE(Operation->Prepare(FreshPairs));
	ASSERT_TRUE(Operation->TryCommit());
	EXPECT_EQ(New->Reference.Get(), OtherNew);
	EXPECT_EQ(OtherNew->Reference.Get(), New);
	EXPECT_EQ(FindPackage(OtherPath.GetView()), OtherPrepared);
	EXPECT_TRUE(Operation->Retire());
	MarkObjectHierarchyAsGarbage(OtherPrepared);
}
