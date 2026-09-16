#include "MaterialTestSupport.h"
#include "ObjectCacheContext.h"

namespace
{
	auto LegacyDependents(const Durin::DMaterialInterface* Root) -> std::vector<Durin::FObjectKey>
	{
		std::vector<Durin::FObjectKey> Result;
		for (auto* Object : Durin::GDObjectArray.Snapshot(Durin::EObjectQueryScope::LiveOnly))
			if (auto* Material = Durin::Cast<Durin::DMaterialInterface>(Object);
				Durin::IsValid(Material) && Material->IsDependent(Root)) Result.emplace_back(Material);
		std::ranges::sort(Result);
		return Result;
	}
	auto HandleEquals(Durin::FObjectKey Left, Durin::FObjectKey Right) -> bool
	{
		return Left == Right;
	}

	auto ContainsHandle(
		std::span<const Durin::FObjectKey> Handles,
		Durin::FObjectKey Expected
	) -> bool
	{
		return std::ranges::find_if(Handles, [Expected](Durin::FObjectKey Handle) {
			return HandleEquals(Handle, Expected);
		}) != Handles.end();
	}

	auto IsSortedByHandle(std::span<const Durin::FObjectKey> Handles) -> bool
	{
		return std::ranges::is_sorted(Handles, [](Durin::FObjectKey Left, Durin::FObjectKey Right) {
			return Left < Right;
		});
	}
}

TEST(FMaterialDependencyTests, ScopedQueriesMatchLegacyAndBuildOnce)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "CacheBase");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CacheChild");
	auto* Leaf = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CacheLeaf");
	ASSERT_TRUE(Child->SetParent(Base));
	ASSERT_TRUE(Leaf->SetParent(Child));
	const auto Expected = LegacyDependents(Base);
	{
		Durin::FObjectCacheContext Context;
		EXPECT_EQ(Context.GetDiagnostics().SnapshotCount, 0u);
		const auto First = Context.GetMaterialsAffectedByMaterial(Base);
		std::vector<Durin::FObjectKey> Actual;
		for (auto* Object : First) Actual.emplace_back(Object);
		EXPECT_EQ(Actual, Expected);
		Durin::DMaterialInterface* Roots[] = {Base, Child, Leaf, Base, nullptr};
		Actual.clear();
		for (auto* Object : Context.GetMaterialsAffectedByMaterials(Roots)) Actual.emplace_back(Object);
		EXPECT_EQ(Actual, Expected);
		Actual.clear();
		for (auto* Object : Context.GetDirectMaterialChildren(Base)) Actual.emplace_back(Object);
		EXPECT_EQ(Actual, Durin::GetLoadedDirectMaterialChildren(Base));
		EXPECT_EQ(Context.GetDiagnostics().QueryCount, 3u);
		EXPECT_EQ(Context.GetDiagnostics().SnapshotCount, 1u);
		EXPECT_EQ(Context.GetDiagnostics().ParentTableBuildCount, 1u);
		Actual.clear();
		for (auto* Object : First) Actual.emplace_back(Object);
		EXPECT_EQ(Actual, Expected); // Later queries cannot replace owned result storage.
		Context.EndDiscovery();
		Durin::MarkAsGarbage(Leaf);
		Durin::CollectGarbage();
		EXPECT_TRUE(Durin::GDObjectArray.Contains(Leaf));
		Actual.clear();
		for (auto* Object : First) Actual.emplace_back(Object);
		EXPECT_EQ(Actual.size(), 2u); // Pending kill is filtered while physical deletion is deferred.
	}
	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialDependencyTests, ScopedQueriesPreserveExcludedAncestorsAndCorruptCycles)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "CacheCycleBase");
	auto* Middle = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CacheCycleMiddle");
	auto* Leaf = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CacheCycleLeaf");
	ASSERT_TRUE(Middle->SetParent(Base));
	ASSERT_TRUE(Leaf->SetParent(Middle));
	Durin::MarkAsGarbage(Middle);
	{
		Durin::FObjectCacheContext Context;
		std::vector<Durin::FObjectKey> Actual;
		for (auto* Object : Context.GetMaterialsAffectedByMaterial(Base)) Actual.emplace_back(Object);
		EXPECT_EQ(Actual, LegacyDependents(Base));
		EXPECT_TRUE(ContainsHandle(Actual, Durin::FObjectKey(Leaf)));
	}
	auto* Parent = static_cast<Durin::FObjectProperty*>(Leaf->GetClass()->FindPropertyByName("Parent"));
	Parent->SetObjectPropertyValue(Middle, Leaf);
	{
		Durin::FObjectCacheContext Context;
		std::vector<Durin::FObjectKey> Actual;
		for (auto* Object : Context.GetMaterialsAffectedByMaterial(Leaf)) Actual.emplace_back(Object);
		EXPECT_EQ(Actual, LegacyDependents(Leaf));
	}
	Durin::MarkAsGarbage(Leaf);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialDependencyTests, MultipleRootUpdateBaseline)
{
	InitializeDObjectSystem();
	auto* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "BatchBaselineFirst");
	auto* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "BatchBaselineSecond");
	Durin::ResetMaterialLoadedQueryDiagnostics();
	First->PostEditChangeProperty({});
	Second->PostEditChangeProperty({});
	const auto Counts = Durin::GetMaterialLoadedQueryDiagnostics();
	EXPECT_EQ(Counts.QueryCount, 2u);
	EXPECT_EQ(Counts.SnapshotCount, 2u);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
}

