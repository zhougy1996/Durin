#include "MaterialTestSupport.h"

#include "Materials/MaterialRenderProxy.h"

#include <algorithm>
#include <future>
#include <limits>

namespace
{
	struct FMaterialProxySnapshot
	{
		Durin::FMaterialRenderData RenderData;
		const Durin::FMaterialRenderProxy* ParentIdentity = nullptr;
		Durin::uint64 LocalVersion = 0;
		Durin::uint64 ResolvedVersion = 0;
		Durin::uint64 ObservedParentResolvedVersion = 0;
		Durin::uint64 StalePublicationCount = 0;
	};

	auto CaptureMaterialProxy(
		const Durin::FMaterialRenderProxyRef& Proxy,
		bool bResolve = true
	) -> FMaterialProxySnapshot
	{
		FMaterialProxySnapshot Snapshot;
		struct FCaptureMaterialRenderProxyCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "CaptureMaterialRenderProxy";
			}
		};
		Durin::EnqueueRenderCommand<FCaptureMaterialRenderProxyCommand>(
			[Proxy, &Snapshot, bResolve](
				Durin::FRHICommandListImmediate&) {
				if (bResolve)
				{
					Snapshot.RenderData =
						Proxy->Resolve_RenderThread();
				}
				Snapshot.ParentIdentity =
					Proxy->GetParentProxyIdentity_RenderThread();
				Snapshot.LocalVersion =
					Proxy->GetLocalVersion_RenderThread();
				Snapshot.ResolvedVersion =
					Proxy->GetResolvedVersion_RenderThread();
				Snapshot.ObservedParentResolvedVersion =
					Proxy->GetObservedParentResolvedVersion_RenderThread();
				Snapshot.StalePublicationCount =
					Proxy->GetStalePublicationCount_RenderThread();
			});
		WaitForRenderingThread();
		return Snapshot;
	}

		auto ExpectRenderDataMatches(
		const Durin::FMaterialRenderData& Actual,
		const Durin::FMaterialRenderData& Expected
	) -> void
	{
		const Durin::FMaterialRenderV2Binding ActualBinding =
			GetMaterialBinding(Actual);
		const Durin::FMaterialRenderV2Binding ExpectedBinding =
			GetMaterialBinding(Expected);
		ExpectColorNear(ActualBinding.BaseColor, ExpectedBinding.BaseColor);
		EXPECT_EQ(
			ActualBinding.Textures[0],
			ExpectedBinding.Textures[0]);
		EXPECT_EQ(Actual.PipelineIdentity, Expected.PipelineIdentity);
		EXPECT_EQ(
			Actual.Representation.GetLayout().Identity,
			Expected.Representation.GetLayout().Identity);
		EXPECT_TRUE(std::ranges::equal(
			Actual.Representation.GetUniformPayload(),
			Expected.Representation.GetUniformPayload()));
		EXPECT_TRUE(std::ranges::equal(
			Actual.Representation.GetResources(),
			Expected.Representation.GetResources()));
	}
}

