#pragma once

// Conservative structural check on rendered geometry. UV/normal seams are
// welded by position for connectivity only; the mesh itself is never welded.
namespace AssetHiveLodQuality
{
struct FPart
{
    FBox Bounds = FBox(ForceInit);
    int32 Vertices = 0, Edges = 0, Faces = 0;
    TArray<int32> TriangleOffsets;
    int32 Euler() const { return Vertices - Edges + Faces; }
};

inline TArray<FPart> Describe(const TArray<FVector3f>& Positions, const TArray<uint32>& Indices)
{
    TMap<FVector3f, int32> Unique;
    TArray<int32> Remap, Parents;
    TArray<FVector3f> Points;
    for (const FVector3f& P : Positions)
    {
        int32* Found = Unique.Find(P);
        if (!Found) {
            const int32 Id = Points.Add(P);
            Unique.Add(P, Id); Parents.Add(Id); Remap.Add(Id);
        } else Remap.Add(*Found);
    }
    auto Root = [&Parents](int32 Id) {
        while (Parents[Id] != Id) { Parents[Id] = Parents[Parents[Id]]; Id = Parents[Id]; }
        return Id;
    };
    TSet<uint64> Edges;
    TSet<int32> Used;
    TArray<FIntVector> Faces;
    TArray<int32> TriangleOffsets;
    for (int32 I = 0; I + 2 < Indices.Num(); I += 3)
    {
        if (!Remap.IsValidIndex(Indices[I]) || !Remap.IsValidIndex(Indices[I+1]) || !Remap.IsValidIndex(Indices[I+2])) return {};
        const FIntVector Face(Remap[Indices[I]], Remap[Indices[I+1]], Remap[Indices[I+2]]);
        if (Face.X == Face.Y || Face.X == Face.Z || Face.Y == Face.Z) continue;
        Faces.Add(Face);
        TriangleOffsets.Add(I / 3);
        for (int32 E = 0; E < 3; ++E) {
            const int32 A = Face[E], B = Face[(E+1)%3];
            Parents[Root(A)] = Root(B); Used.Add(A);
            Edges.Add((uint64(FMath::Min(A, B)) << 32) | uint32(FMath::Max(A, B)));
        }
    }
    TMap<int32, int32> Parts;
    TArray<FPart> Result;
    for (int32 Id : Used) {
        const int32 R = Root(Id);
        if (!Parts.Contains(R)) Parts.Add(R, Result.AddDefaulted());
        FPart& Part = Result[Parts[R]];
        ++Part.Vertices; Part.Bounds += FVector(Points[Id]);
    }
    for (uint64 Edge : Edges) ++Result[Parts[Root(int32(Edge >> 32))]].Edges;
    for (int32 I = 0; I < Faces.Num(); ++I) {
        FPart& Part = Result[Parts[Root(Faces[I].X)]];
        ++Part.Faces; Part.TriangleOffsets.Add(TriangleOffsets[I]);
    }
    return Result;
}

inline bool PreservesStructure(const TArray<FPart>& Source, const TArray<FPart>& Reduced)
{
    if (Source.IsEmpty() || Source.Num() != Reduced.Num()) return false;
    TSet<int32> Matched;
    for (const FPart& Part : Source)
    {
        int32 Best = INDEX_NONE;
        double Distance = TNumericLimits<double>::Max();
        for (int32 I = 0; I < Reduced.Num(); ++I) {
            if (Matched.Contains(I) || Part.Euler() != Reduced[I].Euler()) continue;
            const double D = FVector::DistSquared(Part.Bounds.GetCenter(), Reduced[I].Bounds.GetCenter());
            if (D < Distance) { Best = I; Distance = D; }
        }
        if (Best == INDEX_NONE) return false;
        // Bound displacement relative to EACH component, not the whole mesh:
        // tiny chain links must not disappear inside a large costume's budget.
        const double Tolerance = FMath::Max(0.0001, Part.Bounds.GetSize().Length() * 0.025);
        if (!Part.Bounds.Min.Equals(Reduced[Best].Bounds.Min, Tolerance)
            || !Part.Bounds.Max.Equals(Reduced[Best].Bounds.Max, Tolerance)) return false;
        Matched.Add(Best);
    }
    return true;
}
}
