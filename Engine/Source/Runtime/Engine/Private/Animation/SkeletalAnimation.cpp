#include "Animation/SkeletalAnimation.h"

#include "Math/Operations.h"
#include "Math/TransformDecomposition.h"
#include "SkeletalMesh/SkeletalMeshResources.h"

namespace Durin
{
	namespace
	{
		constexpr double ReferenceMatrixTolerance = 1.0e-5;

		auto ToDoubleMatrix(const FMatrix4f& Source) -> FMatrix
		{
			FMatrix Result(0.0);
			for (uint32 Column = 0; Column < 4; ++Column)
				for (uint32 Row = 0; Row < 4; ++Row)
					Result[Column][Row] = static_cast<double>(Source[Column][Row]);
			return Result;
		}

		auto TryToFloatMatrix(const FMatrix& Source, FMatrix4f& OutMatrix) -> bool
		{
			if (!Math::IsFinite(Source)) return false;
			FMatrix4f Candidate(0.0f);
			for (uint32 Column = 0; Column < 4; ++Column)
			{
				for (uint32 Row = 0; Row < 4; ++Row)
				{
					Candidate[Column][Row] = static_cast<float>(Source[Column][Row]);
					if (!std::isfinite(Candidate[Column][Row])) return false;
				}
			}
			OutMatrix = Candidate;
			return true;
		}

		auto IsNear(const FMatrix& Left, const FMatrix& Right) -> bool
		{
			for (uint32 Column = 0; Column < 4; ++Column)
				for (uint32 Row = 0; Row < 4; ++Row)
					if (std::abs(Left[Column][Row] - Right[Column][Row]) > ReferenceMatrixTolerance)
						return false;
			return true;
		}

		auto TryDecomposeReference(
			const FSkeletonTransform& Source,
			FTransform& OutTransform,
			std::string& OutError) -> bool
		{
			const FMatrix Matrix = ToDoubleMatrix(Source.ToMatrix4f());
			FTransform Candidate;
			if (!TryMakeTransformFromMatrix(Matrix, Candidate))
				return Fail("Skeleton reference transform decomposition failed.", &OutError);
			if (!Math::IsFinite(Candidate.Translation) || !Math::IsFinite(Candidate.Rotation)
				|| !Math::IsFinite(Candidate.Scale3D))
				return Fail("Skeleton reference transform decomposition is non-finite.", &OutError);
			if (std::abs(Candidate.Scale3D.x) <= kSmallNumber
				|| std::abs(Candidate.Scale3D.y) <= kSmallNumber
				|| std::abs(Candidate.Scale3D.z) <= kSmallNumber)
				return Fail("Skeleton reference transform scale is singular.", &OutError);
			FQuat NormalizedRotation;
			if (!Math::TryNormalize(Candidate.Rotation, NormalizedRotation))
				return Fail("Skeleton reference transform rotation is invalid.", &OutError);
			Candidate.Rotation = NormalizedRotation;
			if (!IsNear(Matrix, Candidate.ToMatrix()))
				return Fail("Skeleton reference transform contains unsupported shear or perspective.", &OutError);
			OutTransform = Candidate;
			return true;
		}

		auto SampleVectorTrack(const FAnimationTrackData& Track, float Time) -> FVector3
		{
			const auto Upper = std::ranges::upper_bound(Track.Times, Time);
			if (Upper == Track.Times.begin()) return FVector3(Track.VectorValues.front());
			if (Upper == Track.Times.end()) return FVector3(Track.VectorValues.back());
			const size_t Right = static_cast<size_t>(Upper - Track.Times.begin());
			const size_t Left = Right - 1;
			if (Track.Times[Left] == Time || Track.Interpolation == EAnimationInterpolation::Step)
				return FVector3(Track.VectorValues[Left]);
			const double Alpha = static_cast<double>(Time - Track.Times[Left])
				/ static_cast<double>(Track.Times[Right] - Track.Times[Left]);
			return Math::Lerp(
				FVector3(Track.VectorValues[Left]),
				FVector3(Track.VectorValues[Right]),
				Alpha);
		}

		auto ToQuaternion(const FVector4f& Value) -> FQuat
		{
			return FQuat(
				static_cast<double>(Value.w),
				static_cast<double>(Value.x),
				static_cast<double>(Value.y),
				static_cast<double>(Value.z));
		}