TEST(FMaterialRenderProxyTests, StableIdentityPublishesVersionsAndRejectsStaleState)
{
	FRenderSceneHarness Harness;
	auto* Material = Durin::NewObject<Durin::DMaterial>(
		nullptr, "StableProxyMaterial");
	Durin::FMaterialRenderProxyRef Proxy =
		Material->GetMaterialRenderProxy();
	Durin::FMaterialRenderProxyRef SameProxy =
		Material->GetMaterialRenderProxy();
	ASSERT_TRUE(Proxy);
	EXPECT_EQ(Proxy.GetReference(), SameProxy.GetReference());

	const FMaterialProxySnapshot Initial =
		CaptureMaterialProxy(Proxy);
	EXPECT_GT(Initial.LocalVersion, 0);
	EXPECT_GT(Initial.ResolvedVersion, 0);
	ExpectRenderDataMatches(
		Initial.RenderData, Material->GetRenderData());
	const FMaterialProxySnapshot Cached =
		CaptureMaterialProxy(Proxy);
	EXPECT_EQ(Cached.ResolvedVersion, Initial.ResolvedVersion);

	ASSERT_TRUE(Material->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.2, 0.4, 0.7)));
	const FMaterialProxySnapshot Updated =
		CaptureMaterialProxy(Proxy);
	EXPECT_GT(Updated.LocalVersion, Initial.LocalVersion);
	EXPECT_GT(Updated.ResolvedVersion, Initial.ResolvedVersion);
	ExpectRenderDataMatches(
		Updated.RenderData, Material->GetRenderData());

	Durin::FMaterialStaticProperties StaticProperties;
	StaticProperties.BlendMode =
		Durin::EMaterialBlendMode::Masked;
	StaticProperties.ShadingModel =
		Durin::EMaterialShadingModel::Unlit;
	StaticProperties.bTwoSided = true;
	StaticProperties.DepthWritePolicy =
		Durin::EMaterialDepthWritePolicy::Disabled;
	StaticProperties.OpacityMaskThreshold = 0.55f;
	ASSERT_TRUE(Material->SetStaticProperties(StaticProperties));
	const FMaterialProxySnapshot StaticUpdated =
		CaptureMaterialProxy(Proxy);
	EXPECT_GT(
		StaticUpdated.LocalVersion, Updated.LocalVersion);
	EXPECT_GT(
		StaticUpdated.ResolvedVersion, Updated.ResolvedVersion);
	ExpectRenderDataMatches(
		StaticUpdated.RenderData, Material->GetRenderData());

	Durin::FMaterialRenderProxyPublication StalePublication;
	StalePublication.LocalVersion = Updated.LocalVersion;
	StalePublication.LocalLayer.Parameters.push_back({
		.Id = Durin::MaterialParameters::BaseColorId,
		.Type = Durin::EMaterialParameterType::Vector,
		.VectorValue = Durin::FVector3(0.0),
	});
	bool bStaleApplied = true;
	struct FApplyStaleMaterialProxyPublicationCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "ApplyStaleMaterialProxyPublication";
		}
	};
	Durin::EnqueueRenderCommand<
		FApplyStaleMaterialProxyPublicationCommand>(
		[Proxy, Publication = std::move(StalePublication),
		 &bStaleApplied](Durin::FRHICommandListImmediate&) mutable {
			bStaleApplied = Proxy->ApplyPublication_RenderThread(
				std::move(Publication));
		});
	WaitForRenderingThread();

	const FMaterialProxySnapshot AfterStale =
		CaptureMaterialProxy(Proxy);
	EXPECT_FALSE(bStaleApplied);
	EXPECT_EQ(
		AfterStale.LocalVersion, StaticUpdated.LocalVersion);
	EXPECT_EQ(
		AfterStale.StalePublicationCount,
		StaticUpdated.StalePublicationCount + 1);
	EXPECT_EQ(
		AfterStale.ResolvedVersion,
		StaticUpdated.ResolvedVersion);
	ExpectRenderDataMatches(
		AfterStale.RenderData, StaticUpdated.RenderData);

	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(Proxy));
	Durin::ReleaseMaterialRenderProxy_GameThread(
		std::move(SameProxy));
	WaitForRenderingThread();
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialRenderProxyTests, CanonicalV2ValuesMatchDirectCompilationForBasesAndInstances)
{
	FRenderSceneHarness Harness;
	auto* Base = Durin::NewObject<Durin::DMaterial>(
		nullptr, "CanonicalProxyBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(
		nullptr, "CanonicalProxyInstance");
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(
		nullptr, "CanonicalProxyTexture");
	ASSERT_TRUE(Instance->SetParent(Base));

	ASSERT_TRUE(Base->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.15, 0.35, 0.55)));
	ASSERT_TRUE(Base->SetVectorParameterValue(
		Durin::MaterialParameters::NormalName(),
		Durin::FVector3(0.0, 2.0, 0.0)));
	ASSERT_TRUE(Base->SetScalarParameterValue(
		Durin::MaterialParameters::MetallicName(), 0.81f));
	ASSERT_TRUE(Base->SetScalarParameterValue(
		Durin::MaterialParameters::RoughnessName(), 0.23f));
	ASSERT_TRUE(Base->SetScalarParameterValue(
		Durin::MaterialParameters::AmbientOcclusionName(), 0.47f));
	ASSERT_TRUE(Base->SetVectorParameterValue(
		Durin::MaterialParameters::EmissiveName(),
		Durin::FVector3(3.0, 5.0, 7.0)));
	ASSERT_TRUE(Base->SetScalarParameterValue(
		Durin::MaterialParameters::OpacityName(), 0.68f));
	ASSERT_TRUE(Base->SetScalarParameterValue(
		Durin::MaterialParameters::OpacityMaskName(), 0.39f));

	for (const Durin::FMaterialParameterDefinition& Definition
		: Durin::GetCanonicalMaterialParameterDefinitions())
	{
		if (Definition.Type == Durin::EMaterialParameterType::Texture)
		{
			ASSERT_TRUE(Base->SetTextureParameterValue(
				Definition.Name, Texture));
		}
		else if (std::ranges::find(
			Durin::MaterialParameters::UVChannelIds, Definition.Id)
			!= Durin::MaterialParameters::UVChannelIds.end())
		{
			ASSERT_TRUE(Base->SetScalarParameterValue(
				Definition.Name, 2.6f));
		}
		else if (std::ranges::find(
			Durin::MaterialParameters::UVScaleIds, Definition.Id)
			!= Durin::MaterialParameters::UVScaleIds.end())
		{
			ASSERT_TRUE(Base->SetVector2ParameterValue(
				Definition.Name, Durin::FVector2(2.0, -3.0)));
		}
		else if (std::ranges::find(
			Durin::MaterialParameters::UVOffsetIds, Definition.Id)
			!= Durin::MaterialParameters::UVOffsetIds.end())
		{
			ASSERT_TRUE(Base->SetVector2ParameterValue(
				Definition.Name, Durin::FVector2(7.0, -11.0)));
		}
	}

	Durin::FMaterialRenderProxyRef BaseProxy =
		Base->GetMaterialRenderProxy();
	const FMaterialProxySnapshot BaseSnapshot =
		CaptureMaterialProxy(BaseProxy);
	ExpectRenderDataMatches(BaseSnapshot.RenderData, Base->GetRenderData());
	const Durin::FMaterialRenderV2Binding BaseBinding =
		GetMaterialBinding(BaseSnapshot.RenderData);
	EXPECT_FLOAT_EQ(BaseBinding.Metallic, 0.81f);
	EXPECT_FLOAT_EQ(BaseBinding.Roughness, 0.23f);
	EXPECT_EQ(BaseBinding.Normal, Durin::FVector3f(0.0f, 1.0f, 0.0f));
	EXPECT_EQ(BaseBinding.Emissive, Durin::FVector3f(3.0f, 5.0f, 7.0f));
	for (size_t Role = 0; Role < BaseBinding.UVChannels.size(); ++Role)
	{
		EXPECT_FLOAT_EQ(BaseBinding.UVChannels[Role], 3.0f);
		EXPECT_EQ(BaseBinding.UVScales[Role], Durin::FVector2f(2.0f, -3.0f));
		EXPECT_EQ(BaseBinding.UVOffsets[Role], Durin::FVector2f(7.0f, -11.0f));
	}

	ASSERT_TRUE(Instance->SetScalarParameterValue(
		Durin::MaterialParameters::MetallicName(), 0.17f));
	ASSERT_TRUE(Instance->SetScalarParameterValue(
		Durin::MaterialParameters::RoughnessName(),
		std::numeric_limits<float>::quiet_NaN()));
	ASSERT_TRUE(Instance->SetVectorParameterValue(
		Durin::MaterialParameters::NormalName(), Durin::FVector3(0.0)));
	ASSERT_TRUE(Instance->SetVectorParameterValue(
		Durin::MaterialParameters::EmissiveName(),
		Durin::FVector3(100.0, -2.0, 4.0)));
	ASSERT_TRUE(Instance->SetScalarParameterValue(
		Durin::FName("RoughnessUVChannel"), 1.4f));
	ASSERT_TRUE(Instance->SetVector2ParameterValue(
		Durin::FName("NormalUVScale"),
		Durin::FVector2(2048.0, -2048.0)));

	Durin::FMaterialRenderProxyRef InstanceProxy =
		Instance->GetMaterialRenderProxy();
	const FMaterialProxySnapshot InstanceSnapshot =
		CaptureMaterialProxy(InstanceProxy);
	ExpectRenderDataMatches(
		InstanceSnapshot.RenderData, Instance->GetRenderData());
	const Durin::FMaterialRenderV2Binding InstanceBinding =
		GetMaterialBinding(InstanceSnapshot.RenderData);
	EXPECT_FLOAT_EQ(InstanceBinding.Metallic, 0.17f);
	EXPECT_FLOAT_EQ(InstanceBinding.Roughness, 0.5f);
	EXPECT_EQ(InstanceBinding.Normal, Durin::FVector3f(0.0f, 0.0f, 1.0f));
	EXPECT_EQ(InstanceBinding.Emissive, Durin::FVector3f(64.0f, 0.0f, 4.0f));
	EXPECT_FLOAT_EQ(InstanceBinding.UVChannels[3], 1.0f);
	EXPECT_EQ(
		InstanceBinding.UVScales[1],
		Durin::FVector2f(1024.0f, -1024.0f));

	Durin::MarkAsGarbage(Texture);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(InstanceProxy));
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(BaseProxy));
	WaitForRenderingThread();
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialRenderProxyTests, OrdinarySetterDoesNotRunLoadedMaterialQuery)
{
	FRenderSceneHarness Harness;
	auto* Material = Durin::NewObject<Durin::DMaterial>(
		nullptr, "ProxyOnlyQueryMaterial");
	Durin::FMaterialRenderProxyRef Proxy = Material->GetMaterialRenderProxy();
	Durin::ResetMaterialLoadedQueryDiagnostics();

	ASSERT_TRUE(Material->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.25, 0.5, 0.75)));
	const Durin::FMaterialLoadedQueryDiagnostics Diagnostics =
		Durin::GetMaterialLoadedQueryDiagnostics();
	EXPECT_EQ(Diagnostics.LastOperation, Durin::EMaterialLoadedQueryOperation::None);
	EXPECT_EQ(Diagnostics.QueryCount, 0);
	EXPECT_EQ(Diagnostics.SnapshotCount, 0);
	EXPECT_EQ(Diagnostics.ScannedObjectCount, 0);
	EXPECT_EQ(Diagnostics.ScannedMaterialCount, 0);

	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(Proxy));
	WaitForRenderingThread();
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialRenderProxyTests, CoalescesQueuedPublicationsPerProxy)
{
	FRenderSceneHarness Harness;
	auto* Material = Durin::NewObject<Durin::DMaterial>(
		nullptr, "CoalescedProxyMaterial");
	Durin::FMaterialRenderProxyRef Proxy =
		Material->GetMaterialRenderProxy();
	CaptureMaterialProxy(Proxy);
	Durin::ResetMaterialRenderProxyCounters();

	auto CommandStarted = std::make_shared<std::promise<void>>();
	std::future<void> CommandStartedFuture = CommandStarted->get_future();
	auto AllowCommandCompletion = std::make_shared<std::promise<void>>();
	std::shared_future<void> AllowCommandCompletionFuture =
		AllowCommandCompletion->get_future().share();
	struct FBlockMaterialPublicationCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "BlockMaterialPublication";
		}
	};
	Durin::EnqueueRenderCommand<FBlockMaterialPublicationCommand>(
		[CommandStarted, AllowCommandCompletionFuture](
			Durin::FRHICommandListImmediate&) {
			CommandStarted->set_value();
			AllowCommandCompletionFuture.wait();
		});
	CommandStartedFuture.wait();

	ASSERT_TRUE(Material->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.2, 0.3, 0.4)));
	ASSERT_TRUE(Material->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.4, 0.5, 0.6)));
	ASSERT_TRUE(Material->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.7, 0.8, 0.9)));
	const Durin::FMaterialRenderProxyCounters Queued =
		Durin::GetMaterialRenderProxyCounters();
	EXPECT_EQ(Queued.PublicationCount, 0);
	EXPECT_EQ(Queued.CoalescedPublicationCount, 2);

	AllowCommandCompletion->set_value();
	const FMaterialProxySnapshot Updated = CaptureMaterialProxy(Proxy);
	ExpectColorNear(
		GetMaterialBinding(Updated.RenderData).BaseColor,
		Durin::FVector4f(0.7f, 0.8f, 0.9f, 1.0f));
	const Durin::FMaterialRenderProxyCounters Applied =
		Durin::GetMaterialRenderProxyCounters();
	EXPECT_EQ(Applied.PublicationCount, 1);
	EXPECT_EQ(Applied.CoalescedPublicationCount, 2);

	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(Proxy));
	WaitForRenderingThread();
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialRenderProxyTests, DescendantsResolveParentChangesLazilyAcrossLongChains)
{
	FRenderSceneHarness Harness;
	auto* FirstBase = Durin::NewObject<Durin::DMaterial>(
		nullptr, "ProxyChainFirstBase");
	auto* SecondBase = Durin::NewObject<Durin::DMaterial>(
		nullptr, "ProxyChainSecondBase");
	ASSERT_TRUE(FirstBase->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.1, 0.2, 0.3)));
	ASSERT_TRUE(SecondBase->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.7, 0.6, 0.5)));

	constexpr Durin::uint32 ChainLength = 32;
	std::vector<Durin::DMaterialInstance*> Chain;
	Chain.reserve(ChainLength);
	Durin::DMaterialInterface* Parent = FirstBase;
	for (Durin::uint32 Index = 0; Index < ChainLength; ++Index)
	{
		auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(
			nullptr,
			Durin::FName(std::format("ProxyChainInstance{}", Index)));
		ASSERT_TRUE(Instance->SetParent(Parent));
		Chain.push_back(Instance);
		Parent = Instance;
	}

	Durin::DMaterialInstance* Leaf = Chain.back();
	Durin::FMaterialRenderProxyRef LeafProxy =
		Leaf->GetMaterialRenderProxy();
	const FMaterialProxySnapshot Initial =
		CaptureMaterialProxy(LeafProxy);
	ExpectRenderDataMatches(
		Initial.RenderData, Leaf->GetRenderData());

	ASSERT_TRUE(FirstBase->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.25, 0.45, 0.65)));
	const FMaterialProxySnapshot BeforeLazyResolve =
		CaptureMaterialProxy(LeafProxy, false);
	EXPECT_EQ(
		BeforeLazyResolve.ResolvedVersion,
		Initial.ResolvedVersion);

	const FMaterialProxySnapshot AfterLazyResolve =
		CaptureMaterialProxy(LeafProxy);
	EXPECT_GT(
		AfterLazyResolve.ResolvedVersion,
		Initial.ResolvedVersion);
	ExpectRenderDataMatches(
		AfterLazyResolve.RenderData, Leaf->GetRenderData());

	ASSERT_TRUE(Chain.front()->SetParent(SecondBase));
	const FMaterialProxySnapshot Reparented =
		CaptureMaterialProxy(LeafProxy);
	EXPECT_GT(
		Reparented.ResolvedVersion,
		AfterLazyResolve.ResolvedVersion);
	ExpectRenderDataMatches(
		Reparented.RenderData, Leaf->GetRenderData());

	ASSERT_TRUE(Chain[ChainLength / 2]->SetScalarParameterValue(
		Durin::MaterialParameters::OpacityName(), 0.35f));
	const FMaterialProxySnapshot LocallyOverridden =
		CaptureMaterialProxy(LeafProxy);
	EXPECT_FLOAT_EQ(
		GetMaterialBinding(LocallyOverridden.RenderData).BaseColor.a,
		0.35f);
	ExpectRenderDataMatches(
		LocallyOverridden.RenderData, Leaf->GetRenderData());

	for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
	{
		Durin::MarkAsGarbage(*It);
	}
	Durin::MarkAsGarbage(SecondBase);
	Durin::MarkAsGarbage(FirstBase);
	Durin::CollectGarbage();
	Durin::ReleaseMaterialRenderProxy_GameThread(
		std::move(LeafProxy));
	WaitForRenderingThread();
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialRenderProxyTests, ImportExchangePublishesCurrentParentProxy)
{
	FRenderSceneHarness Harness;
	auto* FirstBase = Durin::NewObject<Durin::DMaterial>(
		nullptr, "ProxyImportFirstBase");
	auto* SecondBase = Durin::NewObject<Durin::DMaterial>(
		nullptr, "ProxyImportSecondBase");
	auto* First = Durin::NewObject<Durin::DMaterialInstance>(
		nullptr, "ProxyImportFirst");
	auto* Second = Durin::NewObject<Durin::DMaterialInstance>(
		nullptr, "ProxyImportSecond");
	ASSERT_TRUE(FirstBase->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.15, 0.25, 0.35)));
	ASSERT_TRUE(SecondBase->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.65, 0.55, 0.45)));
	ASSERT_TRUE(First->SetParent(FirstBase));
	ASSERT_TRUE(Second->SetParent(SecondBase));
	ASSERT_TRUE(First->SetScalarParameterValue(
		Durin::MaterialParameters::OpacityName(), 0.3f));
	ASSERT_TRUE(Second->SetScalarParameterValue(
		Durin::MaterialParameters::OpacityName(), 0.8f));

	Durin::FMaterialRenderProxyRef FirstBaseProxy =
		FirstBase->GetMaterialRenderProxy();
	Durin::FMaterialRenderProxyRef SecondBaseProxy =
		SecondBase->GetMaterialRenderProxy();
	Durin::FMaterialRenderProxyRef FirstProxy =
		First->GetMaterialRenderProxy();
	Durin::FMaterialRenderProxyRef SecondProxy =
		Second->GetMaterialRenderProxy();
	const FMaterialProxySnapshot FirstBefore =
		CaptureMaterialProxy(FirstProxy);
	const FMaterialProxySnapshot SecondBefore =
		CaptureMaterialProxy(SecondProxy);
	EXPECT_EQ(
		FirstBefore.ParentIdentity,
		FirstBaseProxy.GetReference());
	EXPECT_EQ(
		SecondBefore.ParentIdentity,
		SecondBaseProxy.GetReference());

	First->ExchangeImportedState(*Second);
	const FMaterialProxySnapshot FirstAfter =
		CaptureMaterialProxy(FirstProxy);
	const FMaterialProxySnapshot SecondAfter =
		CaptureMaterialProxy(SecondProxy);
	EXPECT_EQ(
		FirstAfter.ParentIdentity,
		SecondBaseProxy.GetReference());
	EXPECT_EQ(
		SecondAfter.ParentIdentity,
		FirstBaseProxy.GetReference());
	EXPECT_GT(FirstAfter.LocalVersion, FirstBefore.LocalVersion);
	EXPECT_GT(SecondAfter.LocalVersion, SecondBefore.LocalVersion);
	ExpectRenderDataMatches(
		FirstAfter.RenderData, First->GetRenderData());
	ExpectRenderDataMatches(
		SecondAfter.RenderData, Second->GetRenderData());

	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::MarkAsGarbage(SecondBase);
	Durin::MarkAsGarbage(FirstBase);
	Durin::CollectGarbage();
	Durin::ReleaseMaterialRenderProxy_GameThread(
		std::move(SecondProxy));
	Durin::ReleaseMaterialRenderProxy_GameThread(
		std::move(FirstProxy));
	Durin::ReleaseMaterialRenderProxy_GameThread(
		std::move(SecondBaseProxy));
	Durin::ReleaseMaterialRenderProxy_GameThread(
		std::move(FirstBaseProxy));
	WaitForRenderingThread();
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialRenderProxyTests, PublishedStateOutlivesOwnersAndPostLoadDuplication)
{
	FRenderSceneHarness Harness;
	auto* Base = Durin::NewObject<Durin::DMaterial>(
		nullptr, "ProxyLifetimeBase");
	auto* Source = Durin::NewObject<Durin::DMaterialInstance>(
		nullptr, "ProxyLifetimeSource");
	ASSERT_TRUE(Base->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.3, 0.5, 0.7)));
	ASSERT_TRUE(Source->SetParent(Base));
	ASSERT_TRUE(Source->SetScalarParameterValue(
		Durin::MaterialParameters::OpacityName(), 0.45f));

	std::string Error;
	auto* Duplicate = Durin::Cast<Durin::DMaterialInstance>(
		Durin::DuplicateObjectGraph(
			Source, nullptr, "ProxyLifetimeDuplicate", &Error));
	ASSERT_NE(Duplicate, nullptr) << Error;
	Durin::FMaterialRenderProxyRef DuplicateProxy =
		Duplicate->GetMaterialRenderProxy();
	const FMaterialProxySnapshot BeforeDestruction =
		CaptureMaterialProxy(DuplicateProxy);
	ExpectRenderDataMatches(
		BeforeDestruction.RenderData, Duplicate->GetRenderData());

	Durin::MarkAsGarbage(Duplicate);
	Durin::MarkAsGarbage(Source);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Duplicate));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Source));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Base));

	const FMaterialProxySnapshot AfterDestruction =
		CaptureMaterialProxy(DuplicateProxy);
	ExpectRenderDataMatches(
		AfterDestruction.RenderData,
		BeforeDestruction.RenderData);
	EXPECT_EQ(
		AfterDestruction.LocalVersion,
		BeforeDestruction.LocalVersion);

	Durin::ReleaseMaterialRenderProxy_GameThread(
		std::move(DuplicateProxy));
	WaitForRenderingThread();
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialRenderProxyTests, StressSharedUsersSlotsInterleavedPublicationAndDestruction)
{
	FRenderSceneHarness Harness;
	constexpr Durin::uint32 ChainLength = 48;
	constexpr Durin::uint32 SharedUserCount = 12;
	constexpr Durin::uint32 SlotCount = 8;
	constexpr Durin::uint32 UnrelatedMaterialCount = 256;
	auto CapturePrimitiveCount = [&Harness]() -> size_t {
		size_t Count = 0;
		struct FCaptureMaterialStressPrimitiveCountCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "CaptureMaterialStressPrimitiveCount";
			}
		};
		Durin::EnqueueRenderCommand<
			FCaptureMaterialStressPrimitiveCountCommand>(
			[&Count, Scene = Harness.Scene](
				Durin::FRHICommandListImmediate&) {
				Count = Scene->GetPrimitiveSceneProxies().size();
			});
		WaitForRenderingThread();
		return Count;
	};

	auto* Base = Durin::NewObject<Durin::DMaterial>(
		nullptr, "ProxyStressBase");
	auto* AlternateBase = Durin::NewObject<Durin::DMaterial>(
		nullptr, "ProxyStressAlternateBase");
	ASSERT_TRUE(Base->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.1, 0.2, 0.3)));
	ASSERT_TRUE(AlternateBase->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.8, 0.7, 0.6)));

	std::vector<Durin::DMaterialInstance*> Chain;
	Chain.reserve(ChainLength);
	Durin::DMaterialInterface* Parent = Base;
	for (Durin::uint32 Index = 0; Index < ChainLength; ++Index)
	{
		auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(
			nullptr,
			Durin::FName(std::format("ProxyStressChain{}", Index)));
		ASSERT_TRUE(Instance->SetParent(Parent));
		Chain.push_back(Instance);
		Parent = Instance;
	}
	Durin::DMaterialInstance* Leaf = Chain.back();
	Durin::FMaterialRenderProxyRef LeafProxy =
		Leaf->GetMaterialRenderProxy();
	ASSERT_TRUE(LeafProxy);
	const FMaterialProxySnapshot InitialLeaf = CaptureMaterialProxy(LeafProxy);

	auto* Shared = Durin::NewObject<Durin::DMaterial>(
		nullptr, "ProxyStressShared");
	auto* Replacement = Durin::NewObject<Durin::DMaterial>(
		nullptr, "ProxyStressReplacement");
	std::vector<Durin::DMaterial*> SlotMaterials;
	SlotMaterials.reserve(SlotCount);
	for (Durin::uint32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(
			nullptr,
			Durin::FName(std::format("ProxyStressSlotMaterial{}", SlotIndex)));
		ASSERT_TRUE(Material->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(
				0.05 + 0.05 * SlotIndex,
				0.15 + 0.04 * SlotIndex,
				0.25 + 0.03 * SlotIndex)));
		SlotMaterials.push_back(Material);
	}
	ASSERT_TRUE(Shared->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.2, 0.4, 0.6)));
	ASSERT_TRUE(Replacement->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.9, 0.1, 0.2)));

	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	ASSERT_NE(Mesh, nullptr);
	for (Durin::uint32 SlotIndex = 1; SlotIndex < SlotCount; ++SlotIndex)
	{
		AddDebugMaterialSlot(
			Mesh,
			std::format("ProxyStressSlot{}", SlotIndex));
	}

	std::vector<Durin::DStaticMeshComponent*> Components;
	Components.reserve(SharedUserCount);
	for (Durin::uint32 UserIndex = 0; UserIndex < SharedUserCount; ++UserIndex)
	{
		auto* Component = Harness.CreateStaticMeshComponent(
			Durin::FName(std::format("ProxyStressUser{}", UserIndex)));
		ASSERT_NE(Component, nullptr);
		Component->SetStaticMesh(Mesh);
		Component->SetMaterial(0, Shared);
		for (Durin::uint32 SlotIndex = 1; SlotIndex < SlotCount; ++SlotIndex)
		{
			Component->SetMaterial(SlotIndex, SlotMaterials[SlotIndex]);
		}
		Component->RegisterComponent();
		Components.push_back(Component);
	}

	const FMaterialSlotsSnapshot InitialSlots =
		CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(InitialSlots.Materials.size(), SlotCount);
	EXPECT_EQ(
		InitialSlots.MaterialProxies[0],
		Shared->GetMaterialRenderProxy().GetReference());
	EXPECT_EQ(
		CapturePrimitiveCount(),
		static_cast<size_t>(SharedUserCount));

	std::vector<Durin::DMaterial*> UnrelatedMaterials;
	UnrelatedMaterials.reserve(UnrelatedMaterialCount);
	for (Durin::uint32 Index = 0; Index < UnrelatedMaterialCount; ++Index)
	{
		UnrelatedMaterials.push_back(Durin::NewObject<Durin::DMaterial>(
			nullptr,
			Durin::FName(std::format("ProxyStressUnrelated{}", Index))));
	}

	Durin::FMaterialRenderProxyRef SharedProxy =
		Shared->GetMaterialRenderProxy();
	Durin::FMaterialRenderProxyRef QueuedDestructionProxy;
	{
		auto* QueuedDestructionMaterial = Durin::NewObject<Durin::DMaterial>(
			nullptr, "ProxyStressQueuedDestruction");
		QueuedDestructionProxy =
			QueuedDestructionMaterial->GetMaterialRenderProxy();
		ASSERT_TRUE(QueuedDestructionProxy);
		CaptureMaterialProxy(QueuedDestructionProxy);
		Durin::MarkAsGarbage(QueuedDestructionMaterial);
	}

	Durin::ResetMaterialLoadedQueryDiagnostics();
	Durin::ResetMaterialRenderProxyCounters();
	const auto CommandStarted = std::make_shared<std::promise<void>>();
	std::future<void> CommandStartedFuture = CommandStarted->get_future();
	const auto AllowCommandCompletion = std::make_shared<std::promise<void>>();
	std::shared_future<void> AllowCommandCompletionFuture =
		AllowCommandCompletion->get_future().share();
	struct FBlockMaterialStressCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "BlockMaterialStressPublication";
		}
	};
	Durin::EnqueueRenderCommand<FBlockMaterialStressCommand>(
		[CommandStarted, AllowCommandCompletionFuture](
			Durin::FRHICommandListImmediate&) {
			CommandStarted->set_value();
			AllowCommandCompletionFuture.wait();
		});
	CommandStartedFuture.wait();

	for (Durin::uint32 UpdateIndex = 0; UpdateIndex < 6; ++UpdateIndex)
	{
		ASSERT_TRUE(Base->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(
				0.25 + 0.05 * UpdateIndex,
				0.35 + 0.04 * UpdateIndex,
				0.45 + 0.03 * UpdateIndex)));
		ASSERT_TRUE(Shared->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(
				0.3 + 0.05 * UpdateIndex,
				0.45 + 0.03 * UpdateIndex,
				0.6 - 0.02 * UpdateIndex)));
	}
	Components[0]->SetMaterial(0, Replacement);
	Components[0]->SetMaterial(0, Shared);
	Components[1]->SetMaterial(1, Replacement);
	Components[1]->SetMaterial(1, SlotMaterials[1]);

	AllowCommandCompletion->set_value();
	const FMaterialSlotsSnapshot UpdatedSlots =
		CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(UpdatedSlots.Materials.size(), SlotCount);
	EXPECT_EQ(
		UpdatedSlots.MaterialProxies[0],
		SharedProxy.GetReference());
	EXPECT_EQ(
		CapturePrimitiveCount(),
		static_cast<size_t>(SharedUserCount));
	ExpectColorNear(
		GetMaterialBinding(UpdatedSlots.Materials[0]).BaseColor,
		Durin::FVector4f(0.55f, 0.60f, 0.50f, 1.0f));

	const FMaterialProxySnapshot UpdatedLeaf = CaptureMaterialProxy(LeafProxy);
	EXPECT_GT(UpdatedLeaf.ResolvedVersion, InitialLeaf.ResolvedVersion);
	ExpectColorNear(
		GetMaterialBinding(UpdatedLeaf.RenderData).BaseColor,
		Durin::FVector4f(0.50f, 0.55f, 0.60f, 1.0f));
	const Durin::FMaterialLoadedQueryDiagnostics QueryDiagnostics =
		Durin::GetMaterialLoadedQueryDiagnostics();
	EXPECT_EQ(
		QueryDiagnostics.LastOperation,
		Durin::EMaterialLoadedQueryOperation::None);
	EXPECT_EQ(QueryDiagnostics.QueryCount, 0);
	EXPECT_EQ(QueryDiagnostics.SnapshotCount, 0);
	EXPECT_EQ(QueryDiagnostics.ScannedObjectCount, 0);
	EXPECT_EQ(QueryDiagnostics.ScannedMaterialCount, 0);
	const Durin::FMaterialRenderProxyCounters ProxyCounters =
		Durin::GetMaterialRenderProxyCounters();
	EXPECT_GE(ProxyCounters.PublicationCount, 1);
	EXPECT_GE(ProxyCounters.CoalescedPublicationCount, 1);

	Durin::CollectGarbage();
	const FMaterialProxySnapshot AfterQueuedDestruction =
		CaptureMaterialProxy(QueuedDestructionProxy);
	EXPECT_GT(AfterQueuedDestruction.ResolvedVersion, 0);

	for (Durin::DStaticMeshComponent* Component : Components)
	{
		Component->UnregisterComponent();
		Durin::MarkAsGarbage(Component);
	}
	for (Durin::DMaterial* Material : UnrelatedMaterials)
	{
		Durin::MarkAsGarbage(Material);
	}
	for (Durin::DMaterial* Material : SlotMaterials)
	{
		Durin::MarkAsGarbage(Material);
	}
	for (Durin::DMaterialInstance* Instance : Chain)
	{
		Durin::MarkAsGarbage(Instance);
	}
	Durin::MarkAsGarbage(Replacement);
	Durin::MarkAsGarbage(Shared);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(AlternateBase);
	Durin::MarkAsGarbage(Base);
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(QueuedDestructionProxy));
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(SharedProxy));
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(LeafProxy));
	WaitForRenderingThread();
	Harness.Shutdown();
	Durin::CollectGarbage();
}
