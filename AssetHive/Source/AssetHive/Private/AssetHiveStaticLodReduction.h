#pragma once

#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "OverlappingCorners.h"
#include "IMeshReductionInterfaces.h"
#include "IMeshReductionManagerModule.h"
#include "AssetHiveLodQuality.h"

namespace AssetHiveStaticLodReduction
{
inline TArray<AssetHiveLodQuality::FPart> Describe(const FMeshDescription& Mesh, TArray<FTriangleID>* TriangleIds = nullptr)
{
    TArray<FVector3f> Positions;
    Positions.SetNum(Mesh.Vertices().GetArraySize());
    const auto VertexPositions = FStaticMeshConstAttributes(Mesh).GetVertexPositions();
    for (FVertexID V : Mesh.Vertices().GetElementIDs()) Positions[V.GetValue()] = VertexPositions[V];
    TArray<uint32> Indices;
    for (FTriangleID T : Mesh.Triangles().GetElementIDs()) {
        if (TriangleIds) TriangleIds->Add(T);
        for (FVertexID V : Mesh.GetTriangleVertices(T)) Indices.Add(V.GetValue());
    }
    return AssetHiveLodQuality::Describe(Positions, Indices);
}

inline FMeshDescription Extract(const FMeshDescription& Source, const TSet<FPolygonID>& Keep)
{
    FMeshDescription Result(Source);
    TArray<FPolygonID> Remove;
    for (FPolygonID P : Result.Polygons().GetElementIDs()) if (!Keep.Contains(P)) Remove.Add(P);
    Result.DeletePolygons(Remove);
    FElementIDRemappings Remap;
    Result.Compact(Remap);
    return Result;
}

// Reduce connected pieces independently. A chain link that cannot be safely
// simplified must not force the entire garment back to the artist source.
inline bool Reduce(const FMeshDescription& Source, float Ratio, FMeshDescription& Result)
{
    auto& Manager = FModuleManager::LoadModuleChecked<IMeshReductionManagerModule>(TEXT("MeshReductionInterface"));
    IMeshReduction* Reducer = Manager.GetStaticMeshReductionInterface();
    if (!Reducer) return false;
    TArray<FTriangleID> TriangleIds;
    const auto Parts = Describe(Source, &TriangleIds);
    if (Parts.IsEmpty()) return false;
    FStaticMeshAttributes(Result).Register();
    FStaticMeshOperations::FAppendSettings Append;
    for (bool& MergeUV : Append.bMergeUVChannels) MergeUV = true;
    TSet<FPolygonID> Protected;
    for (const auto& Part : Parts) {
        TSet<FPolygonID> Polygons;
        for (int32 Offset : Part.TriangleOffsets) Polygons.Add(Source.GetTrianglePolygon(TriangleIds[Offset]));
        // Small pieces are inexpensive and disproportionately important to the
        // silhouette. Keep their exact source attributes, including all UVs.
        if (Part.Faces <= 128) { Protected.Append(Polygons); continue; }
        const FMeshDescription Piece = Extract(Source, Polygons);
        const auto Original = Describe(Piece);
        FOverlappingCorners Overlaps;
        FStaticMeshOperations::FindOverlappingCorners(Overlaps, Piece, 0.f);
        bool bAccepted = false;
        // A shared ladder makes successive LOD candidate sets nested, avoiding
        // a coarser level becoming heavier just because its retry steps differ.
        for (float Keep : {.0625f, .125f, .25f, .5f, .65f, .8f, .9f, .95f}) {
            if (Keep < Ratio) continue;
            FMeshReductionSettings Settings;
            Settings.PercentTriangles = Keep;
            Settings.WeldingThreshold = 0.f;
            Settings.SilhouetteImportance = EMeshFeatureImportance::Highest;
            Settings.TextureImportance = EMeshFeatureImportance::Highest;
            Settings.ShadingImportance = EMeshFeatureImportance::High;
            FMeshDescription Reduced;
            FStaticMeshAttributes(Reduced).Register();
            float Deviation = 0.f;
            Reducer->ReduceMeshDescription(Reduced, Deviation, Piece, Overlaps, Settings);
            const double Limit = FMath::Max(.0001, Part.Bounds.GetSize().Length() * .01);
            if (FMath::IsFinite(Deviation) && Deviation <= Limit
                && AssetHiveLodQuality::PreservesStructure(Original, Describe(Reduced))) {
                FStaticMeshOperations::AppendMeshDescription(Reduced, Result, Append);
                bAccepted = true; break;
            }
        }
        if (!bAccepted) Protected.Append(Polygons);
    }
    if (!Protected.IsEmpty()) FStaticMeshOperations::AppendMeshDescription(Extract(Source, Protected), Result, Append);
    return AssetHiveLodQuality::PreservesStructure(Parts, Describe(Result));
}
}
