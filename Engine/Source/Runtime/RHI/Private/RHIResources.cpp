#include "RHIResources.h"

namespace Doge
{
	FRHIResource::FRHIResource(ERHIResourceType InResourceType)
		: ResourceType(InResourceType)
	{
	}

	FRHIResource::~FRHIResource()
	{
	}

	FRHIShader::FRHIShader(ERHIResourceType InResourceType, EShaderFrequency InFrequency)
		: FRHIResource(InResourceType)
		, Frequency(InFrequency)
	{
	}
} // namespace Doge