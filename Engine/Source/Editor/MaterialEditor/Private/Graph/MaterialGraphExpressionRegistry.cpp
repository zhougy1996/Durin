#include "MaterialGraphExpressionRegistry.h"
#include "DObject/Class.h"

namespace Durin::MaterialEditor
{
	auto NewRegisteredMaterialExpression(DClass* Class) -> DMaterialExpression*
	{
		const auto* Description = FindMaterialExpressionDescription(Class);
		if (!Description || !Description->bRegisterForEditorCreation) return nullptr;
		return NewObject<DMaterialExpression>(Class, nullptr, NAME_None);
	}
}
