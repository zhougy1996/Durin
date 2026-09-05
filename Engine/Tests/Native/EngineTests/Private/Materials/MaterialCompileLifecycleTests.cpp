#include "MaterialTestSupport.h"

#include "Asset/AssetCompilingManager.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialCookedProgram.h"
#include "Modules/ModuleManager.h"
#include "Threading/Task.h"

namespace
{
	auto WaitForMaterialCompile(
		Durin::DMaterial& Material,
		std::chrono::milliseconds Timeout = std::chrono::seconds(10)) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + Timeout;
		while (std::chrono::steady_clock::now() < Deadline)
		{
			Durin::FAssetCompilingManager::Get().ProcessAsyncTasks();
			const Durin::EMaterialCompileState State =
				Material.GetMaterialCompileStatus().State;
			if (State != Durin::EMaterialCompileState::Pending
				&& State != Durin::EMaterialCompileState::Running)
				return State == Durin::EMaterialCompileState::Ready;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

	auto EditFirstScalarConstant(
		const Durin::DMaterial& Material,
		float Delta) -> Durin::FMaterialProgram
	{
		Durin::FMaterialProgram Program = *Material.GetMaterialProgram();
		Program.Outputs.RoughnessDefault.X += Delta;
		return Program;
	}
}

TEST(FMaterialCompileLifecycleTests,
	LatestGenerationSingleFlightLastKnownGoodAndShutdownAreBounded)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	const bool bOwnsScheduler = !Durin::IsTaskSchedulerRunning();
	if (bOwnsScheduler) ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	ASSERT_TRUE(Durin::InitializeAssetCompilingManager());

	auto* First = Durin::NewObject<Durin::DMaterial>(
		nullptr, "AsyncCompileFirst");
	auto* Second = Durin::NewObject<Durin::DMaterial>(
		nullptr, "AsyncCompileSecond");
	ASSERT_EQ(First->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::NeverRequested);
	ASSERT_EQ(Second->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::NeverRequested);
	ASSERT_TRUE(Durin::RequestMaterialRecompile(*First));
	ASSERT_TRUE(Durin::RequestMaterialRecompile(*Second));
	ASSERT_EQ(First->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::Running);
	ASSERT_EQ(Second->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::Pending);
	EXPECT_GE(Durin::GetMaterialCompilationDiagnostics()
		.SingleFlightConsumers, 1u);
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	ASSERT_TRUE(WaitForMaterialCompile(*Second));
	const auto InitialProgram = First->GetAcceptedCompiledProgram();
	ASSERT_TRUE(InitialProgram);
	EXPECT_EQ(InitialProgram, Second->GetAcceptedCompiledProgram());
	EXPECT_EQ(Second->GetMaterialCompileStatus().CacheOutcome,
		Durin::EMaterialCompileCacheOutcome::SingleFlight);
	EXPECT_TRUE(First->GetMaterialCompileStatus().IsCurrent());
	ASSERT_TRUE(Durin::RequestMaterialRecompile(*First));
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	EXPECT_EQ(First->GetMaterialCompileStatus().CacheOutcome,
		Durin::EMaterialCompileCacheOutcome::RetainedHit);
	const Durin::FMaterialRenderProxyRef DemandedProxy =
		First->GetMaterialRenderProxy();
	const uint64 DemandedGeneration =
		First->GetMaterialCompileStatus().RequestGeneration;
	const uint64 UnusedGeneration =
		Second->GetMaterialCompileStatus().RequestGeneration;
	Durin::NotifyMaterialShaderReload(false);
	Durin::FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(First->GetMaterialCompileStatus().RequestGeneration,
		DemandedGeneration + 1);
	EXPECT_EQ(Second->GetMaterialCompileStatus().RequestGeneration,
		UnusedGeneration);
	ASSERT_TRUE(WaitForMaterialCompile(*First));

