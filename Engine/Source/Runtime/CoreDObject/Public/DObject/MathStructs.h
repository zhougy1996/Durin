#pragma once

#include "CoreDObjectAPI.h"

namespace Durin
{
	class DStruct;

	COREDOBJECT_API auto Z_Construct_DStruct_FVector2f() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_FVector3f() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_FVector4f() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_FVector2() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_FVector3() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_FVector4() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_FQuatf() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_FQuat() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_FMatrix4f() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_FTransform() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_FLinearColor() -> DStruct*;
} // namespace Durin
