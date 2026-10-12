#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/ITextureCompressorModule.h"
#if DURIN_WITH_EDITOR
#include "TextureBuildSession.h"
#endif

namespace Durin
{
	auto ITextureCompressorModule::StartupModule() -> void
	{
#if DURIN_WITH_EDITOR
		TexturePrivate::RegisterBuildFunctions();
#endif
	}
	auto ITextureCompressorModule::ShutdownModule() -> void
	{
#if DURIN_WITH_EDITOR
		TexturePrivate::ShutdownBuildSessions();
#endif
	}

	auto ITextureCompressorModule::Get() -> ITextureCompressorModule*
	{
#if DURIN_WITH_EDITOR
		const auto Info = FModuleManager::Get().FindModule("TextureCompressor");
		if (Info && Info->State.load() == EModuleState::Active)
			return static_cast<ITextureCompressorModule*>(Info->Module.get());
#endif
		return nullptr;
	}
}

#endif
