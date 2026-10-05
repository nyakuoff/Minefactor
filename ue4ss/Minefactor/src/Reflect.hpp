#pragma once
// (Copied from AbioticMediaPlayer/native/AbioticBrowser/src/UnrealReflect.hpp.)
//
//
// Helpers for calling arbitrary reflected UFUNCTIONs that have no typed C++
// wrapper in RE-UE4SS's SDK (e.g. UPrimitiveComponent::CreateDynamicMaterial-
// Instance, UMaterialInstanceDynamic::SetTextureParameterValue,
// UKismetRenderingLibrary::ImportBufferAsTexture2D) — needed throughout M2/M3
// since those classes aren't in RE-UE4SS's typed SDK (only UObject/AActor/
// UWorld/UGameplayStatics/a few Kismet libraries are).
//
// Same technique RE-UE4SS's own BPMacros.hpp uses internally (see e.g.
// GameplayStatics.cpp's FindNearestActor): look up each parameter's real
// runtime offset via UFunction::FindProperty rather than assuming a
// hand-written C++ struct's layout matches the engine's — unsafe, since
// padding/field order/editor-only-property-stripping can differ per engine
// build (this is NOT the same as the version-gated-but-verified fixed-layout
// structs GameplayStatics.hpp uses for a few extremely well-known engine
// functions; those were manually reverse-engineered/pinned by RE-UE4SS's own
// maintainers, which we have no equivalent of for the functions below).
//
// BPMacros itself assumes an implicit `this` receiver (it's meant to be used
// inside a method of the UObject-derived class being called), which doesn't
// fit calling instance functions on an arbitrary UObject* found via
// reflection at runtime — these take the receiver explicitly instead.
//
#include <cstdint>
#include <format>
#include <new>
#include <vector>

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Core/Containers/Array.hpp>

#include "Log.hpp"

namespace mf::reflect
{
    using RC::Unreal::FName;
    using RC::Unreal::FNAME_Find;
    using RC::Unreal::FProperty;
    using RC::Unreal::TArray;
    using RC::Unreal::UFunction;
    using RC::Unreal::UObject;
    namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

    // One reflected-function call in progress: the resolved UFunction and a
    // zeroed parameter buffer sized to match, ready for named Set/Get calls.
    // Falsy (via `if (!call)`) if the function couldn't be resolved.
    struct Call
    {
        UObject* receiver = nullptr;
        UFunction* function = nullptr;
        std::vector<std::uint8_t> params;

        explicit operator bool() const { return function != nullptr; }
    };

    // Finds `functionName` on `receiver`'s class chain (works regardless of
    // which ancestor class actually declares it). Logs and returns a falsy
    // Call if not found.
    inline Call Begin(UObject* receiver, const wchar_t* functionName)
    {
        if (!receiver)
        {
            return {};
        }
        UFunction* fn = receiver->GetFunctionByNameInChain(functionName);
        if (!fn)
        {
            mf::LogLine(std::format(L"reflect: function '{}' not found on {}", functionName, receiver->GetName()));
            return {};
        }
        return Call{receiver, fn, std::vector<std::uint8_t>(static_cast<std::size_t>(fn->GetParmsSize()), 0)};
    }

    // Static/CDO-dispatched call — the convention Kismet function libraries
    // (GameplayStatics, KismetRenderingLibrary, ...) use: call through the
    // class's default object, found by its "/Script/Module.Default__Class" path.
    inline Call BeginStatic(const wchar_t* cdoObjectPath, const wchar_t* functionName)
    {
        UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, cdoObjectPath);
        if (!cdo)
        {
            mf::LogLine(std::format(L"reflect: CDO '{}' not found", cdoObjectPath));
            return {};
        }
        return Begin(cdo, functionName);
    }

    // Diagnostic-only: walks the function's real parameter list and logs each
    // name, so a "param not found" failure (e.g. FNAME_Find missing a name
    // that isn't interned anywhere else yet, or a wrong guessed name) leaves
    // ground truth in the log instead of another blind guess next time.
    inline void LogAvailableParams(UFunction* function)
    {
        std::wstring names;
        for (FProperty* field : function->ForEachPropertyInChain())
        {
            if (!field)
            {
                continue;
            }
            if (!names.empty())
            {
                names += L", ";
            }
            names += field->GetName();
        }
        mf::LogLine(std::format(L"reflect: {} actual param(s): [{}]", function->GetName(), names));
    }

    template <typename T>
    bool Set(Call& call, const wchar_t* paramName, T value)
    {
        FProperty* prop = call.function->FindProperty(FName(paramName, FNAME_Find));
        if (!prop)
        {
            mf::LogLine(std::format(L"reflect: param '{}' not found on {}", paramName, call.function->GetName()));
            LogAvailableParams(call.function);
            return false;
        }
        *reinterpret_cast<T*>(&call.params[prop->GetOffset_Internal()]) = value;
        return true;
    }

    template <typename T>
    T Get(Call& call, const wchar_t* paramName)
    {
        FProperty* prop = call.function->FindProperty(FName(paramName, FNAME_Find));
        if (!prop)
        {
            mf::LogLine(std::format(L"reflect: param '{}' not found on {}", paramName, call.function->GetName()));
            LogAvailableParams(call.function);
            return T{};
        }
        return *reinterpret_cast<T*>(&call.params[prop->GetOffset_Internal()]);
    }

    // Raw address of a param inside the call's buffer (for struct params with
    // no convenient C++ type — e.g. copying an opaque out-struct from one call
    // into another call's in-param). nullptr if not found.
    inline void* ParamAddr(Call& call, const wchar_t* paramName)
    {
        FProperty* prop = call.function->FindProperty(FName(paramName, FNAME_Find));
        if (!prop)
        {
            mf::LogLine(std::format(L"reflect: param '{}' not found on {}", paramName, call.function->GetName()));
            LogAvailableParams(call.function);
            return nullptr;
        }
        return &call.params[prop->GetOffset_Internal()];
    }

    // Byte size of a param's type (FProperty::GetSize) — pairs with ParamAddr
    // for raw struct copies. 0 if not found.
    inline int ParamSize(Call& call, const wchar_t* paramName)
    {
        FProperty* prop = call.function->FindProperty(FName(paramName, FNAME_Find));
        return prop ? prop->GetSize() : 0;
    }

    // TArray params need placement-construction in the zeroed slot before use
    // (matches RE-UE4SS's own UE_COPY_STL_VECTOR_AS_TARRAY macro technique) —
    // assigning a separately-built TArray over raw zeroed memory rather than
    // constructing in place is UB in the strict sense, even though it happens
    // to work because a zeroed TArray already looks default-constructed.
    // Returns the in-place array so the caller can Append/Add into it, or
    // nullptr if the param wasn't found (logged).
    template <typename ElementT>
    TArray<ElementT>* SetArrayParam(Call& call, const wchar_t* paramName)
    {
        FProperty* prop = call.function->FindProperty(FName(paramName, FNAME_Find));
        if (!prop)
        {
            mf::LogLine(std::format(L"reflect: array param '{}' not found on {}", paramName, call.function->GetName()));
            LogAvailableParams(call.function);
            return nullptr;
        }
        auto* arr = reinterpret_cast<TArray<ElementT>*>(&call.params[prop->GetOffset_Internal()]);
        new (arr) TArray<ElementT>();
        return arr;
    }

    inline void Invoke(Call& call)
    {
        call.receiver->ProcessEvent(call.function, call.params.data());
    }
}
