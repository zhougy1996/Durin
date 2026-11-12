#include "Materials/MaterialInterface.h"
#include "Shader/ShaderCompilerCore.h"
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialShaderMap.h"
#include "MaterialPreparationQueue.h"
#include "Materials/ObjectCacheContext.h"
#include "MaterialLoadedQueryDiagnostics.h"
#include "MaterialDependencyIndex.h"

#include "Asset/Asset.h"
#include "Shader/ShaderCompilingManager.h"
#include "Asset/AssetCompilingManager.h"
#include "Modules/ModuleManager.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Materials/MaterialInstance.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Logging/LogMacros.h"
#include "Texture/Texture2D.h"
#include "Threading/RunnableThread.h"
#include "RenderingThread.h"

namespace Durin
{
#if DURIN_WITH_EDITORONLY_DATA
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
#endif

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
#if DURIN_WITH_EDITORONLY_DATA
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
				auto& Revision = Material->CompilationState.DefaultResource->Status.AuthoredRevision;
				Revision = Revision == std::numeric_limits<uint64>::max() ? 1 : Revision + 1;
				Private::FMaterialCompilationLifecycle::ScheduleEdit(*Material, &Context);
			}
		Context.EndDiscovery();
		for (const auto Key : Owners)
			if (auto* Material = Cast<DMaterialInterface>(Key.ResolveObjectPtr())) Material->ParameterChanges.Broadcast();