		auto NormalizedQuaternion(const FVector4f& Value) -> FQuat
		{
			FQuat Result = FQuatConstants::Identity;
			const bool bNormalized = Math::TryNormalize(ToQuaternion(Value), Result);
			check(bNormalized);
			(void)bNormalized;
			return Result;
		}

		auto SampleRotationTrack(const FAnimationTrackData& Track, float Time) -> FQuat
		{
			const auto Upper = std::ranges::upper_bound(Track.Times, Time);
			if (Upper == Track.Times.begin()) return NormalizedQuaternion(Track.RotationValues.front());
			if (Upper == Track.Times.end()) return NormalizedQuaternion(Track.RotationValues.back());
			const size_t Right = static_cast<size_t>(Upper - Track.Times.begin());
			const size_t Left = Right - 1;
			FQuat Start = NormalizedQuaternion(Track.RotationValues[Left]);
			if (Track.Times[Left] == Time || Track.Interpolation == EAnimationInterpolation::Step)
				return Start;
			FQuat End = NormalizedQuaternion(Track.RotationValues[Right]);
			double Dot = Math::Dot(Start, End);
			if (Dot < 0.0)
			{
				End = -End;
				Dot = -Dot;
			}
			const double Alpha = static_cast<double>(Time - Track.Times[Left])
				/ static_cast<double>(Track.Times[Right] - Track.Times[Left]);
			FQuat Result;
			if (Dot > 0.9995)
			{
				Result = Start + Alpha * (End - Start);
			}
			else
			{
				const double Theta = std::acos(std::clamp(Dot, -1.0, 1.0));
				const double SinTheta = std::sin(Theta);
				Result = (std::sin((1.0 - Alpha) * Theta) / SinTheta) * Start
					+ (std::sin(Alpha * Theta) / SinTheta) * End;
			}
			return Math::Normalize(Result);
		}

		auto ValidateBindingShape(const FSkeletalAnimationBinding& Binding, std::string& OutError) -> bool
		{
			const size_t BoneCount = Binding.ParentIndices.size();
			if (BoneCount == 0 || BoneCount > MaximumSkeletonBones
				|| Binding.ReferenceLocalTransforms.size() != BoneCount)
				return Fail("Skeletal animation binding has invalid reference-pose counts.", &OutError);
			if (Binding.PaletteBoneIndices.empty()
				|| Binding.PaletteBoneIndices.size() != Binding.InverseBindMatrices.size()
				|| Binding.PaletteBoneIndices.size() != Binding.InfluenceBounds.size()
				|| Binding.PaletteBoneIndices.size() > BoneCount
				|| static_cast<uint64>(Binding.PaletteBoneIndices.size()) * sizeof(FMatrix4f)
					> MaximumSkeletalPosePaletteBytes)
				return Fail("Skeletal animation binding has invalid palette counts.", &OutError);
			for (size_t Bone = 0; Bone < BoneCount; ++Bone)
			{
				const int32 Parent = Binding.ParentIndices[Bone];
				if (Parent < -1 || Parent >= static_cast<int32>(Bone))
					return Fail("Skeletal animation binding is not parent-before-child.", &OutError);
			}
			for (uint16 Bone : Binding.PaletteBoneIndices)
				if (Bone >= BoneCount)
					return Fail("Skeletal animation palette references an invalid bone.", &OutError);
			return true;
		}
	}

