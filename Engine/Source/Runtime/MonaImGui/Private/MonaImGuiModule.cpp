#include "MonaCoreGlobals.h"
#include "MonaImGuiBackend.h"

namespace Durin
{
	// Installs and removes the process-wide Mona ImGui backend.
	class FMonaImGuiModule : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			Backend = std::make_unique<MonaImGui::FMonaImGuiBackend>();
			Backend->Initialize();
			if (!Mona::RegisterUIBackend(*Backend))
			{
				Backend->Shutdown();
				Backend.reset();
				throw std::runtime_error("Mona already has an active UI backend.");
			}
		}

		auto ShutdownModule() -> void override
		{
			if (!Backend) return;
			try
			{
				Backend->Shutdown();
			}
			catch (...)
			{
				Mona::UnregisterUIBackend(*Backend);
				Backend.reset();
				throw;
			}
			if (!Mona::UnregisterUIBackend(*Backend))
				throw std::runtime_error("MonaImGui no longer owns the active UI backend.");
			Backend.reset();
		}

	private:
		std::unique_ptr<MonaImGui::FMonaImGuiBackend> Backend;
	};

	IMPLEMENT_MODULE(FMonaImGuiModule, MonaImGui)
}
