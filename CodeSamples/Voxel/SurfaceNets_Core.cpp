/*
 * Portfolio excerpt — not a standalone translation unit.
 * Production source: AGP_VoxelChunkActor::GenerateRenderBuffers() and
 * AGP_VoxelChunkActor::BuildCollisionOnlyRegion().
 *
 * The excerpt keeps dual-cell vertex generation, one representative
 * surface-crossing loop, and the separate greedy collision mesh.
 * Setup/accessor helpers, Y/Z crossing loops, triangulation validation,
 * UV/tangent work, and ProceduralMesh application are omitted.
 */

// -----------------------------------------------------------------------------
// 1. One Surface Nets vertex per mixed-occupancy dual cell
// -----------------------------------------------------------------------------

bool AGP_VoxelChunkActor::GenerateRenderBuffers(
    const FGP_VoxelGrid& GlobalGrid,
    const FIntVector& RegionOrigin,
    const FIntVector& RegionSize,
    float VoxelSize,
    const TMap<EGP_VoxelId, FLinearColor>& InOreTintMap,
    const FVector& ChunkWorldOrigin,
    FGP_ChunkRenderBuffers& OutBuffers)
{
    // [Omitted: validation, output arrays, and grid/accessor helpers.]

    const int32 CellDimX = RegionSize.X + 1;
    const int32 CellDimY = RegionSize.Y + 1;
    const int32 CellDimZ = RegionSize.Z + 1;

    TArray<int32> CellVert;
    CellVert.Init(INDEX_NONE, CellDimX * CellDimY * CellDimZ);

    static const int32 CornerOff[8][3] =
    {
        {0,0,0},{1,0,0},{0,1,0},{1,1,0},
        {0,0,1},{1,0,1},{0,1,1},{1,1,1}
    };

    static const int32 EdgePairs[12][2] =
    {
        {0,1},{0,2},{1,3},{2,3},
        {4,5},{4,6},{5,7},{6,7},
        {0,4},{1,5},{2,6},{3,7}
    };

    for (int32 cz = -1; cz <= RegionSize.Z - 1; ++cz)
    {
        for (int32 cy = -1; cy <= RegionSize.Y - 1; ++cy)
        {
            for (int32 cx = -1; cx <= RegionSize.X - 1; ++cx)
            {
                bool bAllSolid = true;
                bool bAllAir = true;

                bool Solid[8];
                FVector P[8];

                for (int32 c = 0; c < 8; ++c)
                {
                    const int32 vx = cx + CornerOff[c][0];
                    const int32 vy = cy + CornerOff[c][1];
                    const int32 vz = cz + CornerOff[c][2];

                    const bool bS = IsSolid(GetVoxel(vx, vy, vz));
                    Solid[c] = bS;
                    P[c] = VoxelCenterPosLocal(vx, vy, vz);

                    bAllSolid &= bS;
                    bAllAir &= !bS;
                }

                if (bAllSolid || bAllAir)
                    continue;

                FVector Sum = FVector::ZeroVector;
                int32 Count = 0;

                for (int32 e = 0; e < 12; ++e)
                {
                    const int32 a = EdgePairs[e][0];
                    const int32 b = EdgePairs[e][1];
                    if (Solid[a] == Solid[b]) continue;

                    Sum += (P[a] + P[b]) * 0.5f;
                    ++Count;
                }

                if (Count <= 0) continue;

                const FVector Vtx = Sum / (float)Count;
                auto S = [&](int i) -> float { return Solid[i] ? 1.0f : 0.0f; };

                const float gx = (S(1) + S(3) + S(5) + S(7)) - (S(0) + S(2) + S(4) + S(6));
                const float gy = (S(2) + S(3) + S(6) + S(7)) - (S(0) + S(1) + S(4) + S(5));
                const float gz = (S(4) + S(5) + S(6) + S(7)) - (S(0) + S(1) + S(2) + S(3));

                FVector Grad(gx, gy, gz);
                FVector Vn = (-Grad).GetSafeNormal();
                if (Vn.IsNearlyZero()) Vn = FVector(0, 0, 1);

                const FLinearColor VertexColor = ResolveVertexColor(cx, cy, cz);
                CellVert[CellIndex(cx, cy, cz)] = AddVertex(Vtx, Vn, VertexColor);
            }
        }
    }

    // -------------------------------------------------------------------------
    // 2. Store four neighboring cell vertices as a surface quad
    // -------------------------------------------------------------------------

    auto GetCellVertIndex = [&](int32 cx, int32 cy, int32 cz) -> int32
        {
            if (cx < -1 || cy < -1 || cz < -1) return INDEX_NONE;
            if (cx > RegionSize.X - 1 || cy > RegionSize.Y - 1 || cz > RegionSize.Z - 1) return INDEX_NONE;
            return CellVert[CellIndex(cx, cy, cz)];
        };

    auto IsLocalVoxelInRegion = [&](int32 lx, int32 ly, int32 lz) -> bool
        {
            return
                lx >= 0 && lx < RegionSize.X &&
                ly >= 0 && ly < RegionSize.Y &&
                lz >= 0 && lz < RegionSize.Z;
        };

    // -------------------------------------------------------------------------
    // 3. Connect cell vertices around one representative X-axis crossing
    // -------------------------------------------------------------------------

    // [Omitted: the production EmitQuad lambda stores four cell indices,
    // desired normal, and fallback face data in an FPendingQuad.]

    for (int32 z = 0; z < RegionSize.Z; ++z)
    {
        for (int32 y = 0; y < RegionSize.Y; ++y)
        {
            for (int32 x = -1; x < RegionSize.X; ++x)
            {
                const bool A = IsSolid(GetVoxel(x, y, z));
                const bool B = IsSolid(GetVoxel(x + 1, y, z));
                if (A == B) continue;

                if (!IsLocalVoxelInRegion(A ? x + 1 : x, y, z))
                {
                    continue;
                }

                const int32 v00 = GetCellVertIndex(x, y - 1, z - 1);
                const int32 v10 = GetCellVertIndex(x, y, z - 1);
                const int32 v11 = GetCellVertIndex(x, y, z);
                const int32 v01 = GetCellVertIndex(x, y - 1, z);

                const FVector Desired = A ? FVector(1, 0, 0) : FVector(-1, 0, 0);
                const EGP_VoxelId SolidId = A ? GetVoxel(x, y, z) : GetVoxel(x + 1, y, z);
                const float FX = (x + 1) * VoxelSize;
                const float Y0 = y * VoxelSize;
                const float Y1 = (y + 1) * VoxelSize;
                const float Z0 = z * VoxelSize;
                const float Z1 = (z + 1) * VoxelSize;
                EmitQuad(
                    v00,
                    v10,
                    v11,
                    v01,
                    Desired,
                    FVector(FX, Y0, Z0),
                    FVector(FX, Y1, Z0),
                    FVector(FX, Y1, Z1),
                    FVector(FX, Y0, Z1),
                    ResolveVoxelColor(SolidId));
            }
        }
    }

    // [Omitted: Y/Z crossing loops, quad triangulation/validation,
    // render-array transfer, and UV/tangent generation.]

    return true;
}

