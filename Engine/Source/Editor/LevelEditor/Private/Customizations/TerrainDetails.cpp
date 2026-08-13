#include "TerrainDetails.h"

#include "Components/TerrainComponent.h"
#include "DObject/Class.h"
#include "LevelEditorCustomizations.h"
#include "MonaImGui.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin::Editor::Level
{
	namespace
	{
		constexpr size_t MaximumTerrainDiagnosticBytes = 2'048;

		auto Bounded(std::string_view Text) -> std::string
		{
			if (Text.size() <= MaximumTerrainDiagnosticBytes) return std::string(Text);
			std::string Result(Text.substr(0, MaximumTerrainDiagnosticBytes - 3));
			Result += "...";
			return Result;
		}

		auto RenderStatusText(ETerrainRenderStatus Status) -> const char*
		{
			switch (Status)
			{
			case ETerrainRenderStatus::Unavailable: return "Unavailable";
			case ETerrainRenderStatus::Ready: return "Ready";
			case ETerrainRenderStatus::InvalidProperties: return "Invalid properties";
			case ETerrainRenderStatus::MissingHeightmap: return "Missing heightmap";
			case ETerrainRenderStatus::InvalidPayload: return "Invalid payload";
			case ETerrainRenderStatus::ExtentRejected: return "Extent rejected";
			}
			return "Unknown";
		}

		auto CollisionStatusText(ETerrainCollisionStatus Status) -> const char*
		{
			switch (Status)
			{
			case ETerrainCollisionStatus::Unavailable: return "Unavailable";
			case ETerrainCollisionStatus::Ready: return "Ready";
			case ETerrainCollisionStatus::InvalidProperties: return "Invalid properties";
			case ETerrainCollisionStatus::MissingHeightmap: return "Missing heightmap";
			case ETerrainCollisionStatus::InvalidPayload: return "Invalid payload";
			case ETerrainCollisionStatus::ExtentRejected: return "Extent rejected";
			case ETerrainCollisionStatus::BuildFailed: return "Build failed";
			}
			return "Unknown";
		}

		auto DrawFact(std::string_view Label, std::string_view Value) -> void
		{
			MonaImGui::PropertyEdit::BeginRow(std::string(Label).c_str(), true);
			ImGui::TextWrapped("%s", std::string(Value).c_str());
			MonaImGui::PropertyEdit::EndRow(true);
		}

		class FTerrainDetailsCustomization final : public IObjectDetailsCustomization
		{
		public:
			auto CustomizeDetails(FLevelEditorContext&, DObject* Object,
				FObjectPropertyViewBuilder& Builder) -> void override
			{
				auto* Component = Cast<DTerrainComponent>(Object);
				if (!Component) return;
				for (std::string_view Name : {"RenderStatus", "LastRenderDiagnostic",
					"CollisionStatus", "LastCollisionDiagnostic"})
					if (FProperty* Property = Component->GetClass()->FindPropertyByName(Name))
						Builder.HideProperty(Property);

				Builder.AddCustomRow("Terrain Asset Heightmap Width Height Samples Revision Retained Bytes",
					[Component](::Durin::Editor::FPropertyView&,
						const ::Durin::Editor::FPropertyViewContext&)
					{
						DTerrainHeightmap* Heightmap = Component->GetHeightmap();
						DrawFact("Asset facts", Heightmap ? std::format(
							"{} x {} samples | revision {} | {} retained bytes",
							Heightmap->GetWidth(), Heightmap->GetHeight(), Heightmap->GetRevision(),
							Heightmap->GetPayload() ? Heightmap->GetPayload()->GetRetainedBytes() : 0)
							: "No heightmap assigned");
						if (Heightmap && !Heightmap->GetLastDiagnostic().empty())
							DrawFact("Asset diagnostic", Bounded(Heightmap->GetLastDiagnostic()));
						return false;
					});

				Builder.AddCustomRow("Terrain Render Status Diagnostic Resource Revision",
					[Component](::Durin::Editor::FPropertyView&,
						const ::Durin::Editor::FPropertyViewContext&)
					{
						DrawFact("Render status", RenderStatusText(Component->GetRenderStatus()));
						if (!Component->GetLastRenderDiagnostic().empty())
							DrawFact("Render diagnostic", Bounded(Component->GetLastRenderDiagnostic()));
						return false;
					});

				Builder.AddCustomRow("Terrain Collision Status Diagnostic Revision Resource Bytes Cells Nodes Depth",
					[Component](::Durin::Editor::FPropertyView&,
						const ::Durin::Editor::FPropertyViewContext&)
					{
						const FTerrainCollisionFacts Facts = Component->GetCollisionFacts();
						DrawFact("Collision status", std::format("{} | revision {} | resource {}",
							CollisionStatusText(Facts.Status), Facts.CollisionRevision, Facts.ResourceIdentity));
						DrawFact("Collision facts", std::format(
							"{} x {} | {} cells | {} nodes | depth {} | {} retained bytes | {} peak bytes",
							Facts.Width, Facts.Height, Facts.Cells, Facts.Nodes, Facts.MaximumDepth,
							Facts.RetainedBytes, Facts.EstimatedPeakBytes));
						if (!Component->GetLastCollisionDiagnostic().empty())
							DrawFact("Collision diagnostic", Bounded(Component->GetLastCollisionDiagnostic()));
						return false;
					});
			}
		};
	}

	auto CreateTerrainDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>
	{
		return std::make_shared<FTerrainDetailsCustomization>();
	}
}