TEST(FMaterialDependencyTests, CanonicalParentChainDefinesDependencySemantics)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "DependencyBase");
	Durin::DMaterial* Unrelated = Durin::NewObject<Durin::DMaterial>(nullptr, "DependencyUnrelated");
	Durin::DMaterialInstance* Direct = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "DependencyDirect");
	Durin::DMaterialInstance* Transitive = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "DependencyTransitive");
	ASSERT_TRUE(Direct->SetParent(Base));
	ASSERT_TRUE(Transitive->SetParent(Direct));

	EXPECT_FALSE(Base->IsDependent(nullptr));
	EXPECT_TRUE(Base->IsDependent(Base));
	EXPECT_FALSE(Base->IsDependent(Direct));
	EXPECT_TRUE(Direct->IsDependent(Direct));
	EXPECT_TRUE(Direct->IsDependent(Base));
	EXPECT_TRUE(Transitive->IsDependent(Direct));
	EXPECT_TRUE(Transitive->IsDependent(Base));
	EXPECT_FALSE(Transitive->IsDependent(Unrelated));

	Durin::MarkAsGarbage(Transitive);
	Durin::MarkAsGarbage(Direct);
	Durin::MarkAsGarbage(Unrelated);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialDependencyTests, CorruptParentCycleTerminatesDependencyQueries)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Unrelated = Durin::NewObject<Durin::DMaterial>(nullptr, "CycleGuardUnrelated");
	Durin::DMaterialInstance* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CycleGuardFirst");
	Durin::DMaterialInstance* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CycleGuardSecond");
	Durin::DMaterialInstance* Third = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CycleGuardThird");
	Durin::FProperty* ParentProperty = First->GetClass()->FindPropertyByName("Parent");
	ASSERT_NE(ParentProperty, nullptr);
	auto* ObjectParentProperty = static_cast<Durin::FObjectProperty*>(ParentProperty);
	ObjectParentProperty->SetObjectPropertyValue(First, Second);
	ObjectParentProperty->SetObjectPropertyValue(Second, First);

	EXPECT_TRUE(First->IsDependent(First));
	EXPECT_TRUE(First->IsDependent(Second));
	EXPECT_FALSE(First->IsDependent(Unrelated));
	EXPECT_FALSE(Second->IsDependent(Unrelated));
	EXPECT_FALSE(Third->SetParent(First));
	EXPECT_EQ(Third->GetParent(), nullptr);
	ObjectParentProperty->SetObjectPropertyValue(Third, First);
	Third->PostLoad();
	EXPECT_EQ(Third->GetParent(), nullptr);
	EXPECT_FALSE(Third->IsDependent(First));
	EXPECT_NE(&Third->GetStaticProperties(), nullptr);

	ObjectParentProperty->SetObjectPropertyValue(First, nullptr);
	ObjectParentProperty->SetObjectPropertyValue(Second, nullptr);
	ObjectParentProperty->SetObjectPropertyValue(Third, nullptr);
	Durin::MarkAsGarbage(Third);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::MarkAsGarbage(Unrelated);
	Durin::CollectGarbage();
}

