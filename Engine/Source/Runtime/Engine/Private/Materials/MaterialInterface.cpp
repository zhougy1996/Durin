#include "Materials/MaterialInterface.h"

#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialUpdateContext.h"
#include "Texture/Texture2D.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto CheckMaterialQueryThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto SortObjectHandles(std::vector<FObjectHandle>& Handles) -> void
		{
			std::ranges::sort(Handles, [](FObjectHandle Left, FObjectHandle Right) {
				return Left.Index < Right.Index
					|| (Left.Index == Right.Index && Left.Generation < Right.Generation);
			});
		}

		const FMaterialStaticProperties GDefaultMaterialStaticProperties;
	}

	DMaterialInterface::DMaterialInterface(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
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
		FResolvedMaterialParameter Parameter;
		if (ResolveParameterValue(MaterialParameters::BaseColorId, Parameter))
		{
			const FVector3& BaseColor = Parameter.Value.VectorValue;
			Result.BaseColor.r = static_cast<float>(std::clamp(BaseColor.x, 0.0, 1.0));
			Result.BaseColor.g = static_cast<float>(std::clamp(BaseColor.y, 0.0, 1.0));
			Result.BaseColor.b = static_cast<float>(std::clamp(BaseColor.z, 0.0, 1.0));
		}

		if (ResolveParameterValue(MaterialParameters::BaseColorTextureId, Parameter)
			&& Parameter.Value.TextureValue != nullptr)
		{
			Result.BaseColorTexture =
				Parameter.Value.TextureValue->GetTextureReferenceRHI();
		}

		if (ResolveParameterValue(MaterialParameters::OpacityId, Parameter))
		{
			Result.BaseColor.a = std::clamp(Parameter.Value.ScalarValue, 0.0f, 1.0f);
		}

		if (ResolveParameterValue(MaterialParameters::SpecularStrengthId, Parameter))
		{
			Result.SpecularStrength = std::clamp(Parameter.Value.ScalarValue, 0.0f, 1.0f);
		}
		if (ResolveParameterValue(MaterialParameters::ShininessId, Parameter))
		{
			Result.Shininess = std::clamp(Parameter.Value.ScalarValue, 1.0f, 256.0f);
		}
		const FMaterialStaticProperties& StaticProperties = GetStaticProperties();
		Result.PipelineIdentity.ShaderMap.BlendMode = StaticProperties.BlendMode;
		Result.PipelineIdentity.ShaderMap.ShadingModel = StaticProperties.ShadingModel;
		Result.PipelineIdentity.ShaderMap.OpacityMaskThreshold = StaticProperties.OpacityMaskThreshold;
		Result.PipelineIdentity.bTwoSided = StaticProperties.bTwoSided;
		Result.PipelineIdentity.DepthWritePolicy = StaticProperties.DepthWritePolicy;
		return Result;
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

	auto DMaterialInterface::MarkRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags) -> void
	{
		FMaterialUpdateContext Context;
		MarkRenderDataDirty(Context, DirtyFlags);
		Context.Flush();
	}

	auto DMaterialInterface::MarkRenderDataDirty(
		FMaterialUpdateContext& Context,
		EMaterialRenderDirtyFlags DirtyFlags
	) -> void
	{
		Context.AddMaterial(this, DirtyFlags);
	}

	auto GetLoadedDirectMaterialChildren(
		const DMaterialInterface* Parent
	) -> std::vector<FObjectHandle>
	{
		CheckMaterialQueryThread();
		if (!IsValid(Parent)) return {};

		std::vector<FObjectHandle> Result;
		const std::vector<DObject*> Objects = GDObjectArray.Snapshot();
		for (DObject* Object : Objects)
		{
			auto* Instance = Cast<DMaterialInstance>(Object);
			if (!IsValid(Instance) || Instance->GetParent() != Parent) continue;
			const FObjectHandle Handle = MakeObjectHandle(Instance);
			if (!IsObjectHandleNull(Handle)) Result.push_back(Handle);
		}
		SortObjectHandles(Result);
		return Result;
	}

	auto GetLoadedMaterialDependents(
		const DMaterialInterface* Dependency
	) -> std::vector<FObjectHandle>
	{
		CheckMaterialQueryThread();
		if (!IsValid(Dependency)) return {};

		std::vector<FObjectHandle> Result;
		const std::vector<DObject*> Objects = GDObjectArray.Snapshot();
		for (DObject* Object : Objects)
		{
			auto* Material = Cast<DMaterialInterface>(Object);
			if (!IsValid(Material) || !Material->IsDependent(Dependency)) continue;
			const FObjectHandle Handle = MakeObjectHandle(Material);
			if (!IsObjectHandleNull(Handle)) Result.push_back(Handle);
		}
		SortObjectHandles(Result);
		return Result;
	}
}
