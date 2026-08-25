#include "SkeletalMesh/Skeleton.h"

#include "DObject/Package.h"

#include "AssetCook.h"
#include "Hash/XxHash.h"
#include "Math/Operations.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	namespace
	{
		constexpr double MinimumSkeletonScale = 1.0e-8;
		constexpr double SkeletonMatrixTolerance = 1.0e-5;

		auto CanonicalFloat(double Value) -> double
		{
			float Canonical = static_cast<float>(Value);
			if (Canonical == 0.0f) Canonical = 0.0f;
			return static_cast<double>(Canonical);
		}

	}

	auto FSkeletonTransform::IsValid(std::string* OutError) const -> bool
	{
		if (!Math::IsFinite(Row0) || !Math::IsFinite(Row1)
			|| !Math::IsFinite(Row2) || !Math::IsFinite(Row3))
			return Fail("Skeleton transform contains a non-finite component.", OutError);
		for (const FVector4* Row : {&Row0, &Row1, &Row2, &Row3})
		{
			if (CanonicalFloat(Row->x) != Row->x || CanonicalFloat(Row->y) != Row->y
				|| CanonicalFloat(Row->z) != Row->z || CanonicalFloat(Row->w) != Row->w)
				return Fail("Skeleton transform is not canonical float32 data.", OutError);
		}
		if (std::abs(Row3.x) > SkeletonMatrixTolerance
			|| std::abs(Row3.y) > SkeletonMatrixTolerance
			|| std::abs(Row3.z) > SkeletonMatrixTolerance
			|| std::abs(Row3.w - 1.0) > SkeletonMatrixTolerance)
			return Fail("Skeleton transform must be an affine matrix.", OutError);

		const FVector3 Column0(Row0.x, Row1.x, Row2.x);
		const FVector3 Column1(Row0.y, Row1.y, Row2.y);
		const FVector3 Column2(Row0.z, Row1.z, Row2.z);
		const double Scale0 = Math::Length(Column0);
		const double Scale1 = Math::Length(Column1);
		const double Scale2 = Math::Length(Column2);
		if (Scale0 <= MinimumSkeletonScale
			|| Scale1 <= MinimumSkeletonScale
			|| Scale2 <= MinimumSkeletonScale)
			return Fail("Skeleton transform scale is singular.", OutError);
		const FVector3 Axis0 = Column0 / Scale0;
		const FVector3 Axis1 = Column1 / Scale1;
		const FVector3 Axis2 = Column2 / Scale2;
		if (std::abs(Math::Dot(Axis0, Axis1)) > SkeletonMatrixTolerance
			|| std::abs(Math::Dot(Axis0, Axis2)) > SkeletonMatrixTolerance
			|| std::abs(Math::Dot(Axis1, Axis2)) > SkeletonMatrixTolerance)
			return Fail("Skeleton transform contains unsupported shear.", OutError);
		if (OutError) OutError->clear();
		return true;
	}

	auto FSkeletonTransform::CanonicalizeFloat32() -> void
	{
		for (FVector4* Row : {&Row0, &Row1, &Row2, &Row3})
		{
			Row->x = CanonicalFloat(Row->x);
			Row->y = CanonicalFloat(Row->y);
			Row->z = CanonicalFloat(Row->z);
			Row->w = CanonicalFloat(Row->w);
		}
	}

	auto FSkeletonTransform::ToMatrix4f() const -> FMatrix4f
	{
		FMatrix4f Result(0.0f);
		const FVector4* Rows[] = {&Row0, &Row1, &Row2, &Row3};
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			Result[0][Row] = static_cast<float>(Rows[Row]->x);
			Result[1][Row] = static_cast<float>(Rows[Row]->y);
			Result[2][Row] = static_cast<float>(Rows[Row]->z);
			Result[3][Row] = static_cast<float>(Rows[Row]->w);
		}
		return Result;
	}

	DSkeleton::DSkeleton(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer) {}

	auto DSkeleton::ComputeCompatibilityIdentity(
		std::span<const FSkeletonBone> InBones,
		std::string& OutIdentity,
		std::string& OutError) -> bool
	{
		if (InBones.empty() || InBones.size() > MaximumSkeletonBones)
		{
			OutError = "Skeleton bone count is outside the supported range.";
			return false;
		}
		std::unordered_set<FName> Names;
		uint32 RootCount = 0;
		for (size_t BoneIndex = 0; BoneIndex < InBones.size(); ++BoneIndex)
		{
			const FSkeletonBone& Bone = InBones[BoneIndex];
			if (Bone.Name.IsNone() || !Names.insert(Bone.Name).second)
			{
				OutError = "Skeleton bone names must be non-None and unique.";
				return false;
			}
			if (Bone.ParentIndex == -1)
			{
				++RootCount;
			}
			else if (Bone.ParentIndex < 0 || static_cast<size_t>(Bone.ParentIndex) >= BoneIndex)
			{
				OutError = std::format(
					"Skeleton bone {} has an invalid or non-parent-first parent index.", BoneIndex);
				return false;
			}
			std::string TransformError;
			if (!Bone.ReferenceTransform.IsValid(&TransformError))
			{
				OutError = std::format("Skeleton bone '{}' is invalid: {}", Bone.Name.ToString(), TransformError);
				return false;
			}
		}
		if (RootCount != 1)
		{
			OutError = "Skeleton canonical hierarchy must contain exactly one root.";
			return false;
		}

		FBinaryWriter Writer;
		Writer.WriteBytes(std::as_bytes(std::span(std::string_view("DSKC"))));
		Writer.WriteU32(SkeletonCompatibilityEncodingVersion);
		Writer.WriteU32(static_cast<uint32>(InBones.size()));
		for (const FSkeletonBone& Bone : InBones)
		{
			const std::string Name = Bone.Name.ToString();
			Writer.WriteI32(Bone.ParentIndex);
			Writer.WriteU32(static_cast<uint32>(Name.size()));
			Writer.WriteBytes(std::as_bytes(std::span(Name)));
			const FMatrix4f Matrix = Bone.ReferenceTransform.ToMatrix4f();
			for (uint32 Row = 0; Row < 4; ++Row)
				for (uint32 Column = 0; Column < 4; ++Column)
				{
					float Value = Matrix[Column][Row];
					if (Value == 0.0f) Value = 0.0f;
					Writer.WriteFloat(Value);
				}
		}
		OutIdentity = FXxHash128::HashBuffer(Writer.GetBytes()).ToString();
		OutError.clear();
		return true;
	}

	auto DSkeleton::InitializeCanonicalBones(
		std::vector<FSkeletonBone> InBones,
		std::string& OutError) -> bool
	{
		for (FSkeletonBone& Bone : InBones)
			Bone.ReferenceTransform.CanonicalizeFloat32();
		std::string Identity;
		if (!ComputeCompatibilityIdentity(InBones, Identity, OutError)) return false;
		Bones = std::move(InBones);
		CompatibilityIdentity = std::move(Identity);
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DSkeleton::Validate(std::string& OutError) const -> bool
	{
		std::string Expected;
		if (!ComputeCompatibilityIdentity(Bones, Expected, OutError)) return false;
		if (CompatibilityIdentity != Expected)
		{
			OutError = std::format(
				"Skeleton compatibility identity '{}' does not match canonical hierarchy '{}'.",
				CompatibilityIdentity, Expected);
			return false;
		}
		OutError.clear();
		return true;
	}

	auto DSkeleton::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		if (Validate(OutError)) return true;
		OutError = std::format("{}: {}", GetName(), OutError);
		return false;
	}

	auto DSkeleton::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
			return Fail(std::format(
				"Skeleton '{}' supports only the Win64 game cook target.", GetObjectPath()), &OutError);
		if (!GetPackage() || !Validate(OutError)) return false;
		std::vector<std::byte> PackageBytes;
		const Asset::FAssetResult Serialized =
			Asset::SerializeAssetPackageBytes(GetPackage(), PackageBytes);
		if (!Serialized) return Fail(Serialized.Message, &OutError);
		return Context.AddPackage(
			std::string(VirtualPackagePath), std::move(PackageBytes), {}, &OutError);
	}

	auto DSkeleton::PrepareImportedStateExchange(
		DSkeleton& Candidate,
		std::string& OutError) -> std::unique_ptr<FSkeletonImportedStateExchange>
	{
		if (&Candidate == this)
		{
			OutError = "Skeleton imported-state exchange requires distinct assets.";
			return nullptr;
		}
		if (!Validate(OutError))
		{
			OutError = std::format("Target Skeleton is invalid: {}", OutError);
			return nullptr;
		}
		if (!Candidate.Validate(OutError))
		{
			OutError = std::format("Candidate Skeleton is invalid: {}", OutError);
			return nullptr;
		}
		OutError.clear();
		return std::unique_ptr<FSkeletonImportedStateExchange>(
			new FSkeletonImportedStateExchange(*this, Candidate));
	}

	FSkeletonImportedStateExchange::FSkeletonImportedStateExchange(
		DSkeleton& InTarget,
		DSkeleton& InCandidate)
		: Target(&InTarget), Candidate(&InCandidate) {}

	FSkeletonImportedStateExchange::~FSkeletonImportedStateExchange() = default;

	auto FSkeletonImportedStateExchange::Swap() noexcept -> void
	{
		check(Target && Candidate && Target != Candidate);
		std::swap(Target->Bones, Candidate->Bones);
		std::swap(Target->CompatibilityIdentity, Candidate->CompatibilityIdentity);
		Target->MarkPackageDirty();
	}

	auto FSkeletonImportedStateExchange::Commit() noexcept -> void
	{
		if (bCommitted) return;
		Swap();
		bCommitted = true;
	}

	auto FSkeletonImportedStateExchange::Reverse() noexcept -> void
	{
		if (!bCommitted) return;
		Swap();
		bCommitted = false;
	}

	auto FSkeletonImportedStateExchange::Finalize() noexcept -> void
	{
		Target = nullptr;
		Candidate = nullptr;
	}
}
