#include <gtest/gtest.h>

#include "Animation/SkeletalAnimation.h"
#include "Components/SkeletalMeshComponent.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "Math/Operations.h"
#include "Materials/Material.h"

namespace
{
	auto NextObjectName(std::string_view Prefix) -> Durin::FName
	{
		static Durin::uint32 Counter = 0;
		return Durin::FName(std::format("{}{}", Prefix, ++Counter));
	}

	auto MatrixTransform(const Durin::FMatrix& Matrix) -> Durin::FSkeletonTransform
	{
		Durin::FSkeletonTransform Result;
		Durin::FVector4* Rows[] = {&Result.Row0, &Result.Row1, &Result.Row2, &Result.Row3};
		for (Durin::uint32 Row = 0; Row < 4; ++Row)
			for (Durin::uint32 Column = 0; Column < 4; ++Column)
				(*Rows[Row])[Column] = Matrix[Column][Row];
		Result.CanonicalizeFloat32();
		return Result;
	}

	auto FloatMatrix(const Durin::FMatrix& Matrix) -> Durin::FMatrix4f
	{
		Durin::FMatrix4f Result(0.0f);
		for (Durin::uint32 Column = 0; Column < 4; ++Column)
			for (Durin::uint32 Row = 0; Row < 4; ++Row)
				Result[Column][Row] = static_cast<float>(Matrix[Column][Row]);
		return Result;
	}

	auto MakeSkeleton(std::vector<Durin::FSkeletonBone> Bones) -> Durin::DSkeleton*
	{
		auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, NextObjectName("PoseSkeleton"));
		std::string Error;
		EXPECT_TRUE(Skeleton->InitializeCanonicalBones(std::move(Bones), Error)) << Error;
		return Skeleton;
	}

	auto MakeMesh(
		Durin::DSkeleton& Skeleton,
		std::vector<Durin::uint16> Palette,
		std::vector<Durin::FMatrix4f> InverseBinds,
		const Durin::FMatrix& MeshBind = Durin::FMatrix(1.0)) -> Durin::DSkeletalMesh*
	{
		auto Payload = std::make_shared<Durin::FSkeletalMeshPayloadData>();
		Payload->Positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
		Payload->Normals = std::vector<Durin::FVector3f>(3, {0.0f, 0.0f, 1.0f});
		Payload->Tangents = std::vector<Durin::FVector4f>(3, {1.0f, 0.0f, 0.0f, 1.0f});
		Payload->UVChannels[0] = std::vector<Durin::FVector2f>(3, {0.0f, 0.0f});
		Payload->Colors = std::vector<Durin::FVector4f>(3, Durin::FVector4f(1.0f));
		Payload->Indices = {0, 1, 2};
		Payload->Influences.resize(3);
		for (auto& Influence : Payload->Influences)
		{
			Influence.BoneIndices[0] = 0;
			Influence.Weights[0] = 1.0f;
			Influence.Count = 1;
		}
		Payload->LocalBounds = Durin::FBox({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f});
		Payload->Sections.push_back({
			.Name = Durin::FName("Body"),
			.FirstIndex = 0,
			.IndexCount = 3,
			.MinVertexIndex = 0,
			.MaxVertexIndex = 2,
			.MaterialSlotIndex = 0,
			.LocalBounds = Payload->LocalBounds});
		Payload->PaletteBoneIndices = std::move(Palette);
		Payload->InverseBindMatrices = std::move(InverseBinds);

		auto* Mesh = Durin::NewObject<Durin::DSkeletalMesh>(nullptr, NextObjectName("PoseMesh"));
		std::string Error;
		EXPECT_TRUE(Mesh->InitializeFromImportedData({
			.Skeleton = &Skeleton,
			.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
			.MeshNodeBindTransform = MatrixTransform(MeshBind),
			.MaterialSlots = {{.Name = Durin::FName("Body"), .SourceMaterialIndex = 0}},
			.Payload = std::move(Payload)}, Error)) << Error;
		return Mesh;
	}

	auto MakeClip(
		Durin::DSkeleton& Skeleton,
		std::vector<Durin::FAnimationTrackData> Tracks,
		float Duration = 2.0f) -> Durin::DAnimationClip*
	{
		auto Payload = std::make_shared<Durin::FAnimationClipPayloadData>();
		Payload->DurationSeconds = Duration;
		Payload->Tracks = std::move(Tracks);
		auto* Clip = Durin::NewObject<Durin::DAnimationClip>(nullptr, NextObjectName("PoseClip"));
		std::string Error;
		EXPECT_TRUE(Clip->InitializeFromImportedData({
			.Skeleton = &Skeleton,
			.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
			.ClipName = Durin::FName("Contract"),
			.Payload = std::move(Payload)}, Error)) << Error;
		return Clip;
	}

	auto ExpectMatrixNear(
		const Durin::FMatrix4f& Expected,
		const Durin::FMatrix4f& Actual,
		float Tolerance = 1.0e-5f) -> void
	{
		for (Durin::uint32 Column = 0; Column < 4; ++Column)
			for (Durin::uint32 Row = 0; Row < 4; ++Row)
				EXPECT_NEAR(Expected[Column][Row], Actual[Column][Row], Tolerance);
	}

	auto MakeLinearTranslationClip(Durin::DSkeleton& Skeleton) -> Durin::DAnimationClip*
	{
		return MakeClip(Skeleton, {{
			.BoneIndex = 0,
			.Path = Durin::EAnimationTrackPath::Translation,
			.Interpolation = Durin::EAnimationInterpolation::Linear,
			.Times = {0.0f, 2.0f},
			.VectorValues = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}}}});
	}
}

