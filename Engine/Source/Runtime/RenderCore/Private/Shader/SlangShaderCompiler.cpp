#include "SlangShaderCompiler.h"

#include "slang.h"
#include "slang-com-ptr.h"
#include <stdexcept>

namespace Doge
{
	static Slang::ComPtr<slang::ISession> CreateSlangSession()
	{
		Slang::ComPtr<slang::IGlobalSession> GlobalSession;
		if (SLANG_FAILED(slang_createGlobalSession(SLANG_API_VERSION, GlobalSession.writeRef())))
		{
			throw std::runtime_error("slang_createGlobalSession failed");
		}

		// Target: SPIR-V 1.5
		slang::TargetDesc TargetDesc = {};
		TargetDesc.format = SLANG_SPIRV;
		TargetDesc.profile = GlobalSession->findProfile("spirv_1_5");

		slang::SessionDesc SessionDesc = {};
		SessionDesc.targets = &TargetDesc;
		SessionDesc.targetCount = 1;

		Slang::ComPtr<slang::ISession> Session;
		if (SLANG_FAILED(GlobalSession->createSession(SessionDesc, Session.writeRef())))
		{
			throw std::runtime_error("createSession failed");
		}

		return Session;
	}

	FSlangShaderCompiler::FSlangShaderCompiler()
	{
		SlangSession = CreateSlangSession();
	}

	FSlangShaderCompiler::~FSlangShaderCompiler()
	{
	}
} // namespace Doge