	auto BuildSkeletalAnimationBinding(
		const DSkeletalMesh& Mesh,
		const DAnimationClip* Clip,
		FSkeletalAnimationBinding& OutBinding,
		std::string& OutError) -> bool
	{
		std::string ValidationError;
		if (!Mesh.Validate(ValidationError))
			return Fail(std::format("Skeletal mesh is invalid: {}", ValidationError), &OutError);
		const DSkeleton* Skeleton = Mesh.GetSkeleton();
		const std::shared_ptr<const FSkeletalMeshPayloadData> MeshPayload = Mesh.GetPayloadData();
		if (!Skeleton || !MeshPayload)
			return Fail("Skeletal mesh has no runtime Skeleton or payload.", &OutError);

		FSkeletalAnimationBinding Candidate;
		Candidate.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity();
		Candidate.ParentIndices.reserve(Skeleton->GetBoneCount());
		Candidate.ReferenceLocalTransforms.reserve(Skeleton->GetBoneCount());
		for (const FSkeletonBone& Bone : Skeleton->GetBones())
		{
			FTransform Reference;
			if (!TryDecomposeReference(Bone.ReferenceTransform, Reference, OutError)) return false;
			Candidate.ParentIndices.push_back(Bone.ParentIndex);
			Candidate.ReferenceLocalTransforms.push_back(Reference);
		}

		if (Clip)
		{
			if (!Clip->Validate(ValidationError))
				return Fail(std::format("Animation clip is invalid: {}", ValidationError), &OutError);
			const DSkeleton* ClipSkeleton = Clip->GetSkeleton();
			if (!ClipSkeleton
				|| ClipSkeleton->GetCompatibilityIdentity() != Candidate.SkeletonCompatibilityIdentity
				|| Clip->GetSkeletonCompatibilityIdentity() != Candidate.SkeletonCompatibilityIdentity)
				return Fail("Animation clip is structurally incompatible with the skeletal mesh.", &OutError);
			Candidate.ClipPayload = Clip->GetPayloadData();
			if (!Candidate.ClipPayload)
				return Fail("Animation clip has no runtime payload.", &OutError);
		}

		Candidate.MeshNodeBindTransform = ToDoubleMatrix(Mesh.GetMeshNodeBindTransform().ToMatrix4f());
		if (!Math::TryInverse(Candidate.MeshNodeBindTransform, Candidate.InverseMeshNodeBindTransform))
			return Fail("Skeletal mesh bind transform is singular or non-finite.", &OutError);
		Candidate.PaletteBoneIndices = MeshPayload->PaletteBoneIndices;
		Candidate.InverseBindMatrices = MeshPayload->InverseBindMatrices;
		const FSkeletalMeshRenderData* RenderData = Mesh.GetRenderData();
		if (!RenderData)
			return Fail("Skeletal mesh has no render data for animated bounds.", &OutError);
		Candidate.InfluenceBounds = RenderData->InfluenceBounds;
		if (!ValidateBindingShape(Candidate, OutError)) return false;

		OutBinding = std::move(Candidate);
		OutError.clear();
		return true;
	}

	auto EvaluateSkeletalPose(
		const FSkeletalAnimationBinding& Binding,
		float SampleTimeSeconds,
		uint64 Revision,
		std::shared_ptr<const FSkeletalPosePalette>& OutCandidate,
		std::string& OutError) -> bool
	{
		if (!ValidateBindingShape(Binding, OutError)) return false;
		if (Revision == 0) return Fail("Skeletal pose revision must be non-zero.", &OutError);
		if (!std::isfinite(SampleTimeSeconds) || SampleTimeSeconds < 0.0f)
			return Fail("Skeletal pose sample time must be finite and non-negative.", &OutError);
		if (Binding.ClipPayload)
		{
			if (SampleTimeSeconds > Binding.ClipPayload->DurationSeconds)
				return Fail("Skeletal pose sample time exceeds clip duration.", &OutError);
		}
		else if (SampleTimeSeconds != 0.0f)
			return Fail("Reference-only skeletal bindings can only sample time zero.", &OutError);

		std::vector<FTransform> LocalTransforms = Binding.ReferenceLocalTransforms;
		if (Binding.ClipPayload)
		{
			for (const FAnimationTrackData& Track : Binding.ClipPayload->Tracks)
			{
				if (Track.BoneIndex >= LocalTransforms.size())
					return Fail("Animation track references an invalid bone.", &OutError);
				FTransform& Local = LocalTransforms[Track.BoneIndex];
				switch (Track.Path)
				{
				case EAnimationTrackPath::Translation:
					Local.Translation = SampleVectorTrack(Track, SampleTimeSeconds);
					break;
				case EAnimationTrackPath::Rotation:
					Local.Rotation = SampleRotationTrack(Track, SampleTimeSeconds);
					break;
				case EAnimationTrackPath::Scale:
					Local.Scale3D = SampleVectorTrack(Track, SampleTimeSeconds);
					break;
				}
			}
		}

		std::vector<FMatrix> ComponentMatrices(LocalTransforms.size(), FMatrix(1.0));
		for (size_t Bone = 0; Bone < LocalTransforms.size(); ++Bone)
		{
			const FMatrix Local = LocalTransforms[Bone].ToMatrix();
			ComponentMatrices[Bone] = Binding.ParentIndices[Bone] >= 0
				? ComponentMatrices[static_cast<size_t>(Binding.ParentIndices[Bone])] * Local
				: Local;
			if (!Math::IsFinite(ComponentMatrices[Bone]))
				return Fail("Skeletal component-space pose is non-finite.", &OutError);
		}

		auto Candidate = std::make_shared<FSkeletalPosePalette>();
		Candidate->Revision = Revision;
		Candidate->SampleTimeSeconds = SampleTimeSeconds;
		Candidate->SkeletonCompatibilityIdentity = Binding.SkeletonCompatibilityIdentity;
		Candidate->Matrices.resize(Binding.PaletteBoneIndices.size());
		for (size_t PaletteIndex = 0; PaletteIndex < Candidate->Matrices.size(); ++PaletteIndex)
		{
			const FMatrix PaletteMatrix = Binding.InverseMeshNodeBindTransform
				* ComponentMatrices[Binding.PaletteBoneIndices[PaletteIndex]]
				* ToDoubleMatrix(Binding.InverseBindMatrices[PaletteIndex]);
			if (!TryToFloatMatrix(PaletteMatrix, Candidate->Matrices[PaletteIndex]))
				return Fail("Skeletal pose palette contains a non-finite matrix.", &OutError);
			const FBox& InfluenceBound = Binding.InfluenceBounds[PaletteIndex];
			if (!InfluenceBound.bIsValid) continue;
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector3 Point(
					(Corner & 1u) ? InfluenceBound.Max.x : InfluenceBound.Min.x,
					(Corner & 2u) ? InfluenceBound.Max.y : InfluenceBound.Min.y,
					(Corner & 4u) ? InfluenceBound.Max.z : InfluenceBound.Min.z);
				const FVector3 Transformed(PaletteMatrix * FVector4(Point, 1.0));
				if (!Math::IsFinite(Transformed))
					return Fail("Skeletal pose bound contains a non-finite point.", &OutError);
				Candidate->LocalBounds.AddPoint(Transformed);
			}
		}
		if (!Candidate->LocalBounds.bIsValid)
			return Fail("Skeletal pose has no influenced local bound.", &OutError);

