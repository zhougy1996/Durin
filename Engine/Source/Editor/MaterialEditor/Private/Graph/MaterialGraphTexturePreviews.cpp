#include "MaterialGraphCanvas.h"
#include "MaterialGraphExpressionState.h"
#include "Asset/Asset.h"
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
			FTextureRHIRef Texture;
			Mona::IMonaUIBackend* Backend = nullptr;
			bool bOwned = false;
			~FRegisteredPreview()
			{
				if (bOwned && Backend == Mona::GetActiveUIBackend()) Backend->UnregisterTexture(Texture);
			}
		};
		// Multiple open documents may display the same published allocation.
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
			auto Allocation = Parameter->DefaultValue.Texture->GetPublishedTexture();
			if (!Allocation) continue;
			auto& Entry = Registrations[Allocation.GetReference()];
			auto Shared = Entry.lock();
			if (!Shared || Shared->Backend != Backend)
			{
				Shared = std::make_shared<FRegisteredPreview>();
				Shared->Texture = Allocation;
				Shared->Backend = Backend;
				Shared->bOwned = !Backend->IsTextureRegistered(Allocation.GetReference());
				if (Shared->bOwned) Backend->RegisterTexture(Allocation);
				Entry = Shared;
			}
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
				const auto Cursor = ImGui::GetCursorScreenPos();
				ImGui::SetCursorScreenPos(Position);
				Found->second->Backend->DrawImage(Found->second->Texture.GetReference(), {Size, Size});
				ImGui::SetCursorScreenPos(Cursor);
				ImGui::Dummy({0.0f, 0.0f});
				return;
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
				std::string Error;
				DTexture2D* Texture = nullptr;
				if (FObjectPath::TryCreate(Asset.AssetPath.data(), Path, &Error) && LoadObject(Path, Texture) && Texture)
				{
					GraphEditInternals::FOwnedGraphSnapshot State;
					if (State.Capture(Material))
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
						const auto Id = Expression->Id;
						State.Expressions.emplace_back(Expression.Get());
						const auto Mouse = ImGui::GetMousePos();
						State.Presentation.Nodes.push_back({Id, static_cast<int32>((Mouse.x - CanvasMinimum.x - Pan.x) / Zoom),
							static_cast<int32>((Mouse.y - CanvasMinimum.y - Pan.y) / Zoom)});
						const auto Result = GraphEditInternals::CommitOwnedExpressions(Material, std::move(State), "Add Texture Sample Parameter", &Transactions);
						if (!Result) ReportError(Result.Message);
						else SelectedNodes = {Id};
					}
				}
				else ReportError(Error.empty() ? "Drop a Texture2D asset to create a sample parameter." : Error);
			}
		ImGui::EndDragDropTarget();
	}
}
