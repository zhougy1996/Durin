#include "Panels/DetailsPropertyEditing.h"

#include "Components/StaticMeshComponent.h"
#include "DObject/Archive.h"
#include "DObject/DurinPropertyTypes.h"
#include "LevelEditorContext.h"
#include "Materials/MaterialInstance.h"
#include "Math/Transform.h"
#include "Misc/StringHelper.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	namespace
	{
		auto Fail(std::string* OutError, std::string_view Message) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}

		auto AssignObjectPropertyValue(DObject* Object, const FProperty* Property, uint32 ArrayIndex, DObject* Value, std::string* OutError) -> bool
		{
			if (!Object || !Property || Property->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
				return Fail(OutError, "The Details object-property target is invalid.");
			auto* ObjectProperty = static_cast<const FObjectProperty*>(Property);
			if (Value && ObjectProperty->GetReferencedClass() && !Value->IsA(ObjectProperty->GetReferencedClass()))
				return Fail(OutError, "Selected asset has an incompatible type.");

			if (auto* Component = Cast<DStaticMeshComponent>(Object))
			{
				if (Property->NamePrivate == FName("StaticMesh"))
				{
					DStaticMesh* Mesh = Value ? Cast<DStaticMesh>(Value) : nullptr;
					if (Value && !Mesh) return Fail(OutError, "Selected asset is not a static mesh.");
					Component->SetStaticMesh(Mesh);
					return true;
				}
				if (Property->NamePrivate == FName("Material"))
				{
					DMaterialInterface* Material = Value ? Cast<DMaterialInterface>(Value) : nullptr;
					if (Value && !Material) return Fail(OutError, "Selected asset is not a material.");
					Component->SetMaterial(Material);
					return true;
				}
			}

			if (auto* Instance = Cast<DMaterialInstance>(Object); Instance && Property->NamePrivate == FName("Parent"))
			{
				DMaterialInterface* Parent = Value ? Cast<DMaterialInterface>(Value) : nullptr;
				if (Value && !Parent) return Fail(OutError, "Selected asset is not a material.");
				if (!Instance->SetParent(Parent)) return Fail(OutError, "A material instance cannot create a parent cycle.");
				return true;
			}

			ObjectProperty->SetObjectPropertyValue(Object, Value, ArrayIndex);
			return true;
		}

		class FDetailsPropertyMutationAdapter final : public IReflectedPropertyMutationAdapter
		{
		public:
			auto Capture(const FReflectedPropertyEditTarget& Target, FPropertyValueSnapshot& OutSnapshot, std::string* OutError) const -> bool override
			{
				return GetGenericReflectedPropertyMutationAdapter().Capture(Target, OutSnapshot, OutError);
			}

			auto Apply(const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& Snapshot, std::string* OutError) const -> bool override
			{
				return ApplySnapshot(Target, Snapshot, OutError);
			}

			auto Restore(const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& Snapshot, std::string* OutError) const -> bool override
			{
				return ApplySnapshot(Target, Snapshot, OutError);
			}

		private:
			auto ApplySnapshot(const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& Snapshot, std::string* OutError) const -> bool
			{
				const auto& Generic = GetGenericReflectedPropertyMutationAdapter();
				const bool bTopLevelValue = Target.SnapshotProperty == Target.LeafProperty && Target.SnapshotContainer == Target.Object;
				const bool bRelativeTransform = bTopLevelValue && Cast<DSceneComponent>(Target.Object)
					&& Target.LeafProperty->NamePrivate == FName("RelativeTransform")
					&& Target.LeafProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct;
				const bool bObjectProperty = bTopLevelValue && Target.LeafProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Object;
				if (!bRelativeTransform && !bObjectProperty) return Generic.Apply(Target, Snapshot, OutError);

				// Decode through reflected storage, then restore it before invoking the setter.
				// Setters often compare against the stored field, so leaving the proposal in
				// place would suppress exactly the runtime side effects this adapter preserves.
				FPropertyValueSnapshot Previous;
				if (!Generic.Capture(Target, Previous, OutError) || !Generic.Apply(Target, Snapshot, OutError)) return false;
				if (bRelativeTransform)
				{
					const FTransform Value = *Target.LeafProperty->ContainerPtrToValuePtr<FTransform>(Target.LeafContainer, Target.LeafArrayIndex);
					if (!Generic.Restore(Target, Previous, OutError)) return false;
					Cast<DSceneComponent>(Target.Object)->SetRelativeTransform(Value);
					return true;
				}

				auto* ObjectProperty = static_cast<const FObjectProperty*>(Target.LeafProperty);
				DObject* Value = ObjectProperty->GetObjectPropertyValue(Target.LeafContainer, Target.LeafArrayIndex);
				if (!Generic.Restore(Target, Previous, OutError)) return false;
				return AssignObjectPropertyValue(Target.Object, Target.LeafProperty, Target.LeafArrayIndex, Value, OutError);
			}
		};

		const FDetailsPropertyMutationAdapter GDetailsPropertyMutationAdapter;
	}

	auto MakeDetailsPropertyDisplayName(
		std::string_view PropertyName,
		DurinCodeGen::EPropertyGenFlags Kind,
		std::string_view ExplicitDisplayName
	) -> std::string
	{
		if (!ExplicitDisplayName.empty()) return std::string(ExplicitDisplayName);

		// The leading b is a C++ type convention, not part of the user-facing name.
		// Requiring an uppercase successor keeps unrelated names such as "border" intact.
		if (Kind == DurinCodeGen::EPropertyGenFlags::Bool && PropertyName.size() > 1 && PropertyName.front() == 'b'
			&& std::isupper(static_cast<unsigned char>(PropertyName[1])))
		{
			PropertyName.remove_prefix(1);
		}
		return StringUtils::HumanizeName(PropertyName);
	}

	auto AssignDetailsObjectProperty(
		FLevelEditorContext& Context,
		DObject* Object,
		FProperty* Property,
		uint32 ArrayIndex,
		DObject* Value
	) -> bool
	{
		if (!Object || !Property || Property->GetKind() != DurinCodeGen::EPropertyGenFlags::Object) return false;
		auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
		if (ObjectProperty->GetObjectPropertyValue(Object, ArrayIndex) == Value) return false;
		std::string Error;
		if (!AssignObjectPropertyValue(Object, Property, ArrayIndex, Value, &Error))
		{
			Context.SetError(std::move(Error));
			return false;
		}
		return true;
	}

	auto GetDetailsPropertyMutationAdapter() -> const IReflectedPropertyMutationAdapter&
	{
		return GDetailsPropertyMutationAdapter;
	}
}
