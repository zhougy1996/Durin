#include "SkeletalMesh/Skeleton.h"

#include "AssetCook.h"
#include "Hash/XxHash.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		constexpr double MinimumSkeletonScale = 1.0e-8;
		constexpr double SkeletonMatrixTolerance = 1.0e-5;

		auto Fail(std::string* OutError, std::string Message) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto CanonicalFloat(double Value) -> double
		{
			float Canonical = static_cast<float>(Value);
			if (Canonical == 0.0f) Canonical = 0.0f;
			return static_cast<double>(Canonical);
		}

		class FCompatibilityWriter
		{
		public:
			auto WriteBytes(std::span<const uint8> Values) -> void
			{
				Bytes.insert(Bytes.end(), Values.begin(), Values.end());
			}

			auto WriteU32(uint32 Value) -> void
			{
				for (uint32 Shift = 0; Shift < 32; Shift += 8)
					Bytes.push_back(static_cast<uint8>((Value >> Shift) & 0xffu));
			}

			auto WriteI32(int32 Value) -> void
			{
				WriteU32(std::bit_cast<uint32>(Value));
			}

			auto WriteFloat(float Value) -> void
			{
				if (Value == 0.0f) Value = 0.0f;
				WriteU32(std::bit_cast<uint32>(Value));
			}

			auto GetBytes() const -> std::span<const uint8> { return Bytes; }

		private:
			std::vector<uint8> Bytes;
		};
	}

	auto FSkeletonTransform::IsValid(std::string* OutError) const -> bool
	{
		if (!Math::IsFinite(Row0) || !Math::IsFinite(Row1)
			|| !Math::IsFinite(Row2) || !Math::IsFinite(Row3))
			return Fail(OutError, "Skeleton transform contains a non-finite component.");
		for (const FVector4* Row : {&Row0, &Row1, &Row2, &Row3})
		{
			if (CanonicalFloat(Row->x) != Row->x || CanonicalFloat(Row->y) != Row->y
				|| CanonicalFloat(Row->z) != Row->z || CanonicalFloat(Row->w) != Row->w)
				return Fail(OutError, "Skeleton transform is not canonical float32 data.");
		}
		if (std::abs(Row3.x) > SkeletonMatrixTolerance
			|| std::abs(Row3.y) > SkeletonMatrixTolerance
			|| std::abs(Row3.z) > SkeletonMatrixTolerance
			|| std::abs(Row3.w - 1.0) > SkeletonMatrixTolerance)
			return Fail(OutError, "Skeleton transform must be an affine matrix.");

		const FVector3 Column0(Row0.x, Row1.x, Row2.x);
		const FVector3 Column1(Row0.y, Row1.y, Row2.y);
		const FVector3 Column2(Row0.z, Row1.z, Row2.z);
		const double Scale0 = Math::Length(Column0);
		const double Scale1 = Math::Length(Column1);
		const double Scale2 = Math::Length(Column2);
		if (Scale0 <= MinimumSkeletonScale
			|| Scale1 <= MinimumSkeletonScale
			|| Scale2 <= MinimumSkeletonScale)
			return Fail(OutError, "Skeleton transform scale is singular.");
		const FVector3 Axis0 = Column0 / Scale0;
		const FVector3 Axis1 = Column1 / Scale1;
		const FVector3 Axis2 = Column2 / Scale2;
		if (std::abs(Math::Dot(Axis0, Axis1)) > SkeletonMatrixTolerance
			|| std::abs(Math::Dot(Axis0, Axis2)) > SkeletonMatrixTolerance
			|| std::abs(Math::Dot(Axis1, Axis2)) > SkeletonMatrixTolerance)
			return Fail(OutError, "Skeleton transform contains unsupported shear.");
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

		FCompatibilityWriter Writer;
		Writer.WriteBytes(std::span<const uint8>(reinterpret_cast<const uint8*>("DSKC"), 4));
		Writer.WriteU32(SkeletonCompatibilityEncodingVersion);
		Writer.WriteU32(static_cast<uint32>(InBones.size()));
		for (const FSkeletonBone& Bone : InBones)
		{
			const std::string Name = Bone.Name.ToString();
			Writer.WriteI32(Bone.ParentIndex);
			Writer.WriteU32(static_cast<uint32>(Name.size()));
			Writer.WriteBytes(std::span<const uint8>(
				reinterpret_cast<const uint8*>(Name.data()), Name.size()));
			const FMatrix4f Matrix = Bone.ReferenceTransform.ToMatrix4f();
			for (uint32 Row = 0; Row < 4; ++Row)
				for (uint32 Column = 0; Column < 4; ++Column)
					Writer.WriteFloat(Matrix[Column][Row]);
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
			return Fail(&OutError, std::format(
				"Skeleton '{}' supports only the Win64 game cook target.", GetObjectPath()));
		if (!GetPackage() || !Validate(OutError)) return false;
		std::vector<uint8> PackageBytes;
		const Asset::FAssetResult Serialized =
			Asset::SerializeAssetPackageBytes(GetPackage(), PackageBytes);
		if (!Serialized) return Fail(&OutError, Serialized.Message);
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
