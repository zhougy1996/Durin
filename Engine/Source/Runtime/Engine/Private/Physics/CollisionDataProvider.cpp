#include "Physics/PhysicsMeshInputTask.h"

namespace Durin
{
	namespace
	{
		class FResidentPhysicsMeshInputTask final : public FPhysicsMeshInputTask
		{
		public:
			FResidentPhysicsMeshInputTask(FTriMeshCollisionData InData, uint64 InBytes)
				: Data(std::move(InData)), Bytes(InBytes) {}
			auto GetWorkingSetBytes() const -> uint64 override { return Bytes; }
			auto Execute(const FAssetBuildTaskContext& Context) const
				-> std::expected<FTriMeshCollisionData, std::string> override
			{
				if (Context.IsCancelled()) return {};
				if (Bytes > Context.MaximumWorkingSetBytes)
					return std::unexpected(MakeAssetBuildDiagnostic("Collision input exceeds the reservation.", "Input"));
				return Data;
			}
		private:
			FTriMeshCollisionData Data;
			uint64 Bytes;
		};
	}

	auto IInterface_CollisionDataProvider::CreatePhysicsMeshInputTask() const
		-> std::expected<std::unique_ptr<FPhysicsMeshInputTask>, std::string>
	{
		auto Data = GetPhysicsTriMeshData();
		if (!Data) return std::unexpected(Data.error());
		FAssetBuildMemoryEstimate Memory{512ull * 1024 * 1024, 1024 * 1024};
		if (!Memory.Add(Data->Positions.size(), 512) || !Memory.Add(Data->Indices.size(), 192))
			return std::unexpected(MakeAssetBuildDiagnostic("Collision input exceeds its working-set budget.", "Input"));
		return std::make_unique<FResidentPhysicsMeshInputTask>(std::move(*Data), Memory.Bytes);
	}

}
