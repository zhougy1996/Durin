#include "GeometrySubmission.h"

#include <gtest/gtest.h>

namespace Durin
{
	TEST(FGeometrySubmissionTests, ChecksStreamBoundariesWithoutOverflow)
	{
		const FGeometryStreamRange Stream{64, 4, 16, 12};
		EXPECT_TRUE(Stream.Contains(0, 4));
		EXPECT_TRUE(Stream.Contains(4, 0));
		EXPECT_FALSE(Stream.Contains(4, 1));
		EXPECT_FALSE(Stream.Contains(std::numeric_limits<uint64>::max(), 2));
		EXPECT_FALSE(Stream.Contains(1, std::numeric_limits<uint64>::max()));
		EXPECT_FALSE((FGeometryStreamRange{64, 65, 16, 12}).Contains(0, 0));
		EXPECT_FALSE((FGeometryStreamRange{64, 0, 0, 12}).Contains(0, 1));
		EXPECT_FALSE((FGeometryStreamRange{64, 0, 8, 12}).Contains(0, 1));
		EXPECT_TRUE((FGeometryStreamRange{std::numeric_limits<uint64>::max(), 0, 1, 1})
			.Contains(std::numeric_limits<uint64>::max() - 1, 1));
	}

	TEST(FGeometrySubmissionTests, PreservesDirectAndInstanceOffsets)
	{
		const FGeometryStreamRange Vertices{120, 0, 12, 12};
		const std::array Instances{FGeometryStreamRange{96, 0, 16, 16}};
		FGeometryDrawRange Draw{
			.bIndexed = false, .ElementCount = 6, .FirstElement = 4,
			.InstanceCount = 3, .FirstInstance = 3};
		EXPECT_EQ(Draw.Validate(Vertices, {}, Instances), EGeometrySubmissionOutcome::Submitted);
		EXPECT_EQ(Draw.GetDrawArguments(), (FRHIDrawArguments{6, 3, 4, 3}));
		++Draw.FirstElement;
		EXPECT_EQ(Draw.Validate(Vertices, {}, Instances), EGeometrySubmissionOutcome::InvalidSubmission);
		--Draw.FirstElement;
		++Draw.FirstInstance;
		EXPECT_EQ(Draw.Validate(Vertices, {}, Instances), EGeometrySubmissionOutcome::InvalidSubmission);
	}

	TEST(FGeometrySubmissionTests, ChecksIndexFormatAndSignedBaseVertex)
	{
		const FGeometryStreamRange Vertices{36, 0, 12, 12};
		const FGeometryStreamRange Indices{12, 0, 2, 2};
		FGeometryDrawRange Draw{
			.ElementCount = 3, .FirstElement = 3, .VertexOffset = -7,
			.InstanceCount = 2, .FirstInstance = 5,
			.MinVertexIndex = 7, .MaxVertexIndex = 9};
		EXPECT_EQ(Draw.Validate(Vertices, Indices), EGeometrySubmissionOutcome::Submitted);
		EXPECT_EQ(Draw.GetIndexedDrawArguments(), (FRHIDrawIndexedArguments{3, 2, 3, -7, 5}));
		Draw.VertexOffset = -8;
		EXPECT_EQ(Draw.Validate(Vertices, Indices), EGeometrySubmissionOutcome::InvalidSubmission);
		Draw.VertexOffset = -6;
		EXPECT_EQ(Draw.Validate(Vertices, Indices), EGeometrySubmissionOutcome::InvalidSubmission);
		Draw.VertexOffset = -7;
		EXPECT_EQ(Draw.Validate(Vertices, {12, 1, 2, 2}), EGeometrySubmissionOutcome::InvalidSubmission);
		EXPECT_EQ(Draw.Validate(Vertices, {12, 0, 1, 1}), EGeometrySubmissionOutcome::InvalidSubmission);
		Draw.FirstElement = std::numeric_limits<uint32>::max();
		EXPECT_EQ(Draw.Validate(Vertices, Indices), EGeometrySubmissionOutcome::InvalidSubmission);
	}

	TEST(FGeometrySubmissionTests, DistinguishesEmptyInvalidAndUnsupported)
	{
		const FGeometryStreamRange Vertices{36, 0, 12, 12};
		FGeometryDrawRange Draw{.bIndexed = false};
		EXPECT_EQ(Draw.Validate({}, {}), EGeometrySubmissionOutcome::Empty);
		Draw.ElementCount = 2;
		EXPECT_EQ(Draw.Validate(Vertices, {}), EGeometrySubmissionOutcome::InvalidSubmission);
		Draw.Topology = EGeometryTopology::LineList;
		EXPECT_EQ(Draw.Validate(Vertices, {}), EGeometrySubmissionOutcome::Submitted);
		Draw.Topology = EGeometryTopology::Count;
		EXPECT_EQ(Draw.Validate(Vertices, {}), EGeometrySubmissionOutcome::Unsupported);
		Draw.Topology = EGeometryTopology::TriangleList;
		Draw.InstanceCount = 0;
		EXPECT_EQ(Draw.Validate({}, {}), EGeometrySubmissionOutcome::Empty);
	}
}
