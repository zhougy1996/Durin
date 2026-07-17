#include "Panels/DetailsPropertyEditing.h"

#include "Components/StaticMeshComponent.h"
#include "DObject/DurinPropertyTypes.h"
#include "LevelEditorContext.h"
#include "Materials/MaterialInstance.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
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
		if (Value && ObjectProperty->GetReferencedClass() && !Value->IsA(ObjectProperty->GetReferencedClass()))
		{
			Context.SetError("Selected asset has an incompatible type.");
			return false;
		}

		if (auto* Component = Cast<DStaticMeshComponent>(Object))
		{
			if (Property->NamePrivate == FName("StaticMesh"))
			{
				DStaticMesh* Mesh = Value ? Cast<DStaticMesh>(Value) : nullptr;
				if ((Value && !Mesh) || Component->GetStaticMesh() == Mesh) return false;
				Component->SetStaticMesh(Mesh);
				return true;
			}
			if (Property->NamePrivate == FName("Material"))
			{
				DMaterialInterface* Material = Value ? Cast<DMaterialInterface>(Value) : nullptr;
				if ((Value && !Material) || Component->GetMaterial() == Material) return false;
				Component->SetMaterial(Material);
				return true;
			}
		}

		if (auto* Instance = Cast<DMaterialInstance>(Object); Instance && Property->NamePrivate == FName("Parent"))
		{
			DMaterialInterface* Parent = Value ? Cast<DMaterialInterface>(Value) : nullptr;
			if ((Value && !Parent) || Instance->GetParent() == Parent) return false;
			if (!Instance->SetParent(Parent))
			{
				Context.SetError("A material instance cannot create a parent cycle.");
				return false;
			}
			return true;
		}

		if (ObjectProperty->GetObjectPropertyValue(Object, ArrayIndex) == Value) return false;
		ObjectProperty->SetObjectPropertyValue(Object, Value, ArrayIndex);
		Object->MarkPackageDirty();
		return true;
	}
}
