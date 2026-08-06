#include "RHIResources.h"

#include "Math/Operations.h"

#include "Math/Vector.h"

namespace Durin
{
	auto ValidateTextureCreateDesc(const FRHITextureCreateDesc& CreateDesc, std::string& OutError) -> bool
	{
		auto Fail = [&OutError](std::string Message) {
			OutError = std::move(Message);
			return false;
		};

		if (CreateDesc.Extent.x <= 0 || CreateDesc.Extent.y <= 0) return Fail("Texture extent must be nonzero.");
		if (CreateDesc.Depth == 0) return Fail("Texture depth must be nonzero.");
		if (CreateDesc.ArraySize == 0) return Fail("Texture array size must be nonzero.");
		if (CreateDesc.NumMips == 0) return Fail("Texture mip count must be nonzero.");
		if (CreateDesc.NumSamples == 0) return Fail("Texture sample count must be nonzero.");
		if (CreateDesc.Format == EPixelFormat::Unknown) return Fail("Texture pixel format must be specified.");

		if (CreateDesc.Dimension == ETextureDimension::TextureCube)
		{
			if (CreateDesc.Extent.x != CreateDesc.Extent.y) return Fail("TextureCube width and height must be equal.");
			if (CreateDesc.ArraySize != TextureCubeFaceCount) return Fail("TextureCube must contain exactly six array layers.");
			if (CreateDesc.Depth != 1) return Fail("TextureCube depth must be one.");
			if (CreateDesc.NumSamples != 1) return Fail("TextureCube must be single-sampled.");
		}

		OutError.clear();
		return true;
	}

	auto ValidateTexture2DUpdate(
		const FRHITextureDesc& TextureDesc,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch,
		std::string& OutError
	) -> bool
	{
		auto Fail = [&OutError](std::string Message) {
			OutError = std::move(Message);
			return false;
		};

		if (MipIndex >= TextureDesc.NumMips) return Fail("Texture upload mip index is outside the texture mip range.");
		if (ArraySlice >= TextureDesc.ArraySize) return Fail("Texture upload array slice is outside the texture layer range.");
		if (UpdateRegion.SrcX < 0 || UpdateRegion.SrcY < 0) return Fail("Texture upload source offsets must be nonnegative.");
		if (UpdateRegion.Width == 0 || UpdateRegion.Height == 0) return Fail("Texture upload region must be nonzero.");

		const uint32 MipWidth = std::max(1u, static_cast<uint32>(TextureDesc.Extent.x) >> MipIndex);
		const uint32 MipHeight = std::max(1u, static_cast<uint32>(TextureDesc.Extent.y) >> MipIndex);
		if (static_cast<uint64>(UpdateRegion.DestX) + UpdateRegion.Width > MipWidth
			|| static_cast<uint64>(UpdateRegion.DestY) + UpdateRegion.Height > MipHeight)
		{
			return Fail("Texture upload destination region exceeds the selected mip.");
		}

		const FPixelFormatInfo& FormatInfo = GetPixelFormatInfo(TextureDesc.Format);
		if (FormatInfo.BytesPerBlock == 0 || FormatInfo.BlockSize == 0)
		{
			return Fail("Texture upload requires a valid pixel format layout.");
		}

		const uint32 BlockSize = FormatInfo.BlockSize;
		if (UpdateRegion.DestX % BlockSize != 0 || UpdateRegion.DestY % BlockSize != 0
			|| static_cast<uint32>(UpdateRegion.SrcX) % BlockSize != 0
			|| static_cast<uint32>(UpdateRegion.SrcY) % BlockSize != 0)
		{
			return Fail("Texture upload source and destination offsets must be block-aligned.");
		}
		if ((UpdateRegion.Width % BlockSize != 0 && UpdateRegion.DestX + UpdateRegion.Width != MipWidth)
			|| (UpdateRegion.Height % BlockSize != 0 && UpdateRegion.DestY + UpdateRegion.Height != MipHeight))
		{
			return Fail("Texture upload dimensions must be block-aligned unless the region reaches the mip edge.");
		}

		const uint64 SourceBlockX = static_cast<uint32>(UpdateRegion.SrcX) / BlockSize;
		const uint64 RegionBlocksWide = (static_cast<uint64>(UpdateRegion.Width) + BlockSize - 1) / BlockSize;
		const uint64 RequiredPitch = (SourceBlockX + RegionBlocksWide) * FormatInfo.BytesPerBlock;
		if (RequiredPitch > SourcePitch) return Fail("Texture upload source pitch is too small for the requested source region.");

		OutError.clear();
		return true;
	}

