#include "RoadNet/RoadSurface.h"
#include "Math/Operations.h"

namespace Durin::RoadNet
{
	namespace
	{
		constexpr double PositionTolerance = 1.e-3;
		constexpr double FrameTolerance = 1.e-3;

		auto SurfaceUp(const FRoadSurface& Surface, const FVector3& Position, FVector3& Up) -> bool
		{
			return Math::TryNormalize(Surface.Mode == ERoadSurfaceMode::Sphere
				? Position - Surface.Origin : Surface.Normal, Up, 1.e-18);
		}

		auto FrameAt(const FRoadSurface& Surface, const FVector3& Position,
			const FVector3& Derivative, FSplineMeshFrame& Frame) -> bool
		{
			FVector3 ReferenceUp;
			if (!SurfaceUp(Surface, Position, ReferenceUp)
				|| !Math::TryNormalize(Derivative, Frame.Forward, 1.e-18)
				|| !Math::TryNormalize(Math::Cross(ReferenceUp, Frame.Forward), Frame.Side, 1.e-18)) return false;
			Frame.Up = Math::Cross(Frame.Forward, Frame.Side);
			return std::abs(Math::Dot(Frame.Forward, Frame.Up)) <= 1.e-8
				&& std::abs(Math::Length(Frame.Up) - 1.0) <= 1.e-8;
		}

		auto Project(const FRoadSurface& Surface, FSplineSample& Sample) -> bool
		{
			if (Surface.Mode == ERoadSurfaceMode::Plane)
			{
				const auto N = Math::Normalize(Surface.Normal);
				Sample.Position += N * (Surface.ElevationMeters - Math::Dot(Sample.Position - Surface.Origin, N));
				Sample.FirstDerivative -= N * Math::Dot(Sample.FirstDerivative, N);
			}
			else if (Surface.Mode == ERoadSurfaceMode::Sphere)
			{
				const auto V = Sample.Position - Surface.Origin;
				const double Length = Math::Length(V);
				if (!std::isfinite(Length) || Length <= 1.e-6) return false;
				const auto N = V / Length;
				const double Radius = Surface.RadiusMeters + Surface.ElevationMeters;
				Sample.Position = Surface.Origin + Radius * N;
				Sample.FirstDerivative = (Sample.FirstDerivative - N * Math::Dot(N, Sample.FirstDerivative)) * (Radius / Length);
			}
			return Math::IsFinite(Sample.Position) && Math::IsFinite(Sample.FirstDerivative)
				&& Math::Length(Sample.FirstDerivative) > 1.e-9;
		}

		auto ValidateSurface(const FRoadSurface& Surface, std::string& Error) -> bool
		{
			FVector3 N;
			if (Surface.Mode > ERoadSurfaceMode::Sphere
				|| (Surface.Mode != ERoadSurfaceMode::Unconstrained && (!Math::IsFinite(Surface.Origin)
				|| std::max({std::abs(Surface.Origin.x), std::abs(Surface.Origin.y), std::abs(Surface.Origin.z)}) > RoadCoordinateLimit
				|| !std::isfinite(Surface.ElevationMeters) || std::abs(Surface.ElevationMeters) > RoadCoordinateLimit))
				|| (Surface.Mode != ERoadSurfaceMode::Sphere && !Math::TryNormalize(Surface.Normal, N, 1.e-18))
				|| (Surface.Mode == ERoadSurfaceMode::Sphere && (!std::isfinite(Surface.RadiusMeters) || Surface.RadiusMeters < 1.0
					|| Surface.RadiusMeters > RoadCoordinateLimit || Surface.RadiusMeters + Surface.ElevationMeters < 1.0
					|| Surface.RadiusMeters + Surface.ElevationMeters > RoadCoordinateLimit)))
			{
				Error = "Road surface has unsupported mode, coordinates, normal or radius.";
				return false;
			}
			return true;
		}
	}