TEST(FSkeletalPoseEvaluatorTests, BuildsReferencePoseInPaletteOrderAndMeshSpace)
{
	Durin::FTransform Parent;
	Parent.Translation = {1.0, 0.0, 0.0};
	Parent.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
		90.0, Durin::FVector3(0.0, 0.0, 1.0));
	Parent.Scale3D = {2.0, 1.0, 1.0};
	Durin::FTransform Child;
	Child.Translation = {1.0, 0.0, 0.0};
	auto* Skeleton = MakeSkeleton({
		{.Name = Durin::FName("Root"), .ParentIndex = -1,
			.ReferenceTransform = MatrixTransform(Parent.ToMatrix())},
		{.Name = Durin::FName("Child"), .ParentIndex = 0,
			.ReferenceTransform = MatrixTransform(Child.ToMatrix())}});
	auto* Mesh = MakeMesh(*Skeleton, {1, 0}, {Durin::FMatrix4f(1.0f), Durin::FMatrix4f(1.0f)});

	Durin::FSkeletalAnimationBinding Binding;
	std::string Error;
	ASSERT_TRUE(Durin::BuildSkeletalAnimationBinding(*Mesh, nullptr, Binding, Error)) << Error;
	std::shared_ptr<const Durin::FSkeletalPosePalette> Candidate;
	ASSERT_TRUE(Durin::EvaluateSkeletalPose(Binding, 0.0f, 1, Candidate, Error)) << Error;
	ASSERT_NE(Candidate, nullptr);
	ASSERT_EQ(Candidate->Matrices.size(), 2u);
	EXPECT_NEAR(Candidate->Matrices[0][3][0], 1.0f, 1.0e-5f);
	EXPECT_NEAR(Candidate->Matrices[0][3][1], 2.0f, 1.0e-5f);
	ExpectMatrixNear(FloatMatrix(Parent.ToMatrix()), Candidate->Matrices[1]);
}

TEST(FSkeletalPoseEvaluatorTests, SamplesStepLinearMixedChannelsAndExactKeys)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"),
		.ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeClip(*Skeleton, {
		{.BoneIndex = 0, .Path = Durin::EAnimationTrackPath::Translation,
			.Interpolation = Durin::EAnimationInterpolation::Linear,
			.Times = {0.0f, 2.0f},
			.VectorValues = {{0.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}}},
		{.BoneIndex = 0, .Path = Durin::EAnimationTrackPath::Scale,
			.Interpolation = Durin::EAnimationInterpolation::Linear,
			.Times = {0.0f, 2.0f},
			.VectorValues = {{1.0f, 1.0f, 1.0f}, {3.0f, 1.0f, 1.0f}}},
		{.BoneIndex = 0, .Path = Durin::EAnimationTrackPath::Rotation,
			.Interpolation = Durin::EAnimationInterpolation::Step,
			.Times = {0.0f, 1.0f, 2.0f},
			.RotationValues = {{0.0f, 0.0f, 0.0f, 1.0f},
				{0.0f, 0.0f, 0.7071068f, 0.7071068f}, {0.0f, 0.0f, 1.0f, 0.0f}}}});

	Durin::FSkeletalAnimationBinding Binding;
	std::string Error;
	ASSERT_TRUE(Durin::BuildSkeletalAnimationBinding(*Mesh, Clip, Binding, Error)) << Error;
	std::shared_ptr<const Durin::FSkeletalPosePalette> Candidate;
	ASSERT_TRUE(Durin::EvaluateSkeletalPose(Binding, 0.5f, 4, Candidate, Error)) << Error;
	EXPECT_NEAR(Candidate->Matrices[0][3][0], 1.0f, 1.0e-5f);
	EXPECT_NEAR(Candidate->Matrices[0][0][0], 1.5f, 1.0e-5f);
	ASSERT_TRUE(Durin::EvaluateSkeletalPose(Binding, 1.0f, 5, Candidate, Error)) << Error;
	EXPECT_NEAR(Candidate->Matrices[0][3][0], 2.0f, 1.0e-5f);
	EXPECT_NEAR(Candidate->Matrices[0][0][0], 0.0f, 1.0e-5f);
	EXPECT_NEAR(Candidate->Matrices[0][0][1], 2.0f, 1.0e-5f);
	ASSERT_TRUE(Durin::EvaluateSkeletalPose(Binding, 2.0f, 6, Candidate, Error)) << Error;
	EXPECT_NEAR(Candidate->Matrices[0][3][0], 4.0f, 1.0e-5f);
	EXPECT_NEAR(Candidate->Matrices[0][0][0], -3.0f, 1.0e-5f);
}

