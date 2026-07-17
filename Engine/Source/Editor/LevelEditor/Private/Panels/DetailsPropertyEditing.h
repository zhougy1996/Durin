#pragma once

namespace Durin
{
	class DObject;
	class FLevelEditorContext;
	class FProperty;

	auto AssignDetailsObjectProperty(
		FLevelEditorContext& Context,
		DObject* Object,
		FProperty* Property,
		uint32 ArrayIndex,
		DObject* Value
	) -> bool;
}
