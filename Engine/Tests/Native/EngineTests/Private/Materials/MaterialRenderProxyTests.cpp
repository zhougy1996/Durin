#include "MaterialTestSupport.h"

#include "Materials/MaterialRenderProxy.h"

#include <future>

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
		ExpectColorNear(Actual.BaseColor, Expected.BaseColor);
		EXPECT_EQ(Actual.BaseColorTexture, Expected.BaseColorTexture);
		EXPECT_FLOAT_EQ(
			Actual.SpecularStrength, Expected.SpecularStrength);
		EXPECT_FLOAT_EQ(Actual.Shininess, Expected.Shininess);
		EXPECT_EQ(Actual.PipelineIdentity, Expected.PipelineIdentity);
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
		Updated.RenderData.BaseColor,
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
		LocallyOverridden.RenderData.BaseColor.a, 0.35f);
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