TEST(FSkeletalPoseEvaluatorTests, NormalizesQuaternionAndUsesFrozenPaletteFormula)
{
	Durin::FTransform BoneReference;
	BoneReference.Translation = {12.0, 0.0, 0.0};
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(BoneReference.ToMatrix())}});
	Durin::FTransform MeshBind;
	MeshBind.Translation = {10.0, 0.0, 0.0};
	Durin::FTransform InverseBind;
	InverseBind.Translation = {-2.0, 0.0, 0.0};
	auto* Mesh = MakeMesh(*Skeleton, {0}, {FloatMatrix(InverseBind.ToMatrix())}, MeshBind.ToMatrix());
	auto* Clip = MakeClip(*Skeleton, {
		{.BoneIndex = 0, .Path = Durin::EAnimationTrackPath::Translation,
			.Interpolation = Durin::EAnimationInterpolation::Linear,
			.Times = {0.0f, 2.0f},
			.VectorValues = {{12.0f, 0.0f, 0.0f}, {14.0f, 0.0f, 0.0f}}},
		{.BoneIndex = 0, .Path = Durin::EAnimationTrackPath::Rotation,
			.Interpolation = Durin::EAnimationInterpolation::Linear,
			.Times = {0.0f, 2.0f},
			.RotationValues = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}}}});

	Durin::FSkeletalAnimationBinding Binding;
	std::string Error;
	ASSERT_TRUE(Durin::BuildSkeletalAnimationBinding(*Mesh, Clip, Binding, Error)) << Error;
	std::shared_ptr<const Durin::FSkeletalPosePalette> Candidate;
	ASSERT_TRUE(Durin::EvaluateSkeletalPose(Binding, 0.0f, 1, Candidate, Error)) << Error;
	ExpectMatrixNear(Durin::FMatrix4f(1.0f), Candidate->Matrices[0]);
	ASSERT_TRUE(Durin::EvaluateSkeletalPose(Binding, 1.0f, 2, Candidate, Error)) << Error;
	EXPECT_NEAR(Candidate->Matrices[0][3][0], 1.0f, 1.0e-5f);
	EXPECT_NEAR(Candidate->Matrices[0][0][0], 1.0f, 1.0e-5f);
}

TEST(FSkeletalPoseEvaluatorTests, RejectsInvalidBindingWithoutReplacingCandidate)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	Durin::FSkeletalAnimationBinding Binding;
	std::string Error;
	ASSERT_TRUE(Durin::BuildSkeletalAnimationBinding(*Mesh, nullptr, Binding, Error)) << Error;
	std::shared_ptr<const Durin::FSkeletalPosePalette> Candidate;
	ASSERT_TRUE(Durin::EvaluateSkeletalPose(Binding, 0.0f, 1, Candidate, Error)) << Error;
	const auto Previous = Candidate;
	Binding.PaletteBoneIndices[0] = 42;
	EXPECT_FALSE(Durin::EvaluateSkeletalPose(Binding, 0.0f, 2, Candidate, Error));
	EXPECT_EQ(Candidate, Previous);
}

