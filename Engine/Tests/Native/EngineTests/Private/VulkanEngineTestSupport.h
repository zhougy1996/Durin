#pragma once

#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "RHIInitialization.h"
#include "Window/GenericWindow.h"
#include "Window/GenericWindowDefinition.h"

namespace Durin::Tests
{
	class FVulkanEngineTestApplication final : public FGenericApplication
	{
	public:
		explicit FVulkanEngineTestApplication(
			std::shared_ptr<FGenericWindow> InWindow)
			: Window(std::move(InWindow))
		{
		}

		auto FindWindowByNativeWindowHandle(void* NativeWindowHandle)
			-> std::shared_ptr<FGenericWindow> override
		{
			if (Window
				&& Window->GetOSNativeWindowHandle() == NativeWindowHandle)
			{
				return Window;
			}
			return nullptr;
		}

	private:
		std::shared_ptr<FGenericWindow> Window;
	};

	class FVulkanEngineTestPresentation
	{
	public:
		FVulkanEngineTestPresentation()
		{
			if (!InitializeApplicationCore()) return;
			bApplicationCoreInitialized = true;
			Window = MakePlatformWindow();
			auto Definition = std::make_shared<FGenericWindowDefinition>();
			Definition->XDesiredPositionOnScreen = 0.0f;
			Definition->YDesiredPositionOnScreen = 0.0f;
			Definition->WidthDesiredOnScreen = 64.0f;
			Definition->HeightDesiredOnScreen = 64.0f;
			Definition->Title = "Vulkan engine test admission";
			Window->Initialize(Definition);
			if (!Window->GetOSNativeWindowHandle()) return;
			Application =
				std::make_shared<FVulkanEngineTestApplication>(Window);
			GApp = Application;
		}

		~FVulkanEngineTestPresentation()
		{
			if (GApp == Application) GApp = nullptr;
			Application.reset();
			Window.reset();
			if (bApplicationCoreInitialized) ShutdownApplicationCore();
		}

		auto GetInitializationContext() -> FRHIInitializationContext
		{
			void* NativeWindowHandle = Window
				? Window->GetOSNativeWindowHandle()
				: nullptr;
			if (!NativeWindowHandle || !Application)
				return FRHIInitializationContext::Headless();
			if (!GApp
				|| !GApp->FindWindowByNativeWindowHandle(NativeWindowHandle))
			{
				GApp = Application;
			}
			return FRHIInitializationContext::Presentation({
				.NativeWindowHandle = NativeWindowHandle});
		}

	private:
		bool bApplicationCoreInitialized = false;
		std::shared_ptr<FGenericWindow> Window;
		std::shared_ptr<FVulkanEngineTestApplication> Application;
	};

	inline auto GetVulkanEngineTestInitializationContext()
		-> FRHIInitializationContext
	{
		#ifdef _WIN32
		static FVulkanEngineTestPresentation Presentation;
		return Presentation.GetInitializationContext();
		#else
		return FRHIInitializationContext::Headless();
		#endif
	}
}