	const uint64 InitialGeneration =
		First->GetMaterialCompileStatus().RequestGeneration;
	Durin::FMaterialProgramValidationResult Validation;
	ASSERT_TRUE(First->SetMaterialProgram(
		EditFirstScalarConstant(*First, 0.03125f), Validation));
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), InitialProgram);
	EXPECT_TRUE(First->GetMaterialCompileStatus().bLastKnownGoodDisplayed);
	ASSERT_TRUE(First->SetMaterialProgram(
		EditFirstScalarConstant(*First, 0.0625f), Validation));
	EXPECT_EQ(First->GetMaterialCompileStatus().RequestGeneration,
		InitialGeneration + 2);
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), InitialProgram);
	ASSERT_TRUE(WaitForMaterialCompile(*First))
		<< "state=" << static_cast<uint32>(First->GetMaterialCompileStatus().State)
		<< " category=" << static_cast<uint32>(First->GetMaterialCompileStatus().ResultCategory)
		<< " diagnostic=" << (First->GetMaterialCompileDiagnostics().empty()
			? std::string("<none>")
			: First->GetMaterialCompileDiagnostics().front().Source.Message);
	ASSERT_TRUE(First->GetAcceptedCompiledProgram());
	EXPECT_NE(First->GetAcceptedCompiledProgram()->Identity,
		InitialProgram->Identity);
	EXPECT_FALSE(First->GetMaterialCompileStatus().bLastKnownGoodDisplayed);

	const auto LastKnownGood = First->GetAcceptedCompiledProgram();
	const Durin::FMaterialStaticProperties LastKnownGoodProperties =
		First->GetRenderableStaticProperties();
	Durin::FMaterialStaticProperties FailedProperties =
		First->GetStaticProperties();
	FailedProperties.BlendMode = Durin::EMaterialBlendMode::Translucent;
	FailedProperties.bTwoSided = !FailedProperties.bTwoSided;
	ASSERT_TRUE(First->SetStaticProperties(FailedProperties));
	const Durin::FMaterialCompileStatus Pending =
		First->GetMaterialCompileStatus();
	Durin::FAssetCompilingManager::Get().MarkCompilationAsCanceled(*First);
	Durin::FMaterialCompileResult Failed{
		.Owner = Durin::MakeObjectHandle(First),
		.AuthoredRevision = Pending.AuthoredRevision,
		.Generation = Pending.RequestGeneration,
		.DependencyRevision = Pending.DependencyRevision,
		.ProgramIdentity = Pending.RequestedIdentity,
		.Target = Pending.Target,
		.State = Durin::EMaterialCompileState::Failed,
		.Category = Durin::EMaterialCompileResultCategory::Compile,
	};
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(Failed)));
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), LastKnownGood);
	EXPECT_EQ(First->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::Failed);
	EXPECT_TRUE(First->GetMaterialCompileStatus().bLastKnownGoodDisplayed);
	const Durin::FMaterialStaticProperties RenderableProperties =
		First->GetRenderableStaticProperties();
	EXPECT_EQ(RenderableProperties.BlendMode,
		LastKnownGoodProperties.BlendMode);
	EXPECT_EQ(RenderableProperties.bTwoSided, FailedProperties.bTwoSided);
	EXPECT_EQ(First->GetRenderData().PlanningPassIdentity.ShaderMap.BlendMode,
		LastKnownGoodProperties.BlendMode);

	Durin::FMaterialCompileResult Stale{
		.Owner = Durin::MakeObjectHandle(First),
		.AuthoredRevision = Pending.AuthoredRevision,
		.Generation = Pending.RequestGeneration - 1,
		.DependencyRevision = Pending.DependencyRevision,
		.ProgramIdentity = LastKnownGood->Identity,
		.Target = Pending.Target,
		.State = Durin::EMaterialCompileState::Ready,
		.CompiledProgram = LastKnownGood,
	};
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(Stale)));
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), LastKnownGood);
	Durin::FMaterialCompileResult WrongTarget{
		.Owner = Durin::MakeObjectHandle(First),
		.AuthoredRevision = Pending.AuthoredRevision,
		.Generation = Pending.RequestGeneration,
		.DependencyRevision = Pending.DependencyRevision,
		.ProgramIdentity = LastKnownGood->Identity,
		.Target = "wrong-target",
		.State = Durin::EMaterialCompileState::Ready,
		.CompiledProgram = LastKnownGood,
	};
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(WrongTarget)));
	Durin::FMaterialCompileResult WrongDependency{
		.Owner = Durin::MakeObjectHandle(First),
		.AuthoredRevision = Pending.AuthoredRevision,
		.Generation = Pending.RequestGeneration,
		.DependencyRevision = Pending.DependencyRevision + 1,
		.ProgramIdentity = LastKnownGood->Identity,
		.Target = Pending.Target,
		.State = Durin::EMaterialCompileState::Ready,
		.CompiledProgram = LastKnownGood,
	};
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(WrongDependency)));
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), LastKnownGood);

	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
	Durin::ShutdownAssetCompilingManager();
	const Durin::FMaterialCompilationDiagnostics Shutdown =
		Durin::GetMaterialCompilationDiagnostics();
	EXPECT_FALSE(Shutdown.bAcceptingRequests);
	EXPECT_EQ(Shutdown.InFlightCount, 0u);
	EXPECT_EQ(Shutdown.OutstandingConsumerCount, 0u);
	EXPECT_EQ(Shutdown.PendingPublicationCount, 0u);
	EXPECT_EQ(Shutdown.RetainedProgramCount, 0u);
	EXPECT_EQ(Shutdown.RetainedProgramBytes, 0u);
	if (bOwnsScheduler)
		Durin::ShutdownTaskSystem(Durin::ETaskShutdownMode::Drain);
}