TEST(FSkeletalPoseEvaluatorTests, RejectsIncompatibleClipWithoutReplacingBinding)
{
	auto* MeshSkeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	Durin::FTransform AlternateReference;
	AlternateReference.Translation = {1.0, 0.0, 0.0};
	auto* ClipSkeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(AlternateReference.ToMatrix())}});
	auto* Mesh = MakeMesh(*MeshSkeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeClip(*ClipSkeleton, {{
		.BoneIndex = 0,
		.Path = Durin::EAnimationTrackPath::Translation,
		.Interpolation = Durin::EAnimationInterpolation::Linear,
		.Times = {0.0f, 2.0f},
		.VectorValues = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}}});

	Durin::FSkeletalAnimationBinding Binding;
	Binding.SkeletonCompatibilityIdentity = "retained";
	std::string Error;
	EXPECT_FALSE(Durin::BuildSkeletalAnimationBinding(*Mesh, Clip, Binding, Error));
	EXPECT_EQ(Binding.SkeletonCompatibilityIdentity, "retained");
	EXPECT_NE(Error.find("incompatible"), std::string::npos) << Error;
}

TEST(FSkeletalAnimationInstanceTests, AdvancesPausesAndPublishesOnlyTimeChanges)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeLinearTranslationClip(*Skeleton);
	Durin::FSkeletalAnimationInstance Instance;
	std::string Error;
	ASSERT_TRUE(Instance.Bind(*Mesh, Clip, Error)) << Error;
	ASSERT_TRUE(Instance.IsBound());
	ASSERT_FALSE(Instance.IsPlaying());
	ASSERT_EQ(Instance.GetRevision(), 1u);
	const auto Initial = Instance.GetLatestPosePalette();

	ASSERT_TRUE(Instance.Play(Error)) << Error;
	ASSERT_TRUE(Instance.Tick(0.5f, Error)) << Error;
	EXPECT_FLOAT_EQ(Instance.GetTimeSeconds(), 0.5f);
	EXPECT_EQ(Instance.GetRevision(), 2u);
	EXPECT_NEAR(Instance.GetLatestPosePalette()->Matrices[0][3][0], 0.5f, 1.0e-5f);
	EXPECT_NE(Instance.GetLatestPosePalette(), Initial);

	Instance.Pause();
	const auto Paused = Instance.GetLatestPosePalette();
	ASSERT_TRUE(Instance.Tick(1.0f, Error)) << Error;
	EXPECT_EQ(Instance.GetLatestPosePalette(), Paused);
	EXPECT_EQ(Instance.GetRevision(), 2u);
	ASSERT_TRUE(Instance.Tick(0.0f, Error)) << Error;
	EXPECT_EQ(Instance.GetLatestPosePalette(), Paused);
}

TEST(FSkeletalAnimationInstanceTests, WrapsLargeDeltasAndExactLoopBoundaries)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeLinearTranslationClip(*Skeleton);
	Durin::FSkeletalAnimationInstance Instance;
	std::string Error;
	ASSERT_TRUE(Instance.Bind(*Mesh, Clip, Error)) << Error;
	ASSERT_TRUE(Instance.Seek(1.0f, Error)) << Error;
	ASSERT_TRUE(Instance.Play(Error)) << Error;
	ASSERT_TRUE(Instance.Tick(5.0f, Error)) << Error;
	EXPECT_FLOAT_EQ(Instance.GetTimeSeconds(), 0.0f);
	EXPECT_TRUE(Instance.IsPlaying());
	EXPECT_NEAR(Instance.GetLatestPosePalette()->Matrices[0][3][0], 0.0f, 1.0e-5f);
	ASSERT_TRUE(Instance.Tick(1'000'001.0f, Error)) << Error;
	EXPECT_FLOAT_EQ(Instance.GetTimeSeconds(), 1.0f);
}

TEST(FSkeletalAnimationInstanceTests, ClampsNonLoopingPlaybackAndStopsAtTerminalPose)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeLinearTranslationClip(*Skeleton);
	Durin::FSkeletalAnimationInstance Instance;
	Instance.SetLooping(false);
	std::string Error;
	ASSERT_TRUE(Instance.Bind(*Mesh, Clip, Error)) << Error;
	ASSERT_TRUE(Instance.Seek(1.5f, Error)) << Error;
	ASSERT_TRUE(Instance.Play(Error)) << Error;
	ASSERT_TRUE(Instance.Tick(10.0f, Error)) << Error;
	EXPECT_FLOAT_EQ(Instance.GetTimeSeconds(), 2.0f);
	EXPECT_FALSE(Instance.IsPlaying());
	EXPECT_NEAR(Instance.GetLatestPosePalette()->Matrices[0][3][0], 2.0f, 1.0e-5f);
	const auto Terminal = Instance.GetLatestPosePalette();
	ASSERT_TRUE(Instance.Tick(1.0f, Error)) << Error;
	EXPECT_EQ(Instance.GetLatestPosePalette(), Terminal);
}

