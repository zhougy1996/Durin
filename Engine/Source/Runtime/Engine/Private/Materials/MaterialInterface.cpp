#include "Materials/MaterialInterface.h"
#include "MaterialCompileRetryQueue.h"
#include "Materials/ObjectCacheContext.h"
#include "MaterialLoadedQueryDiagnostics.h"

#include "Asset/Asset.h"
#include "Asset/AssetCompilingManager.h"
#include "Modules/ModuleManager.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Materials/MaterialInstance.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Logging/LogMacros.h"
#include "Texture/Texture2D.h"
#include "Threading/RunnableThread.h"
#include "RenderingThread.h"
#include <unordered_set>
#include <unordered_map>

namespace Durin
{
	auto DMaterialInterface::SetImportProvenance(FMaterialImportProvenance InProvenance) -> bool
	{
		if (IsDynamicInstance()) return false;
		if (InProvenance.RecipeId.size() > 128 || InProvenance.StructuralKey.size() > 16384 ||
			InProvenance.SourceIdentity.size() > 4096 || InProvenance.OutputIdentity.size() > 4096) return false;
		if (ImportProvenance == InProvenance) return true;
		ImportProvenance = std::move(InProvenance);
		MarkPackageDirty();
		return true;
	}

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
		) -> std::vector<FObjectKey>
		{
			CheckMaterialQueryThread();
			std::vector<FObjectKey> Result;
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
				const FObjectKey Handle = FObjectKey(Material);
				if (!IsObjectKeyNull(Handle)) Result.push_back(Handle);
			}
			std::ranges::sort(Result, [](FObjectKey Left, FObjectKey Right) {
				return Left < Right;
			});
			GMaterialLoadedQueryDiagnostics.LastResultCount = Result.size();
			return Result;
		}
	}

	auto Private::GetMutableMaterialLoadedQueryDiagnostics() -> FMaterialLoadedQueryDiagnostics&
	{
		return GMaterialLoadedQueryDiagnostics;
	}

	auto GetMaterialFunctionChangedEvent() -> FMaterialFunctionChangedEvent&
	{
		static FMaterialFunctionChangedEvent Event;
		return Event;
	}

	auto NotifyMaterialFunctionChanged(const DMaterialFunctionInterface& Function) -> void
	{
		CheckMaterialQueryThread();
		GetMaterialFunctionChangedEvent().Broadcast(Function);
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
		FObjectCacheContext Context;
		const auto Owners = QueryLoadedMaterialHandles(EMaterialLoadedQueryOperation::Dependents,
			[&](const DMaterialInterface* Material) {
				std::unordered_set<const DMaterialFunctionInterface*> Visited;
				const auto Depends = [&](auto&& Self, const DMaterialFunctionInterface* Candidate) -> bool {
					if (Candidate == &Function) return true;
					if (!IsValid(Candidate) || !Visited.emplace(Candidate).second) return false;
					// Malformed excessive closures must still fail/recover when edited.
					if (Visited.size() > MaterialFunctionMaxDependencies) return true;
					for (const auto& Dependency : Candidate->GetFunctionDependencies())
						if (Self(Self, Dependency.Get())) return true;
					return false;
				};
				FResolvedMaterialProperties Resolved;
				const auto Error = ResolveMaterialProperties(*Material, Resolved);
				if (!Error) return false;
				const auto* Root = Cast<DMaterial>(ResolveObjectKey(Resolved.Root));
				if (!Root) return false;
				for (const auto& Expression : Root->GetExpressionCollection().Expressions)
					if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get());
						Call && Depends(Depends, Call->Function.Get())) return true;
				return false;
			});
		for (const auto Handle : Owners)
			if (auto* Material = Cast<DMaterialInterface>(ResolveObjectKey(Handle)); IsValid(Material))
			{
				auto& Revision = Material->CompilationOwner.MaterialCompileStatus.AuthoredRevision;
				Revision = Revision == std::numeric_limits<uint64>::max() ? 1 : Revision + 1;
				Private::FMaterialCompilationLifecycle::ScheduleEdit(*Material, &Context);
			}
		Context.EndDiscovery();
		for (const auto Key : Owners)
			if (auto* Material = Cast<DMaterialInterface>(Key.ResolveObjectPtr())) Material->ParameterChanges.Broadcast();
	}

	auto ResolveMaterialProperties(const DMaterialInterface& Material,
		FResolvedMaterialProperties& OutProperties) -> FMaterialOperationResult
	{
		CheckMaterialQueryThread();
		std::array<const DMaterialInterface*, MaterialMaximumParentDepth> Chain{};
		size_t Count = 0;
		const DMaterial* Root = nullptr;
		for (auto* Current = &Material; Current; Current = Current->GetParent())
		{
			if (Count == Chain.size()
				|| std::ranges::contains(Chain.begin(), Chain.begin() + Count, Current))
			{
				return {EMaterialPropertyError::ParentCycleOrDepthExceeded};
			}
			if (!IsValid(Current)) break;
			Chain[Count++] = Current;
			if ((Root = Cast<DMaterial>(Current))) break;
		}
		if (!Root)
		{
			return {EMaterialPropertyError::ParentChainNoValidRootMaterial};
		}
		FResolvedMaterialProperties Result;
		Result.Root = FObjectKey(const_cast<DMaterial*>(Root));
		Result.Properties = Root->GetStaticProperties();
		if (const auto Validation = ValidateMaterialStaticProperties(Result.Properties); !Validation) return Validation;
		Result.Sources.fill(Result.Root);
		while (Count > 1)
		{
			const auto* Instance = Cast<DMaterialInstance>(Chain[--Count - 1]);
			if (!Instance)
			{
				return {EMaterialPropertyError::ParentChainContainsUnsupportedMaterialOwner};
			}
			const auto& Overrides = Instance->GetPropertyOverrides();
			if (const auto Validation = ValidateMaterialStaticProperties(Overrides.Values); !Validation) return Validation;
			Overrides.ApplyTo(Result.Properties);
			const std::array Enabled{Overrides.bOverrideBlendMode, Overrides.bOverrideShadingModel,
				Overrides.bOverrideOpacityMaskThreshold, Overrides.bOverrideTwoSided,
				Overrides.bOverrideDepthWritePolicy};
			for (size_t Index = 0; Index < Enabled.size(); ++Index)
				if (Enabled[Index]) Result.Sources[Index] = FObjectKey(const_cast<DMaterialInstance*>(Instance));
		}
		Result.ShaderProperties = CanonicalizeMaterialShaderProperties(Result.Properties);
		OutProperties = Result;
		return {};
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
		const FMaterialStaticProperties& CandidateProperties,
		bool bForceRecompile, FObjectCacheContext* Context) -> bool
	{
		if (IsDynamicInstance()) return false;
		Private::GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(this));
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return false;
		CompilationOwner.LastObservedShaderProperties = CanonicalizeMaterialShaderProperties(CandidateProperties);
		FModuleManager::Get().LoadModule("RenderCore");
		FMaterialCompilerEnvironment Environment;
		const auto EnvironmentError = BuildDefaultMaterialCompilerEnvironment(
			Environment);
		if (!EnvironmentError)
		{
			CompilationOwner.MaterialCompileStatus.RequestGeneration =
				CompilationOwner.MaterialCompileStatus.RequestGeneration
					== std::numeric_limits<uint64>::max()
					? 1 : CompilationOwner.MaterialCompileStatus.RequestGeneration + 1;
			CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Failed;
			CompilationOwner.MaterialCompileStatus.ResultCategory =
				EMaterialCompileResultCategory::Dependency;
			CompilationOwner.MaterialCompileDiagnostics = {{
				.Category = EMaterialCompileResultCategory::Dependency,
				.Source = {
					.Category = EMaterialProgramDiagnosticCategory::Dependency,
					.Error = std::move(EnvironmentError.Error)},
				.AssetPath = GetObjectPath(),
				.Generation = CompilationOwner.MaterialCompileStatus.RequestGeneration,
			}};
			RetireFailedMaterialGeneration(Context);
			return false;
		}
		auto Snapshot = SnapshotMaterialCompilerInput(*this, std::move(Environment));
		if (!Snapshot)
		{
			auto& Status = CompilationOwner.MaterialCompileStatus;
			Status.RequestGeneration = Status.RequestGeneration == std::numeric_limits<uint64>::max()
				? 1 : Status.RequestGeneration + 1;
			Status.State = EMaterialCompileState::Failed;
			Status.ResultCategory = !Snapshot.Diagnostics.empty()
				&& Snapshot.Diagnostics.front().Category == EMaterialProgramDiagnosticCategory::Dependency
				? EMaterialCompileResultCategory::Dependency : EMaterialCompileResultCategory::Validation;
			CompilationOwner.MaterialCompileDiagnostics.clear();
			for (const auto& Diagnostic : Snapshot.Diagnostics)
				CompilationOwner.MaterialCompileDiagnostics.push_back({
					.Category = Diagnostic.Category == EMaterialProgramDiagnosticCategory::Dependency
						? EMaterialCompileResultCategory::Dependency : EMaterialCompileResultCategory::Validation,
					.Source = Diagnostic,
					.AssetPath = GetObjectPath(), .Generation = Status.RequestGeneration});
			RetireFailedMaterialGeneration(Context);
			return false;
		}
		auto& Input = Snapshot.Snapshot->Input;
		Input.StaticProperties = CandidateProperties;
		CompilationOwner.LastObservedParameters = Input.Parameters;
		return Private::FMaterialCompilationLifecycle::Submit(
			*this, std::move(Input), bForceRecompile, std::move(Snapshot.Snapshot->FunctionOwners), Context);
	}

	auto DMaterialInterface::InvalidateMaterialCompilation(bool bIncludeSelf, bool bOnlyIfShaderChanged, FObjectCacheContext* Context) -> void
	{
		CheckMaterialQueryThread();
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
		std::optional<FObjectCacheContext> LocalContext;
		if (!Context) { LocalContext.emplace(); Context = &*LocalContext; }
		for (auto* Owner : Context->GetMaterialsAffectedByMaterial(this))
		{
			if (!IsValid(Owner) || Owner->IsDynamicInstance() || (!bIncludeSelf && Owner == this)) continue;
			if (bOnlyIfShaderChanged
				&& CanonicalizeMaterialShaderProperties(Owner->GetStaticProperties())
					== Owner->CompilationOwner.LastObservedShaderProperties) continue;
			auto& Status = Owner->CompilationOwner.MaterialCompileStatus;
			Status.AuthoredRevision = Status.AuthoredRevision == std::numeric_limits<uint64>::max()
				? 1 : Status.AuthoredRevision + 1;
			Status.ParentChainRevision = Status.ParentChainRevision == std::numeric_limits<uint64>::max()
				? 1 : Status.ParentChainRevision + 1;
			Private::FMaterialCompilationLifecycle::ScheduleEdit(*Owner, Context);
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
		const auto& Layer = CompilationOwner.RenderLayer;
		const auto& Authored = GetStaticProperties();
		FMaterialStaticProperties Result = Layer.CompiledProgram
			? Layer.StaticProperties.value_or(Authored) : Authored;
		if (!Layer.CompiledProgram || CanonicalizeMaterialShaderProperties(Authored)
			== CanonicalizeMaterialShaderProperties(Result))
		{
			Result.bTwoSided = Authored.bTwoSided;
			Result.DepthWritePolicy = Authored.DepthWritePolicy;
		}
		Result.OpacityMaskThreshold = CanonicalizeMaterialShaderProperties(Result).OpacityMaskThreshold;
		return Result;
	}



	auto DMaterialInterface::GetAcceptedCompiledProgram() const
		-> std::shared_ptr<const FMaterialCompilerResult>
	{
		if (!CompilationOwner.RenderLayer.CompiledProgram
			&& GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedProgramData.GetMetadata().LogicalSize != 0)
		{
			const_cast<DMaterialInterface*>(this)->LoadCookedProgram();
		}
		return CompilationOwner.RenderLayer.CompiledProgram;
	}

	auto DMaterialInterface::AdoptParentRuntimeProgram() -> bool
	{
		CompilationOwner.RenderLayer = {};
		CompilationOwner.AcceptedExpressionSources.clear();
		auto* Parent = GetParent();
		const auto Program = Parent ? Parent->GetAcceptedCompiledProgram() : nullptr;
		if (!Program || CanonicalizeMaterialShaderProperties(GetStaticProperties())
			!= CanonicalizeMaterialShaderProperties(Parent->GetRenderableStaticProperties())) return false;
		CompilationOwner.RenderLayer.CompiledProgram = Program;
		CompilationOwner.RenderLayer.StaticProperties = GetStaticProperties();
		CompilationOwner.RenderLayer.Parameters = BuildMaterialLocalRenderLayer().Parameters;
		auto& Status = CompilationOwner.MaterialCompileStatus;
		Private::GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(this));
		Status.State = EMaterialCompileState::Ready;
		Status.CompiledIdentity = Program->Identity;
		Status.RequestedIdentity = Program->Identity;
		Status.Target = Program->Target;
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
			if (Active == Parameters.end() || Active->Type != Parameter.GetType()) continue;
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
			SubmitMaterialRenderProxyState(BuildMaterialLocalRenderLayer());
		}
		return MaterialRenderProxy;
	}

	auto DMaterialInterface::BeginDestroy() -> void
	{
		Private::GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(this));
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
		FObjectCacheContext Context;
		std::vector<FWeakObjectPtr> Notifications;
		// A live child must retire a broken chain even if no compile is pending.
		for (DObject* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
		{
			auto* Owner = Cast<DMaterialInstance>(Object);
			if (!IsValid(Owner)) continue;
			FResolvedMaterialProperties Resolved;
			const auto Error = ResolveMaterialProperties(*Owner, Resolved);
			if (Error) continue;
			FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Owner);
			auto& Status = Owner->CompilationOwner.MaterialCompileStatus;
			Status.State = EMaterialCompileState::Failed;
			Status.ResultCategory = EMaterialCompileResultCategory::Dependency;
			Owner->CompilationOwner.MaterialCompileDiagnostics = {{
				.Category = EMaterialCompileResultCategory::Dependency,
				.Source = {.Category = EMaterialProgramDiagnosticCategory::Dependency, .Error = Error.Error},
				.AssetPath = Owner->GetObjectPath(), .Generation = Status.RequestGeneration}};
			Owner->RetireFailedMaterialGeneration(&Context);
			Notifications.emplace_back(Owner);
		}
		Context.EndDiscovery();
		for (const auto& Weak : Notifications)
			if (auto* Owner = Cast<DMaterialInterface>(Weak.Get())) Owner->ParameterChanges.Broadcast();
		bAcceptingMaterialProxyPublications = false;
		ReleaseMaterialRenderProxy_GameThread(
			std::move(MaterialRenderProxy));
		Super::BeginDestroy();
	}

	auto DMaterialInterface::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		FObjectCacheContext Context;
		PostEditChangePropertyWithContext(Event, Context);
	}

	auto DMaterialInterface::PostEditChangePropertyWithContext(const FPropertyChangedEvent& Event, FObjectCacheContext& Context) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("StaticProperties"))
		{
			MarkRenderDataDirty(
				EMaterialRenderDirtyFlags::ShaderMap
				| EMaterialRenderDirtyFlags::PipelineState, false, &Context);
		}
	}

	auto DMaterialInterface::BuildMaterialLocalRenderLayer() const
		-> FMaterialLocalRenderLayer
	{
		if (IsDynamicInstance())
		{
			auto* Parent = GetParent();
			if (!IsValid(Parent)) return {};
			// Start from the parent's complete accepted generation, including retained
			// values while authored declarations or shader configuration are pending.
			auto Layer = Parent->BuildMaterialLocalRenderLayer();
			const auto* Instance = Cast<DMaterialInstance>(this);
			for (auto& Parameter : Layer.Parameters)
			{
				FMaterialParameterValue Value;
				if (Instance->GetLocalParameterValue(Parameter.Id, Value) && Value.GetType() == Parameter.GetType())
					Parameter = BuildMaterialLocalRenderParameter(Parameter.Id, Value);
			}
			return Layer;
		}
		FMaterialLocalRenderLayer Result;
		Result.CompiledProgram = GetAcceptedCompiledProgram();
		Result.StaticProperties = GetRenderableStaticProperties();
		if (!Result.CompiledProgram) return Result;
		// This context lives only for this build. Validate the chain and index the
		// declarations once, then visit each owner's typed storage once. Calling
		// ResolveParameterValue per active parameter repeats all of that work.
		std::unordered_map<FGuid, FResolvedMaterialParameter> Parameters;
		FResolvedMaterialProperties Properties;
		const auto Error = ResolveMaterialProperties(*this, Properties);
		if (Error)
		{
			auto* Root = Cast<DMaterial>(ResolveObjectKey(Properties.Root));
			const auto Definitions = Root->GetParameterDefinitions();
			Parameters.reserve(Definitions.size());
			for (const auto& Definition : Definitions)
			{
				FResolvedMaterialParameter Resolved;
				Resolved.Definition = &Definition;
				Resolved.Value = Definition.Value;
				Parameters.emplace(Definition.Id, std::move(Resolved));
			}
			// Nearest matching override wins; an orphan or a stale type must not
			// hide a matching ancestor value or the root default.
			for (auto* Owner = this; Owner != Root; Owner = Owner->GetParent())
			{
				const auto* Instance = Cast<DMaterialInstance>(Owner);
				Instance->VisitLocalParameterValues([&](const FGuid& Id, const FMaterialParameterValue& Value) {
					const auto It = Parameters.find(Id);
					if (It == Parameters.end() || It->second.Source
						|| Value.GetType() != It->second.Definition->Type) return;
					It->second.Value = Value;
					It->second.Source = const_cast<DMaterialInstance*>(Instance);
				});
			}
		}
		std::unordered_map<FGuid, const FMaterialLocalRenderParameter*> Retained;
		Retained.reserve(CompilationOwner.RenderLayer.Parameters.size());
		for (const auto& Parameter : CompilationOwner.RenderLayer.Parameters)
			Retained.emplace(Parameter.Id, &Parameter);
		Result.Parameters.reserve(Result.CompiledProgram->ActiveParameters.size());
		for (const auto& Parameter : Result.CompiledProgram->ActiveParameters)
		{
			const auto Resolved = Parameters.find(Parameter.Id);
			if (Resolved != Parameters.end() && Resolved->second.Definition->Type == Parameter.Type)
			{
				Result.Parameters.push_back(BuildMaterialLocalRenderParameter(
					Parameter.Id, Resolved->second.Value));
				continue;
			}
			const auto Previous = Retained.find(Parameter.Id);
			if (Previous != Retained.end() && Previous->second->GetType() == Parameter.Type)
				Result.Parameters.push_back(*Previous->second);
		}
		return Result;
	}

	auto DMaterialInterface::RetireFailedMaterialGeneration(FObjectCacheContext* Context) -> void
	{
		CompilationOwner.RenderLayer = {};
		CompilationOwner.AcceptedExpressionSources.clear();
		CompilationOwner.MaterialCompileStatus.CompiledIdentity = {};
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap
			| EMaterialRenderDirtyFlags::PipelineState, false, Context);
	}

	auto DMaterialInterface::PublishMaterialRenderProxyState() -> void
	{
		CheckMaterialQueryThread();
		if (!bAcceptingMaterialProxyPublications) return;
		PublishMaterialRenderProxyState(MaterialRenderProxy
			? BuildMaterialLocalRenderLayer() : FMaterialLocalRenderLayer{});
	}

	auto DMaterialInterface::PublishMaterialRenderProxyState(
		FMaterialLocalRenderLayer LocalLayer) -> void
	{
		CheckMaterialQueryThread();
		if (!bAcceptingMaterialProxyPublications) return;
		++MaterialProxyLocalVersion;
		if (MaterialProxyLocalVersion == 0) ++MaterialProxyLocalVersion;
		SubmitMaterialRenderProxyState(std::move(LocalLayer));
	}

	auto DMaterialInterface::RefreshReloadedAssetBindings() -> void
	{
		PublishMaterialRenderProxyState();
		ParameterChanges.Broadcast();
	}

	auto DMaterialInterface::SubmitMaterialRenderProxyState(
		FMaterialLocalRenderLayer LocalLayer) const -> void
	{
		if (!bAcceptingMaterialProxyPublications
			|| !MaterialRenderProxy
			|| MaterialProxyLocalVersion == 0)
		{
			return;
		}
		// A pre-start edit is rebuilt on the next proxy request after render admission opens.
		const auto Admission = GetRenderCommandAdmissionState();
		if (Admission == ERenderCommandAdmissionState::Stopped) return;
		checkf(Admission == ERenderCommandAdmissionState::Running,
			"Material publication must finish before render-command shutdown.");

		FMaterialRenderProxyPublication Publication{
			.LocalLayer = std::move(LocalLayer),
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

	auto DMaterialInterface::NotifyParameterChanges() -> void
	{
		CheckMaterialQueryThread();
		FObjectCacheContext Context;
		const auto Dependents = Context.GetMaterialsAffectedByMaterial(this);
		Context.EndDiscovery();
		BroadcastParameterChanges(Dependents);
	}

	auto DMaterialInterface::BroadcastParameterChanges(const TObjectCacheIterator<DMaterialInterface>& Dependents) -> void
	{
		ParameterChanges.Broadcast();
		for (auto* Owner : Dependents)
			if (IsValid(Owner) && Owner != this)
				Owner->ParameterChanges.Broadcast();
	}

	auto DMaterialInterface::MarkRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags,
		bool bNotifyParameterChanges, FObjectCacheContext* Context) -> void
	{
		if (DirtyFlags == EMaterialRenderDirtyFlags::None) return;
		auto Publish = [](DMaterialInterface& Owner) {
			++Owner.RenderStateVersion;
			if (Owner.RenderStateVersion == 0) ++Owner.RenderStateVersion;
			if (Owner.CompilationOwner.RenderLayer.CompiledProgram)
			{
				// Retention and publication must describe the same resolved snapshot.
				// Update retention even when this owner cannot publish a proxy.
				auto LocalLayer = Owner.BuildMaterialLocalRenderLayer();
				Owner.CompilationOwner.RenderLayer.Parameters = LocalLayer.Parameters;
				Owner.CompilationOwner.RenderLayer.StaticProperties = LocalLayer.StaticProperties;
				Owner.PublishMaterialRenderProxyState(std::move(LocalLayer));
			}
			else
			{
				Owner.PublishMaterialRenderProxyState();
			}
		};
		std::optional<FObjectCacheContext> LocalContext;
		if (!Context) { LocalContext.emplace(); Context = &*LocalContext; }
		const auto Dependents = Context->GetMaterialsAffectedByMaterial(this);
		Publish(*this);
		for (auto* Owner : Dependents)
		{
			if (IsValid(Owner) && Owner != this) Publish(*Owner);
		}
		if (bNotifyParameterChanges)
		{
			Context->EndDiscovery();
			BroadcastParameterChanges(Dependents);
		}
	}

	auto GetLoadedDirectMaterialChildren(const DMaterialInterface* Parent) -> std::vector<FObjectKey>
	{
		FObjectCacheContext Context;
		std::vector<FObjectKey> Result;
		for (auto* Object : Context.GetDirectMaterialChildren(Parent)) Result.emplace_back(Object);
		return Result;
	}

	auto GetLoadedMaterialDependents(const DMaterialInterface* Dependency) -> std::vector<FObjectKey>
	{
		FObjectCacheContext Context;
		std::vector<FObjectKey> Result;
		for (auto* Object : Context.GetMaterialsAffectedByMaterial(Dependency)) Result.emplace_back(Object);
		return Result;
	}
}