	auto ValidateRoadPlacement(const FTransform& Placement, std::string& OutError) -> bool
	{
		if (Placement.Scale3D != FVector3(1.0) || !Math::IsFinite(Placement.Translation)
			|| !std::isfinite(Placement.Rotation.w) || !std::isfinite(Placement.Rotation.x)
			|| !std::isfinite(Placement.Rotation.y) || !std::isfinite(Placement.Rotation.z)
			|| std::abs(Placement.Rotation.w * Placement.Rotation.w + Placement.Rotation.x * Placement.Rotation.x
				+ Placement.Rotation.y * Placement.Rotation.y + Placement.Rotation.z * Placement.Rotation.z - 1.0) > 1.e-8)
		{
			OutError = "Road placement requires finite translation, a unit rotation and identity scale.";
			return false;
		}
		OutError.clear();
		return true;
	}

	namespace
	{
		auto BuildIntervals(const FRoad& Road, const FRoadSurface& InSurface, bool bProject,
			std::vector<FRoadInterval>& Intervals, std::string& OutError) -> bool
		{
			if (!ValidateSurface(InSurface, OutError) || Road.ReferenceLine.GetNumPoints() < 2) return false;
			const auto Source = Road.ReferenceLine.BuildEvaluationData();
			auto Fail = [&](std::string_view Reason) {
				OutError = std::format("Road '{}' curve intervals: {}", Road.Id.ToString(), Reason);
				return false;
			};
			for (uint32 Segment = 0; Segment < Source->GetNumSegments(); ++Segment)
			{
				std::function<bool(double, double, uint32, uint32)> Fit;
				Fit = [&](double A, double B, uint32 Depth, uint32 Key) -> bool {
					auto Start = Source->Evaluate({Segment, A});
					auto End = Source->Evaluate({Segment, B});
					if (bProject && (!Project(InSurface, Start) || !Project(InSurface, End))) return Fail("singular projection or derivative.");
					FSplineMeshFrame StartFrame, EndFrame;
					if (!FrameAt(InSurface, Start.Position, Start.FirstDerivative, StartFrame)
						|| !FrameAt(InSurface, End.Position, End.FirstDerivative, EndFrame)) return Fail("degenerate reference up.");
					FSplineMeshParams Params;
					Params.StartPosition = Start.Position;
					Params.EndPosition = End.Position;
					Params.StartTangent = Start.FirstDerivative * (B - A);
					Params.EndTangent = End.FirstDerivative * (B - A);
					Params.SplineUpDirection = StartFrame.Up;
					const auto BaseEnd = FSplineMeshDeformer::Evaluate(Params, 1.0);
					Params.EndRollRadians = std::atan2(Math::Dot(EndFrame.Forward, Math::Cross(BaseEnd.Frame.Up, EndFrame.Up)),
						Math::Dot(BaseEnd.Frame.Up, EndFrame.Up));
					bool Split = Math::Dot(StartFrame.Forward, EndFrame.Forward) <= 0.0 || Math::Dot(StartFrame.Up, EndFrame.Up) <= 0.0;
					FVector3 Previous = Start.Position;
					double PolylineLength = 0.0;
					std::array<double, 5> Distances{};
					double SpeedIntegral = Math::Length(Start.FirstDerivative) + Math::Length(End.FirstDerivative);
					for (int Index = 1; Index <= 4; ++Index)
					{
						const double T = Index * 0.25;
						auto Exact = Source->Evaluate({Segment, A + (B - A) * T});
						if (bProject && !Project(InSurface, Exact)) return Fail("singular interior projection or derivative.");
						FSplineMeshFrame ExactFrame;
						if (!FrameAt(InSurface, Exact.Position, Exact.FirstDerivative, ExactFrame)) return Fail("singular interior reference up.");
						const auto Approx = FSplineMeshDeformer::Evaluate(Params, T);
						Split |= Math::Length(Approx.Position - Exact.Position) > PositionTolerance
							|| Math::Dot(Approx.Frame.Up, ExactFrame.Up) < std::cos(FrameTolerance)
							|| Math::Dot(Approx.Frame.Forward, ExactFrame.Forward) < std::cos(FrameTolerance);
						PolylineLength += Math::Length(Exact.Position - Previous);
						Distances[Index] = PolylineLength;
						Previous = Exact.Position;
						if (Index < 4) SpeedIntegral += (Index == 2 ? 2.0 : 4.0) * Math::Length(Exact.FirstDerivative);
					}
					SpeedIntegral *= (B - A) / 12.0;
					// Distance-table interpolation must also resolve nonuniform speed on straight cubics.
					Split |= bProject && (std::abs(Distances[1] - Distances[2] * 0.5) > 1.e-5
						|| std::abs(Distances[3] - (Distances[2] + Distances[4]) * 0.5) > 1.e-5);
					// Reserve headroom for the spline foundation's two-chord distance leaves.
					Split |= bProject && std::abs(SpeedIntegral - PolylineLength) > 1.e-5 * (B - A) / Source->GetNumSegments() + 1.e-7 * SpeedIntegral;
					// Bernstein coefficients bound squared radius over the entire cubic,
					// including between the error probes (convex-hull property).
					if (bProject && InSurface.Mode == ERoadSurfaceMode::Sphere)
					{
						const std::array<FVector3, 4> C{Params.StartPosition - InSurface.Origin,
							Params.StartPosition + Params.StartTangent / 3.0 - InSurface.Origin,
							Params.EndPosition - Params.EndTangent / 3.0 - InSurface.Origin,
							Params.EndPosition - InSurface.Origin};
						constexpr int Choose3[]{1, 3, 3, 1}, Choose6[]{1, 6, 15, 20, 15, 6, 1};
						const double R = InSurface.RadiusMeters + InSurface.ElevationMeters;
						for (int K = 0; K <= 6; ++K)
						{
							double Coefficient = 0;
							for (int I = std::max(0, K - 3); I <= std::min(3, K); ++I)
								Coefficient += Math::Dot(C[I], C[K - I]) * Choose3[I] * Choose3[K - I] / Choose6[K];
							Split |= Coefficient < (R - PositionTolerance) * (R - PositionTolerance)
								|| Coefficient > (R + PositionTolerance) * (R + PositionTolerance);
						}
					}
					if (Split)
					{
						if (Depth == 16) return Fail("maximum subdivision depth exceeded.");
						const double Mid = (A + B) * 0.5;
						return Fit(A, Mid, Depth + 1, Key * 2) && Fit(Mid, B, Depth + 1, Key * 2 + 1);
					}
					if (Intervals.size() == 65536) return Fail("maximum interval count exceeded.");
					Intervals.push_back({Road.ReferenceLine.GetPoints()[Segment].Id, Key, Params, Segment, A, B});
					return true;
				};
				if (!Fit(0.0, 1.0, 0, 1)) return false;
			}
			return true;
		}

