#pragma once
#include "IDetailCustomization.h"

class FAssetHiveModelCatalogDetails : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance();
    virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override;
};
