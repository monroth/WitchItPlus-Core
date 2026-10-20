// Private shared implementation; also compiled with reflection doubles by the no-engine gate.
    // Only the engine's weak-reference side effect is needed here. Never copy
    // its soft-reference return through the SDK's compile-time struct layout.
    static void AllocateSerialNumberViaEngine(UObject* Object)
    {
        static auto* Function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:Conv_ObjectToSoftObjectReference"));
        static auto* Self = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!Function || !Self) { throw std::runtime_error("Weak object serial allocation: engine conversion unavailable"); }
        auto* Input = Function->FindProperty(FName(STR("Object"), FNAME_Find));
        auto* Result = Function->GetReturnProperty();
        const auto Size = Function->GetParmsSize();
        const auto Fits = [Size](FProperty* Property) {
            return Property && Property->GetOffset_Internal() >= 0 && Property->GetSize() > 0 &&
                   Property->GetOffset_Internal() <= Size && Property->GetSize() <= Size - Property->GetOffset_Internal();
        };
        if (!Fits(Input) || !Fits(Result) || Input->GetSize() != sizeof(UObject*))
        { throw std::runtime_error("Weak object serial allocation: invalid reflected parameters"); }
        std::vector<uint8> Params(Size, 0);
        Result->InitializeValue_InContainer(Params.data());
        struct ReturnStorage
        {
            FProperty* Property;
            void* Params;
            ~ReturnStorage() { Property->DestroyValue_InContainer(Params); }
        } Return{Result, Params.data()};
        *Input->ContainerPtrToValuePtr<UObject*>(Params.data()) = Object;
        Self->ProcessEvent(Function, Params.data());
    }

    int32 FUObjectArray::AllocateSerialNumber(int32 Index)
    {
        FUObjectItem* ObjectItem = IndexToObject(Index);
        checkSlow(ObjectItem);

        volatile int32 *SerialNumberPtr = &ObjectItem->GetSerialNumber();
        if (!*SerialNumberPtr)
        {
            // RE-UE4SS FIX (Corporalwill): [can't use MasterSerialNumber, so we use a library function that allocates as a side effect]
            // [TSoftObjectPtr<UObject> --> FWeakPtr --> AllocateSerialNumber]
            AllocateSerialNumberViaEngine(ObjectItem->GetUObject());
            // RE-UE4SS FIX END
        }
        return *SerialNumberPtr;
    }

