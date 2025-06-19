#include "RHIGlobals.h"

#include "DynamicRHI.h"

TArray<const char*> GKleeRequiredVulkanInstanceExtensions;

static auto CreateDynamicRHI() -> IDynamicRHI*
{
	IDynamicRHIModule* DynamicRHIModule = FModuleManager::LoadModule<IDynamicRHIModule>("VulkanRHI");
	if (!DynamicRHIModule)
	{
		DOGE_ERROR("Failed to load VulkanRHI module");
		return nullptr;
	}
	return DynamicRHIModule->CreateRHI();
}

auto RHIInit() -> void
{
	DOGE_DEBUG("Initializing RHI");
	GDynamicRHI = CreateDynamicRHI();
	if (GDynamicRHI == nullptr)
	{
		DOGE_ERROR("Failed to create dynamic RHI");
		return;
	}
	GDynamicRHI->Init();
}

auto RHIExit() -> void
{
	delete GDynamicRHI;
}