TEST(FMaterialDependencyTests, LoadedQueriesSeparateDirectChildrenFromTransitiveDependents)
{
	InitializeDObjectSystem();
	Durin::ResetMaterialLoadedQueryDiagnostics();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "LoadedQueryBase");
	Durin::DMaterial* Unrelated = Durin::NewObject<Durin::DMaterial>(nullptr, "LoadedQueryUnrelated");
	Durin::DMaterialInstance* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "LoadedQueryFirst");
	Durin::DMaterialInstance* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "LoadedQuerySecond");
	Durin::DMaterialInstance* Other = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "LoadedQueryOther");
	ASSERT_TRUE(First->SetParent(Base));
	ASSERT_TRUE(Second->SetParent(First));
	ASSERT_TRUE(Other->SetParent(Unrelated));
	Durin::ResetMaterialLoadedQueryDiagnostics();

	const std::vector<Durin::FObjectKey> Direct = Durin::GetLoadedDirectMaterialChildren(Base);
	const Durin::FMaterialLoadedQueryDiagnostics DirectDiagnostics =
		Durin::GetMaterialLoadedQueryDiagnostics();
	ASSERT_EQ(Direct.size(), 1);
	EXPECT_EQ(DirectDiagnostics.LastOperation, Durin::EMaterialLoadedQueryOperation::DirectChildren);
	EXPECT_EQ(DirectDiagnostics.QueryCount, 1);
	EXPECT_EQ(DirectDiagnostics.SnapshotCount, 1);
	EXPECT_GE(DirectDiagnostics.ScannedMaterialCount, 5);
	EXPECT_EQ(DirectDiagnostics.LastResultCount, Direct.size());
	EXPECT_TRUE(HandleEquals(Direct.front(), Durin::FObjectKey(First)));
	EXPECT_FALSE(ContainsHandle(Direct, Durin::FObjectKey(Base)));
	EXPECT_FALSE(ContainsHandle(Direct, Durin::FObjectKey(Second)));

	const std::vector<Durin::FObjectKey> Dependents = Durin::GetLoadedMaterialDependents(Base);
	const Durin::FMaterialLoadedQueryDiagnostics DependentDiagnostics =
		Durin::GetMaterialLoadedQueryDiagnostics();
	EXPECT_EQ(Dependents.size(), 3);
	EXPECT_EQ(DependentDiagnostics.LastOperation, Durin::EMaterialLoadedQueryOperation::Dependents);
	EXPECT_EQ(DependentDiagnostics.QueryCount, 2);
	EXPECT_EQ(DependentDiagnostics.SnapshotCount, 2);
	EXPECT_EQ(DependentDiagnostics.LastResultCount, Dependents.size());
	EXPECT_TRUE(ContainsHandle(Dependents, Durin::FObjectKey(Base)));
	EXPECT_TRUE(ContainsHandle(Dependents, Durin::FObjectKey(First)));
	EXPECT_TRUE(ContainsHandle(Dependents, Durin::FObjectKey(Second)));
	EXPECT_FALSE(ContainsHandle(Dependents, Durin::FObjectKey(Other)));
	EXPECT_TRUE(IsSortedByHandle(Dependents));

	EXPECT_TRUE(Durin::GetLoadedDirectMaterialChildren(nullptr).empty());
	EXPECT_TRUE(Durin::GetLoadedMaterialDependents(nullptr).empty());

	Durin::MarkAsGarbage(Other);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::MarkAsGarbage(Unrelated);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialDependencyTests, InstancePropertyEditPublishesDependentsOnce)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "EditNotificationBase");
	Base->SetEditCompileMode(Durin::EMaterialEditCompileMode::Manual);
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "EditNotificationInstance");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "EditNotificationChild");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Child->SetParent(Instance));

	auto ExpectSingleNotification = [&](Durin::FProperty* Property, uint64 ExpectedQueries) {
		const auto InstanceVersion = Instance->GetRenderStateVersion();
		const auto ChildVersion = Child->GetRenderStateVersion();
		const auto BaseVersion = Base->GetRenderStateVersion();
		Durin::ResetMaterialLoadedQueryDiagnostics();
		Instance->PostEditChangeProperty({.MemberProperty = Property});
		EXPECT_EQ(Instance->GetRenderStateVersion(), InstanceVersion + 1);
		EXPECT_EQ(Child->GetRenderStateVersion(), ChildVersion + 1);
		EXPECT_EQ(Base->GetRenderStateVersion(), BaseVersion);
		const auto Diagnostics = Durin::GetMaterialLoadedQueryDiagnostics();
		EXPECT_EQ(Diagnostics.QueryCount, ExpectedQueries);
		EXPECT_EQ(Diagnostics.SnapshotCount, 1u);
		EXPECT_EQ(Diagnostics.ParentTableBuildCount, 1u);
	};

	auto* ParentProperty = Instance->GetClass()->FindPropertyByName("Parent");
	auto* OverridesProperty = Instance->GetClass()->FindPropertyByName("PropertyOverrides");
	auto* ScalarProperty = Instance->GetClass()->FindPropertyByName("ScalarParameterValues");
	ASSERT_NE(ParentProperty, nullptr);
	ASSERT_NE(OverridesProperty, nullptr);
	ASSERT_NE(ScalarProperty, nullptr);
	const auto Revision = Instance->GetMaterialCompileStatus().AuthoredRevision;
	const auto ChildRevision = Child->GetMaterialCompileStatus().AuthoredRevision;
	// Static edits share one snapshot between compilation and publication queries.
	ExpectSingleNotification(ParentProperty, 2);
	EXPECT_EQ(Instance->GetMaterialCompileStatus().AuthoredRevision, Revision + 1);
	EXPECT_EQ(Child->GetMaterialCompileStatus().AuthoredRevision, ChildRevision + 1);
	auto* Overrides = OverridesProperty->ContainerPtrToValuePtr<Durin::FMaterialPropertyOverrides>(Instance);
	Overrides->bOverrideShadingModel = true;
	Overrides->Values.ShadingModel = Durin::EMaterialShadingModel::Unlit;
	ExpectSingleNotification(OverridesProperty, 2);
	EXPECT_EQ(Instance->GetMaterialCompileStatus().AuthoredRevision, Revision + 2);
	EXPECT_EQ(Child->GetMaterialCompileStatus().AuthoredRevision, ChildRevision + 2);
	EXPECT_EQ(Child->GetStaticProperties().ShadingModel, Durin::EMaterialShadingModel::Unlit);
	const auto DynamicRevision = Instance->GetMaterialCompileStatus().AuthoredRevision;
	ExpectSingleNotification(ScalarProperty, 1);
	ExpectSingleNotification(nullptr, 1);
	EXPECT_EQ(Instance->GetMaterialCompileStatus().AuthoredRevision, DynamicRevision);

	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialDependencyTests, ParameterNotificationsFollowCompleteDependentPublication)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "NotificationOrderBase");
	Base->SetEditCompileMode(Durin::EMaterialEditCompileMode::Manual);
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "NotificationOrderInstance");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "NotificationOrderChild");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Child->SetParent(Instance));
	const auto InstanceVersion = Instance->GetRenderStateVersion();
	const auto ChildVersion = Child->GetRenderStateVersion();
	const auto BaseVersion = Base->GetRenderStateVersion();
	uint32 InstanceNotifications = 0, ChildNotifications = 0, BaseNotifications = 0;
	auto CheckPublished = [&] {
		EXPECT_EQ(Instance->GetRenderStateVersion(), InstanceVersion + 1);
		EXPECT_EQ(Child->GetRenderStateVersion(), ChildVersion + 1);
		EXPECT_EQ(Base->GetRenderStateVersion(), BaseVersion);
	};
	const auto InstanceHandle = Instance->GetParameterChanges().AddLambda([&] {
		++InstanceNotifications;
		CheckPublished();
	});
	const auto ChildHandle = Child->GetParameterChanges().AddLambda([&] {
		++ChildNotifications;
		CheckPublished();
	});
	const auto BaseHandle = Base->GetParameterChanges().AddLambda([&] { ++BaseNotifications; });
	Durin::ResetMaterialLoadedQueryDiagnostics();
	Instance->PostEditChangeProperty({});
	EXPECT_EQ(InstanceNotifications, 1u);
	EXPECT_EQ(ChildNotifications, 1u);
	EXPECT_EQ(BaseNotifications, 0u);
	EXPECT_EQ(Durin::GetMaterialLoadedQueryDiagnostics().QueryCount, 1u);
	EXPECT_EQ(Durin::GetMaterialLoadedQueryDiagnostics().SnapshotCount, 1u);
	Instance->GetParameterChanges().Remove(InstanceHandle);
	Child->GetParameterChanges().Remove(ChildHandle);
	Base->GetParameterChanges().Remove(BaseHandle);
	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialDependencyTests, LoadedQueriesFilterGarbageAndReturnGenerationSafeHandles)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "FilteredQueryBase");
	Durin::DMaterialInstance* Live = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FilteredQueryLive");
	Durin::DMaterialInstance* Garbage = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FilteredQueryGarbage");
	ASSERT_TRUE(Live->SetParent(Base));
	ASSERT_TRUE(Garbage->SetParent(Base));
	const Durin::FObjectKey GarbageHandle = Durin::FObjectKey(Garbage);
	Durin::MarkAsGarbage(Garbage);

	const std::vector<Durin::FObjectKey> Direct = Durin::GetLoadedDirectMaterialChildren(Base);
	ASSERT_EQ(Direct.size(), 1);
	EXPECT_TRUE(HandleEquals(Direct.front(), Durin::FObjectKey(Live)));
	EXPECT_FALSE(ContainsHandle(Direct, GarbageHandle));

	Durin::AddToRoot(Base);
	Durin::AddToRoot(Live);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectKey(GarbageHandle), nullptr);
	Durin::DMaterialInstance* Replacement = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FilteredQueryReplacement");
	const Durin::FObjectKey ReplacementHandle = Durin::FObjectKey(Replacement);
	EXPECT_NE(ReplacementHandle, GarbageHandle);
	EXPECT_EQ(Durin::ResolveObjectKey(GarbageHandle), nullptr);

	Durin::RemoveFromRoot(Live);
	Durin::RemoveFromRoot(Base);
	Durin::MarkAsGarbage(Replacement);
	Durin::MarkAsGarbage(Live);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialDependencyTests, ReentrantParentEditUsesFreshBatchAndCompletesSynchronously)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "ReentrantBase");
	auto* Other = Durin::NewObject<Durin::DMaterial>(nullptr, "ReentrantOther");
	Base->SetEditCompileMode(Durin::EMaterialEditCompileMode::Manual);
	Other->SetEditCompileMode(Durin::EMaterialEditCompileMode::Manual);
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "ReentrantInstance");
	auto* Leaf = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "ReentrantLeaf");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Leaf->SetParent(Instance));
	const auto Before = Leaf->GetRenderStateVersion();
	uint32 LeafNotifications = 0;
	const auto LeafListener = Leaf->GetParameterChanges().AddLambda([&] { ++LeafNotifications; });
	const auto Listener = Instance->GetParameterChanges().AddLambda([&] {
		EXPECT_EQ(Leaf->GetRenderStateVersion(), Before + 1);
		EXPECT_TRUE(Leaf->SetParent(Other));
		EXPECT_EQ(Leaf->GetParent(), Other);
		EXPECT_EQ(Leaf->GetRenderStateVersion(), Before + 2);
		Durin::CollectGarbage();
		EXPECT_TRUE(Durin::GDObjectArray.Contains(Leaf));
	});
	Durin::ResetMaterialLoadedQueryDiagnostics();
	Instance->PostEditChangeProperty({});
	EXPECT_EQ(LeafNotifications, 2u); // Nested publication, then the outer prepared notification.
	EXPECT_EQ(Durin::GetMaterialLoadedQueryDiagnostics().SnapshotCount, 2u);
	EXPECT_EQ(Durin::GetMaterialLoadedQueryDiagnostics().ParentTableBuildCount, 2u);
	Instance->GetParameterChanges().Remove(Listener);
	Instance->PostEditChangeProperty({});
	EXPECT_EQ(LeafNotifications, 2u); // The next batch sees the new canonical parent.
	Leaf->GetParameterChanges().Remove(LeafListener);
	Durin::MarkAsGarbage(Leaf);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Other);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialDependencyTests, NotificationMayRetireLaterRecipientWithoutDanglingPointers)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "RetireNotificationBase");
	Base->SetEditCompileMode(Durin::EMaterialEditCompileMode::Manual);
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "RetireNotificationInstance");
	auto* Leaf = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "RetireNotificationLeaf");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Leaf->SetParent(Instance));
	const Durin::FObjectKey LeafKey(Leaf);
	uint32 LeafNotifications = 0;
	Leaf->GetParameterChanges().AddLambda([&] { ++LeafNotifications; });
	const auto Listener = Instance->GetParameterChanges().AddLambda([&] {
		Durin::MarkAsGarbage(Leaf);
		Durin::CollectGarbage();
		EXPECT_TRUE(Durin::GDObjectArray.Contains(Leaf));
	});
	Instance->PostEditChangeProperty({});
	EXPECT_EQ(LeafNotifications, 0u);
	Instance->GetParameterChanges().Remove(Listener);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::GDObjectArray.Resolve(LeafKey), nullptr);
}
TEST(FMaterialDependencyTests, ImmediateCompilationSharesDiscoveryAcrossOwners)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "SharedCompileBase");
	auto* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "SharedCompileFirst");
	auto* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "SharedCompileSecond");
	ASSERT_TRUE(First->SetParent(Base));
	ASSERT_TRUE(Second->SetParent(Base));
	Durin::ResetMaterialLoadedQueryDiagnostics();
	ASSERT_TRUE(Base->CompileEdits());
	const auto Counts = Durin::GetMaterialLoadedQueryDiagnostics();
	EXPECT_GE(Counts.QueryCount, 2u);
	EXPECT_EQ(Counts.SnapshotCount, 1u);
	EXPECT_EQ(Counts.ParentTableBuildCount, 1u);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialDependencyTests, DependencyCacheDoesNotApplyPropertyResolutionDepthLimit)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "DeepQueryBase");
	std::vector<Durin::DMaterialInterface*> Chain{Base};
	for (uint32 I = 0; I < Durin::MaterialMaximumParentDepth + 2; ++I)
	{
		auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, Durin::FName(std::format("DeepQuery{}", I).c_str()));
		auto* Parent = static_cast<Durin::FObjectProperty*>(Child->GetClass()->FindPropertyByName("Parent"));
		Parent->SetObjectPropertyValue(Child, Chain.back());
		Chain.push_back(Child);
	}
	{
		Durin::FObjectCacheContext Context;
		std::vector<Durin::FObjectKey> Actual;
		for (auto* Object : Context.GetMaterialsAffectedByMaterial(Base)) Actual.emplace_back(Object);
		EXPECT_EQ(Actual, LegacyDependents(Base));
		EXPECT_EQ(Actual.size(), Chain.size());
	}
	for (auto* Object : Chain) Durin::MarkAsGarbage(Object);
	Durin::CollectGarbage();
}