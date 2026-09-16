#include "MaterialTestSupport.h"

namespace
{
	auto HandleEquals(Durin::FObjectHandle Left, Durin::FObjectHandle Right) -> bool
	{
		return Left.Index == Right.Index && Left.Generation == Right.Generation;
	}

	auto ContainsHandle(
		std::span<const Durin::FObjectHandle> Handles,
		Durin::FObjectHandle Expected
	) -> bool
	{
		return std::ranges::find_if(Handles, [Expected](Durin::FObjectHandle Handle) {
			return HandleEquals(Handle, Expected);
		}) != Handles.end();
	}

	auto IsSortedByHandle(std::span<const Durin::FObjectHandle> Handles) -> bool
	{
		return std::ranges::is_sorted(Handles, [](Durin::FObjectHandle Left, Durin::FObjectHandle Right) {
			return Left.Index < Right.Index
				|| (Left.Index == Right.Index && Left.Generation < Right.Generation);
		});
	}
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

	const std::vector<Durin::FObjectHandle> Direct = Durin::GetLoadedDirectMaterialChildren(Base);
	const Durin::FMaterialLoadedQueryDiagnostics DirectDiagnostics =
		Durin::GetMaterialLoadedQueryDiagnostics();
	ASSERT_EQ(Direct.size(), 1);
	EXPECT_EQ(DirectDiagnostics.LastOperation, Durin::EMaterialLoadedQueryOperation::DirectChildren);
	EXPECT_EQ(DirectDiagnostics.QueryCount, 1);
	EXPECT_EQ(DirectDiagnostics.SnapshotCount, 1);
	EXPECT_GE(DirectDiagnostics.ScannedMaterialCount, 5);
	EXPECT_EQ(DirectDiagnostics.LastResultCount, Direct.size());
	EXPECT_TRUE(HandleEquals(Direct.front(), Durin::MakeObjectHandle(First)));
	EXPECT_FALSE(ContainsHandle(Direct, Durin::MakeObjectHandle(Base)));
	EXPECT_FALSE(ContainsHandle(Direct, Durin::MakeObjectHandle(Second)));

	const std::vector<Durin::FObjectHandle> Dependents = Durin::GetLoadedMaterialDependents(Base);
	const Durin::FMaterialLoadedQueryDiagnostics DependentDiagnostics =
		Durin::GetMaterialLoadedQueryDiagnostics();
	EXPECT_EQ(Dependents.size(), 3);
	EXPECT_EQ(DependentDiagnostics.LastOperation, Durin::EMaterialLoadedQueryOperation::Dependents);
	EXPECT_EQ(DependentDiagnostics.QueryCount, 2);
	EXPECT_EQ(DependentDiagnostics.SnapshotCount, 2);
	EXPECT_EQ(DependentDiagnostics.LastResultCount, Dependents.size());
	EXPECT_TRUE(ContainsHandle(Dependents, Durin::MakeObjectHandle(Base)));
	EXPECT_TRUE(ContainsHandle(Dependents, Durin::MakeObjectHandle(First)));
	EXPECT_TRUE(ContainsHandle(Dependents, Durin::MakeObjectHandle(Second)));
	EXPECT_FALSE(ContainsHandle(Dependents, Durin::MakeObjectHandle(Other)));
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
		EXPECT_EQ(Diagnostics.SnapshotCount, ExpectedQueries);
	};

	auto* ParentProperty = Instance->GetClass()->FindPropertyByName("Parent");
	auto* OverridesProperty = Instance->GetClass()->FindPropertyByName("PropertyOverrides");
	auto* ScalarProperty = Instance->GetClass()->FindPropertyByName("ScalarParameterValues");
	ASSERT_NE(ParentProperty, nullptr);
	ASSERT_NE(OverridesProperty, nullptr);
	ASSERT_NE(ScalarProperty, nullptr);
	const auto Revision = Instance->GetMaterialCompileStatus().AuthoredRevision;
	const auto ChildRevision = Child->GetMaterialCompileStatus().AuthoredRevision;
	// Static edits perform one compilation query and one publication query.
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
	const Durin::FObjectHandle GarbageHandle = Durin::MakeObjectHandle(Garbage);
	Durin::MarkAsGarbage(Garbage);

	const std::vector<Durin::FObjectHandle> Direct = Durin::GetLoadedDirectMaterialChildren(Base);
	ASSERT_EQ(Direct.size(), 1);
	EXPECT_TRUE(HandleEquals(Direct.front(), Durin::MakeObjectHandle(Live)));
	EXPECT_FALSE(ContainsHandle(Direct, GarbageHandle));

	Durin::AddToRoot(Base);
	Durin::AddToRoot(Live);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectHandle(GarbageHandle), nullptr);
	Durin::DMaterialInstance* Replacement = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FilteredQueryReplacement");
	const Durin::FObjectHandle ReplacementHandle = Durin::MakeObjectHandle(Replacement);
	EXPECT_EQ(ReplacementHandle.Index, GarbageHandle.Index);
	EXPECT_NE(ReplacementHandle.Generation, GarbageHandle.Generation);
	EXPECT_EQ(Durin::ResolveObjectHandle(GarbageHandle), nullptr);

	Durin::RemoveFromRoot(Live);
	Durin::RemoveFromRoot(Base);
	Durin::MarkAsGarbage(Replacement);
	Durin::MarkAsGarbage(Live);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}