TEST(FSkeletalAnimationInstanceTests, AppliesRateSeekAndStopControlContract)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeLinearTranslationClip(*Skeleton);
	Durin::FSkeletalAnimationInstance Instance;
	std::string Error;
	ASSERT_TRUE(Instance.Bind(*Mesh, Clip, Error)) << Error;
	ASSERT_TRUE(Instance.SetPlayRate(0.0f, Error)) << Error;
	ASSERT_TRUE(Instance.Play(Error)) << Error;
	const auto ZeroRate = Instance.GetLatestPosePalette();
	ASSERT_TRUE(Instance.Tick(1.0f, Error)) << Error;
	EXPECT_EQ(Instance.GetLatestPosePalette(), ZeroRate);
	ASSERT_TRUE(Instance.SetPlayRate(2.0f, Error)) << Error;
	ASSERT_TRUE(Instance.Tick(0.25f, Error)) << Error;
	EXPECT_FLOAT_EQ(Instance.GetTimeSeconds(), 0.5f);
	ASSERT_TRUE(Instance.Seek(-0.5f, Error)) << Error;
	EXPECT_FLOAT_EQ(Instance.GetTimeSeconds(), 1.5f);
	ASSERT_TRUE(Instance.Stop(Error)) << Error;
	EXPECT_FLOAT_EQ(Instance.GetTimeSeconds(), 0.0f);
	EXPECT_FALSE(Instance.IsPlaying());
	EXPECT_NEAR(Instance.GetLatestPosePalette()->Matrices[0][3][0], 0.0f, 1.0e-5f);
	const auto Stopped = Instance.GetLatestPosePalette();
	ASSERT_TRUE(Instance.Stop(Error)) << Error;
	EXPECT_EQ(Instance.GetLatestPosePalette(), Stopped);
	EXPECT_FALSE(Instance.SetPlayRate(-1.0f, Error));
	EXPECT_FLOAT_EQ(Instance.GetPlayRate(), 2.0f);
}

TEST(FSkeletalAnimationInstanceTests, RetainsCoherentStateAfterInvalidTickAndRebind)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	Durin::FTransform AlternateReference;
	AlternateReference.Translation = {3.0, 0.0, 0.0};
	auto* OtherSkeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(AlternateReference.ToMatrix())}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeLinearTranslationClip(*Skeleton);
	auto* IncompatibleClip = MakeLinearTranslationClip(*OtherSkeleton);
	Durin::FSkeletalAnimationInstance Instance;
	std::string Error;
	ASSERT_TRUE(Instance.Bind(*Mesh, Clip, Error)) << Error;
	ASSERT_TRUE(Instance.Play(Error)) << Error;
	ASSERT_TRUE(Instance.Tick(0.5f, Error)) << Error;
	const float PreviousTime = Instance.GetTimeSeconds();
	const Durin::uint64 PreviousRevision = Instance.GetRevision();
	const auto PreviousCandidate = Instance.GetLatestPosePalette();

	EXPECT_FALSE(Instance.Tick(std::numeric_limits<float>::quiet_NaN(), Error));
	EXPECT_FALSE(Instance.Bind(*Mesh, IncompatibleClip, Error));
	EXPECT_FLOAT_EQ(Instance.GetTimeSeconds(), PreviousTime);
	EXPECT_EQ(Instance.GetRevision(), PreviousRevision);
	EXPECT_EQ(Instance.GetLatestPosePalette(), PreviousCandidate);
	EXPECT_TRUE(Instance.IsPlaying());
}

TEST(FSkeletalAnimationInstanceTests, RetainedCandidatesSurviveLaterRevisionsAndUnbind)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeLinearTranslationClip(*Skeleton);
	Durin::FSkeletalAnimationInstance Instance;
	std::string Error;
	ASSERT_TRUE(Instance.Bind(*Mesh, Clip, Error)) << Error;
	const auto First = Instance.GetLatestPosePalette();
	ASSERT_TRUE(Instance.Seek(1.0f, Error)) << Error;
	const auto Second = Instance.GetLatestPosePalette();
	ASSERT_NE(First, Second);
	EXPECT_EQ(First->Revision, 1u);
	EXPECT_NEAR(First->Matrices[0][3][0], 0.0f, 1.0e-5f);
	EXPECT_EQ(Second->Revision, 2u);
	EXPECT_NEAR(Second->Matrices[0][3][0], 1.0f, 1.0e-5f);
	Instance.Unbind();
	EXPECT_FALSE(Instance.IsBound());
	EXPECT_EQ(Instance.GetRevision(), 0u);
	EXPECT_EQ(Instance.GetLatestPosePalette(), nullptr);
	EXPECT_EQ(First->Revision, 1u);
	EXPECT_EQ(Second->Revision, 2u);
}