		auto FitCurve(FRoad& Road, const FRoadSurface& Surface, std::string& Error) -> bool
		{
			if (Surface.Mode == ERoadSurfaceMode::Unconstrained) return true;
			std::vector<FRoadInterval> Intervals;
			if (!BuildIntervals(Road, Surface, true, Intervals, Error)) return false;
			FSplineCurve FinalCurve;

			std::vector<FSplinePoint> Points;
			for (const auto& Interval : Intervals)
			{
				FSplinePoint Point(Interval.Params.StartPosition);
				if (Points.empty() || Interval.SourcePointId != Intervals[Points.size() - 1].SourcePointId)
					Point.Id = Interval.SourcePointId;
				Point.TangentMode = ESplineTangentMode::ManualBroken;
				Point.LeaveTangent = Interval.Params.StartTangent;
				Point.ArriveTangent = Points.empty() ? Point.LeaveTangent : Intervals[Points.size() - 1].Params.EndTangent;
				Points.push_back(Point);
			}
			FSplinePoint Last(Intervals.back().Params.EndPosition);
			Last.Id = Road.ReferenceLine.GetPoints().back().Id;
			Last.TangentMode = ESplineTangentMode::ManualBroken;
			Last.ArriveTangent = Intervals.back().Params.EndTangent;
			Last.LeaveTangent = Last.ArriveTangent;
			Points.push_back(Last);
			FinalCurve.SetPoints(std::move(Points));
			Road.ReferenceLine = std::move(FinalCurve);
			return true;
		}
	}

