#include "MaterialGraph.h"
#include "MaterialGraphEditInternals.h"
#include "Graph/MaterialGraphChange.h"

#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin::MaterialEditor
{
	using namespace GraphEditInternals;

	namespace
	{
		// Replays only changed positions so unrelated presentation edits survive undo.
		class FMaterialGraphParameterTransaction final : public FMaterialCommandChange
		{
		public:
			auto GetModifiedObjects() const -> std::vector<DObject*> override
			{ return {Material.Get()}; }

			FMaterialGraphParameterTransaction(
				DMaterial& InMaterial,
				FGuid InParameterId,
				FMaterialParameterValue InBeforeValue,
				FMaterialParameterValue InAfterValue)
				: Material(&InMaterial)
				, ParameterId(InParameterId)
				, BeforeValue(std::move(InBeforeValue))
				, AfterValue(std::move(InAfterValue))
			{
				AffectedPackages.front() = InMaterial.GetPackage();
			}

			auto GetDescription() const -> std::string_view override
			{
				return "Edit Material Parameter";
			}
			auto GetOwningModule() const -> std::string_view override
			{
				return "MaterialEditor";
			}
			auto GetAffectedPackages() const -> std::span<DPackage* const> override
			{
				return AffectedPackages;
			}
			auto Replay(ETransactionOperation Operation) -> FMaterialChangeResult override
			{ return Apply(Operation == ETransactionOperation::Undo ? BeforeValue : AfterValue); }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				BeforeValue.AddReferencedObjects(Collector);
				AfterValue.AddReferencedObjects(Collector);
			}

		private:
			auto Apply(const FMaterialParameterValue& Value) -> FMaterialChangeResult
			{
				DMaterial* Target = Material.Get();
				if (!Target) return {{.Code = EMaterialChangeError::TargetUnavailable, .ParameterId = ParameterId}};
				const auto Applied = Target->SetParameterValue(ParameterId, Value);
				if (!Applied) return {{.Code = EMaterialChangeError::MaterialWrite, .ParameterId = ParameterId,
					.Message = Applied.error()}};
				return {};
			}

			TWeakObjectPtr<DMaterial> Material;
			FGuid ParameterId;
			mutable FMaterialParameterValue BeforeValue;
			mutable FMaterialParameterValue AfterValue;
			std::array<DPackage*, 1> AffectedPackages{};
		};

	}

	namespace GraphEditInternals
	{
		auto MakeMaterialGraphParameterTransaction(
			DMaterial& Material,
			FGuid ParameterId,
			FMaterialParameterValue BeforeValue,
			FMaterialParameterValue AfterValue)
			-> std::unique_ptr<FMaterialCommandChange>
		{
			return std::make_unique<FMaterialGraphParameterTransaction>(
				Material, ParameterId, std::move(BeforeValue), std::move(AfterValue));
		}

	}
}
