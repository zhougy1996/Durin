#include "Threading/TaskComposition.h"
#include "MaterialTestSupport.h"

#include "Asset/AssetCompilingManager.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialCookedProgram.h"
#include "Modules/ModuleManager.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"

#include <iostream>

namespace
{
	auto MeasureInstanceVariantQualificationBaseline() -> void;

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
	MeasureInstanceVariantQualificationBaseline();

	auto* First = Durin::NewObject<Durin::DMaterial>(
		nullptr, "AsyncCompileFirst");
	auto* Second = Durin::NewObject<Durin::DMaterial>(
		nullptr, "AsyncCompileSecond");
	ASSERT_EQ(First->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::NeverRequested);
	ASSERT_EQ(Second->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::NeverRequested);
	{
		// Single-flight requires overlapping requests. A warm compiler can finish
		// before the second submission unless the fixture holds worker entry.
		const uint32 WorkerCount = Durin::GetTaskSchedulerDiagnostics().WorkerCount;
		Durin::FThreadEvent Started, Release;
		std::atomic<uint32> StartedCount = 0;
		std::vector<Durin::FTaskHandle> Blockers;
		struct FReleaseWorkers
		{
			Durin::FThreadEvent& Event;
			std::vector<Durin::FTaskHandle>& Tasks;
			~FReleaseWorkers() { Event.Trigger(); for (const auto& Task : Tasks) Durin::WaitTask(Task); }
		} ReleaseWorkers{Release, Blockers};
		for (uint32 Index = 0; Index < WorkerCount; ++Index)
			Blockers.push_back(Durin::Tasks::LaunchTask("HoldMaterialSingleFlight", [&] {
				if (StartedCount.fetch_add(1) + 1 == WorkerCount) Started.Trigger();
				Release.WaitFor(2.0);
			}).GetCompletion().GetTaskHandle());
		ASSERT_TRUE(Started.WaitFor(1.0));
		ASSERT_TRUE(Durin::RequestMaterialRecompile(*First));
		ASSERT_TRUE(Durin::RequestMaterialRecompile(*Second));
		ASSERT_EQ(First->GetMaterialCompileStatus().State,
			Durin::EMaterialCompileState::Running);
		ASSERT_EQ(Second->GetMaterialCompileStatus().State,
			Durin::EMaterialCompileState::Pending);
		EXPECT_GE(Durin::GetMaterialCompilationDiagnostics()
			.SingleFlightConsumers, 1u);
	}
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
	auto Validation = First->SetMaterialProgram(
		EditFirstScalarConstant(*First, 0.03125f));
	ASSERT_TRUE(Validation);
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), InitialProgram);
	EXPECT_TRUE(First->GetMaterialCompileStatus().bLastKnownGoodDisplayed);
	ASSERT_TRUE((Validation = First->SetMaterialProgram(
		EditFirstScalarConstant(*First, 0.0625f))));
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

	auto ParameterValidation = First->SetMaterialProgram(
		Durin::MakePBRMaterialProgram());

	ASSERT_TRUE(ParameterValidation);
	auto* PendingInstance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PendingParameterEdit");
	ASSERT_TRUE(PendingInstance->SetParent(First));
	EXPECT_TRUE(First->GetAcceptedCompiledProgram()->ActiveParameters.empty());
	ASSERT_TRUE(PendingInstance->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.2, 0.4)));
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	ASSERT_TRUE(First->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.6, 0.8)));
	const auto LastKnownGood = First->GetAcceptedCompiledProgram();
	const Durin::FMaterialStaticProperties LastKnownGoodProperties =
		First->GetRenderableStaticProperties();
	Durin::FMaterialStaticProperties FailedProperties =
		First->GetStaticProperties();
	FailedProperties.BlendMode = Durin::EMaterialBlendMode::Translucent;
	FailedProperties.bTwoSided = !FailedProperties.bTwoSided;
	ASSERT_TRUE(First->SetStaticProperties(FailedProperties));
	ASSERT_TRUE((ParameterValidation = First->SetMaterialProgram(
		Durin::MakeDefaultMaterialProgram())));
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
	EXPECT_FALSE(First->GetAcceptedCompiledProgram()->ActiveParameters.empty());
	ExpectColorNear(GetMaterialBinding(First->GetRenderData()).BaseColor,
		Durin::FVector4f(0.2f, 0.6f, 0.8f, 1.0f));
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

	// Failed replacement retains the old schema after authored declarations are deleted.
	{
		auto* Root = Durin::NewObject<Durin::DMaterial>(nullptr, "RetainedDeclarationRoot");
		ASSERT_TRUE(Root->SetMaterialProgram(Durin::MakePBRMaterialProgram()));
		ASSERT_TRUE(WaitForMaterialCompile(*Root));
		ASSERT_NE(Root, nullptr);
		ASSERT_NE(Root->GetAcceptedCompiledProgram(), nullptr);
		auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "RetainedDeclarationInstance");
		ASSERT_TRUE(Instance->SetParent(Root));
		ASSERT_TRUE(Root->SetScalarParameterValue(Durin::MaterialParameters::MetallicName(), 0.65f));
		ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::MetallicName(), 0.9f));
		const auto Accepted = Root->GetAcceptedCompiledProgram();
		Durin::FMaterialParameterDefinition Definition;
		Definition.Id = Durin::FGuid::NewGuid();
		Definition.Name = "IndependentAmount";
		Durin::FMaterialProgram Program;
		Durin::FMaterialProgramNode Node;
		Node.Id = Durin::FGuid::NewGuid();
		Node.ParameterId = Definition.Id;
		Node.Opcode = Durin::EMaterialProgramOpcode::Parameter;
		Program.Nodes.push_back(Node);
		Program.Outputs.Roughness.SourceNodeId = Node.Id;
		Durin::FThreadEvent Started, Release;
		std::atomic<uint32> StartedCount = 0;
		std::vector<Durin::FTaskHandle> Blockers;
		const auto WorkerCount = Durin::GetTaskSchedulerDiagnostics().WorkerCount;
		struct FReleaseWorkers
		{
			Durin::FThreadEvent& Release;
			std::vector<Durin::FTaskHandle>& Tasks;
			~FReleaseWorkers() { Release.Trigger(); for (const auto& Task : Tasks) Durin::WaitTask(Task); }
		};
		{
			FReleaseWorkers ReleaseWorkers{Release, Blockers};
			for (uint32 Index = 0; Index < WorkerCount; ++Index)
				Blockers.push_back(Durin::Tasks::LaunchTask("HoldRetainedSchemaCompile", [&] {
					if (StartedCount.fetch_add(1) + 1 == WorkerCount) Started.Trigger();
					Release.WaitFor(10.0);
				}).GetCompletion().GetTaskHandle());
			ASSERT_TRUE(Started.WaitFor(2.0));
			ASSERT_TRUE(Root->SetMaterialDefinitionsAndProgram({Definition}, Program));
			const auto Pending = Root->GetMaterialCompileStatus();
			Durin::FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Root);
			Durin::FMaterialCompileResult Failed{
				.Owner = Durin::MakeObjectHandle(Root),
				.AuthoredRevision = Pending.AuthoredRevision,
				.Generation = Pending.RequestGeneration,
				.DependencyRevision = Pending.DependencyRevision,
				.ProgramIdentity = Pending.RequestedIdentity,
				.Target = Pending.Target,
				.State = Durin::EMaterialCompileState::Failed,
				.Category = Durin::EMaterialCompileResultCategory::Compile};
			EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(*Root, std::move(Failed)));
		}
		EXPECT_EQ(Root->GetAcceptedCompiledProgram(), Accepted);
		EXPECT_FLOAT_EQ(GetMaterialBinding(Root->GetRenderData()).Metallic, 0.65f);
		EXPECT_FLOAT_EQ(GetMaterialBinding(Instance->GetRenderData()).Metallic, 0.9f);
		Durin::MarkAsGarbage(Instance);
		Durin::MarkAsGarbage(Root);
	}

	Durin::MarkAsGarbage(PendingInstance);
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