#endif
	}

	auto NotifyMaterialParameterCollectionChanged(
		DMaterialParameterCollection& Collection) -> void
	{
		CheckMaterialQueryThread();
		for (auto* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
			if (auto* Subsystem = Cast<DMaterialParameterCollectionSubsystem>(Object))
				if (auto Refreshed = Subsystem->RefreshCollection(Collection); !Refreshed)
					DURIN_ERROR("Failed to refresh material parameter collection '{}': {}",
						Collection.GetObjectPath(), Refreshed.error().Message);
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
		FObjectCacheContext Context;
		const auto Owners = QueryLoadedMaterialHandles(
			EMaterialLoadedQueryOperation::Dependents,
			[&](const DMaterialInterface* Material) {
				const auto Program = Material->CompilationState.DefaultResource->RenderLayer.CompiledProgram;
				return Program && std::ranges::contains(Program->ActiveCollections,
					Collection.GetCollectionId(),
					&FMaterialParameterCollectionLayout::CollectionId);
			});
		for (const auto Handle : Owners)
			if (auto* Material = Cast<DMaterialInterface>(ResolveObjectKey(Handle));
				IsValid(Material))
			{
				auto& Revision =
					Material->CompilationState.DefaultResource->Status.AuthoredRevision;
				Revision = Revision == std::numeric_limits<uint64>::max()
					? 1 : Revision + 1;
				Private::FMaterialCompilationLifecycle::ScheduleEdit(*Material, &Context);
			}
		Context.EndDiscovery();
	}

	auto ResolveMaterialProperties(const DMaterialInterface& Material,
		FResolvedMaterialProperties& OutProperties) -> std::expected<void, std::string>
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
				return std::unexpected("Material parent chain exceeds 64 owners or contains a cycle.");
			}
			if (!IsValid(Current)) break;
			Chain[Count++] = Current;
			if ((Root = Cast<DMaterial>(Current))) break;
		}
		if (!Root)
		{
			return std::unexpected("Material parent chain has no valid root material.");
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
				return std::unexpected("Material parent chain contains an unsupported material owner.");
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

	auto GetMaterialProgramStatistics(const DMaterialInterface& Material)
		-> FMaterialProgramStatistics
	{
		CheckMaterialQueryThread();
		FMaterialProgramStatistics Result;
		const DMaterialInterface* StatusOwner = &Material;
		if (Material.IsDynamicInstance() && IsValid(Material.GetParent()))
			StatusOwner = Material.GetParent();
		const auto& Status = StatusOwner->GetMaterialCompileStatus();
		Result.CacheOutcome = Status.CacheOutcome;
		const auto Program = Material.GetAcceptedCompiledProgram();
		if (!Program) return Result;
		Result.State = Status.IsCurrent()
			? EMaterialProgramStatisticsState::Current
			: EMaterialProgramStatisticsState::LastKnownGood;
		Result.ProgramIdentity = Program->GetShaderMap().ShaderMapId;
		Result.NormalizedIRNodeCount = Program->GetDebugData().IR.Nodes.size();
		Result.ActiveParameterCount = Program->GetShaderMap().ActiveParameters.size();
		Result.UniformPayloadBytes = Program->GetShaderMap().Layout.UniformPayloadSize;
		Result.TextureCount = std::ranges::count_if(Program->GetShaderMap().Layout.Fields,
			[](const FMaterialRenderField& Field) {
				return Field.Storage == EMaterialRenderFieldStorage::Resource
					&& Field.Type == EMaterialRenderValueType::Texture2D;
			});
		Result.SamplerCount = Result.TextureCount;
		Result.GeneratedSourceBytes = Program->GetDebugData().GeneratedSource.size();
		Result.CompiledShaderCount = Program->GetShaderMap().CompiledShaders.size();
		Result.DependencyCount = Program->GetShaderMap().Dependencies.size();
		Result.CompileTime = Program->GetShaderMap().CompileTime;
		for (const FShaderCode& Shader : Program->GetShaderMap().CompiledShaders)
		{
			const uint64 Bytes = Shader.Code.size();
			if (Bytes > std::numeric_limits<uint64>::max() - Result.CompiledCodeBytes)
			{
				Result.CompiledCodeBytes = std::numeric_limits<uint64>::max();
				Result.bSizeOverflow = true;
				break;
			}
			Result.CompiledCodeBytes += Bytes;
		}
		return Result;
	}

	auto GetLoadedMaterialFamilyStatistics(const DMaterialInterface& Material)
		-> FLoadedMaterialFamilyStatistics
	{
		CheckMaterialQueryThread();
		FLoadedMaterialFamilyStatistics Result;
		FResolvedMaterialProperties Resolved;
		if (!ResolveMaterialProperties(Material, Resolved)) return Result;
		auto* Root = Cast<DMaterialInterface>(ResolveObjectKey(Resolved.Root));
		if (!Root) return Result;
		std::vector<FMaterialProgramIdentity> Programs;
		std::vector<FMaterialStaticProperties> StaticConfigurations;
		for (const FObjectKey Key : GetLoadedMaterialDependents(Root))
		{
			auto* Owner = Cast<DMaterialInterface>(ResolveObjectKey(Key));
			if (!IsValid(Owner) || Owner->IsDynamicInstance()) continue;
			++Result.LoadedOwnerCount;
			const auto& Status = Owner->GetMaterialCompileStatus();
			const bool bPending = Owner->IsCompiling();
			const bool bFailed = !Status.HasUnsubmittedEdits()
				&& (Status.State == EMaterialCompileState::Failed);
			Result.PendingOwnerCount += bPending;
			Result.FailedOwnerCount += bFailed;
			const auto Program = Owner->GetAcceptedCompiledProgram();
			if (!Program) continue;
			++Result.AcceptedOwnerCount;
			if (!std::ranges::contains(Programs, Program->GetShaderMap().ShaderMapId))
				Programs.push_back(Program->GetShaderMap().ShaderMapId);
			const FMaterialStaticProperties Properties =
				Owner->GetMaterialResource().GetStaticProperties();
			if (!std::ranges::contains(StaticConfigurations, Properties))
				StaticConfigurations.push_back(Properties);
		}
		Result.DistinctProgramCount = Programs.size();
		Result.DistinctStaticConfigurationCount = StaticConfigurations.size();
		return Result;
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
		bool bForceRecompile, FObjectCacheContext* Context,
		std::optional<FMaterialCompilerEnvironment> RequestedEnvironment,
		std::shared_ptr<FMaterialCompilationResource> Resource) -> bool
	{
		if (IsDynamicInstance()) return false;
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return false;
		if (FWeakObjectPtr(this).GetKey().IsNull() && IsMaterialCompilationAcceptingRequests()) return true;
		if (!Resource) Resource = CompilationState.DefaultResource;
		Resource->Status.AuthoredRevision = CompilationState.DefaultResource->Status.AuthoredRevision;
		CompilationState.LastObservedShaderProperties = CanonicalizeMaterialShaderProperties(CandidateProperties);
		FModuleManager::Get().LoadModule("RenderCore");
		FMaterialCompilerEnvironment Environment;
		const auto EnvironmentError = RequestedEnvironment
			? std::expected<void, std::string>{}
			: BuildDefaultMaterialCompilerEnvironment(Environment);
		if (RequestedEnvironment) Environment = std::move(*RequestedEnvironment);
		if (!EnvironmentError)
		{
			Private::FMaterialCompilationLifecycle::ResetRequest(*this, Resource);
			Resource->Status.State = EMaterialCompileState::Failed;
			Resource->Diagnostics = {{.Message = EnvironmentError.error()}};
			Resource->bRequested = false;
			Resource->RenderLayer = {};
			Resource->AcceptedExpressionSources.clear();
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap, false, Context);
			return false;
		}
		auto Snapshot = SnapshotMaterialCompilerInput(*this, std::move(Environment));
		if (!Snapshot)
		{
			Private::FMaterialCompilationLifecycle::ResetRequest(*this, Resource);
			auto& Status = Resource->Status;
			Status.State = EMaterialCompileState::Failed;
			Resource->Diagnostics = std::move(Snapshot.Diagnostics);
			Resource->bRequested = false;
			Resource->RenderLayer = {};
			Resource->AcceptedExpressionSources.clear();
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap, false, Context);
			return false;
		}
		auto& Input = Snapshot.Snapshot->Input;
		Input.StaticProperties = CandidateProperties;
		CompilationState.LastObservedParameters = Input.Parameters;
		return Private::FMaterialCompilationLifecycle::Submit(
			*this, std::move(Input), bForceRecompile, Context, std::move(Resource));
	}

	auto DMaterialInterface::RequestPreviewProgramCompile(
		EMaterialQualityLevel Quality, ERHIFeatureLevel FeatureLevel) -> bool
	{
		if (!IsSupportedMaterialQualityLevel(Quality)
			|| !IsSupportedMaterialFeatureLevel(FeatureLevel)
			|| IsDynamicInstance() || GetAssetRuntimeConfiguration().RequiresCookedPayload()) return false;
		FModuleManager::Get().LoadModule("RenderCore");
		FMaterialCompilerEnvironment Environment;
		if (!BuildDefaultMaterialCompilerEnvironment(Environment)
			|| FeatureLevel > Environment.FeatureLevel) return false;
		Environment.Quality = Quality;
		Environment.FeatureLevel = FeatureLevel;
		return RequestProgramCompile(GetStaticProperties(), false, nullptr,
			std::move(Environment));
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
					== Owner->CompilationState.LastObservedShaderProperties) continue;
			auto& Status = Owner->CompilationState.DefaultResource->Status;
			Status.AuthoredRevision = Status.AuthoredRevision == std::numeric_limits<uint64>::max()
				? 1 : Status.AuthoredRevision + 1;
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

	auto DMaterialInterface::ResolveResourceProperties() const
		-> FMaterialStaticProperties
	{
		if (IsDynamicInstance()) return IsValid(GetParent())
			? GetParent()->ResolveResourceProperties() : FMaterialStaticProperties{};
		const auto& Layer = CompilationState.DefaultResource->RenderLayer;
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
		return CompilationState.DefaultResource->RenderLayer.CompiledProgram;
	}

	auto DMaterialInterface::GetMaterialCompileStatus() const -> FMaterialCompileStatus
	{
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetMaterialCompileStatus() : FMaterialCompileStatus{};
		auto Status = CompilationState.DefaultResource->Status;
		Status.bHasScheduledEdits = CompilationState.EditCompileDeadline.has_value();
		const auto& Resource = *CompilationState.DefaultResource;
		if (Resource.bPending || Resource.CookedPreparation) Status.State = EMaterialCompileState::Running;
		Status.CompiledIdentity = Resource.RenderLayer.CompiledProgram ? Resource.RenderLayer.CompiledProgram->GetShaderMap().ShaderMapId : FMaterialProgramIdentity{};
		Status.bDependenciesCurrent = Status.CompiledDependencyRevision == GetShaderReloadGeneration()
			&& Resource.RenderLayer.CompiledProgram && Resource.RenderLayer.CompiledProgram->FeatureLevel == Resource.FeatureLevel
			&& Resource.RenderLayer.CompiledProgram->Quality == Resource.Quality;
		return Status;
	}

	auto DMaterialInterface::HasValidGameThreadShaderMap() const -> bool
	{
		if (IsDynamicInstance()) return IsValid(GetParent()) && GetParent()->HasValidGameThreadShaderMap();
		return CompilationState.DefaultResource->RenderLayer.HasValidGameThreadShaderMap();
	}

	auto DMaterialInterface::IsCompiling() const -> bool
	{
		CheckMaterialQueryThread();
		if (IsDynamicInstance()) return IsValid(GetParent()) && GetParent()->IsCompiling();
		return HasPendingMaterialCompilation(*this);
	}

	auto DMaterialInterface::IsComplete() const -> bool
	{
		CheckMaterialQueryThread();
		if (IsDynamicInstance()) return IsValid(GetParent()) && GetParent()->IsComplete();
		return CompilationState.DefaultResource->RenderLayer.CompiledProgram
			&& (GetAssetRuntimeConfiguration().RequiresCookedPayload()
				|| GetMaterialCompileStatus().IsCurrent());
	}

	auto DMaterialInterface::GetUsedTextures(std::vector<DTexture2D*>& OutTextures) const -> void
	{
		CheckMaterialQueryThread();
		OutTextures.clear();
		const DMaterialInterface* Owner = this;
		while (Owner->IsDynamicInstance() && IsValid(Owner->GetParent())) Owner = Owner->GetParent();
		const auto& Program = Owner->CompilationState.DefaultResource->RenderLayer.CompiledProgram;
		if (!Program) return;
		for (const auto& Parameter : Program->GetShaderMap().ActiveParameters)
		{
			if (Parameter.Type != EMaterialParameterType::Texture) continue;
			FResolvedMaterialParameter Resolved;
			if (!ResolveParameterValue(Parameter.Id, Resolved) || Resolved.Value.GetType() != EMaterialParameterType::Texture) continue;
			auto* Texture = Resolved.Value.GetTexture().Texture.Get();
			if (IsValid(Texture) && std::ranges::find(OutTextures, Texture) == OutTextures.end()) OutTextures.push_back(Texture);
		}
	}

	auto DMaterialInterface::AdoptParentRuntimeProgram() -> bool
	{
		const auto& Resource = CompilationState.DefaultResource;
		FShaderCompilingManager::Get().CancelCompilation(Resource);
		Resource->CookedPreparation.reset();
		Resource->RenderLayer = {};
		Resource->AcceptedExpressionSources.clear();
		auto* Parent = GetParent();
		const auto Program = Parent ? Parent->GetAcceptedCompiledProgram() : nullptr;
		if (!Program || CanonicalizeMaterialShaderProperties(GetStaticProperties())
			!= CanonicalizeMaterialShaderProperties(Parent->ResolveResourceProperties())) return false;
		CompilationState.DefaultResource->RenderLayer.CompiledProgram = Program;
		CompilationState.DefaultResource->RenderLayer.StaticProperties = GetStaticProperties();
		CompilationState.DefaultResource->RenderLayer.Parameters = BuildMaterialResource().Parameters;
		auto& Status = CompilationState.DefaultResource->Status;
		Private::GetMaterialEditCompileQueue().Remove(FWeakObjectPtr(this));
		Status.State = EMaterialCompileState::Ready;
		Status.CompiledRevision = Status.SubmittedRevision = Status.AuthoredRevision;
		Status.CompiledDependencyRevision = Parent->GetMaterialCompileStatus().CompiledDependencyRevision;
		Resource->Material = FWeakObjectPtr(this);
		Resource->ShaderRevision = Status.CompiledDependencyRevision;
		Resource->FeatureLevel = Program->FeatureLevel;
		Resource->Quality = Program->Quality;
		Resource->Backend = Parent->CompilationState.DefaultResource->Backend;
		Resource->BulkRevision = CookedProgramData.GetContentRevision();
		Resource->bCooked = GetAssetRuntimeConfiguration().RequiresCookedPayload();
		Resource->CookRoot = GetAssetRuntimeConfiguration().GetCookRoot();
		Resource->CookTarget = GetAssetRuntimeConfiguration().GetCookTargetPlatform();
		CompilationState.EditCompileDeadline.reset();
		Status.Target = Program->GetShaderMap().Target;
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

	auto DMaterialInterface::GetRenderProxy() const
		-> FMaterialRenderProxyRef
	{
		CheckMaterialQueryThread();
		if (IsTemplateObject() || !bAcceptingMaterialProxyPublications) return {};
		if (bMaterialProxyPublicationDirty)
		{
			SubmitMaterialRenderProxyState(BuildMaterialResource());
		}
		return MaterialRenderProxy;
	}

	auto DMaterialInterface::BeginDestroy() -> void
	{
		PSOPrecacheOwner.Invalidate();
		ReleaseFeatureLevelResources();
		CompilationState.DefaultResource->CookedPreparation.reset();
		Private::GetMaterialPreparationQueue().Remove(FWeakObjectPtr(this));
		Private::GetMaterialEditCompileQueue().Remove(FWeakObjectPtr(this));
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
		FObjectCacheContext Context;
		std::vector<FWeakObjectPtr> Notifications;
		// A live child must retire a broken chain even if no compile is pending.
		for (auto* Material : Context.GetMaterialsAffectedByMaterial(this))
		{
			auto* Owner = Cast<DMaterialInstance>(Material);
			if (!IsValid(Owner)) continue;
			FResolvedMaterialProperties Resolved;
			const auto Error = ResolveMaterialProperties(*Owner, Resolved);
			if (Error) continue;
			FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Owner);
			auto& Status = Owner->CompilationState.DefaultResource->Status;
			Status.State = EMaterialCompileState::Failed;
			Owner->CompilationState.DefaultResource->Diagnostics = {{.Message = Error.error()}};
			Owner->RetireFailedMaterialGeneration(&Context);
			Notifications.emplace_back(Owner);
		}
		Context.EndDiscovery();
		for (const auto& Weak : Notifications)
			if (auto* Owner = Cast<DMaterialInterface>(Weak.Get())) Owner->ParameterChanges.Broadcast();
		Private::RemoveMaterialDependency(*this);
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

	auto DMaterialInterface::BuildMaterialResource() const
		-> FMaterialResource
	{
		if (IsDynamicInstance())
		{
			auto* Parent = GetParent();
			if (!IsValid(Parent)) return {};
			// Start from the parent's complete accepted generation, including retained
			// values while authored declarations or shader configuration are pending.
			auto Layer = Parent->BuildMaterialResource();
			const auto* Instance = Cast<DMaterialInstance>(this);
			for (auto& Parameter : Layer.Parameters)
			{
				FMaterialParameterValue Value;
				if (Instance->GetLocalParameterValue(Parameter.Id, Value) && Value.GetType() == Parameter.GetType())
					Parameter = BuildMaterialLocalRenderParameter(Parameter.Id, Value);
			}
			return Layer;
		}
		auto Accepted = CompilationState.DefaultResource->RenderLayer;
		Accepted.CompiledProgram = GetAcceptedCompiledProgram();
		Accepted.StaticProperties = ResolveResourceProperties();
		Accepted.bIsCompiling = IsCompiling();
		return BuildMaterialResource(std::move(Accepted));
	}

	auto DMaterialInterface::BuildMaterialResource(FMaterialResource Accepted) const -> FMaterialResource
	{
		auto Result = Accepted;
		Result.Parameters.clear();
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
		Retained.reserve(Accepted.Parameters.size());
		for (const auto& Parameter : Accepted.Parameters)
			Retained.emplace(Parameter.Id, &Parameter);
		Result.Parameters.reserve(Result.CompiledProgram->GetShaderMap().ActiveParameters.size());
		for (const auto& Parameter : Result.CompiledProgram->GetShaderMap().ActiveParameters)
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
		CompilationState.DefaultResource->RenderLayer = {};
		CompilationState.DefaultResource->AcceptedExpressionSources.clear();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap
			| EMaterialRenderDirtyFlags::PipelineState, false, Context);
	}

	auto DMaterialInterface::PublishMaterialRenderProxyState() -> void
	{
		CheckMaterialQueryThread();
		if (!bAcceptingMaterialProxyPublications) return;
		PublishMaterialRenderProxyState(MaterialRenderProxy
			? BuildMaterialResource() : FMaterialResource{});
	}

	auto DMaterialInterface::PublishMaterialRenderProxyState(
		FMaterialResource LocalLayer) -> void
	{
		CheckMaterialQueryThread();
		if (!bAcceptingMaterialProxyPublications) return;
		bMaterialProxyPublicationDirty = true;
		SubmitMaterialRenderProxyState(std::move(LocalLayer));
		PublishFeatureLevelResources();
	}

	auto DMaterialInterface::RefreshReloadedAssetBindings() -> void
	{
		Private::RefreshMaterialDependency(*this);
		PublishMaterialRenderProxyState();
		ParameterChanges.Broadcast();
	}

	auto DMaterialInterface::SubmitMaterialRenderProxyState(
		FMaterialResource LocalLayer) const -> void
	{
		if (!bAcceptingMaterialProxyPublications
			|| !MaterialRenderProxy)
		{
			return;
		}
		// A pre-start edit is rebuilt on the next proxy request after render admission opens.
		const auto Admission = GetRenderCommandAdmissionState();
		if (Admission == ERenderCommandAdmissionState::Stopped) return;
		checkf(Admission == ERenderCommandAdmissionState::Running,
			"Material publication must finish before render-command shutdown.");

		std::ranges::sort(LocalLayer.Parameters, {}, &FMaterialLocalRenderParameter::Id);
		MaterialRenderProxy->QueuePublication_GameThread(std::move(LocalLayer));
		bMaterialProxyPublicationDirty = false;
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
		auto Publish = [DirtyFlags](DMaterialInterface& Owner) {
			if (EnumHasAnyFlags(DirtyFlags,
				EMaterialRenderDirtyFlags::ShaderMap | EMaterialRenderDirtyFlags::PipelineState | EMaterialRenderDirtyFlags::ParentChain))
				Owner.PSOPrecacheOwner.Invalidate();
			++Owner.RenderChangeCount;
			if (Owner.RenderChangeCount == 0) ++Owner.RenderChangeCount;
			if (Owner.CompilationState.DefaultResource->RenderLayer.CompiledProgram)
			{
				// Retention and publication must describe the same resolved snapshot.
				// Update retention even when this owner cannot publish a proxy.
				auto LocalLayer = Owner.BuildMaterialResource();
				Owner.CompilationState.DefaultResource->RenderLayer.Parameters = LocalLayer.Parameters;
				Owner.CompilationState.DefaultResource->RenderLayer.StaticProperties = LocalLayer.StaticProperties;
				Owner.PublishMaterialRenderProxyState(std::move(LocalLayer));
			}
			else
			{
				Owner.PublishMaterialRenderProxyState();
			}
		};
		// Dynamic instances cannot be material parents. Their stable proxy is the
		// complete dependent boundary, so avoid a loaded-object family query for
		// every gameplay parameter commit.
		if (IsDynamicInstance())
		{
			Publish(*this);
			if (bNotifyParameterChanges) ParameterChanges.Broadcast();
			return;
		}
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
