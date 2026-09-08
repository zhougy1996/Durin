#include "NamePoolDiagnosticsWindow.h"
#include "MonaImGui.h"

namespace Durin::Editor::MainFrame
{
	namespace
	{
		auto DrawShards(const char* Label, const std::vector<FNamePoolShardStats>& Shards) -> void
		{
			uint64 Created = 0, Used = 0, Capacity = 0;
			for (const auto& Shard : Shards)
			{
				Created += Shard.CreatedEntries;
				Used += Shard.UsedSlots;
				Capacity += Shard.Capacity;
			}
			ImGui::Text("%s: %llu entries, %llu / %llu slots (%.1f%%)", Label,
				static_cast<unsigned long long>(Created), static_cast<unsigned long long>(Used),
				static_cast<unsigned long long>(Capacity), Capacity ? 100.0 * Used / Capacity : 0.0);
			if (!ImGui::TreeNode(Label)) return;
			if (ImGui::BeginTable("Shards", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
				| ImGuiTableFlags_ScrollY, ImVec2(0, MonaImGui::ScaleUI(240.0f))))
			{
				ImGui::TableSetupColumn("Shard");
				ImGui::TableSetupColumn("Created entries");
				ImGui::TableSetupColumn("Used / capacity");
				ImGui::TableSetupColumn("Load");
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableHeadersRow();
				ImGuiListClipper Clipper;
				Clipper.Begin(static_cast<int>(Shards.size()));
				while (Clipper.Step())
					for (int Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
					{
						const auto& Shard = Shards[Index];
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::Text("%d", Index);
						ImGui::TableNextColumn(); ImGui::Text("%u", Shard.CreatedEntries);
						ImGui::TableNextColumn(); ImGui::Text("%u / %u", Shard.UsedSlots, Shard.Capacity);
						ImGui::TableNextColumn(); ImGui::Text("%.1f%%", Shard.Capacity ? 100.0 * Shard.UsedSlots / Shard.Capacity : 0.0);
					}
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}
	}

	auto FNamePoolDiagnosticsWindow::Draw(bool& bOpen) -> void
	{
		if (!bOpen) { bWasVisible = false; return; }
		ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(720.0f), MonaImGui::ScaleUI(580.0f)), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Name Pool###Durin.NamePoolDiagnostics", &bOpen))
		{
			bWasVisible = false;
			ImGui::End();
			return;
		}
		const double Now = ImGui::GetTime();
		bool bRefresh = ImGui::Button("Refresh");
		ImGui::SameLine();
		ImGui::Checkbox("Auto refresh (1 s)", &bAutoRefresh);
		if (!bWasVisible || bRefresh || (bAutoRefresh && Now - LastSampleTime >= 1.0))
		{
			auto Next = GetNamePoolStats();
			EntriesPerSecond = bWasVisible && LastSampleTime >= 0.0 && Now > LastSampleTime
				? double(Next.CreatedEntries - Stats.CreatedEntries) / (Now - LastSampleTime) : 0.0;
			Stats = std::move(Next);
			LastSampleTime = Now;
		}
		bWasVisible = true;
		ImGui::TextDisabled("Process-wide snapshot; shards are sampled independently. Age: %.1f s", Now - LastSampleTime);
		ImGui::Separator();
		ImGui::Text("Created entries: %llu   |   Growth: %.1f entries/s",
			static_cast<unsigned long long>(Stats.CreatedEntries), EntriesPerSecond);
		ImGui::Text("Blocks: %u active / %u allocated / %u maximum (%u KiB each)",
			Stats.ActiveBlocks, Stats.AllocatedBlocks, Stats.MaxBlocks, Stats.BlockSizeBytes / 1024);
		ImGui::Text("Entry storage: %.2f MiB   |   Allocated blocks: %.2f MiB",
			Stats.EntryBytes / 1048576.0, Stats.AllocatedBlockBytes / 1048576.0);
		ImGui::Text("Hash slots: %.2f MiB", Stats.SlotBytes / 1048576.0);
		ImGui::TextWrapped("Entry storage includes headers and alignment. Block allocation includes spare capacity; hash slots are separate. Display lookups can reuse comparison entries.");
		ImGui::Separator();
		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(160.0f));
		ImGui::InputInt("Target total blocks", &TargetBlocks);
		TargetBlocks = std::clamp(TargetBlocks, 1, static_cast<int>(Stats.MaxBlocks));
		ImGui::Text("Target capacity: %.2f MiB", double(TargetBlocks) * Stats.BlockSizeBytes / 1048576.0);
		ImGui::BeginDisabled(static_cast<uint32>(TargetBlocks) <= Stats.AllocatedBlocks);
		if (ImGui::Button("Preallocate blocks"))
		{
			ReserveNamePoolBlocks(static_cast<uint32>(TargetBlocks));
			// Refresh on the next draw without including an artificial near-zero rate interval.
			bWasVisible = false;
		}
		ImGui::EndDisabled();
		ImGui::TextWrapped("Preallocation reserves entry storage for future names. It does not create names or prefill hash tables. Capacity remains allocated until process exit.");
		ImGui::Separator();
		DrawShards("Comparison", Stats.ComparisonShards);
		DrawShards("Display", Stats.DisplayShards);
		ImGui::End();
	}
}
