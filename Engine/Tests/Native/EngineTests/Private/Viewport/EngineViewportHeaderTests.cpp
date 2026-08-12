#include "gtest/gtest.h"

#include "Client/SceneViewport.h"

TEST(FEngineViewportHeaderTests, ExposesOnlyTheMonaCoreDisplaySourceBoundary)
{
	static_assert(std::is_base_of_v<Durin::IViewportDisplaySource, Durin::FSceneViewport>);
	SUCCEED();
}