TEST(FSkeletalAnimationInstanceTests, DetachedBindingSurvivesSourcePayloadReplacement)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeLinearTranslationClip(*Skeleton);
	Durin::FSkeletalAnimationInstance Instance;
	std::string Error;
	ASSERT_TRUE(Instance.Bind(*Mesh, Clip, Error)) << Error;

	Durin::FTransform ReplacementInverseBind;
	ReplacementInverseBind.Translation = {10.0, 0.0, 0.0};
	auto* ReplacementMesh = MakeMesh(
		*Skeleton, {0}, {FloatMatrix(ReplacementInverseBind.ToMatrix())});
	auto* ReplacementClip = MakeClip(*Skeleton, {{
		.BoneIndex = 0,
		.Path = Durin::EAnimationTrackPath::Translation,
		.Interpolation = Durin::EAnimationInterpolation::Linear,
		.Times = {0.0f, 2.0f},
		.VectorValues = {{0.0f, 0.0f, 0.0f}, {200.0f, 0.0f, 0.0f}}}});
	auto MeshExchange = Mesh->PrepareImportedStateExchange(*ReplacementMesh, Error);
	ASSERT_NE(MeshExchange, nullptr) << Error;
	auto ClipExchange = Clip->PrepareImportedStateExchange(*ReplacementClip, Error);
	ASSERT_NE(ClipExchange, nullptr) << Error;
	MeshExchange->Commit();
	ClipExchange->Commit();
	MeshExchange->Finalize();
	ClipExchange->Finalize();

	ASSERT_TRUE(Instance.Seek(1.0f, Error)) << Error;
	EXPECT_NEAR(Instance.GetLatestPosePalette()->Matrices[0][3][0], 1.0f, 1.0e-5f);
}

TEST(DSkeletalMeshComponentTests, RoutesRegistrationPlayTickProxyAndTeardown)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeLinearTranslationClip(*Skeleton);
	auto* Component = Durin::NewObject<Durin::DSkeletalMeshComponent>(
		nullptr, NextObjectName("SkeletalComponent"));
	std::string Error;
	EXPECT_FALSE(Component->SetAnimationClip(Clip, Error));
	ASSERT_TRUE(Component->SetSkeletalMesh(Mesh, Error)) << Error;
	ASSERT_TRUE(Component->SetAnimationClip(Clip, Error)) << Error;
	EXPECT_EQ(Component->GetLatestPosePalette(), nullptr);

	Component->RegisterComponent();
	ASSERT_TRUE(Component->IsRegistered());
	ASSERT_NE(Component->GetLatestPosePalette(), nullptr);
	EXPECT_EQ(Component->GetPlaybackRevision(), 1u);
	std::unique_ptr<Durin::FPrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	ASSERT_NE(Proxy, nullptr);
	EXPECT_EQ(Proxy->GetKind(), Durin::EPrimitiveSceneProxyKind::SkeletalMesh);
	auto& SkeletalProxy = static_cast<Durin::FSkeletalMeshSceneProxy&>(*Proxy);
	EXPECT_EQ(SkeletalProxy.GetPose()->Revision, 1u);
	EXPECT_TRUE(SkeletalProxy.GetLocalBounds().bIsValid);
	Component->DispatchBeginPlay();
	EXPECT_TRUE(Component->IsPlaying());
	Component->TickComponent(0.5f);
	EXPECT_FLOAT_EQ(Component->GetPlaybackTimeSeconds(), 0.5f);
	const auto LiveCandidate = Component->GetLatestPosePalette();
	ASSERT_NE(LiveCandidate, nullptr);

	Component->RouteEndPlay();
	EXPECT_FALSE(Component->HasBegunPlay());
	EXPECT_FALSE(Component->IsPlaying());
	EXPECT_EQ(Component->GetLatestPosePalette(), nullptr);
	EXPECT_NEAR(LiveCandidate->Matrices[0][3][0], 0.5f, 1.0e-5f);
	Component->DispatchBeginPlay();
	EXPECT_TRUE(Component->IsPlaying());
	EXPECT_FLOAT_EQ(Component->GetPlaybackTimeSeconds(), 0.0f);
	Component->UnregisterComponent();
	EXPECT_FALSE(Component->IsRegistered());
	EXPECT_EQ(Component->GetLatestPosePalette(), nullptr);
}

