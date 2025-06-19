#pragma once

extern RHI_API TArray<const char*> GKleeRequiredVulkanInstanceExtensions;

RHI_API auto RHIInit() -> void;

RHI_API auto RHIExit() -> void;