namespace
{
auto MeasureInstanceVariantQualificationBaseline() -> void
{
	struct FFixtureScope
	{
		std::vector<Durin::DObject*> Objects;
		~FFixtureScope()
		{
			for (auto* Object : Objects) Durin::MarkAsGarbage(Object);
			Durin::CollectGarbage();
		}
	} Scope;
	auto* Root = Durin::NewObject<Durin::DMaterial>(nullptr, "VariantFixtureRoot");
	Scope.Objects.push_back(Root);
	ASSERT_TRUE(Root->SetMaterialProgram(Durin::MakePBRMaterialProgram()));
	ASSERT_TRUE(WaitForMaterialCompile(*Root));
	Durin::FMaterialCompilerInput Input;
	Input.Program = *Root->GetMaterialProgram();
	for (const auto& Definition : Root->GetParameterDefinitions())
		Input.Parameters.push_back({Definition.Id, Definition.Type});
	std::string Error;
	ASSERT_TRUE(Durin::BuildDefaultMaterialCompilerEnvironment(Input.Environment, Error)) << Error;

	const auto Before = Durin::GetMaterialCompilationDiagnostics();
	std::array<Durin::DMaterialInstance*, 8> Instances{};
	std::vector<Durin::FMaterialProgramIdentity> Identities;
	uint32 CompatibleOwners = 0;
	for (size_t Index = 0; Index < Instances.size(); ++Index)
	{
		auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(
			nullptr, Durin::FName(std::format("VariantFixture{}", Index)));
		Scope.Objects.push_back(Instance);
		Instances[Index] = Instance;
		ASSERT_TRUE(Instance->SetParent(Index == 7 ? Instances[6]
			: static_cast<Durin::DMaterialInterface*>(Root)));
		auto Properties = Root->GetStaticProperties();
		if (Index == 1) Properties.OpacityMaskThreshold = 0.25f;
		if (Index == 2 || Index == 3)
		{
			Properties.BlendMode = Durin::EMaterialBlendMode::Masked;
			Properties.OpacityMaskThreshold = Index == 2 ? 0.25f : 0.75f;
		}
		if (Index == 4) Properties.BlendMode = Durin::EMaterialBlendMode::Translucent;
		if (Index == 5)
		{
			Properties.bTwoSided = true;
			Properties.DepthWritePolicy = Durin::EMaterialDepthWritePolicy::Disabled;
		}
		if (Index != 0 && Index != 7)
			ASSERT_TRUE(Instance->SetStaticPropertiesOverride(Properties));
		Input.StaticProperties = Instance->GetStaticProperties();
		const auto Normalized = Durin::NormalizeMaterialProgram(Input);
		ASSERT_TRUE(Normalized);
		if (std::ranges::find(Identities, Normalized.Identity) == Identities.end())
			Identities.push_back(Normalized.Identity);
		if (Instance->GetAcceptedCompiledProgram()) ++CompatibleOwners;
	}
	ASSERT_TRUE(Instances[7]->SetParent(Instances[2]));
	EXPECT_EQ(Instances[7]->GetStaticProperties().BlendMode, Durin::EMaterialBlendMode::Masked);
	ASSERT_TRUE(Instances[7]->SetParent(Instances[6]));
	ASSERT_TRUE(Instances[0]->SetScalarParameterValue(Durin::MaterialParameters::MetallicName(), 0.7f));
	Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	const auto After = Durin::GetMaterialCompilationDiagnostics();
	// Canonical properties now share inactive cutoffs; independent instance
	// compilation remains the next qualification stage.
	EXPECT_EQ(Identities.size(), 4u);
	EXPECT_EQ(CompatibleOwners, 5u);
	EXPECT_EQ(After.AcceptedRequests - Before.AcceptedRequests, 0u);
	EXPECT_EQ(After.InFlightCount, 0u);
	EXPECT_EQ(After.OutstandingConsumerCount, 0u);
	EXPECT_EQ(After.PendingPublicationCount, 0u);
	Durin::FByteBuffer Bytes;
	ASSERT_TRUE(Durin::EncodeMaterialCookedProgram(*Root->GetAcceptedCompiledProgram(),
		Root->GetRenderableStaticProperties(), Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, Bytes, Error)) << Error;
	std::cout << "Variant baseline: owners=" << Instances.size()
		<< " effective_identities=" << Identities.size()
		<< " compatible_instances=" << CompatibleOwners
		<< " instance_requests=" << After.AcceptedRequests - Before.AcceptedRequests
		<< " total_requests=" << After.AcceptedRequests
		<< " completed_requests=" << After.CompletedRequests
		<< " retained_programs=" << After.RetainedProgramCount
		<< " retained_bytes=" << After.RetainedProgramBytes
		<< " root_dmat_bytes=" << Bytes.size() << '\n';
}
}

