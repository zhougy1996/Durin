#include "Materials/MaterialInterface.h"

#include "Asset.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Materials/MaterialInstance.h"
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
		Result.Representation = MakeCanonicalMaterialRenderRepresentation();
		FMaterialRenderRepresentationBuilder RepresentationBuilder(
			Result.Representation);
		bool bRepresentationValid = true;
		for (const FMaterialParameterDefinition& Definition : GetCanonicalMaterialParameterDefinitions())
		{
			FResolvedMaterialParameter Parameter;
			if (!ResolveParameterValue(Definition.Id, Parameter)) continue;
			if (Definition.Type == EMaterialParameterType::Texture)
			{
				DTexture2D* Texture = Parameter.Value.TextureValue.Get();
				const bool bExpectedSRGB = Definition.TextureUsage == ETextureUsage::Color;
				if (Texture != nullptr
					&& (Texture->GetUsage() != Definition.TextureUsage
						|| Texture->IsSRGB() != bExpectedSRGB))
				{
					DURIN_WARN_CATEGORY("Material", "Material '{}' parameter '{}' ignored texture '{}' because its usage or sRGB setting is incompatible.",
						GetName(), Definition.Name.ToString(), Texture->GetName());
				}
			}
			const FMaterialLocalRenderParameter LocalParameter =
				BuildMaterialLocalRenderParameter(
					Definition.Id, Definition.Type, Parameter.Value);
			bRepresentationValid = ApplyMaterialLocalRenderParameter(
				RepresentationBuilder, LocalParameter)
				&& bRepresentationValid;
		}
		const FMaterialStaticProperties& StaticProperties = GetStaticProperties();
		Result.PipelineIdentity.ShaderMap.BlendMode = StaticProperties.BlendMode;
		Result.PipelineIdentity.ShaderMap.ShadingModel = StaticProperties.ShadingModel;
		Result.PipelineIdentity.ShaderMap.OpacityMaskThreshold = StaticProperties.OpacityMaskThreshold;
		Result.PipelineIdentity.bTwoSided = StaticProperties.bTwoSided;
		Result.PipelineIdentity.DepthWritePolicy = StaticProperties.DepthWritePolicy;

		FMaterialRenderRepresentation CompiledRepresentation;
		FMaterialRenderValidationDiagnostic ValidationDiagnostic;
		if (bRepresentationValid
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
		return {};
	}

	auto DMaterialInterface::PublishMaterialRenderProxyState() -> void
	{
		CheckMaterialQueryThread();
		if (!bAcceptingMaterialProxyPublications) return;
		++MaterialProxyLocalVersion;
		if (MaterialProxyLocalVersion == 0) ++MaterialProxyLocalVersion;
		SubmitMaterialRenderProxyState();
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
		if (DMaterialInterface* Parent = GetParent();
			IsValid(Parent) && Parent != this)
		{
			Publication.ParentProxy = Parent->MaterialRenderProxy;
		}

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
		++RenderStateVersion;
		if (RenderStateVersion == 0) ++RenderStateVersion;
		PublishMaterialRenderProxyState();
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
