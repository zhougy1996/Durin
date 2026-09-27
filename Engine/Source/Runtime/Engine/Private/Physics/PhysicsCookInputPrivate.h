#pragma once
#include "Physics/PhysicsCookHelper.h"
#include "Serialization/BinaryFormat.h"
#include <bit>

namespace Durin::PhysicsPrivate
{
	inline auto HashInput(FByteView Positions, FByteView Indices, const std::function<bool()>& Cancel)
		-> std::optional<FXxHash128>
	{
		static_assert(std::endian::native == std::endian::little);
		static_assert(sizeof(FVector3f) == 12 && offsetof(FVector3f, y) == 4 && offsetof(FVector3f, z) == 8);
		static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559 && sizeof(uint32) == 4);
		FXxHash128Builder Hash;
		for (const auto& [Bytes, Stride] : {std::pair{Positions, size_t(12)}, std::pair{Indices, size_t(4)}})
		{
			FBinaryWriter Header; Header.WriteU64(Bytes.size() / Stride); Hash.Update(Header.GetBytes());
			for (size_t Offset = 0; Offset < Bytes.size(); Offset += 4096)
			{
				if (Cancel && Cancel()) return {};
				Hash.Update(Bytes.subspan(Offset, std::min<size_t>(4096, Bytes.size() - Offset)));
			}
		}
		if (Cancel && Cancel()) return {};
		return Hash.Finalize();
	}
	inline auto CheckCookReservation(uint64 Positions, uint64 Indices, uint64 MaximumWorkingSetBytes) -> bool
	{
		FAssetBuildMemoryEstimate Memory{MaximumWorkingSetBytes};
		return Memory.Add(1, 1024 * 1024) && Memory.Add(Positions, 512) && Memory.Add(Indices, 192);
	}
	inline auto CheckCookInput(uint64 Positions, uint64 Indices, EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy, uint64 MaximumWorkingSetBytes) -> std::expected<void, FPhysicsCookFailure>
	{
		if (Mode != EBodySetupCollisionSourceMode::ConvexHullFromLOD0 && Mode != EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
			return std::unexpected(FPhysicsCookFailure{"Physics cook source mode is invalid."});
		if (Policy != EBodySetupCollisionQueryPolicy::SimpleOnly && Policy != EBodySetupCollisionQueryPolicy::ComplexOnly
			&& Policy != EBodySetupCollisionQueryPolicy::SimpleAndComplex)
			return std::unexpected(FPhysicsCookFailure{"Physics cook query policy is invalid."});
		if (!CheckCookReservation(Positions, Indices, MaximumWorkingSetBytes))
			return std::unexpected(FPhysicsCookFailure{"Physics cook input and working set exceed the reservation."});
		if (!Positions || !Indices || Indices % 3)
			return std::unexpected(FPhysicsCookFailure{std::format("Physics cook input is malformed ({} vertices, {} indices).", Positions, Indices)});
		return {};
	}
}