TEST(DSkeletalMeshComponentTests, ResolvesMaterialOverridesByIndexAndName)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Component = Durin::NewObject<Durin::DSkeletalMeshComponent>(
		nullptr, NextObjectName("MaterialSkeletalComponent"));
	auto* Material = Durin::NewObject<Durin::DMaterial>(
		nullptr, NextObjectName("SkeletalMaterial"));
	std::string Error;
	ASSERT_TRUE(Component->SetSkeletalMesh(Mesh, Error)) << Error;
	EXPECT_EQ(Component->GetNumMaterials(), 1u);
	EXPECT_TRUE(Component->SetMaterial(0, Material));
	EXPECT_EQ(Component->GetMaterial(0), Material);
	EXPECT_EQ(Component->GetMaterialByName(Durin::FName("Body")), Material);
	EXPECT_TRUE(Component->SetMaterialByName(Durin::FName("Body"), Material));
	EXPECT_FALSE(Component->SetMaterialByName(Durin::FName("Missing"), Material));
	EXPECT_TRUE(Component->ResetMaterial(0));
	EXPECT_EQ(Component->GetMaterial(0), nullptr);
}

TEST(DSkeletalMeshComponentTests, RejectsProspectiveChangesWithoutDestroyingPlayback)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	Durin::FTransform AlternateReference;
	AlternateReference.Translation = {1.0, 0.0, 0.0};
	auto* OtherSkeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(AlternateReference.ToMatrix())}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeLinearTranslationClip(*Skeleton);
	auto* IncompatibleClip = MakeLinearTranslationClip(*OtherSkeleton);
	auto* Component = Durin::NewObject<Durin::DSkeletalMeshComponent>(
		nullptr, NextObjectName("SkeletalComponent"));
	std::string Error;
	ASSERT_TRUE(Component->SetSkeletalMesh(Mesh, Error)) << Error;
	ASSERT_TRUE(Component->SetAnimationClip(Clip, Error)) << Error;
	Component->RegisterComponent();
	Component->DispatchBeginPlay();
	Component->TickComponent(0.5f);
	const auto Previous = Component->GetLatestPosePalette();
	const Durin::uint64 Revision = Component->GetPlaybackRevision();

	EXPECT_FALSE(Component->SetAnimationClip(IncompatibleClip, Error));
	EXPECT_FALSE(Component->SetSkeletalMesh(nullptr, Error));
	EXPECT_FALSE(Component->SetPlayRate(-1.0f, Error));
	EXPECT_EQ(Component->GetAnimationClip(), Clip);
	EXPECT_EQ(Component->GetSkeletalMesh(), Mesh);
	EXPECT_EQ(Component->GetLatestPosePalette(), Previous);
	EXPECT_EQ(Component->GetPlaybackRevision(), Revision);
	EXPECT_FLOAT_EQ(Component->GetPlaybackTimeSeconds(), 0.5f);
	EXPECT_TRUE(Component->IsPlaying());
	Component->UnregisterComponent();
}

TEST(DSkeletalMeshComponentTests, FollowsWorldPauseSingleStepAndReplacementLifecycle)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Clip = MakeLinearTranslationClip(*Skeleton);
	auto* World = Durin::NewObject<Durin::DWorld>(nullptr, NextObjectName("SkeletalWorld"));
	auto* Level = Durin::NewObject<Durin::DLevel>(World, NextObjectName("SkeletalLevel"));
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = World->SpawnActor<Durin::AActor>(NextObjectName("SkeletalActor"));
	ASSERT_NE(Actor, nullptr);
	Actor->SetActorTickEnabled(true);
	auto* Component = Durin::Cast<Durin::DSkeletalMeshComponent>(
		Actor->AddInstanceComponent(
			Durin::DSkeletalMeshComponent::StaticClass(), NextObjectName("SkeletalComponent")));
	ASSERT_NE(Component, nullptr);
	std::string Error;
	ASSERT_TRUE(Component->SetSkeletalMesh(Mesh, Error)) << Error;
	ASSERT_TRUE(Component->SetAnimationClip(Clip, Error)) << Error;
	World->BeginPlay();
	ASSERT_TRUE(Component->IsPlaying());
	World->Tick(0.25f);
	EXPECT_FLOAT_EQ(Component->GetPlaybackTimeSeconds(), 0.25f);

	World->SetPaused(true);
	World->Tick(0.5f);
	EXPECT_FLOAT_EQ(Component->GetPlaybackTimeSeconds(), 0.25f);
	World->RequestSingleStep();
	World->Tick(0.5f);
	EXPECT_FLOAT_EQ(Component->GetPlaybackTimeSeconds(), 0.75f);
	Component->SetComponentTickEnabled(false);
	World->RequestSingleStep();
	World->Tick(0.5f);
	EXPECT_FLOAT_EQ(Component->GetPlaybackTimeSeconds(), 0.75f);

	const auto Retained = Component->GetLatestPosePalette();
	auto* Replacement = Durin::NewObject<Durin::DLevel>(
		World, NextObjectName("ReplacementLevel"));
	ASSERT_TRUE(World->SetCurrentLevel(Replacement));
	EXPECT_FALSE(Component->IsRegistered());
	EXPECT_FALSE(Component->HasBegunPlay());
	EXPECT_EQ(Component->GetLatestPosePalette(), nullptr);
	ASSERT_NE(Retained, nullptr);
	EXPECT_NEAR(Retained->Matrices[0][3][0], 0.75f, 1.0e-5f);
}