TEST(FMaterialCompileLifecycleTests,
	CookedProgramRoundTripIsDeterministicBoundedAndTargetQualified)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	auto* Material = Durin::NewObject<Durin::DMaterial>(
		nullptr, "CookedProgramRoundTrip");
	auto Validation = Material->SetMaterialProgram(
		Durin::MakePBRMaterialProgram());
	ASSERT_TRUE(Validation);
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
	EXPECT_EQ(DecodedProgram->ActiveParameters,
		Material->GetAcceptedCompiledProgram()->ActiveParameters);
	EXPECT_EQ(DecodedProgram->ActiveParameters.size(),
		Durin::GetPBRMaterialParameterDefinitions().size());
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
	ASSERT_FALSE(DecodedProgram->ActiveParameters.empty());
	for (int Corruption = 0; Corruption < 3; ++Corruption)
	{
		auto Invalid = *DecodedProgram;
		if (Corruption == 0) Invalid.ActiveParameters.push_back(Invalid.ActiveParameters.front());
		if (Corruption == 1) Invalid.ActiveParameters.front().Id = {};
		if (Corruption == 2) Invalid.ActiveParameters.front().Type =
			static_cast<Durin::EMaterialParameterType>(255);
		Durin::FByteBuffer InvalidBytes;
		EXPECT_FALSE(Durin::EncodeMaterialCookedProgram(Invalid, DecodedProperties,
			Durin::ECookTargetPlatform::Win64, Durin::ECookTargetProfile::Game,
			InvalidBytes, Error));
	}

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
	Durin::FByteBuffer OldSchemaBytes = FirstBytes;
	OldSchemaBytes[4] = std::byte{2};
	EXPECT_FALSE(Durin::DecodeMaterialCookedProgram(
		OldSchemaBytes, Durin::ECookTargetPlatform::Win64,
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
