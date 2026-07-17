#include "Materials/MaterialInstance.h"

namespace Durin
{
	DMaterialInstance::DMaterialInstance(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DMaterialInstance::SetParent(DMaterialInterface* InParent) -> bool
	{
		for (DMaterialInterface* Candidate = InParent; Candidate != nullptr; Candidate = Candidate->GetParent())
		{
			if (Candidate == this) return false;
		}
		if (Parent == InParent) return true;
		if (Parent != nullptr) Parent->RemoveDependentInstance(this);
		Parent = InParent;
		if (Parent != nullptr) Parent->AddDependentInstance(this);
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::GetParent() const -> DMaterialInterface*
	{
		return Parent.Get();
	}

	auto DMaterialInstance::SetScalarParameterValue(std::string_view Name, float Value) -> void
	{
		const std::string Key(Name);
		if (const auto It = ScalarParameterOverrides.find(Key); It != ScalarParameterOverrides.end() && It->second == Value) return;
		ScalarParameterOverrides[Key] = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
	}

	auto DMaterialInstance::SetVectorParameterValue(std::string_view Name, const FVector3& Value) -> void
	{
		const std::string Key(Name);
		if (const auto It = VectorParameterOverrides.find(Key); It != VectorParameterOverrides.end() && It->second == Value) return;
		VectorParameterOverrides[Key] = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
	}

	auto DMaterialInstance::ClearScalarParameterValue(std::string_view Name) -> bool
	{
		if (ScalarParameterOverrides.erase(std::string(Name)) == 0) return false;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::ClearVectorParameterValue(std::string_view Name) -> bool
	{
		if (VectorParameterOverrides.erase(std::string(Name)) == 0) return false;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::GetScalarParameterValue(std::string_view Name, float& OutValue) const -> bool
	{
		const auto It = ScalarParameterOverrides.find(std::string(Name));
		if (It != ScalarParameterOverrides.end())
		{
			OutValue = It->second;
			return true;
		}
		return Parent != nullptr && Parent->GetScalarParameterValue(Name, OutValue);
	}

	auto DMaterialInstance::GetVectorParameterValue(std::string_view Name, FVector3& OutValue) const -> bool
	{
		const auto It = VectorParameterOverrides.find(std::string(Name));
		if (It != VectorParameterOverrides.end())
		{
			OutValue = It->second;
			return true;
		}
		return Parent != nullptr && Parent->GetVectorParameterValue(Name, OutValue);
	}

	auto DMaterialInstance::BeginDestroy() -> void
	{
		if (Parent != nullptr) Parent->RemoveDependentInstance(this);
		Parent = nullptr;
		Super::BeginDestroy();
	}

	auto DMaterialInstance::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		for (DMaterialInterface* Candidate = Parent.Get(); Candidate != nullptr; Candidate = Candidate->GetParent())
		{
			if (Candidate == this)
			{
				OutError = "A material instance asset contains a parent cycle.";
				return false;
			}
		}
		if (Parent != nullptr) Parent->AddDependentInstance(this);
		return true;
	}

	auto DMaterialInstance::OnParentRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags) -> void
	{
		MarkRenderDataDirty(DirtyFlags | EMaterialRenderDirtyFlags::ParentChain);
	}
}
