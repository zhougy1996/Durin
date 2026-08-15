#include "Settings/LevelViewportSessionSettings.h"

#include "Yaml/Yaml.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto ReadVector(const FYamlNodeView& Node, FVector3& OutValue) -> bool
		{
			if (!Node.IsSequence() || Node.Num() != 3) return false;
			double Values[3];
			for (size_t Index = 0; Index < 3; ++Index)
			{
				if (!Node.GetView(Index).GetValue(Values[Index]) || !std::isfinite(Values[Index])) return false;
			}
			OutValue = {Values[0], Values[1], Values[2]};
			return true;
		}

		auto WriteVector(FYamlNodeRef Parent, std::string_view Key, const FVector3& Value) -> void
		{
			Parent.AddSequence(Key).AppendValue(Value.x).AppendValue(Value.y).AppendValue(Value.z);
		}
	}

	auto LoadLevelViewportStates(const FYamlNodeView& Root, FLevelViewportStateMap& OutStates) -> void
	{
		OutStates.clear();
		const FYamlNodeView Entries = Root.GetView("LevelViewportStates");
		if (!Entries.IsSequence()) return;
		for (size_t Index = 0; Index < Entries.Num(); ++Index)
		{
			const FYamlNodeView Entry = Entries.GetView(Index);
			const std::string Project = Entry.GetView("Project").GetString();
			const std::string Level = Entry.GetView("Level").GetString();
			FLevelViewportCameraState State;
			double OrbitDistance = 0.0, Pitch = 0.0, Yaw = 0.0;
			if (Project.empty() || Level.empty() || !ReadVector(Entry.GetView("Location"), State.Location) ||
				!ReadVector(Entry.GetView("OrbitPivot"), State.OrbitPivot) ||
				!Entry.GetView("OrbitDistance").GetValue(OrbitDistance) || !Entry.GetView("Pitch").GetValue(Pitch) ||
				!Entry.GetView("Yaw").GetValue(Yaw) || !std::isfinite(OrbitDistance) || OrbitDistance <= 0.0 ||
				!std::isfinite(Pitch) || !std::isfinite(Yaw)) continue;
			State.OrbitDistance = OrbitDistance;
			State.Pitch = Pitch;
			State.Yaw = Yaw;
			const double NearClip = Entry.GetView("NearClip").GetDouble(State.NearClip);
			const double FarClip = Entry.GetView("FarClip").GetDouble(State.FarClip);
			const double ViewFadeStart = Entry.GetView("ViewFadeStart").GetDouble(State.ViewFadeStart);
			const double ViewRenderDistance = Entry.GetView("ViewRenderDistance").GetDouble(State.ViewRenderDistance);
			if (std::isfinite(NearClip)) State.NearClip = static_cast<float>(NearClip);
			if (std::isfinite(FarClip)) State.FarClip = static_cast<float>(FarClip);
			if (std::isfinite(ViewFadeStart)) State.ViewFadeStart = static_cast<float>(ViewFadeStart);
			if (std::isfinite(ViewRenderDistance)) State.ViewRenderDistance = static_cast<float>(ViewRenderDistance);
			OutStates[Project][Level] = State;
		}
	}

	auto SaveLevelViewportStates(FYamlNodeRef Root, const FLevelViewportStateMap& States) -> void
	{
		FYamlNodeRef Entries = Root.AddSequence("LevelViewportStates");
		std::vector<std::pair<std::string, std::string>> Keys;
		for (const auto& [Project, Levels] : States)
			for (const auto& [Level, State] : Levels) Keys.emplace_back(Project, Level);
		std::ranges::sort(Keys);
		for (const auto& [Project, Level] : Keys)
		{
			const FLevelViewportCameraState& State = States.at(Project).at(Level);
			FYamlNodeRef Entry = Entries.AppendMap();
			Entry.SetChildValue("Project", Project);
			Entry.SetChildValue("Level", Level);
			WriteVector(Entry, "Location", State.Location);
			WriteVector(Entry, "OrbitPivot", State.OrbitPivot);
			Entry.SetChildValue("OrbitDistance", State.OrbitDistance);
			Entry.SetChildValue("Pitch", State.Pitch);
			Entry.SetChildValue("Yaw", State.Yaw);
			Entry.SetChildValue("NearClip", static_cast<double>(State.NearClip));
			Entry.SetChildValue("FarClip", static_cast<double>(State.FarClip));
			Entry.SetChildValue("ViewFadeStart", static_cast<double>(State.ViewFadeStart));
			Entry.SetChildValue("ViewRenderDistance", static_cast<double>(State.ViewRenderDistance));
		}
	}
}