// -----------------------------------------------------------------------------
// 4. Build a separate greedy collision mesh from Solid/Air face masks
// -----------------------------------------------------------------------------

void AGP_VoxelChunkActor::BuildCollisionOnlyRegion(
    const FGP_VoxelGrid& GlobalGrid,
    const FIntVector& RegionOrigin,
    const FIntVector& RegionSize,
    float VoxelSize)
{
    // [Omitted: validation and VoxelSolid/Pos/AxisUnit/AddColQuad helpers.]

    TArray<int8> Mask;
    int32 Dims[3] = { RegionSize.X, RegionSize.Y, RegionSize.Z };

    for (int d = 0; d < 3; ++d)
    {
        const int u = (d + 1) % 3;
        const int v = (d + 2) % 3;

        const int SizeU = Dims[u];
        const int SizeV = Dims[v];
        Mask.SetNumZeroed(SizeU * SizeV);

        for (int q = 0; q <= Dims[d]; ++q)
        {
            for (int j = 0; j < SizeV; ++j)
                for (int i = 0; i < SizeU; ++i)
                {
                    int a[3] = { 0,0,0 };
                    int b[3] = { 0,0,0 };

                    a[d] = q - 1; b[d] = q;
                    a[u] = i;     b[u] = i;
                    a[v] = j;     b[v] = j;

                    const bool AS = VoxelSolid(a[0], a[1], a[2]);
                    const bool BS = VoxelSolid(b[0], b[1], b[2]);

                    int8 m = 0;
                    if (AS != BS) m = AS ? (int8)+1 : (int8)-1;
                    Mask[i + SizeU * j] = m;
                }

            for (int j = 0; j < SizeV; ++j)
            {
                for (int i = 0; i < SizeU; )
                {
                    const int idx = i + SizeU * j;
                    const int8 m = Mask[idx];
                    if (m == 0) { ++i; continue; }

                    int w = 1;
                    while (i + w < SizeU && Mask[idx + w] == m) ++w;

                    int h = 1;
                    bool bDone = false;
                    while (j + h < SizeV && !bDone)
                    {
                        for (int k = 0; k < w; ++k)
                        {
                            if (Mask[(i + k) + SizeU * (j + h)] != m) { bDone = true; break; }
                        }
                        if (!bDone) ++h;
                    }

                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                            Mask[(i + x) + SizeU * (j + y)] = 0;

                    int p0[3] = { 0,0,0 };
                    int p1[3] = { 0,0,0 };
                    int p2[3] = { 0,0,0 };
                    int p3[3] = { 0,0,0 };

                    p0[d] = q;  p0[u] = i;     p0[v] = j;
                    p1[d] = q;  p1[u] = i + w; p1[v] = j;
                    p2[d] = q;  p2[u] = i + w; p2[v] = j + h;
                    p3[d] = q;  p3[u] = i;     p3[v] = j + h;

                    const FVector DesiredN = AxisUnit(d) * (float)m;
                    AddColQuad(
                        Pos(p0[0], p0[1], p0[2]),
                        Pos(p1[0], p1[1], p1[2]),
                        Pos(p2[0], p2[1], p2[2]),
                        Pos(p3[0], p3[1], p3[2]),
                        DesiredN);

                    i += w;
                }
            }
        }
    }

    // [Omitted: hidden ProceduralMesh collision-section application.]
}
