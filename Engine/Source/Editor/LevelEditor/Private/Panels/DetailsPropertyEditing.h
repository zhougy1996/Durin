#pragma once

#include "DObject/DObjectGlobals.h"
#include "Editor/ReflectedPropertyEditing.h"

namespace Durin
{
	class DObject;
	class FLevelEditorContext;
	class FProperty;

	auto MakeDetailsPropertyDisplayName(
		std::string_view PropertyName,
		DurinCodeGen::EPropertyGenFlags Kind,
		std::string_view ExplicitDisplayName = {}
	) -> std::string;

	auto AssignDetailsObjectProperty(
		FLevelEditorContext& Context,
		DObject* Object,
		FProperty* Property,
		uint32 ArrayIndex,
		DObject* Value
	) -> bool;

	auto GetDetailsPropertyMutationAdapter() -> const IReflectedPropertyMutationAdapter&;
}
