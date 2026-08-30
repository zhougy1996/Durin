#include "StaticMeshBuildFunctionRegistry.h"
#include "Modules/ModuleManager.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshBuildOperations.h"
#include "StaticMesh/StaticMeshPostLoad.h"

namespace Durin
{
	// Owns StaticMesh build-function and collision-provider registration.
	class FStaticMeshBuildModule final
		: public IModuleInterface
		, public IStaticMeshCollisionBuildFeature
		, public IStaticMeshPostLoadFeature
	{
		FModularFeatureRegistration CollisionFeatureRegistration;
		FModularFeatureRegistration PostLoadFeatureRegistration;
		FModuleOwnedCallbackRegistration BuildFunctionCallbackRegistration;

		auto BuildCollisionProduct(
			const FStaticMeshRenderData& RenderData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			FStaticMeshCollisionBuildProduct& OutProduct,
			std::string& OutError) -> bool override
		{
			return Asset::FStaticMeshBuildOperations::BuildCollisionProduct(
				RenderData, Mode, Policy, OutProduct, OutError);
		}

		auto PostLoadUncooked(DStaticMesh& Mesh,
			FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
			std::string& OutError) -> bool override
		{
			if (!Mesh.GetImportedData().IsValid())
			{
				OutError = "StaticMesh canonical imported geometry is missing or invalid.";
				return false;
			}
			const Asset::FStaticMeshReconciliationSnapshot Reconciliation =
				Asset::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(Mesh);
			FStaticMeshBuildProduct Product;
			if (Asset::FStaticMeshBuildOperations::TryLoadImportedProduct(
				Reconciliation, Mesh.GetImportedData(), Product, OutError))
			{
				if (!Asset::FStaticMeshBuildOperations::PublishImportedProduct(
					Mesh, std::move(Product), OutError)) return false;
				OutDiagnostic = Mesh.GetDerivedDataDiagnostic();
				return true;
			}
			if (!OutError.empty()) return false;
			FStaticMeshImportedData Decoded = Mesh.GetImportedData().Decode(OutError);
			if (!OutError.empty()) return false;
			if (!Asset::FStaticMeshBuildOperations::BuildImportedProduct(
				Reconciliation,
				Decoded, "canonical imported geometry", Product, OutError)) return false;
			Product.bMarkPackageDirty = false;
			Product.bContainsImportedData = false;
			Product.bSourceImporterInvoked = false;
			if (!Asset::FStaticMeshBuildOperations::PublishImportedProduct(
				Mesh, std::move(Product), OutError)) return false;
			OutDiagnostic = Mesh.GetDerivedDataDiagnostic();
			return true;
		}

		auto StartupModule() -> void override
		{
			std::string Error;
			BuildFunctionCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration(
					"StaticMeshBuild.BuildFunctions");
			checkf(Asset::InitializeStaticMeshBuildFunctions(
				BuildFunctionCallbackRegistration.GetGate(), &Error),
				"StaticMeshBuild could not register its build functions: {}", Error);
			CollisionFeatureRegistration =
				FModuleStartup::RegisterFeature<IStaticMeshCollisionBuildFeature>(*this);
			PostLoadFeatureRegistration =
				FModuleStartup::RegisterFeature<IStaticMeshPostLoadFeature>(*this);
			checkf(CollisionFeatureRegistration.IsValid(),
				"StaticMeshBuild could not register StaticMesh collision building.");
			checkf(PostLoadFeatureRegistration.IsValid(),
				"StaticMeshBuild could not register StaticMesh post-load building.");
		}

		auto ShutdownModule() -> void override
		{
			Asset::ShutdownStaticMeshBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FStaticMeshBuildModule, StaticMeshBuild)
}
