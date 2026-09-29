#include "Texture/ITextureBuildModule.h"
#if DURIN_WITH_EDITOR
#include "TextureBuildSession.h"
#endif

namespace Durin
{
	auto ITextureBuildModule::StartupModule() -> void
	{
#if DURIN_WITH_EDITOR
		TexturePrivate::RegisterBuildFunctions(*this);
#endif
	}

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
