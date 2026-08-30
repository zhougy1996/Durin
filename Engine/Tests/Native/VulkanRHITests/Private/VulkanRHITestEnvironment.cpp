#include "VulkanRHITestEnvironment.h"

#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "Window/GenericWindow.h"
#include "Window/GenericWindowDefinition.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		// Publishes the fixture window through the production surface lookup boundary.
		class FVulkanTestApplication final : public FGenericApplication
		{
		public:
			explicit FVulkanTestApplication(
				std::shared_ptr<FGenericWindow> InWindow)
				: Window(std::move(InWindow))
			{
			}

			auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle)
				-> std::shared_ptr<FGenericWindow> override
			{
				if (Window
					&& Window->GetOSNativeWindowHandle() == InNativeWindowHandle)
				{
					return Window;
				}
				return nullptr;
			}

		private:
			std::shared_ptr<FGenericWindow> Window;
		};

		// Owns one hidden platform window and its reference-counted ApplicationCore lease.
		class FVulkanTestPresentationWindow
		{
		public:
			FVulkanTestPresentationWindow()
			{
				if (!InitializeApplicationCore()) return;
				bApplicationCoreInitialized = true;
				Window = MakePlatformWindow();
				auto Definition = std::make_shared<FGenericWindowDefinition>();
				Definition->XDesiredPositionOnScreen = 0.0f;
				Definition->YDesiredPositionOnScreen = 0.0f;
				Definition->WidthDesiredOnScreen = 64.0f;
				Definition->HeightDesiredOnScreen = 64.0f;
				Definition->Title = "Vulkan RHI integration test admission";
				Window->Initialize(Definition);
				if (!Window->GetOSNativeWindowHandle()) return;
				Application = std::make_shared<FVulkanTestApplication>(Window);
				GApp = Application;
			}

			~FVulkanTestPresentationWindow()
			{
				if (GApp == Application) GApp = nullptr;
				Application.reset();
				Window.reset();
				if (bApplicationCoreInitialized) ShutdownApplicationCore();
			}

			auto GetNativeHandle() -> void*
			{
				void* Handle = Window ? Window->GetOSNativeWindowHandle() : nullptr;
				if (Handle && (!GApp
					|| !GApp->FindWindowByNativeWindowHandle(Handle)))
				{
					GApp = Application;
				}
				return Handle;
			}

			auto GetInitializationContext() -> FRHIInitializationContext
			{
				void* Handle = GetNativeHandle();
				if (!Handle || !Window)
					return FRHIInitializationContext::Headless();
				return FRHIInitializationContext::Presentation({
					.NativeWindowHandle = Handle});
			}

		private:
			bool bApplicationCoreInitialized = false;
			std::shared_ptr<FGenericWindow> Window;
			std::shared_ptr<FVulkanTestApplication> Application;
		};
	}

	auto GetVulkanTestInitializationContext() -> FRHIInitializationContext
	{
	#if defined(_WIN32) || defined(__APPLE__)
		static FVulkanTestPresentationWindow PresentationWindow;
		return PresentationWindow.GetInitializationContext();
	#else
		return FRHIInitializationContext::Headless();
	#endif
	}
}
