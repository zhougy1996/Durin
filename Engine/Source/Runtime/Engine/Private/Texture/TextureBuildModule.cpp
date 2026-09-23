#include "Texture/ITextureBuildModule.h"

namespace Durin
{
	auto FTextureBuildSession::Acquire() -> FTextureBuildSession
	{
		FTextureBuildSession Session;
#if DURIN_WITH_EDITOR
		auto& Manager = FModuleManager::Get();
		Session.CodeLease = Manager.AcquireCodeLease("TextureBuild");
		if (Session.CodeLease)
		{
			const auto Info = Manager.FindModule("TextureBuild");
			Session.Module = static_cast<ITextureBuildModule*>(Info->Module.get());
			Session.Generation = Info->OwnerGeneration;
		}
#endif
		return Session;
	}
}
