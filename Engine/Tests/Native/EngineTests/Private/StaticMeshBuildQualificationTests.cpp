#include "StaticMesh/StaticMeshBuildTestSupport.h"
#include "StaticMesh/StaticMeshTestEnvironment.h"

using namespace StaticMeshBuildTestSupport;

TEST(FStaticMeshBuildQualificationTests, RepresentativeGeometry)
{
	CheckSourceResidency(true);
}

TEST(FStaticMeshBuildQualificationTests, RepresentativeDetachedBuildBudgets)
{
	CheckDetachedBuildBudgets(true);
}

TEST(FStaticMeshBuildQualificationTests, MeasuresRenderBuildAndPublicationSeparately)
{
	CheckRenderPublicationAndCancellation(true);
}

TEST(FStaticMeshBuildQualificationTests, ConcurrentLargeRenderBuildsSeparateCostsAndRetainCancelledBytes)
{
	CheckConcurrentRenderBuilds(true);
}
