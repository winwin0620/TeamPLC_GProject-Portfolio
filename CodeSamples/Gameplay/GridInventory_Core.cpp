/*
 * Portfolio excerpt — not a standalone translation unit.
 * Production source: UGP_InventoryManager.
 *
 * Selected evidence: rectangle overlap, item footprint validation,
 * first-fit placement, stack compatibility, and add/merge ordering.
 */

namespace
{
    static bool RectsOverlap(
        const FIntPoint& ATopLeft,
        const FIntPoint& ASize,
        const FIntPoint& BTopLeft,
        const FIntPoint& BSize)
    {
        const bool bNoOverlapX = (ATopLeft.X + ASize.X <= BTopLeft.X) ||
            (BTopLeft.X + BSize.X <= ATopLeft.X);
        const bool bNoOverlapY = (ATopLeft.Y + ASize.Y <= BTopLeft.Y) ||
            (BTopLeft.Y + BSize.Y <= ATopLeft.Y);
        return !(bNoOverlapX || bNoOverlapY);
    }
}

bool UGP_InventoryManager::CanPlaceItemAt(
    const FGP_GridInventory& Inventory,
    const FGP_ItemDef& Def,
    const FIntPoint& TopLeft,
    const FGuid* IgnoreInstanceId) const
{
    const int32 SizeX = FMath::Max(1, Def.SizeX);
    const int32 SizeY = FMath::Max(1, Def.SizeY);

    if (TopLeft.X < 0 || TopLeft.Y < 0)
    {
        return false;
    }

    if (TopLeft.X + SizeX > Inventory.Width || TopLeft.Y + SizeY > Inventory.Height)
    {
        return false;
    }

    for (const FGP_InventoryEntry& Existing : Inventory.Entries)
    {
        if (IgnoreInstanceId && Existing.Item.InstanceId == *IgnoreInstanceId)
        {
            continue;
        }

        FGP_ItemDef ExistingDef;
        if (!FindItemDefinitionInternal(Existing.Item.ItemId, ExistingDef))
        {
            return false;
        }

        const FIntPoint ExistingSize(
            FMath::Max(1, ExistingDef.SizeX),
            FMath::Max(1, ExistingDef.SizeY));

        if (RectsOverlap(
            TopLeft,
            FIntPoint(SizeX, SizeY),
            Existing.TopLeft,
            ExistingSize))
        {
            return false;
        }
    }

    return true;
}

bool UGP_InventoryManager::FindFirstFit(
    const FGP_GridInventory& Inventory,
    const FGP_ItemDef& Def,
    FIntPoint& OutTopLeft) const
{
    const int32 SizeX = FMath::Max(1, Def.SizeX);
    const int32 SizeY = FMath::Max(1, Def.SizeY);

    if (SizeX > Inventory.Width || SizeY > Inventory.Height)
    {
        return false;
    }

    for (int32 Y = 0; Y <= Inventory.Height - SizeY; ++Y)
    {
        for (int32 X = 0; X <= Inventory.Width - SizeX; ++X)
        {
            const FIntPoint TestPos(X, Y);
            if (CanPlaceItemAt(Inventory, Def, TestPos, nullptr))
            {
                OutTopLeft = TestPos;
                return true;
            }
        }
    }

    return false;
}
bool UGP_InventoryManager::CanMergeEntries(
    const FGP_InventoryEntry& Source,
    const FGP_InventoryEntry& Target) const
{
    if (Source.Item.ItemId.IsNone() || Target.Item.ItemId.IsNone())
    {
        return false;
    }

    if (Source.Item.ItemId != Target.Item.ItemId)
    {
        return false;
    }

    const int32 MaxStack = GetMaxStackForItem(Target.Item.ItemId);
    if (MaxStack <= 1)
    {
        return false;
    }

    return Target.Item.Count < MaxStack;
}

bool UGP_InventoryManager::AddItemToInventory(
    FGP_GridInventory& Inventory,
    const FName& ItemId,
    int32 Count,
    int32& OutAdded)
{
    OutAdded = 0;

    if (ItemId.IsNone() || Count <= 0)
    {
        return false;
    }

    FGP_ItemDef Def;
    if (!FindItemDefinitionInternal(ItemId, Def))
    {
        return false;
    }

    const int32 MaxStack = FMath::Max(1, Def.MaxStack);
    int32 Remaining = Count;
    bool bChanged = false;

    for (FGP_InventoryEntry& Entry : Inventory.Entries)
    {
        if (Remaining <= 0)
        {
            break;
        }

        if (Entry.Item.ItemId != ItemId)
        {
            continue;
        }

        const int32 Space = MaxStack - Entry.Item.Count;
        if (Space <= 0)
        {
            continue;
        }

        const int32 Add = FMath::Min(Space, Remaining);
        Entry.Item.Count += Add;
        Remaining -= Add;
        OutAdded += Add;
        bChanged = true;
    }

    while (Remaining > 0)
    {
        FIntPoint TopLeft;
        if (!FindFirstFit(Inventory, Def, TopLeft))
        {
            break;
        }

        FGP_InventoryEntry NewEntry;
        NewEntry.Item.InstanceId = FGuid::NewGuid();
        NewEntry.Item.ItemId = ItemId;
        NewEntry.Item.Count = FMath::Min(MaxStack, Remaining);
        NewEntry.TopLeft = TopLeft;

        Inventory.Entries.Add(NewEntry);

        Remaining -= NewEntry.Item.Count;
        OutAdded += NewEntry.Item.Count;
        bChanged = true;
    }

    if (bChanged)
    {
        BroadcastChanged();
    }

    return OutAdded > 0;
}
