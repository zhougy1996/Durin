#include "Materials/MaterialInterface.h"

#include "Asset/Asset.h"
#include "Asset/AssetCompilingManager.h"
#include "Modules/ModuleManager.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Materials/MaterialInstance.h"
#include "Materials/Material.h"
#include "Logging/LogMacros.h"
#include "Texture/Texture2D.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		FMaterialLoadedQueryDiagnostics GMaterialLoadedQueryDiagnostics;

		auto CheckMaterialQueryThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		const FMaterialStaticProperties GDefaultMaterialStaticProperties;

		template <typename Predicate>
		auto QueryLoadedMaterialHandles(
			EMaterialLoadedQueryOperation Operation,
			Predicate&& PredicateFn
		) -> std::vector<FObjectHandle>
		{
			CheckMaterialQueryThread();
			std::vector<FObjectHandle> Result;
			const std::vector<DObject*> Objects = GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly);
			++GMaterialLoadedQueryDiagnostics.QueryCount;
			++GMaterialLoadedQueryDiagnostics.SnapshotCount;
			GMaterialLoadedQueryDiagnostics.LastOperation = Operation;
			GMaterialLoadedQueryDiagnostics.ScannedObjectCount += Objects.size();
			for (DObject* Object : Objects)
			{
				auto* Material = Cast<DMaterialInterface>(Object);
				if (!IsValid(Material)) continue;
				++GMaterialLoadedQueryDiagnostics.ScannedMaterialCount;
				if (!PredicateFn(Material)) continue;
				const FObjectHandle Handle = MakeObjectHandle(Material);
				if (!IsObjectHandleNull(Handle)) Result.push_back(Handle);
			}
			std::ranges::sort(Result, [](FObjectHandle Left, FObjectHandle Right) {
				return Left.Index < Right.Index
				|| (Left.Index == Right.Index && Left.Generation < Right.Generation);
			});
			GMaterialLoadedQueryDiagnostics.LastResultCount = Result.size();
			return Result;
		}
	}

	auto ResolveMaterialProperties(const DMaterialInterface& Material,
		FResolvedMaterialProperties& OutProperties, std::string& OutError) -> bool
	{
		CheckMaterialQueryThread();
		std::array<const DMaterialInterface*, MaterialMaximumParentDepth> Chain{};
		size_t Count = 0;
		const DMaterial* Root = nullptr;
		for (auto* Current = &Material; Current; Current = Current->GetParent())
		{
			if (Count == Chain.size()
				|| std::find(Chain.begin(), Chain.begin() + Count, Current) != Chain.begin() + Count)
			{
				OutError = "Material parent chain exceeds 64 owners or contains a cycle.";
				return false;
			}
			if (!IsValid(Current)) break;
			Chain[Count++] = Current;
			if ((Root = Cast<DMaterial>(Current))) break;
		}
		if (!Root)
		{
			OutError = "Material parent chain has no valid root material.";
			return false;
		}
		FResolvedMaterialProperties Result;
		Result.Root = MakeObjectHandle(const_cast<DMaterial*>(Root));
		Result.Properties = Root->GetStaticProperties();
		if (!ValidateMaterialStaticProperties(Result.Properties, OutError)) return false;
		Result.Sources.fill(Result.Root);
		while (Count > 1)
		{
			const auto* Instance = Cast<DMaterialInstance>(Chain[--Count - 1]);
			if (!Instance)
			{
				OutError = "Material parent chain contains an unsupported material owner.";
				return false;
			}
			const auto& Overrides = Instance->GetPropertyOverrides();
			if (!ValidateMaterialStaticProperties(Overrides.Values, OutError)) return false;
			Overrides.ApplyTo(Result.Properties);
			const std::array Enabled{Overrides.bOverrideBlendMode, Overrides.bOverrideShadingModel,
				Overrides.bOverrideOpacityMaskThreshold, Overrides.bOverrideTwoSided,
				Overrides.bOverrideDepthWritePolicy};
			for (size_t Index = 0; Index < Enabled.size(); ++Index)
				if (Enabled[Index]) Result.Sources[Index] = MakeObjectHandle(const_cast<DMaterialInstance*>(Instance));
		}
		Result.ShaderProperties = CanonicalizeMaterialShaderProperties(Result.Properties);
		OutProperties = Result;
		OutError.clear();
		return true;
	}

	auto GetMaterialLoadedQueryDiagnostics() -> FMaterialLoadedQueryDiagnostics
	{
		CheckMaterialQueryThread();
		return GMaterialLoadedQueryDiagnostics;
	}

	auto ResetMaterialLoadedQueryDiagnostics() -> void
	{
		CheckMaterialQueryThread();
		GMaterialLoadedQueryDiagnostics = {};
	}

	DMaterialInterface::DMaterialInterface(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, MaterialRenderProxy(IsTemplateConstructionPurpose(ObjectInitializer.Purpose)
			? FMaterialRenderProxyRef{}
			: MakeRefCount<FMaterialRenderProxy>())
	{
	}

	auto DMaterialInterface::RequestProgramCompile(
		const FMaterialProgram& CandidateProgram,
		const FMaterialStaticProperties& CandidateProperties,
		bool bForceRecompile) -> bool
	{
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return false;
		CompilationOwner.LastRequestedShaderProperties = CanonicalizeMaterialShaderProperties(CandidateProperties);
		FModuleManager::Get().LoadModule("RenderCore");
		FMaterialCompilerEnvironment Environment;
		std::string EnvironmentError;
		if (!BuildDefaultMaterialCompilerEnvironment(
			Environment, EnvironmentError))
		{
			CompilationOwner.MaterialCompileStatus.RequestGeneration =
				CompilationOwner.MaterialCompileStatus.RequestGeneration
					== std::numeric_limits<uint64>::max()
					? 1 : CompilationOwner.MaterialCompileStatus.RequestGeneration + 1;
			CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Failed;
			CompilationOwner.MaterialCompileStatus.ResultCategory =
				EMaterialCompileResultCategory::Dependency;
			CompilationOwner.MaterialCompileStatus.bHasLastKnownGood =
				CompilationOwner.AcceptedGeneration.Program != nullptr;
			CompilationOwner.MaterialCompileStatus.bLastKnownGoodDisplayed =
				CompilationOwner.AcceptedGeneration.Program != nullptr;
			CompilationOwner.MaterialCompileDiagnostics = {{
				.Category = EMaterialCompileResultCategory::Dependency,
				.Source = {
					.Category = EMaterialProgramDiagnosticCategory::Dependency,
					.Message = std::move(EnvironmentError)},
				.AssetPath = GetObjectPath(),
				.Generation = CompilationOwner.MaterialCompileStatus.RequestGeneration,
				.bLastKnownGoodDisplayed = CompilationOwner.AcceptedGeneration.Program != nullptr,
			}};
			return false;
		}
		FMaterialCompilerInput Input;
		Input.Program = CandidateProgram;
		Input.StaticProperties = CandidateProperties;
		Input.Environment = std::move(Environment);
		Input.Parameters.reserve(GetParameterDefinitions().size());
		for (const FMaterialParameterDefinition& Definition : GetParameterDefinitions())
			Input.Parameters.push_back({Definition.Id, Definition.Type});
		std::ranges::sort(Input.Parameters, {},
			&FMaterialCompilerParameterDeclaration::Id);
		CompilationOwner.LastRequestedParameters = Input.Parameters;
		return Private::FMaterialCompilationLifecycle::Submit(
			*this, std::move(Input), bForceRecompile);
	}

	auto DMaterialInterface::InvalidateMaterialCompilation(bool bIncludeSelf, bool bOnlyIfShaderChanged) -> void
	{
		CheckMaterialQueryThread();
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
		for (const FObjectHandle Handle : GetLoadedMaterialDependents(this))
		{
			auto* Owner = Cast<DMaterialInterface>(ResolveObjectHandle(Handle));
			if (!IsValid(Owner) || (!bIncludeSelf && Owner == this)) continue;
			if (bOnlyIfShaderChanged
				&& CanonicalizeMaterialShaderProperties(Owner->GetStaticProperties())
					== Owner->CompilationOwner.LastRequestedShaderProperties) continue;
			auto& Status = Owner->CompilationOwner.MaterialCompileStatus;
			Status.AuthoredRevision = Status.AuthoredRevision == std::numeric_limits<uint64>::max()
				? 1 : Status.AuthoredRevision + 1;
			Status.ParentChainRevision = Status.ParentChainRevision == std::numeric_limits<uint64>::max()
				? 1 : Status.ParentChainRevision + 1;
			RequestMaterialRecompile(*Owner);
		}
	}

	auto DMaterialInterface::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		return {};
	}

	auto DMaterialInterface::FindParameterDefinition(const FGuid& Id) const -> const FMaterialParameterDefinition*
	{
		const std::span Definitions = GetParameterDefinitions();
		const auto It = std::ranges::find(Definitions, Id, &FMaterialParameterDefinition::Id);
		return It == Definitions.end() ? nullptr : &*It;
	}

	auto DMaterialInterface::FindParameterDefinition(FName Name) const -> const FMaterialParameterDefinition*
	{
		const std::span Definitions = GetParameterDefinitions();
		const auto It = std::ranges::find(Definitions, Name, &FMaterialParameterDefinition::Name);
		return It == Definitions.end() ? nullptr : &*It;
	}

	auto DMaterialInterface::ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetScalarParameterValue(FName Name, float& OutValue) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetParent() const -> DMaterialInterface*
	{
		return nullptr;
	}

	auto DMaterialInterface::GetStaticProperties() const -> const FMaterialStaticProperties&
	{
		return GDefaultMaterialStaticProperties;
	}

	auto DMaterialInterface::GetRenderableStaticProperties() const
		-> FMaterialStaticProperties
	{
		const auto& Accepted = CompilationOwner.AcceptedGeneration;
		FMaterialStaticProperties Result = Accepted.Program ? Accepted.Properties : GetStaticProperties();
		const auto& Authored = GetStaticProperties();
		if (!Accepted.Program || CanonicalizeMaterialShaderProperties(Authored) == Accepted.ShaderProperties)
		{
			Result.bTwoSided = Authored.bTwoSided;
			Result.DepthWritePolicy = Authored.DepthWritePolicy;
		}
		Result.OpacityMaskThreshold = CanonicalizeMaterialShaderProperties(Result).OpacityMaskThreshold;
		return Result;
	}

	auto DMaterialInterface::GetMaterialProgram() const
		-> const FMaterialProgram*
	{
		return nullptr;
	}

	auto DMaterialInterface::GetAcceptedCompiledProgram() const
		-> std::shared_ptr<const FMaterialCompilerResult>
	{
		if (!CompilationOwner.AcceptedGeneration.Program
			&& GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedProgramData.GetMetadata().LogicalSize != 0)
		{
			std::string Error;
			const_cast<DMaterialInterface*>(this)->LoadCookedProgram(Error);
		}
		return CompilationOwner.AcceptedGeneration.Program;
	}

	auto DMaterialInterface::AdoptParentRuntimeProgram() -> bool
	{
		CompilationOwner.AcceptedGeneration = {};
		auto* Parent = GetParent();
		const auto Program = Parent ? Parent->GetAcceptedCompiledProgram() : nullptr;
		if (!Program || CanonicalizeMaterialShaderProperties(GetStaticProperties())
			!= CanonicalizeMaterialShaderProperties(Parent->GetRenderableStaticProperties())) return false;
		CompilationOwner.AcceptedGeneration.Program = Program;
		CompilationOwner.AcceptedGeneration.Properties = GetStaticProperties();
		CompilationOwner.AcceptedGeneration.ShaderProperties = CanonicalizeMaterialShaderProperties(GetStaticProperties());
		CompilationOwner.AcceptedGeneration.Parameters = BuildMaterialLocalRenderLayer().Parameters;
		auto& Status = CompilationOwner.MaterialCompileStatus;
		Status.State = EMaterialCompileState::Ready;
		Status.CompiledAuthoredRevision = Status.AuthoredRevision;
		Status.CompiledIdentity = Program->Identity;
		Status.RequestedIdentity = Program->Identity;
		Status.Target = Program->Target;
		Status.bHasLastKnownGood = true;
		return true;
	}

	auto DMaterialInterface::IsDependent(const DMaterialInterface* TestDependency) const -> bool
	{
		CheckMaterialQueryThread();
		if (!TestDependency) return false;

		std::unordered_set<const DMaterialInterface*> Visited;
		for (const DMaterialInterface* Material = this; Material != nullptr; Material = Material->GetParent())
		{
			if (!Visited.insert(Material).second) return false;
			if (Material == TestDependency) return true;
		}
		return false;
	}

	auto DMaterialInterface::GetRenderData() const -> FMaterialRenderData
	{
		FMaterialRenderData Result;
		Result.CompiledProgram = GetAcceptedCompiledProgram();
		Result.Representation = FMaterialRenderRepresentation{};
		FMaterialRenderRepresentationBuilder RepresentationBuilder = Result.CompiledProgram
			&& Result.CompiledProgram->Layout.Identity.Version == CompiledMaterialRenderLayoutVersion
			&& Result.CompiledProgram->Layout.Identity != Result.Representation.GetLayout().Identity
			? FMaterialRenderRepresentationBuilder(Result.CompiledProgram->Layout)
			: FMaterialRenderRepresentationBuilder(Result.Representation);
		bool bRepresentationValid = true;
		if (Result.CompiledProgram)
			Result.PlanningPassIdentity.ShaderMap.RenderLayout = Result.CompiledProgram->Layout.Identity;
		const auto LocalLayer = BuildMaterialLocalRenderLayer();
		for (const auto& Parameter : LocalLayer.Parameters)
		{
			if (!Result.CompiledProgram) continue;
			const auto& Parameters = Result.CompiledProgram->ActiveParameters;
			const auto Active = std::ranges::find(Parameters, Parameter.Id,
				&FMaterialCompilerParameterDeclaration::Id);
			if (Active == Parameters.end() || Active->Type != Parameter.Type) continue;
			bRepresentationValid = ApplyMaterialLocalRenderParameter(
				RepresentationBuilder, Parameter)
				&& bRepresentationValid;
		}
		const FMaterialStaticProperties StaticProperties =
			GetRenderableStaticProperties();
		Result.PlanningPassIdentity.ShaderMap.BlendMode = StaticProperties.BlendMode;
		Result.PlanningPassIdentity.ShaderMap.ShadingModel = StaticProperties.ShadingModel;
		Result.PlanningPassIdentity.ShaderMap.OpacityMaskThreshold = StaticProperties.OpacityMaskThreshold;
		Result.PlanningPassIdentity.bTwoSided = StaticProperties.bTwoSided;
		Result.PlanningPassIdentity.DepthWritePolicy = StaticProperties.DepthWritePolicy;
		if (Result.CompiledProgram)
			Result.PlanningPassIdentity.ShaderMap.ProgramIdentity =
				Result.CompiledProgram->Identity;

		FMaterialRenderRepresentation CompiledRepresentation;
		FMaterialRenderValidationDiagnostic ValidationDiagnostic;
		const bool bRequiresCompiledProgram = IsA(DMaterial::StaticClass()) || IsA(DMaterialInstance::StaticClass());
		if ((!bRequiresCompiledProgram || Result.CompiledProgram)
			&& bRepresentationValid
			&& RepresentationBuilder.Build(
				CompiledRepresentation, ValidationDiagnostic))
		{
			Result.Representation = std::move(CompiledRepresentation);
		}
		else
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::MaterialDataInvalid);
			Result = GetErrorMaterialRenderData();
		}
		return Result;
	}

	auto DMaterialInterface::GetMaterialRenderProxy() const
		-> FMaterialRenderProxyRef
	{
		CheckMaterialQueryThread();
		if (IsTemplateObject() || !bAcceptingMaterialProxyPublications) return {};
		if (MaterialProxyLocalVersion == 0)
		{
			MaterialProxyLocalVersion = 1;
		}
		if (LastSubmittedMaterialProxyLocalVersion
			< MaterialProxyLocalVersion)
		{
			SubmitMaterialRenderProxyState();
		}
		return MaterialRenderProxy;
	}

	auto DMaterialInterface::BeginDestroy() -> void
	{
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
		// A live child must retire a broken chain even if no compile is pending.
		for (DObject* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
		{
			auto* Owner = Cast<DMaterialInstance>(Object);
			if (!IsValid(Owner)) continue;
			FResolvedMaterialProperties Resolved;
			std::string Error;
			if (ResolveMaterialProperties(*Owner, Resolved, Error)) continue;
			FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Owner);
			Owner->CompilationOwner.AcceptedGeneration = {};
			auto& Status = Owner->CompilationOwner.MaterialCompileStatus;
			Status.State = EMaterialCompileState::Failed;
			Status.ResultCategory = EMaterialCompileResultCategory::Dependency;
			Status.bHasLastKnownGood = false;
			Status.bLastKnownGoodDisplayed = false;
			Owner->CompilationOwner.MaterialCompileDiagnostics = {{
				.Category = EMaterialCompileResultCategory::Dependency,
				.Source = {.Category = EMaterialProgramDiagnosticCategory::Dependency, .Message = Error},
				.AssetPath = Owner->GetObjectPath(), .Generation = Status.RequestGeneration}};
			Owner->PublishMaterialRenderProxyState();
		}
		bAcceptingMaterialProxyPublications = false;
		ReleaseMaterialRenderProxy_GameThread(
			std::move(MaterialRenderProxy));
		Super::BeginDestroy();
	}

	auto DMaterialInterface::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("ParameterDefinitions") || Name == FName("ParameterOverrides"))
		{
			// Reflected editor transactions restore collection storage directly. Route every
			// phase through the same render invalidation normally supplied by setters.
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		}
		else if (Name == FName("StaticProperties"))
		{
			MarkRenderDataDirty(
				EMaterialRenderDirtyFlags::ShaderMap
				| EMaterialRenderDirtyFlags::PipelineState);
		}
	}

	auto DMaterialInterface::BuildMaterialLocalRenderLayer() const
		-> FMaterialLocalRenderLayer
	{
		FMaterialLocalRenderLayer Result;
		Result.CompiledProgram = GetAcceptedCompiledProgram();
		Result.StaticProperties = GetRenderableStaticProperties();
		if (!Result.CompiledProgram) return Result;
		for (const auto& Parameter : Result.CompiledProgram->ActiveParameters)
		{
			FResolvedMaterialParameter Resolved;
			if (ResolveParameterValue(Parameter.Id, Resolved)
				&& Resolved.Definition && Resolved.Definition->Type == Parameter.Type)
			{
				Result.Parameters.push_back(BuildMaterialLocalRenderParameter(
					Parameter.Id, Parameter.Type, Resolved.Value));
				continue;
			}
			const auto& Retained = CompilationOwner.AcceptedGeneration.Parameters;
			const auto Previous = std::ranges::find(Retained, Parameter.Id,
				&FMaterialLocalRenderParameter::Id);
			if (Previous != Retained.end() && Previous->Type == Parameter.Type)
				Result.Parameters.push_back(*Previous);
		}
		return Result;
	}

	auto DMaterialInterface::PublishMaterialRenderProxyState() -> void
	{
		CheckMaterialQueryThread();
		if (!bAcceptingMaterialProxyPublications) return;
		++MaterialProxyLocalVersion;
		if (MaterialProxyLocalVersion == 0) ++MaterialProxyLocalVersion;
		SubmitMaterialRenderProxyState();
	}

	auto DMaterialInterface::RefreshReloadedAssetBindings() -> void
	{
		PublishMaterialRenderProxyState();
	}

	auto DMaterialInterface::SubmitMaterialRenderProxyState() const -> void
	{
		if (!bAcceptingMaterialProxyPublications
			|| !MaterialRenderProxy
			|| MaterialProxyLocalVersion == 0)
		{
			return;
		}

		FMaterialRenderProxyPublication Publication{
			.LocalLayer = BuildMaterialLocalRenderLayer(),
			.LocalVersion = MaterialProxyLocalVersion,
		};
		std::ranges::sort(
			Publication.LocalLayer.Parameters,
			{},
			&FMaterialLocalRenderParameter::Id);

		const uint64 SubmittedVersion = Publication.LocalVersion;
		const bool bAccepted = MaterialRenderProxy->QueuePublication_GameThread(
			std::move(Publication));
		if (bAccepted)
		{
			LastSubmittedMaterialProxyLocalVersion = SubmittedVersion;
		}
	}

	auto DMaterialInterface::MarkRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags) -> void
	{
		if (DirtyFlags == EMaterialRenderDirtyFlags::None) return;
		auto Publish = [](DMaterialInterface& Owner) {
			++Owner.RenderStateVersion;
			if (Owner.RenderStateVersion == 0) ++Owner.RenderStateVersion;
			if (Owner.CompilationOwner.AcceptedGeneration.Program)
			{
				Owner.CompilationOwner.AcceptedGeneration.Parameters =
					Owner.BuildMaterialLocalRenderLayer().Parameters;
				Owner.CompilationOwner.AcceptedGeneration.Properties = Owner.GetRenderableStaticProperties();
			}
			Owner.PublishMaterialRenderProxyState();
		};
		Publish(*this);
		for (const auto Handle : GetLoadedMaterialDependents(this))
		{
			auto* Owner = Cast<DMaterialInterface>(ResolveObjectHandle(Handle));
			if (IsValid(Owner) && Owner != this) Publish(*Owner);
		}
	}

	auto GetLoadedDirectMaterialChildren(
		const DMaterialInterface* Parent
	) -> std::vector<FObjectHandle>
	{
		if (!IsValid(Parent)) return {};
		return QueryLoadedMaterialHandles(
			EMaterialLoadedQueryOperation::DirectChildren,
			[Parent](DMaterialInterface* Material) {
				auto* Instance = Cast<DMaterialInstance>(Material);
				return IsValid(Instance) && Instance->GetParent() == Parent;
			});
	}

	auto GetLoadedMaterialDependents(
		const DMaterialInterface* Dependency
	) -> std::vector<FObjectHandle>
	{
		if (!IsValid(Dependency)) return {};
		return QueryLoadedMaterialHandles(
			EMaterialLoadedQueryOperation::Dependents,
			[Dependency](DMaterialInterface* Material) {
				return Material->IsDependent(Dependency);
			});
	}
}
