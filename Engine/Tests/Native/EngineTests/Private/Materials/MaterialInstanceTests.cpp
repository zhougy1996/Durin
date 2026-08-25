#include "MaterialTestSupport.h"

namespace
{
	auto MakeExpandedMaterial(Durin::DObject* Outer, const char* Name)
		-> Durin::DMaterial*
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(Outer, Name);
		Durin::FMaterialProgramValidationResult Validation;
		if (!Material || !Material->SetMaterialProgram(
			Durin::MakeLegacyExpandedMaterialProgram(), Validation)) return nullptr;
		return Material;
	}
}

TEST(FMaterialTests, BoundMaterialAndParentChangesUpdateProxyInPlace)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "LiveBaseMaterial");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "LiveMaterialInstance");
	EXPECT_TRUE(Instance->SetParent(Base));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("LiveMaterialComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Instance);
	Component->RegisterComponent();
	const FSceneSnapshot Initial = CaptureScene(Harness.Scene);

	const uint64 VersionBefore = Base->GetRenderStateVersion();
	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	const FSceneSnapshot ParentChanged = CaptureScene(Harness.Scene);
	EXPECT_EQ(ParentChanged.Proxy, Initial.Proxy);
	EXPECT_GT(Base->GetRenderStateVersion(), VersionBefore);
	EXPECT_EQ(ParentChanged.ComponentRevision, Initial.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(ParentChanged.Material).BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 1.0f));

	const uint64 NoOpVersion = Base->GetRenderStateVersion();
	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	EXPECT_EQ(Base->GetRenderStateVersion(), NoOpVersion);
	Instance->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.7, 0.6));
	Instance->ClearVectorParameterValue(Durin::MaterialParameters::BaseColorName());
	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.9, 0.1, 0.3));
	const FSceneSnapshot Final = CaptureScene(Harness.Scene);
	EXPECT_EQ(Final.Proxy, Initial.Proxy);
	ExpectColorNear(GetMaterialBinding(Final.Material).BaseColor, Durin::FVector4f(0.9f, 0.1f, 0.3f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Base->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.5f);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, PositionalOverrideTransfersAcrossMeshSwitch)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* Default = MakeExpandedMaterial(nullptr, "BoundMeshDefault");
	Durin::DMaterial* Orphan = MakeExpandedMaterial(nullptr, "DetachedOrphanOverride");
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Slots = static_cast<Durin::FArrayProperty*>(Mesh->GetClass()->FindPropertyByName("MaterialSlots"));
	static_cast<Durin::FMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 0))->DefaultMaterial = Default;
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("DefaultDependencyComponent");
	Component->SetStaticMesh(Mesh);
	Component->RegisterComponent();
	const FSceneSnapshot Initial = CaptureScene(Harness.Scene);
	Default->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	const FSceneSnapshot DefaultChanged = CaptureScene(Harness.Scene);
	EXPECT_EQ(DefaultChanged.Proxy, Initial.Proxy);
	EXPECT_EQ(DefaultChanged.ComponentRevision, Initial.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(DefaultChanged.Material).BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 1.0f));

	ASSERT_TRUE(Component->SetMaterial(0, Orphan));
	Durin::DStaticMesh* OtherMesh = Durin::DStaticMesh::CreateDebugTriangle();
	Component->SetStaticMesh(OtherMesh);
	const FSceneSnapshot BeforeOverrideChange = CaptureScene(Harness.Scene);
	EXPECT_EQ(Component->GetMaterial(0), Orphan);
	Orphan->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.1, 0.3));
	const FSceneSnapshot AfterOverrideChange = CaptureScene(Harness.Scene);
	EXPECT_EQ(AfterOverrideChange.Proxy, BeforeOverrideChange.Proxy);
	EXPECT_EQ(AfterOverrideChange.ComponentRevision, BeforeOverrideChange.ComponentRevision);
	ExpectColorNear(
		GetMaterialBinding(AfterOverrideChange.Material).BaseColor,
		Durin::FVector4f(0.8f, 0.1f, 0.3f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(OtherMesh);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Orphan);
	Durin::MarkAsGarbage(Default);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, BoundTextureChangesUpdateProxyResourceSnapshotInPlace)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "LiveTextureBaseMaterial");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "LiveTextureMaterialInstance");
	Durin::DTexture2D* BaseTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "LiveBaseColorTexture");
	Durin::DTexture2D* OverrideTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "LiveOverrideColorTexture");
	Base->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), BaseTexture);
	ASSERT_TRUE(Instance->SetParent(Base));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("LiveTextureMaterialComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Instance);
	Component->RegisterComponent();
	FSceneSnapshot Initial = CaptureScene(Harness.Scene);
	EXPECT_EQ(GetMaterialBinding(Initial.Material).Textures[0], BaseTexture->GetTextureReferenceRHI());

	Instance->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), OverrideTexture);
	FSceneSnapshot Overridden = CaptureScene(Harness.Scene);
	EXPECT_EQ(Overridden.Proxy, Initial.Proxy);
	EXPECT_EQ(Overridden.ComponentRevision, Initial.ComponentRevision);
	EXPECT_EQ(GetMaterialBinding(Overridden.Material).Textures[0], OverrideTexture->GetTextureReferenceRHI());

	EXPECT_TRUE(Instance->ClearTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName()));
	FSceneSnapshot Inherited = CaptureScene(Harness.Scene);
	EXPECT_EQ(Inherited.Proxy, Initial.Proxy);
	EXPECT_EQ(GetMaterialBinding(Inherited.Material).Textures[0], BaseTexture->GetTextureReferenceRHI());
	// Test snapshots cross back to the game thread, so release their proxy owners while each asset still owns its resource.
	Initial.Material = {};
	Overridden.Material = {};
	Inherited.Material = {};

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(OverrideTexture);
	Durin::MarkAsGarbage(BaseTexture);
	Harness.Shutdown();
	Durin::CollectGarbage();
	// DTexture2D destruction enqueues the final release; briefly restart the worker to drain it in render-thread context.
	Durin::InitRenderingThread();
	WaitForRenderingThread();
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialTests, SceneCommandsPreserveLatestTransformAndReleaseAllProxies)
{
	FRenderSceneHarness Harness;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("SceneCommandComponent");
	Component->SetStaticMesh(Mesh);
	Component->RegisterComponent();
	Component->SetWorldLocation(Durin::FVector3(4.0, 5.0, 6.0));
	const FSceneSnapshot Updated = CaptureScene(Harness.Scene);
	EXPECT_EQ(Updated.ProxyCount, 1);
	EXPECT_NEAR(Updated.Transform[3][0], 4.0, 1.e-6);
	EXPECT_NEAR(Updated.Transform[3][1], 5.0, 1.e-6);
	EXPECT_NEAR(Updated.Transform[3][2], 6.0, 1.e-6);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	EXPECT_EQ(CaptureScene(Harness.Scene).ProxyCount, 0);
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);

	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, InstancesInheritOverrideAndRejectParentCycles)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "BaseMaterial");
	Durin::DMaterialInstance* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FirstInstance");
	Durin::DMaterialInstance* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "SecondInstance");

	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3));
	ASSERT_TRUE(First->SetParent(Base));
	ASSERT_TRUE(Second->SetParent(First));
	ExpectColorNear(GetMaterialBinding(Second->GetRenderData()).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));

	First->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.4f);
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.7, 0.6));
	ExpectColorNear(GetMaterialBinding(Second->GetRenderData()).BaseColor, Durin::FVector4f(0.8f, 0.7f, 0.6f, 0.4f));
	EXPECT_FALSE(First->SetParent(Second));
	EXPECT_EQ(First->GetParent(), Base);

	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, MultiLevelResolutionReportsSupplyingSourceAndCurrentOverrideState)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "ResolutionBase");
	Durin::DMaterialInstance* Parent = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "ResolutionParent");
	Durin::DMaterialInstance* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "ResolutionChild");
	ASSERT_TRUE(Parent->SetParent(Base));
	ASSERT_TRUE(Child->SetParent(Parent));

	Durin::FResolvedMaterialParameter Resolved;
	ASSERT_TRUE(Child->ResolveParameterValue(Durin::MaterialParameters::OpacityId, Resolved));
	EXPECT_EQ(Resolved.Source, Base);
	EXPECT_FALSE(Resolved.bHasLocalOverride);
	EXPECT_FLOAT_EQ(Resolved.Value.ScalarValue, 1.0f);

	ASSERT_TRUE(Parent->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.6f));
	ASSERT_TRUE(Child->ResolveParameterValue(Durin::MaterialParameters::OpacityId, Resolved));
	EXPECT_EQ(Resolved.Source, Parent);
	EXPECT_FALSE(Resolved.bHasLocalOverride);
	EXPECT_FLOAT_EQ(Resolved.Value.ScalarValue, 0.6f);

	ASSERT_TRUE(Child->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.25f));
	ASSERT_TRUE(Child->ResolveParameterValue(Durin::MaterialParameters::OpacityId, Resolved));
	EXPECT_EQ(Resolved.Source, Child);
	EXPECT_TRUE(Resolved.bHasLocalOverride);
	EXPECT_FLOAT_EQ(Resolved.Value.ScalarValue, 0.25f);

	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ParentRemovalPreservesOrphansAndExcludesThemFromRendering)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "OrphanBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "OrphanInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3)));
	ASSERT_TRUE(Instance->HasLocalParameterOverride(Durin::MaterialParameters::BaseColorId));
	EXPECT_FALSE(Instance->IsParameterOverrideOrphan(Durin::MaterialParameters::BaseColorId));

	ASSERT_TRUE(Instance->SetParent(nullptr));
	ASSERT_EQ(Instance->GetParameterOverrides().size(), 1u);
	EXPECT_TRUE(Instance->HasLocalParameterOverride(Durin::MaterialParameters::BaseColorId));
	EXPECT_TRUE(Instance->IsParameterOverrideOrphan(Durin::MaterialParameters::BaseColorId));
	Durin::FResolvedMaterialParameter Resolved;
	EXPECT_FALSE(Instance->ResolveParameterValue(Durin::MaterialParameters::BaseColorId, Resolved));
	ExpectColorNear(GetMaterialBinding(Instance->GetRenderData()).BaseColor, Durin::FVector4f(0.95f, 0.62f, 0.22f, 1.0f));

	ASSERT_TRUE(Instance->SetParent(Base));
	EXPECT_FALSE(Instance->IsParameterOverrideOrphan(Durin::MaterialParameters::BaseColorId));
	ExpectColorNear(GetMaterialBinding(Instance->GetRenderData()).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ASSERT_TRUE(Instance->ClearParameterOverride(Durin::MaterialParameters::BaseColorId));
	EXPECT_TRUE(Instance->GetParameterOverrides().empty());

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, GuidOverrideRejectsUnknownAndPreservesVersionOnNoOp)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "GuidOverrideBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "GuidOverrideInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	const Durin::FGuid Unknown{1, 2, 3, 4};
	const uint64 InitialVersion = Instance->GetRenderStateVersion();
	EXPECT_FALSE(Instance->SetParameterOverride(
		Unknown, Durin::EMaterialParameterType::Scalar, Durin::FMaterialParameterValue::MakeScalar(0.5f)));
	EXPECT_FALSE(Instance->SetParameterOverride(
		Durin::MaterialParameters::BaseColorId,
		Durin::EMaterialParameterType::Scalar,
		Durin::FMaterialParameterValue::MakeScalar(0.5f)));
	EXPECT_EQ(Instance->GetRenderStateVersion(), InitialVersion);
	EXPECT_TRUE(Instance->GetParameterOverrides().empty());

	ASSERT_TRUE(Instance->SetParameterOverride(
		Durin::MaterialParameters::OpacityId,
		Durin::EMaterialParameterType::Scalar,
		Durin::FMaterialParameterValue::MakeScalar(0.5f)));
	const uint64 OverriddenVersion = Instance->GetRenderStateVersion();
	Durin::FMaterialParameterValue SameActiveValue = Durin::FMaterialParameterValue::MakeScalar(0.5f);
	SameActiveValue.VectorValue = Durin::FVector3(9.0);
	ASSERT_TRUE(Instance->SetParameterOverride(
		Durin::MaterialParameters::OpacityId,
		Durin::EMaterialParameterType::Scalar,
		SameActiveValue));
	EXPECT_EQ(Instance->GetRenderStateVersion(), OverriddenVersion);
	EXPECT_EQ(Instance->GetParameterOverrides().size(), 1u);

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, InstanceOverrideStateTracksSetAndClear)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "OverrideStateBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "OverrideStateInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	EXPECT_FALSE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	EXPECT_FALSE(Instance->HasVectorParameterOverride(Durin::MaterialParameters::BaseColorName()));
	EXPECT_FALSE(Instance->HasTextureParameterOverride(Durin::MaterialParameters::BaseColorTextureName()));

	Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.5f);
	Instance->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	Instance->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), nullptr);
	EXPECT_TRUE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	EXPECT_TRUE(Instance->HasVectorParameterOverride(Durin::MaterialParameters::BaseColorName()));
	EXPECT_TRUE(Instance->HasTextureParameterOverride(Durin::MaterialParameters::BaseColorTextureName()));

	EXPECT_TRUE(Instance->ClearScalarParameterValue(Durin::MaterialParameters::OpacityName()));
	EXPECT_TRUE(Instance->ClearVectorParameterValue(Durin::MaterialParameters::BaseColorName()));
	EXPECT_TRUE(Instance->ClearTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName()));
	EXPECT_FALSE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	EXPECT_FALSE(Instance->HasVectorParameterOverride(Durin::MaterialParameters::BaseColorName()));
	EXPECT_FALSE(Instance->HasTextureParameterOverride(Durin::MaterialParameters::BaseColorTextureName()));

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, TextureParametersInheritOverrideAndPreserveExplicitNull)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "TextureBaseMaterial");
	Durin::DMaterialInstance* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FirstTextureInstance");
	Durin::DMaterialInstance* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "SecondTextureInstance");
	Durin::DTexture2D* BaseTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "InheritedBaseColorTexture");
	Durin::DTexture2D* OverrideTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "OverriddenBaseColorTexture");

	Base->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), BaseTexture);
	ASSERT_TRUE(First->SetParent(Base));
	ASSERT_TRUE(Second->SetParent(First));
	EXPECT_EQ(GetMaterialBinding(Second->GetRenderData()).Textures[0], BaseTexture->GetTextureReferenceRHI());

	First->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), OverrideTexture);
	EXPECT_EQ(GetMaterialBinding(Second->GetRenderData()).Textures[0], OverrideTexture->GetTextureReferenceRHI());
	Second->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), nullptr);
	EXPECT_EQ(GetMaterialBinding(Second->GetRenderData()).Textures[0], nullptr);
	EXPECT_TRUE(Second->ClearTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName()));
	EXPECT_EQ(GetMaterialBinding(Second->GetRenderData()).Textures[0], OverrideTexture->GetTextureReferenceRHI());
	EXPECT_TRUE(First->ClearTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName()));
	EXPECT_EQ(GetMaterialBinding(Second->GetRenderData()).Textures[0], BaseTexture->GetTextureReferenceRHI());

	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(OverrideTexture);
	Durin::MarkAsGarbage(BaseTexture);
	Durin::CollectGarbage();
	WaitForRenderingThread();
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialTests, ReflectedTextureParameterKeepsTextureReachable)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::DMaterial* Material = MakeExpandedMaterial(nullptr, "RootedTextureMaterial");
	Durin::DTexture2D* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "ReferencedMaterialTexture");
	Material->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), Texture);
	Durin::AddToRoot(Material);

	Durin::CollectGarbage();
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Material));
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Texture));

	Durin::RemoveFromRoot(Material);
	Durin::CollectGarbage();
	WaitForRenderingThread();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Material));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Texture));
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialTests, ReflectedInstanceOverrideKeepsNestedTextureReachable)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "RootedOverrideBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "RootedOverrideInstance");
	Durin::DTexture2D* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "ReferencedOverrideTexture");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), Texture));
	Durin::AddToRoot(Instance);

	Durin::CollectGarbage();
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Instance));
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Base));
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Texture));

	Durin::RemoveFromRoot(Instance);
	Durin::CollectGarbage();
	WaitForRenderingThread();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Instance));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Base));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Texture));
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialTests, DuplicateInstancePreservesParentAndNestedTextureOverride)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "DuplicateOverrideBase");
	Durin::DMaterialInstance* Source = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "DuplicateOverrideSource");
	Durin::DTexture2D* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "DuplicateOverrideTexture");
	ASSERT_TRUE(Source->SetParent(Base));
	ASSERT_TRUE(Source->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), Texture));

	std::string Error;
	auto* Duplicate = Durin::Cast<Durin::DMaterialInstance>(
		Durin::DuplicateObjectGraph(Source, nullptr, "DuplicateOverrideResult", &Error));
	ASSERT_NE(Duplicate, nullptr) << Error;
	EXPECT_EQ(Duplicate->GetParent(), Base);
	EXPECT_TRUE(Duplicate->HasLocalParameterOverride(Durin::MaterialParameters::BaseColorTextureId));
	Durin::DTexture2D* DuplicateTexture = nullptr;
	ASSERT_TRUE(Duplicate->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), DuplicateTexture));
	EXPECT_EQ(DuplicateTexture, Texture);

	Durin::MarkAsGarbage(Duplicate);
	Durin::MarkAsGarbage(Source);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(Texture);
	Durin::CollectGarbage();
}
