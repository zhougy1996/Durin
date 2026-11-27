#include "Operations/ActorFactoryStaticMesh.h"
#include "Actors/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Factories/ActorFactory.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::LevelEditor
{
	namespace
	{
		class FActorFactoryStaticMesh final : public FActorFactory
		{
		public:
			FActorFactoryStaticMesh() : FActorFactory(DStaticMesh::StaticClass(), AStaticMeshActor::StaticClass()) {}
		private:
			auto ConfigureActor(DObject& Asset, AActor& Actor) const -> void override
			{
				static_cast<AStaticMeshActor&>(Actor).GetStaticMeshComponent()->SetStaticMesh(static_cast<DStaticMesh*>(&Asset));
			}
		};

	}

	auto RegisterStaticMeshActorFactory() -> FModularFeatureRegistration
	{
		static FActorFactoryStaticMesh Mesh;
		return RegisterModularFeature(Mesh);
	}
}