TEST(FMaterialCompileLifecycleTests,
	CookedProgramRoundTripIsDeterministicBoundedAndTargetQualified)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	auto* Material = Durin::NewObject<Durin::DMaterial>(
		nullptr, "CookedProgramRoundTrip");
	ASSERT_TRUE(Material->GetAcceptedCompiledProgram());

	Durin::FByteBuffer FirstBytes;
	Durin::FByteBuffer SecondBytes;
	std::string Error;
	ASSERT_TRUE(Durin::EncodeMaterialCookedProgram(
		*Material->GetAcceptedCompiledProgram(), Material->GetStaticProperties(),
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, FirstBytes, Error)) << Error;
	ASSERT_TRUE(Durin::EncodeMaterialCookedProgram(
		*Material->GetAcceptedCompiledProgram(), Material->GetStaticProperties(),
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, SecondBytes, Error)) << Error;
	EXPECT_EQ(FirstBytes, SecondBytes);
	EXPECT_LE(FirstBytes.size(), Durin::MaterialCookedProgramMaxPayloadBytes);

	Durin::FMaterialStaticProperties DecodedProperties;
	std::shared_ptr<const Durin::FMaterialCompilerResult> DecodedProgram;
	ASSERT_TRUE(Durin::DecodeMaterialCookedProgram(
		FirstBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		DecodedProperties, DecodedProgram, Error)) << Error;
	ASSERT_TRUE(DecodedProgram);
	EXPECT_EQ(DecodedProgram->Identity,
		Material->GetAcceptedCompiledProgram()->Identity);
	EXPECT_EQ(DecodedProperties, Material->GetStaticProperties());
	ASSERT_EQ(DecodedProgram->CompiledShaders.size(),
		Material->GetAcceptedCompiledProgram()->CompiledShaders.size());
	for (size_t Index = 0; Index < DecodedProgram->CompiledShaders.size(); ++Index)
	{
		const Durin::FCompiledShader& Decoded =
			DecodedProgram->CompiledShaders[Index];
		const Durin::FCompiledShader& Source =
			Material->GetAcceptedCompiledProgram()->CompiledShaders[Index];
		EXPECT_EQ(Decoded.Frequency, Source.Frequency);
		EXPECT_EQ(Decoded.SourceEntryPoint, Source.SourceEntryPoint);
		EXPECT_EQ(Decoded.BinaryEntryPoint, Source.BinaryEntryPoint);
		EXPECT_EQ(Decoded.Hash, Source.Hash);
		ASSERT_TRUE(Decoded.Code);
		ASSERT_TRUE(Source.Code);
		EXPECT_EQ(*Decoded.Code, *Source.Code);
	}
	EXPECT_TRUE(DecodedProgram->IR.Nodes.empty());
	EXPECT_TRUE(DecodedProgram->GeneratedSource.empty());

	EXPECT_FALSE(Durin::DecodeMaterialCookedProgram(
		FirstBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::EditorValidation,
		DecodedProperties, DecodedProgram, Error));
	Durin::FMaterialCompilerResult WrongEnvironment =
		*Material->GetAcceptedCompiledProgram();
	WrongEnvironment.CompilerIdentity = "incompatible-compiler";
	Durin::FByteBuffer WrongEnvironmentBytes;
	ASSERT_TRUE(Durin::EncodeMaterialCookedProgram(
		WrongEnvironment, Material->GetStaticProperties(),
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		WrongEnvironmentBytes, Error)) << Error;
	EXPECT_FALSE(Durin::DecodeMaterialCookedProgram(
		WrongEnvironmentBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		DecodedProperties, DecodedProgram, Error));
	WrongEnvironment = *Material->GetAcceptedCompiledProgram();
	WrongEnvironment.Target = "wrong-target";
	ASSERT_TRUE(Durin::EncodeMaterialCookedProgram(
		WrongEnvironment, Material->GetStaticProperties(),
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		WrongEnvironmentBytes, Error)) << Error;
	EXPECT_FALSE(Durin::DecodeMaterialCookedProgram(
		WrongEnvironmentBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		DecodedProperties, DecodedProgram, Error));
	Durin::FByteBuffer TrailingBytes = FirstBytes;
	TrailingBytes.push_back(std::byte{0});
	EXPECT_FALSE(Durin::DecodeMaterialCookedProgram(
		TrailingBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		DecodedProperties, DecodedProgram, Error));

	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}