TEST(DSkeletalMeshComponentTests, DestroyDetachesProducerButNotRetainedCandidate)
{
	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* Component = Durin::NewObject<Durin::DSkeletalMeshComponent>(
		nullptr, NextObjectName("SkeletalComponent"));
	std::string Error;
	ASSERT_TRUE(Component->SetSkeletalMesh(Mesh, Error)) << Error;
	Component->RegisterComponent();
	const auto Retained = Component->GetLatestPosePalette();
	ASSERT_NE(Retained, nullptr);
	Component->DestroyComponent();
	EXPECT_TRUE(Component->IsBeingDestroyed());
	EXPECT_FALSE(Component->IsRegistered());
	EXPECT_EQ(Component->GetLatestPosePalette(), nullptr);
	EXPECT_EQ(Retained->Revision, 1u);
}

TEST(DSkeletalMeshComponentTests, ReflectsFrozenPropertiesAndRejectsInvalidEditDrafts)
{
	InitializeDObjectSystem();
	Durin::DClass* Class = Durin::DSkeletalMeshComponent::StaticClass();
	ASSERT_NE(Class, nullptr);
	EXPECT_TRUE(Class->IsChildOf(Durin::DPrimitiveComponent::StaticClass()));
	for (const std::string_view Name : {
		"SkeletalMesh", "AnimationClip", "bAutoPlay", "bLooping", "PlayRate"})
	{
		EXPECT_NE(Class->FindPropertyByName(Durin::FName(Name)), nullptr) << Name;
	}

	auto* Component = Durin::NewObject<Durin::DSkeletalMeshComponent>(
		nullptr, NextObjectName("SkeletalComponent"));
	auto* Draft = Durin::NewObject<Durin::DSkeletalMeshComponent>(
		nullptr, NextObjectName("SkeletalDraft"));
	Durin::FProperty* PlayRateProperty = Class->FindPropertyByName("PlayRate");
	ASSERT_NE(PlayRateProperty, nullptr);
	*PlayRateProperty->ContainerPtrToValuePtr<float>(Draft) = -1.0f;
	Durin::FPropertyEditProposal Proposal{
		.MemberProperty = PlayRateProperty,
		.LeafProperty = PlayRateProperty,
		.DraftRootProperty = PlayRateProperty,
		.DraftRootContainer = Draft,
		.DraftLeafContainer = Draft};
	std::string Error;
	EXPECT_FALSE(Component->PreEditChangeProperty(Proposal, Error));
	EXPECT_NE(Error.find("non-negative"), std::string::npos) << Error;

	auto* Skeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))}});
	Durin::FTransform AlternateReference;
	AlternateReference.Translation = {1.0, 0.0, 0.0};
	auto* OtherSkeleton = MakeSkeleton({{
		.Name = Durin::FName("Root"), .ParentIndex = -1,
		.ReferenceTransform = MatrixTransform(AlternateReference.ToMatrix())}});
	auto* Mesh = MakeMesh(*Skeleton, {0}, {Durin::FMatrix4f(1.0f)});
	auto* IncompatibleClip = MakeLinearTranslationClip(*OtherSkeleton);
	ASSERT_TRUE(Component->SetSkeletalMesh(Mesh, Error)) << Error;
	auto* ClipProperty = static_cast<Durin::FObjectProperty*>(
		Class->FindPropertyByName("AnimationClip"));
	ASSERT_NE(ClipProperty, nullptr);
	ClipProperty->SetObjectPropertyValue(Draft, IncompatibleClip);
	Proposal.MemberProperty = ClipProperty;
	Proposal.LeafProperty = ClipProperty;
	Proposal.DraftRootProperty = ClipProperty;
	EXPECT_FALSE(Component->PreEditChangeProperty(Proposal, Error));
	EXPECT_NE(Error.find("incompatible"), std::string::npos) << Error;
	EXPECT_EQ(Component->GetAnimationClip(), nullptr);
}
