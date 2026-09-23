#include "Texture/ITextureBuildModule.h"

namespace Durin
{
	auto ITextureBuildModule::Get() -> ITextureBuildModule*
	{
#if DURIN_WITH_EDITOR
		const auto Info = FModuleManager::Get().FindModule("TextureBuild");
		if (Info && Info->State.load() == EModuleState::Active)
			return static_cast<ITextureBuildModule*>(Info->Module.get());
#endif
		return nullptr;
	}
}
