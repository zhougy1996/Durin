#include "EngineTestSupport.h"
#include "RenderingThread.h"

#include <gtest/gtest.h>

namespace
{
	// Keeps material-proxy publications admitted for the complete static-mesh test target.
	class FStaticMeshTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			ASSERT_EQ(
				Durin::GetRenderCommandAdmissionState(),
				Durin::ERenderCommandAdmissionState::Stopped);
			Durin::InitRenderingThread();
		}

		auto TearDown() -> void override
		{
			ASSERT_EQ(
				Durin::GetRenderCommandAdmissionState(),
				Durin::ERenderCommandAdmissionState::Running);
			Durin::FlushRenderingCommands();
			Durin::ShutdownRenderingThread();
		}
	};

	[[maybe_unused]] testing::Environment* GStaticMeshTestEnvironment =
		testing::AddGlobalTestEnvironment(new FStaticMeshTestEnvironment);
}