	auto FitRoadDefinition(FDefinition& Definition, const FRoadSurface& Operation, std::string& OutError) -> bool
	{
		if (!ValidateDefinition(Definition, OutError) || !ValidateSurface(Operation, OutError)) return false;
		auto Candidate = Definition;
		for (auto& Road : Candidate.Roads)
		{
			const double OldLength = Road.ReferenceLine.BuildEvaluationData()->GetLocalLength();
			if (!FitCurve(Road, Operation, OutError)) return false;
			const double NewLength = Road.ReferenceLine.BuildEvaluationData()->GetLocalLength();
			for (auto& Section : Road.LaneSections)
			{
				Section.StartDistanceMeters *= NewLength / OldLength;
				Section.EndDistanceMeters *= NewLength / OldLength;
			}
			Road.LaneSections.back().EndDistanceMeters = NewLength;
		}
		for (auto& Node : Candidate.Nodes)
		{
			// Nodes need positions only: a projection derivative is irrelevant here.
			if (Operation.Mode == ERoadSurfaceMode::Plane)
			{
				const auto N = Math::Normalize(Operation.Normal);
				Node.Position += N * (Operation.ElevationMeters - Math::Dot(Node.Position - Operation.Origin, N));
			}
			else if (Operation.Mode == ERoadSurfaceMode::Sphere)
			{
				FVector3 N;
				if (!Math::TryNormalize(Node.Position - Operation.Origin, N, 1.e-12))
				{ OutError = std::format("Node '{}' is at the sphere center.", Node.Id.ToString()); return false; }
				Node.Position = Operation.Origin + N * (Operation.RadiusMeters + Operation.ElevationMeters);
			}
		}
		for (auto& Junction : Candidate.Junctions)
			for (auto& Connection : Junction.LaneConnections)
				if (Connection.ConnectorCurve.GetNumPoints() != 0)
				{
					FRoad Connector;
					Connector.Id = Connection.Id;
					Connector.ReferenceLine = Connection.ConnectorCurve;
					if (!FitCurve(Connector, Operation, OutError)) return false;
					Connection.ConnectorCurve = std::move(Connector.ReferenceLine);
				}
		Candidate.Planet.bRadialUp = Operation.Mode == ERoadSurfaceMode::Sphere;
		Candidate.Planet.ReferenceUp = Operation.Normal;
		if (Candidate.Planet.bRadialUp)
		{
			Candidate.Planet.Center = Operation.Origin;
			Candidate.Planet.RadiusMeters = Operation.RadiusMeters;
		}
		if (!ValidateDefinition(Candidate, OutError)) return false;
		Definition = std::move(Candidate);
		return true;
	}

