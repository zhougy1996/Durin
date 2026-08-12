module TerrainVertexFactory;

// Compact patch-local integer grid coordinate supplied by the terrain topology cache.
public struct FTerrainVertexFactoryInput
{
    public uint2 gridCoordinate : POSITION;
};