	auto ResolveTextureCubeFaceUv(const FVector3& Direction, ETextureCubeFace& OutFace, FVector2f& OutUv) -> bool
	{
		const double AbsX = std::abs(Direction.x);
		const double AbsY = std::abs(Direction.y);
		const double AbsZ = std::abs(Direction.z);
		const double MajorAxis = std::max({AbsX, AbsY, AbsZ});
		if (!std::isfinite(MajorAxis) || MajorAxis <= 0.0) return false;

		double Sc = 0.0;
		double Tc = 0.0;
		if (AbsX >= AbsY && AbsX >= AbsZ)
		{
			if (Direction.x >= 0.0)
			{
				OutFace = ETextureCubeFace::PositiveX;
				Sc = -Direction.z;
				Tc = -Direction.y;
			}
			else
			{
				OutFace = ETextureCubeFace::NegativeX;
				Sc = Direction.z;
				Tc = -Direction.y;
			}
		}
		else if (AbsY >= AbsZ)
		{
			if (Direction.y >= 0.0)
			{
				OutFace = ETextureCubeFace::PositiveY;
				Sc = Direction.x;
				Tc = Direction.z;
			}
			else
			{
				OutFace = ETextureCubeFace::NegativeY;
				Sc = Direction.x;
				Tc = -Direction.z;
			}
		}
		else if (Direction.z >= 0.0)
		{
			OutFace = ETextureCubeFace::PositiveZ;
			Sc = Direction.x;
			Tc = -Direction.y;
		}
		else
		{
			OutFace = ETextureCubeFace::NegativeZ;
			Sc = -Direction.x;
			Tc = -Direction.y;
		}

		OutUv = FVector2f(
			static_cast<float>((Sc / MajorAxis + 1.0) * 0.5),
			static_cast<float>((Tc / MajorAxis + 1.0) * 0.5)
		);
		return true;
	}

	auto ResolveTextureCubeFacePixelDirection(ETextureCubeFace Face, uint32 PixelX, uint32 PixelY,
		uint32 FaceDimension, FVector3& OutDirection) -> bool
	{
		if (FaceDimension == 0 || PixelX >= FaceDimension || PixelY >= FaceDimension) return false;
		const double A = 2.0 * (static_cast<double>(PixelX) + 0.5) / FaceDimension - 1.0;
		const double B = 2.0 * (static_cast<double>(PixelY) + 0.5) / FaceDimension - 1.0;
		switch (Face)
		{
		case ETextureCubeFace::PositiveX: OutDirection = FVector3(1.0, -B, -A); break;
		case ETextureCubeFace::NegativeX: OutDirection = FVector3(-1.0, -B, A); break;
		case ETextureCubeFace::PositiveY: OutDirection = FVector3(A, 1.0, B); break;
		case ETextureCubeFace::NegativeY: OutDirection = FVector3(A, -1.0, -B); break;
		case ETextureCubeFace::PositiveZ: OutDirection = FVector3(A, -B, 1.0); break;
		case ETextureCubeFace::NegativeZ: OutDirection = FVector3(-A, -B, -1.0); break;
		default: return false;
		}
		const double Length = Math::Length(OutDirection);
		if (!std::isfinite(Length) || Length <= 0.0) return false;
		OutDirection /= Length;
		return true;
	}

	// May use a multiple producer single consumer queue here if the contention is high, but currently we don't have that many threads creating resources, so a simple vector with mutex should be fine.
	std::vector<FRHIResource*> PendingDeletes;
	std::mutex PendingDeletesMutex;

#if DO_CHECK
	// This pointer will be set before any FRHIResource being deleted, then it will be checked and reset in the destructor of FRHIResource.
	// This is to catch any unexpected deletion, such as deleting a resource manually without calling DeleteResources.
	thread_local const FRHIResource* CurrentDeleting = nullptr;
#endif

	FRHIResource::FRHIResource(ERHIResourceType InResourceType)
		: ResourceType(InResourceType)
	{
	}

	FRHIResource::~FRHIResource()
	{
#if DO_CHECK
		// A derived constructor may fail before the resource is ever referenced.
		// Published resources must still arrive through DeleteResources.
		check(IsEngineExitRequested() || CurrentDeleting == this
			|| AtomicFlags.IsUnpublished(std::memory_order_relaxed));
		if (CurrentDeleting == this)
		{
			CurrentDeleting = nullptr;
		}
#endif
	}

	auto FRHIResource::MarkForDelete() const -> void
	{
		if (!AtomicFlags.MarkForDelete(std::memory_order_release))
		{
			std::lock_guard<std::mutex> lock(PendingDeletesMutex);
			PendingDeletes.push_back(const_cast<FRHIResource*>(this));
		}
	}

	auto FRHIResource::DeleteResources(const std::vector<FRHIResource*>& ResourcesToDelete) -> void
	{
		for (FRHIResource* Resource : ResourcesToDelete)
		{
			if (Resource->AtomicFlags.Deleting())
			{
#if DO_CHECK
				CurrentDeleting = Resource;
#endif
				delete Resource;
#if DO_CHECK
				check(CurrentDeleting == nullptr);
#endif
			}
		}
	}

	auto FRHIResource::GatherResourcesToDelete(std::vector<FRHIResource*>& OutResourcesToDelete) -> void
	{
		std::vector<FRHIResource*> LocalTemp;
		{
			std::lock_guard<std::mutex> Lock(PendingDeletesMutex);
			LocalTemp.swap(PendingDeletes);
		}
		OutResourcesToDelete.insert(
			OutResourcesToDelete.end(),
			std::make_move_iterator(LocalTemp.begin()),
			std::make_move_iterator(LocalTemp.end())
		);
	}

	auto FRHIResource::GetNumPendingDeletes() -> size_t
	{
		std::lock_guard<std::mutex> Lock(PendingDeletesMutex);
		return PendingDeletes.size();
	}

} // namespace Durin
