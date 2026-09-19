#include "MaterialGraphCanvas.h"
#include "MaterialGraphEditSession.h"
#include "Asset/Asset.h"
#include "TexturePreview.h"
#include "Editor/AssetDragDrop.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "Texture/Texture2D.h"

namespace Durin::Editor::Material
{
	namespace
	{
		struct FRegisteredPreview
		{
			Texture::FTexturePreview Preview;
			Mona::IMonaUIBackend* Backend = nullptr;
		};
		// Coalesce bounded built previews across nodes and open documents.
		std::unordered_map<FRHITexture*, std::weak_ptr<FRegisteredPreview>> Registrations;
	}
	struct FMaterialGraphCanvas::FTexturePreviewState
	{
		std::unordered_map<FGuid, std::shared_ptr<FRegisteredPreview>> Nodes;
	};
	auto FMaterialGraphCanvas::UpdateTexturePreviews(DMaterial& Material) -> void
	{
		auto* Backend = Mona::GetActiveUIBackend();
		if (!Backend) { TexturePreviews.reset(); return; }
		if (!TexturePreviews) TexturePreviews = std::make_shared<FTexturePreviewState>();
		std::unordered_map<FGuid, std::shared_ptr<FRegisteredPreview>> Current;
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
		{
			const auto* Parameter = Cast<DMaterialExpressionTextureParameter>(Expression.Get());
			if (!Parameter || !Parameter->DefaultValue.Texture.IsValid()) continue;
			auto* Asset = Parameter->DefaultValue.Texture.Get();
			if (Asset->IsResourceUpdatePending() || !Asset->HasUsableResource()) continue;
			auto Allocation = Asset->GetPublishedTexture();
			if (!Allocation) continue;
			auto& Entry = Registrations[Allocation.GetReference()];
			auto Shared = Entry.lock();
			if (!Shared || Shared->Backend != Backend)
			{
				Shared = std::make_shared<FRegisteredPreview>();
				Shared->Backend = Backend;
				Entry = Shared;
			}
			Shared->Preview.SetTexture(Allocation, 256, 256, {.Usage = Asset->GetUsage()});
			Current.emplace(Expression->Id, std::move(Shared));
		}
		TexturePreviews->Nodes = std::move(Current);
		std::erase_if(Registrations, [](const auto& Entry) { return Entry.second.expired(); });
	}
	auto FMaterialGraphCanvas::DrawTexturePreview(const FGuid& NodeId, const ImVec2& Position, float Size) -> void
	{
		if (TexturePreviews)
			if (const auto Found = TexturePreviews->Nodes.find(NodeId); Found != TexturePreviews->Nodes.end())
			{
				auto* Backend = Mona::GetActiveUIBackend();
				auto* Texture = Found->second->Preview.GetTexture();
				if (Backend && Texture)
				{
					const auto Cursor = ImGui::GetCursorScreenPos();
					ImGui::SetCursorScreenPos(Position);
					const bool bDrawn = Backend->DrawImage(Texture, {Size, Size});
					ImGui::SetCursorScreenPos(Cursor);
					ImGui::Dummy({0.0f, 0.0f});
					if (bDrawn) return;
				}
			}
		ImGui::GetWindowDrawList()->AddRectFilled(Position, {Position.x + Size, Position.y + Size}, IM_COL32(62, 67, 76, 255), 3);
		ImGui::GetWindowDrawList()->AddText({Position.x + 5, Position.y + Size * .4f}, IM_COL32(180, 187, 200, 255), "Texture");
	}
	auto FMaterialGraphCanvas::AcceptTextureDrop(DMaterial& Material, DTransactor& Transactions,
		const ImVec2& CanvasMinimum, const FReportError& ReportError) -> void
	{
		if (!ImGui::BeginDragDropTarget()) return;
		if (const auto* Payload = ImGui::AcceptDragDropPayload(AssetDragDropPayloadType))
			if (Payload->DataSize == sizeof(FAssetDragDropPayload))
			{
				const auto& Asset = *static_cast<const FAssetDragDropPayload*>(Payload->Data);
				FObjectPath Path;
				DTexture2D* Texture = nullptr;
				if (FObjectPath::TryCreate(Asset.AssetPath.data(), Path) && LoadObject(Path, Texture) && Texture)
				{
					GraphEditInternals::FGraphEditSession State(Material);
					{
						TStrongObjectPtr<DMaterialExpressionTextureSampleParameter2D> Expression(
							NewObject<DMaterialExpressionTextureSampleParameter2D>(nullptr, NAME_None));
						Expression->Id = FGuid::NewGuid();
						Expression->Metadata.Id = FGuid::NewGuid();
						const auto BaseName = Texture->GetName();
						Expression->Metadata.Name = FName(BaseName);
						for (uint32 Suffix = 1; Material.FindParameterDefinition(Expression->Metadata.Name); ++Suffix)
							Expression->Metadata.Name = FName(std::format("{}{}", BaseName, Suffix));
						Expression->Metadata.DisplayName = Expression->Metadata.Name.ToString();
						Expression->DefaultValue.Texture = Texture;
						Expression->TextureUsage = Texture->GetUsage();
						if (Texture->GetUsage() == ETextureUsage::Normal)
							Expression->DefaultValue.TextureFallback = EMaterialTextureFallback::FlatRGNormal;
						const auto Id = Expression->Id;
						State.Expressions.emplace_back(Expression.Get());
						const auto Mouse = ImGui::GetMousePos();
						State.Presentation.Nodes.push_back({Id, static_cast<int32>((Mouse.x - CanvasMinimum.x - Pan.x) / Zoom),
							static_cast<int32>((Mouse.y - CanvasMinimum.y - Pan.y) / Zoom)});
						const auto Result = State.Commit("Add Texture Sample Parameter", &Transactions);
						if (!Result) ReportError(FormatMaterialGraphCommandResult(Result));
						else SelectedNodes = {Id};
					}
				}
				else ReportError("Drop a Texture2D asset to create a sample parameter.");
			}
		ImGui::EndDragDropTarget();
	}
}
