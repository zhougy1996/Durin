#pragma once

#include "CoreDObjectAPI.h"

namespace Durin
{
	class DStruct;
}

COREDOBJECT_API auto Z_Construct_DStruct_Durin_FVector2f() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FVector3f() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FVector4f() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FVector2() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FVector3() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FVector4() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FQuatf() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FQuat() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FMatrix4f() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FTransform() -> Durin::DStruct*;
COREDOBJECT_API auto Z_Construct_DStruct_Durin_FLinearColor() -> Durin::DStruct*;

// Keep qualified source compatibility without exporting a second helper set.
namespace Durin
{
	using ::Z_Construct_DStruct_Durin_FLinearColor;
	using ::Z_Construct_DStruct_Durin_FMatrix4f;
	using ::Z_Construct_DStruct_Durin_FQuat;
	using ::Z_Construct_DStruct_Durin_FQuatf;
	using ::Z_Construct_DStruct_Durin_FTransform;
	using ::Z_Construct_DStruct_Durin_FVector2;
	using ::Z_Construct_DStruct_Durin_FVector2f;
	using ::Z_Construct_DStruct_Durin_FVector3;
	using ::Z_Construct_DStruct_Durin_FVector3f;
	using ::Z_Construct_DStruct_Durin_FVector4;
	using ::Z_Construct_DStruct_Durin_FVector4f;
} // namespace Durin
