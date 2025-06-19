#pragma once

class IDynamicRHI;

extern RHI_API IDynamicRHI* GDynamicRHI;

extern RHI_API TArray<const char*> GKleeRequiredVulkanInstanceExtensions;

RHI_API auto RHIInit() -> void;

RHI_API auto RHIExit() -> void;

