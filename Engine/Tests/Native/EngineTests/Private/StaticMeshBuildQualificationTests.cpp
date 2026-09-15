#include "StaticMesh/StaticMeshBuildTestSupport.h"
#include "StaticMesh/StaticMeshTestEnvironment.h"

using namespace StaticMeshBuildTestSupport;

TEST(FStaticMeshBuildQualificationTests, RepresentativeGeometry)
{
	CheckSourceResidency(true);
}

TEST(FStaticMeshBuildQualificationTests, RepresentativeCandidateBudgets)
{
	CheckCandidateBudgets(true);
}

TEST(FStaticMeshBuildQualificationTests, MeasuresCompleteCandidateAndPublicationSeparately)
{
	CheckCandidatePublicationAndCancellation(true);
}

TEST(FStaticMeshBuildQualificationTests, ConcurrentLargeCandidatesSeparateCostsAndRetainCancelledBytes)
{
	CheckConcurrentCandidates(true);
}
