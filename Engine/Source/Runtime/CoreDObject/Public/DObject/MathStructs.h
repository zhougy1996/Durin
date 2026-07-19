#pragma once

#include "CoreDObjectAPI.h"

namespace Durin
{
	class DStruct;

	COREDOBJECT_API auto Z_Construct_DStruct_Durin_FVector3() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_Durin_FQuat() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_Durin_FTransform() -> DStruct*;
	COREDOBJECT_API auto Z_Construct_DStruct_Durin_FLinearColor() -> DStruct*;
}

COREDOBJECT_API auto Z_Construct_DStruct_Durin_FVector3() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FQuat() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FTransform() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FLinearColor() -> Durin::DStruct*;