	auto FRoadAlignment::Build(const FRoad& Road, const FRoadPlanet& Planet,
		std::shared_ptr<const FRoadAlignment>& OutSnapshot, std::string& OutError) -> bool
	{
		FDefinition Definition;
		Definition.Planet = Planet;
		Definition.Roads.push_back(Road);
		if (Road.ReferenceLine.GetNumPoints() < 2) { OutError = "Road requires at least two points."; return false; }
		Definition.Nodes.push_back({.Id = Road.StartNodeId, .Position = Road.ReferenceLine.GetPoints().front().Position});
		if (Road.EndNodeId != Road.StartNodeId)
			Definition.Nodes.push_back({.Id = Road.EndNodeId, .Position = Road.ReferenceLine.GetPoints().back().Position});
		if (!ValidateDefinition(Definition, OutError)) return false;
		auto Result = std::make_shared<FRoadAlignment>();
		Result->Planet = Planet;
		Result->Sections = Road.LaneSections;
		FRoadSurface Frame;
		Frame.Mode = Planet.bRadialUp ? ERoadSurfaceMode::Sphere : ERoadSurfaceMode::Unconstrained;
		Frame.Origin = Planet.Center;
		Frame.Normal = Planet.ReferenceUp;
		Frame.RadiusMeters = Planet.RadiusMeters;
		if (!BuildIntervals(Road, Frame, false, Result->Intervals, OutError)) return false;
		// Preserve the exact authored curve and its one canonical distance table.
		Result->Evaluation = Road.ReferenceLine.BuildEvaluationData();
		OutSnapshot = std::move(Result);
		OutError.clear();
		return true;
	}

	auto FRoadAlignment::Sample(double DistanceMeters, FRoadSample& OutSample, std::string& OutError) const -> bool
	{
		if (!std::isfinite(DistanceMeters) || DistanceMeters < 0.0 || DistanceMeters > GetLengthMeters())
		{
			OutError = "Road station is outside the final evaluated interval.";
			return false;
		}
		const auto Value = Evaluation->EvaluateAtLocalDistance(DistanceMeters);
		FRoadSample Result;
		Result.DistanceMeters = DistanceMeters;
		Result.Position = Value.Position;
		const auto Parameter = Evaluation->GetParameterAtLocalDistance(DistanceMeters);
		const auto Interval = std::ranges::find_if(Intervals, [&](const FRoadInterval& Item) {
			return Item.SegmentIndex == Parameter.SegmentIndex && Parameter.T <= Item.EndT;
		});
		if (Interval == Intervals.end()) { OutError = "Missing preview interval."; return false; }
		Result.Frame = FSplineMeshDeformer::Evaluate(Interval->Params,
			(Parameter.T - Interval->StartT) / (Interval->EndT - Interval->StartT)).Frame;
		const FLaneSection* Section = &Sections.back();
		for (const auto& Candidate : Sections)
			if (DistanceMeters < Candidate.EndDistanceMeters) { Section = &Candidate; break; }
		std::vector<const FLane*> Lanes;
		for (const auto& Lane : Section->Lanes) Lanes.push_back(&Lane);
		std::ranges::sort(Lanes, [](const auto* A, const auto* B) {
			return std::abs(static_cast<int64>(A->Index)) < std::abs(static_cast<int64>(B->Index));
		});
		double Left = 0.0, Right = 0.0;
		for (const auto* Lane : Lanes)
		{
			if (Lane->Index > 0)
			{
				Result.Lanes.push_back({Lane->Id, Right, Right + Lane->WidthMeters});
				Right += Lane->WidthMeters;
			}
			else
			{
				Result.Lanes.push_back({Lane->Id, Left - Lane->WidthMeters, Left});
				Left -= Lane->WidthMeters;
			}
		}
		OutSample = std::move(Result);
		OutError.clear();
		return true;
	}

	auto FRoadAlignment::SampleWorld(double DistanceMeters, const FTransform& Placement,
		FRoadSample& OutSample, std::string& OutError) const -> bool
	{
		if (!ValidateRoadPlacement(Placement, OutError)) return false;
		FRoadSample Result;
		if (!Sample(DistanceMeters, Result, OutError)) return false;
		Result.Position = Placement.Rotation * Result.Position + Placement.Translation;
		Result.Frame.Forward = Placement.Rotation * Result.Frame.Forward;
		Result.Frame.Side = Placement.Rotation * Result.Frame.Side;
		Result.Frame.Up = Placement.Rotation * Result.Frame.Up;
		OutSample = std::move(Result);
		return true;
	}
}
