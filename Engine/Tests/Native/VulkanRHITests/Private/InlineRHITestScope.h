#pragma once

#include "CoreMinimal.h"

#include "DynamicRHI.h"
#include "RHIGlobals.h"


namespace Durin::VulkanRHI
{
	class FInlineRHITestScope
	{
	public:
		FInlineRHITestScope()
		{
			_putenv_s("DURIN_RHI_EXECUTION", "inline");
		}

		FInlineRHITestScope(const FInlineRHITestScope&) = delete;
		auto operator=(const FInlineRHITestScope&) -> FInlineRHITestScope& = delete;

		~FInlineRHITestScope()
		{
			if (GDynamicRHI) RHIExit();
			_putenv_s("DURIN_RHI_EXECUTION", "");
		}
	};
}