		OutCandidate = std::move(Candidate);
		OutError.clear();
		return true;
	}

	auto FSkeletalAnimationInstance::Bind(
		const DSkeletalMesh& Mesh,
		const DAnimationClip* Clip,
		std::string& OutError) -> bool
	{
		FSkeletalAnimationBinding CandidateBinding;
		if (!BuildSkeletalAnimationBinding(Mesh, Clip, CandidateBinding, OutError)) return false;
		if (Revision == std::numeric_limits<uint64>::max())
			return Fail("Skeletal animation revision overflowed.", &OutError);
		const uint64 CandidateRevision = Revision + 1;
		std::shared_ptr<const FSkeletalPosePalette> CandidatePose;
		if (!EvaluateSkeletalPose(CandidateBinding, 0.0f, CandidateRevision, CandidatePose, OutError))
			return false;

		Binding = std::move(CandidateBinding);
		TimeSeconds = 0.0f;
		Revision = CandidateRevision;
		bBound = true;
		bPlaying = false;
		std::atomic_store_explicit(&LatestCandidate, std::move(CandidatePose), std::memory_order_release);
		OutError.clear();
		return true;
	}

	auto FSkeletalAnimationInstance::Unbind() -> void
	{
		Binding = {};
		std::atomic_store_explicit(&LatestCandidate,
			std::shared_ptr<const FSkeletalPosePalette>{}, std::memory_order_release);
		TimeSeconds = 0.0f;
		Revision = 0;
		bBound = false;
		bPlaying = false;
	}

	auto FSkeletalAnimationInstance::Play(std::string& OutError) -> bool
	{
		if (!bBound) return Fail("Cannot play an unbound skeletal animation instance.", &OutError);
		if (!Binding.ClipPayload)
			return Fail("Cannot play a reference-only skeletal animation binding.", &OutError);
		bPlaying = true;
		OutError.clear();
		return true;
	}

	auto FSkeletalAnimationInstance::Pause() -> void
	{
		bPlaying = false;
	}

	auto FSkeletalAnimationInstance::EvaluateAt(float InTimeSeconds, std::string& OutError)
		-> std::shared_ptr<const FSkeletalPosePalette>
	{
		if (Revision == std::numeric_limits<uint64>::max())
		{
			Fail("Skeletal animation revision overflowed.", &OutError);
			return nullptr;
		}
		std::shared_ptr<const FSkeletalPosePalette> Candidate;
		if (!EvaluateSkeletalPose(Binding, InTimeSeconds, Revision + 1, Candidate, OutError))
			return nullptr;
		return Candidate;
	}

	auto FSkeletalAnimationInstance::Stop(std::string& OutError) -> bool
	{
		if (!bBound) return Fail("Cannot stop an unbound skeletal animation instance.", &OutError);
		if (TimeSeconds == 0.0f)
		{
			bPlaying = false;
			OutError.clear();
			return true;
		}
		std::shared_ptr<const FSkeletalPosePalette> Candidate = EvaluateAt(0.0f, OutError);
		if (!Candidate) return false;
		TimeSeconds = 0.0f;
		Revision = Candidate->Revision;
		bPlaying = false;
		std::atomic_store_explicit(&LatestCandidate, std::move(Candidate), std::memory_order_release);
		OutError.clear();
		return true;
	}

	auto FSkeletalAnimationInstance::NormalizeTime(double InTimeSeconds) const -> float
	{
		const double Duration = static_cast<double>(GetDurationSeconds());
		double Normalized = bLooping
			? std::fmod(InTimeSeconds, Duration)
			: std::clamp(InTimeSeconds, 0.0, Duration);
		if (Normalized < 0.0) Normalized += Duration;
		float Result = static_cast<float>(Normalized);
		if (bLooping && Result >= GetDurationSeconds()) Result = 0.0f;
		return Result == 0.0f ? 0.0f : Result;
	}

	auto FSkeletalAnimationInstance::Seek(float InTimeSeconds, std::string& OutError) -> bool
	{
		if (!bBound) return Fail("Cannot seek an unbound skeletal animation instance.", &OutError);
		if (!Binding.ClipPayload)
			return Fail("Cannot seek a reference-only skeletal animation binding.", &OutError);
		if (!std::isfinite(InTimeSeconds))
			return Fail("Skeletal animation seek time must be finite.", &OutError);
		const float CandidateTime = NormalizeTime(static_cast<double>(InTimeSeconds));
		if (CandidateTime == TimeSeconds)
		{
			OutError.clear();
			return true;
		}
		std::shared_ptr<const FSkeletalPosePalette> Candidate = EvaluateAt(CandidateTime, OutError);
		if (!Candidate) return false;
		TimeSeconds = CandidateTime;
		Revision = Candidate->Revision;
		if (!bLooping && TimeSeconds == GetDurationSeconds()) bPlaying = false;
		std::atomic_store_explicit(&LatestCandidate, std::move(Candidate), std::memory_order_release);
		OutError.clear();
		return true;
	}

	auto FSkeletalAnimationInstance::Tick(float DeltaSeconds, std::string& OutError) -> bool
	{
		if (!bBound) return Fail("Cannot tick an unbound skeletal animation instance.", &OutError);
		if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0f)
			return Fail("Skeletal animation delta time must be finite and non-negative.", &OutError);
		if (!bPlaying || DeltaSeconds == 0.0f || PlayRate == 0.0f)
		{
			OutError.clear();
			return true;
		}
		check(Binding.ClipPayload);
		const double AdvancedTime = static_cast<double>(TimeSeconds)
			+ static_cast<double>(DeltaSeconds) * static_cast<double>(PlayRate);
		if (!std::isfinite(AdvancedTime))
			return Fail("Skeletal animation time advance overflowed.", &OutError);
		const float CandidateTime = NormalizeTime(AdvancedTime);
		if (CandidateTime == TimeSeconds)
		{
			if (!bLooping && CandidateTime == GetDurationSeconds()) bPlaying = false;
			OutError.clear();
			return true;
		}
		std::shared_ptr<const FSkeletalPosePalette> Candidate = EvaluateAt(CandidateTime, OutError);
		if (!Candidate) return false;
		TimeSeconds = CandidateTime;
		Revision = Candidate->Revision;
		if (!bLooping && TimeSeconds == GetDurationSeconds()) bPlaying = false;
		std::atomic_store_explicit(&LatestCandidate, std::move(Candidate), std::memory_order_release);
		OutError.clear();
		return true;
	}

	auto FSkeletalAnimationInstance::SetPlayRate(float InPlayRate, std::string& OutError) -> bool
	{
		if (!std::isfinite(InPlayRate) || InPlayRate < 0.0f)
			return Fail("Skeletal animation play rate must be finite and non-negative.", &OutError);
		PlayRate = InPlayRate == 0.0f ? 0.0f : InPlayRate;
		OutError.clear();
		return true;
	}
}
