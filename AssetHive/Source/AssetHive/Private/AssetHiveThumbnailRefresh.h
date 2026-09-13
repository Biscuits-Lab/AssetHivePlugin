#pragma once

class UMaterialInstanceConstant;

namespace AssetHiveThumbnailRefresh
{
    void Queue(UMaterialInstanceConstant* Material);
    void Shutdown();
}
