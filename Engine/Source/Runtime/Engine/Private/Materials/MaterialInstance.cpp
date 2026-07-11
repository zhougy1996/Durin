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
		Parent = InParent;
		MarkPackageDirty();
		return true;
	}

	auto DMaterialInstance::GetParent() const -> DMaterialInterface*
	{
		return Parent.Get();
	}

	auto DMaterialInstance::SetScalarParameterValue(std::string_view Name, float Value) -> void
	{
		ScalarParameterOverrides[std::string(Name)] = Value;
		MarkPackageDirty();
	}

	auto DMaterialInstance::SetVectorParameterValue(std::string_view Name, const FVector3& Value) -> void
	{
		VectorParameterOverrides[std::string(Name)] = Value;
		MarkPackageDirty();
	}

	auto DMaterialInstance::ClearScalarParameterValue(std::string_view Name) -> bool
	{
		if (ScalarParameterOverrides.erase(std::string(Name)) == 0) return false;
		MarkPackageDirty();
		return true;
	}

	auto DMaterialInstance::ClearVectorParameterValue(std::string_view Name) -> bool
	{
		if (VectorParameterOverrides.erase(std::string(Name)) == 0) return false;
		MarkPackageDirty();
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
}
