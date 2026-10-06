// Minecraft's world content as procedural meshes in the game's world (Phase 4).
//
// No editor, so no custom materials: we reuse cooked ones found by reading the game's paks (see
// Material()): the game's own lit masked master for blocks/entities and the engine glTF importer's
// lit translucent instance for water/glass. Unlit widget materials remain as a fallback.
// ponytail: no animated sprites yet (water/lava/fire are still frames).
#include "Render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>
#include <windows.h>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Coords.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "Reflect.hpp"
#include "Symbols.hpp"

using namespace RC::Unreal;

namespace mf
{
    namespace
    {
        struct FVec
        {
            double X, Y, Z;
        };
        struct FVec2
        {
            double X, Y;
        };
        struct FLinear
        {
            float R, G, B, A;
        };

        // ---- engine plumbing ---------------------------------------------------------------------

        std::uint8_t* Base()
        {
            static auto* base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
            return base;
        }

        bool SymbolsMatch()
        {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(Base() + reinterpret_cast<IMAGE_DOS_HEADER*>(Base())->e_lfanew);
            return nt->FileHeader.TimeDateStamp == sym::kExeTimestamp;
        }

        UObject* LoadObject(const wchar_t* path)
        {
            using Fn = UObject* (*)(UClass*, UObject*, const wchar_t*, const wchar_t*, std::uint32_t, void*, bool, const void*);
            return reinterpret_cast<Fn>(Base() + sym::StaticLoadObject)(UObject::StaticClass(), nullptr, path, nullptr, 0, nullptr, true, nullptr);
        }

        UObject* Cdo(const wchar_t* path)
        {
            return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path);
        }

        // An FTransform param set to identity, through the struct's own member offsets.
        void IdentityTransform(reflect::Call& call, const wchar_t* param)
        {
            auto* prop = CastField<FStructProperty>(call.function->FindProperty(FName(param, FNAME_Find)));
            if (!prop)
            {
                return;
            }
            auto* at = &call.params[prop->GetOffset_Internal()];
            UScriptStruct* st = prop->GetStruct().Get();
            if (FProperty* rot = st->FindProperty(FName(STR("Rotation"), FNAME_Find)))
            {
                reinterpret_cast<double*>(at + rot->GetOffset_Internal())[3] = 1.0;  // quat w
            }
            if (FProperty* scale = st->FindProperty(FName(STR("Scale3D"), FNAME_Find)))
            {
                auto* s = reinterpret_cast<double*>(at + scale->GetOffset_Internal());
                s[0] = s[1] = s[2] = 1.0;
            }
        }

        void TransformAt(reflect::Call& call, const wchar_t* param, const FVec& at)
        {
            IdentityTransform(call, param);
            auto* prop = CastField<FStructProperty>(call.function->FindProperty(FName(param, FNAME_Find)));
            if (FProperty* t = prop ? prop->GetStruct().Get()->FindProperty(FName(STR("Translation"), FNAME_Find)) : nullptr)
            {
                std::memcpy(&call.params[prop->GetOffset_Internal() + t->GetOffset_Internal()], &at, sizeof(at));
            }
        }

        template <class T>
        void SetArray(reflect::Call& call, const wchar_t* param, const std::vector<T>& values, std::vector<TArray<T>*>& owned)
        {
            auto* arr = reflect::SetArrayParam<T>(call, param);
            if (!arr)
            {
                return;
            }
            arr->SetNum(static_cast<std::int32_t>(values.size()));
            if (!values.empty())
            {
                std::memcpy(arr->GetData(), values.data(), values.size() * sizeof(T));
            }
            owned.push_back(arr);
        }

        // Uncompressed PNG (stored deflate blocks): what ImportBufferAsTexture2D can read, with alpha.
        std::vector<std::uint8_t> EncodePng(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h)
        {
            static std::uint32_t crcTable[256];
            if (!crcTable[1])
            {
                for (std::uint32_t n = 0; n < 256; ++n)
                {
                    std::uint32_t c = n;
                    for (int k = 0; k < 8; ++k)
                    {
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    }
                    crcTable[n] = c;
                }
            }
            std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
            const auto be32 = [&](std::uint32_t v) {
                for (int i = 3; i >= 0; --i)
                {
                    out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
                }
            };
            const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
                be32(static_cast<std::uint32_t>(data.size()));
                const std::size_t start = out.size();
                out.insert(out.end(), type, type + 4);
                out.insert(out.end(), data.begin(), data.end());
                std::uint32_t c = 0xFFFFFFFFu;
                for (std::size_t i = start; i < out.size(); ++i)
                {
                    c = crcTable[(c ^ out[i]) & 0xFF] ^ (c >> 8);
                }
                be32(c ^ 0xFFFFFFFFu);
            };
            std::vector<std::uint8_t> ihdr{std::uint8_t(w >> 24), std::uint8_t(w >> 16), std::uint8_t(w >> 8), std::uint8_t(w),
                                           std::uint8_t(h >> 24), std::uint8_t(h >> 16), std::uint8_t(h >> 8), std::uint8_t(h), 8, 6, 0, 0, 0};
            chunk("IHDR", ihdr);
            // Raw scanlines (filter byte 0 each), wrapped in zlib "stored" blocks.
            std::vector<std::uint8_t> raw;
            raw.reserve(std::size_t(h) * (w * 4 + 1));
            for (std::uint32_t y = 0; y < h; ++y)
            {
                raw.push_back(0);
                raw.insert(raw.end(), rgba + std::size_t(y) * w * 4, rgba + std::size_t(y + 1) * w * 4);
            }
            std::vector<std::uint8_t> z{0x78, 0x01};
            std::uint32_t a = 1, b = 0;
            for (std::size_t i = 0; i < raw.size(); i += 65535)
            {
                const auto n = static_cast<std::uint16_t>(std::min<std::size_t>(65535, raw.size() - i));
                z.push_back(i + n == raw.size() ? 1 : 0);
                z.push_back(std::uint8_t(n));
                z.push_back(std::uint8_t(n >> 8));
                z.push_back(std::uint8_t(~n));
                z.push_back(std::uint8_t(~n >> 8));
                z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
            }
            for (std::uint8_t v : raw)
            {
                a = (a + v) % 65521;
                b = (b + a) % 65521;
            }
            const std::uint32_t adler = (b << 16) | a;
            for (int i = 3; i >= 0; --i)
            {
                z.push_back(static_cast<std::uint8_t>(adler >> (i * 8)));
            }
            chunk("IDAT", z);
            chunk("IEND", {});
            return out;
        }

        // ---- state -----------------------------------------------------------------------------

        // Every engine object we keep is held weakly and re-checked each tick: the engine may destroy
        // any of them (map changes, streaming, GC), and a stale raw pointer is a crash.
        FWeakObjectPtr holderRef, blocksRef, sceneRef, entitiesRef, maskedRef, translucentRef;
        UObject* holder = nullptr;     // this tick's live pointers (refreshed by EnsureHolder)
        UObject* blocks = nullptr;
        UObject* scene = nullptr;
        UObject* entities = nullptr;
        UObject* masked = nullptr;
        UObject* translucent = nullptr;
        std::uint32_t holderEpoch = ~0u;
        bool broken = false;

        struct Texture
        {
            FWeakObjectPtr object;  // UTexture2D
            std::uint32_t w = 0, h = 0;
            std::vector<std::uint8_t> rgba;  // kept to re-import if the engine ever drops it
        };
        std::unordered_map<std::uint32_t, Texture> textures;                 // Minecraft texture id (0 = atlas)
        // (texture, translucent, tint) -> MID. Tint: 0xRGB in 1/15 steps (0xFFF = untinted).
        std::map<std::tuple<std::uint32_t, bool, std::uint16_t>, FWeakObjectPtr> materials;
        bool litOpaque = false, litBlend = false;  // game/glTF materials loaded (else the unlit widget ones)
        bool flipWinding = true;                   // measured from the engine's own box mesh

        std::map<std::tuple<int, int, int>, std::vector<int>> sections;  // block section -> mesh section slots

        // Minecraft's light-emitting blocks as the game's point lights.
        struct Light
        {
            FWeakObjectPtr component;
            float intensity;
            std::uint8_t kind;  // proto::LightKind
            float phase;
        };
        std::map<std::tuple<int, int, int>, std::vector<Light>> lights;  // by block section
        std::vector<int> freeSlots;
        int nextSlot = 0;
        int sceneSections = 0;
        bool entitiesShown = false;
        std::uint64_t nextEntities = 0;

        UObject* AddMeshComponent(UObject* holder, UClass* cls)
        {
            auto call = reflect::Begin(holder, STR("AddComponentByClass"));
            if (!call)
            {
                return nullptr;
            }
            reflect::Set(call, STR("Class"), static_cast<UObject*>(cls));
            IdentityTransform(call, STR("RelativeTransform"));
            reflect::Invoke(call);
            return reflect::Get<UObject*>(call, STR("ReturnValue"));
        }

        void CallInt(UObject* obj, const wchar_t* fn, const wchar_t* param, std::int32_t value)
        {
            if (auto call = reflect::Begin(obj, fn))
            {
                reflect::Set(call, param, value);
                reflect::Invoke(call);
            }
        }

        // Which triangle order the engine treats as front-facing, from its own reference box (rather
        // than trusting memory): our Minecraft->game conversion mirrors space, so it decides the flip.
        void MeasureWinding()
        {
            static bool measured = false;
            if (measured)
            {
                return;
            }
            auto call = reflect::Begin(Cdo(STR("/Script/ProceduralMeshComponent.Default__KismetProceduralMeshLibrary")), STR("GenerateBoxMesh"));
            if (!call)
            {
                return;
            }
            measured = true;
            reflect::Set(call, STR("BoxRadius"), FVec{50, 50, 50});
            auto* verts = reflect::SetArrayParam<FVec>(call, STR("Vertices"));
            auto* tris = reflect::SetArrayParam<std::int32_t>(call, STR("Triangles"));
            reflect::SetArrayParam<FVec>(call, STR("Normals"));
            reflect::SetArrayParam<FVec2>(call, STR("UVs"));
            reflect::Invoke(call);
            if (verts && tris && tris->Num() >= 3)
            {
                const FVec a = verts->GetData()[tris->GetData()[0]], b = verts->GetData()[tris->GetData()[1]], c = verts->GetData()[tris->GetData()[2]];
                const FVec e1{b.X - a.X, b.Y - a.Y, b.Z - a.Z}, e2{c.X - a.X, c.Y - a.Y, c.Z - a.Z};
                const FVec n{e1.Y * e2.Z - e1.Z * e2.Y, e1.Z * e2.X - e1.X * e2.Z, e1.X * e2.Y - e1.Y * e2.X};
                const double outward = n.X * (a.X + b.X + c.X) + n.Y * (a.Y + b.Y + c.Y) + n.Z * (a.Z + b.Z + c.Z);
                // Minecraft's counter-clockwise triangles, mirrored by the axis swap, have their
                // standard cross product pointing inward; flip if the engine wants it outward.
                flipWinding = outward > 0;
                Log(L"render: engine front faces have an {} cross product; flip {}", outward > 0 ? L"outward" : L"inward", flipWinding);
            }
            // ponytail: the out-arrays of this one-off call are left to the engine allocator (a few hundred bytes).
        }

        bool EnsureHolder(UObject* pawn, std::uint32_t epoch)
        {
            if (broken)
            {
                return false;
            }
            holder = holderRef.Get();
            blocks = blocksRef.Get();
            scene = sceneRef.Get();
            entities = entitiesRef.Get();
            masked = maskedRef.Get();
            translucent = translucentRef.Get();
            if (holder && blocks && scene && entities && masked && translucent && holderEpoch == epoch)
            {
                return true;
            }
            if (holderEpoch == epoch)
            {
                Log(L"render: lost engine objects (holder {}, meshes {}/{}/{}, materials {}/{}); rebuilding", holder != nullptr, blocks != nullptr,
                    scene != nullptr, entities != nullptr, masked != nullptr, translucent != nullptr);
            }
            if (!SymbolsMatch())
            {
                broken = true;
                return false;
            }
            if (holder)
            {
                if (auto destroy = reflect::Begin(holder, STR("K2_DestroyActor")))
                {
                    reflect::Invoke(destroy);
                }
                holder = nullptr;
            }
            // A new map: everything Minecraft sent for the old one is gone with its actor.
            sections.clear();
            lights.clear();  // their components went with the old holder
            freeSlots.clear();
            nextSlot = 0;
            sceneSections = 0;
            materials.clear();  // MIDs were made for the old world (still rooted; a few bytes each)
            holderEpoch = epoch;

            // Lit: the game's own masked master (texture "Texture", tint "DiffuseColor", the texture's
            // own sampler so pixel art stays sharp) and the glTF importer's two-sided translucent
            // instance (multiplies by vertex colour: water/glass tints). Unlit widget ones as fallback.
            masked = LoadObject(STR("/Game/Textures/M_AbioticMasked_Master.M_AbioticMasked_Master"));
            translucent = LoadObject(STR("/Interchange/gltf/MaterialInstances/MI_Default_Blend_DS.MI_Default_Blend_DS"));
            litOpaque = masked != nullptr;
            litBlend = translucent != nullptr;
            if (!masked)
            {
                masked = LoadObject(STR("/Engine/EngineMaterials/Widget3DPassThrough_Masked.Widget3DPassThrough_Masked"));
            }
            if (!translucent)
            {
                translucent = LoadObject(STR("/Engine/EngineMaterials/Widget3DPassThrough_Translucent.Widget3DPassThrough_Translucent"));
            }
            Log(L"render: materials {} / {}", litOpaque ? L"lit game master" : L"unlit fallback", litBlend ? L"lit glTF translucent" : L"unlit fallback");
            MeasureWinding();
            maskedRef = masked;
            translucentRef = translucent;
            auto* actorClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.Actor"));
            auto* pmcClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/ProceduralMeshComponent.ProceduralMeshComponent"));
            auto spawn = reflect::Begin(Cdo(STR("/Script/Engine.Default__GameplayStatics")), STR("BeginDeferredActorSpawnFromClass"));
            for (UObject* m : {masked, translucent})
            {
                if (m)
                {
                    m->SetRootSet();
                }
            }
            if (!masked || !translucent || !actorClass || !pmcClass || !spawn)
            {
                Log(L"render: missing engine pieces (material {} / {}, classes {} / {}); world rendering off", masked != nullptr,
                    translucent != nullptr, actorClass != nullptr, pmcClass != nullptr);
                broken = true;
                return false;
            }
            reflect::Set(spawn, STR("WorldContextObject"), pawn);
            reflect::Set(spawn, STR("ActorClass"), static_cast<UObject*>(actorClass));
            IdentityTransform(spawn, STR("SpawnTransform"));
            reflect::Set<std::uint8_t>(spawn, STR("CollisionHandlingOverride"), 1);  // AlwaysSpawn
            reflect::Invoke(spawn);
            holder = reflect::Get<UObject*>(spawn, STR("ReturnValue"));
            if (auto finish = reflect::Begin(Cdo(STR("/Script/Engine.Default__GameplayStatics")), STR("FinishSpawningActor")))
            {
                reflect::Set(finish, STR("Actor"), holder);
                IdentityTransform(finish, STR("SpawnTransform"));
                reflect::Invoke(finish);
            }
            if (!holder)
            {
                Log(L"render: couldn't spawn the holder actor");
                broken = true;
                return false;
            }
            holderRef = holder;
            blocks = AddMeshComponent(holder, pmcClass);
            scene = AddMeshComponent(holder, pmcClass);
            entities = AddMeshComponent(holder, pmcClass);
            blocksRef = blocks;
            sceneRef = scene;
            entitiesRef = entities;
            // Built blocks: solid to the game's NPCs and props, but on a channel our own collision
            // scan ignores (Minecraft already knows its blocks, and must see them go the moment they break).
            if (blocks)
            {
                if (auto call = reflect::Begin(blocks, STR("SetCollisionObjectType")))
                {
                    reflect::Set<std::uint8_t>(call, STR("Channel"), 5);  // ECC_PhysicsBody
                    reflect::Invoke(call);
                }
            }
            for (UObject* c : {scene, entities})
            {
                if (auto call = c ? reflect::Begin(c, STR("SetCollisionEnabled")) : reflect::Call{})
                {
                    reflect::Set<std::uint8_t>(call, STR("NewType"), 0);  // NoCollision
                    reflect::Invoke(call);
                }
            }
            Log(L"render: holder actor ready ({}, {}, {})", blocks != nullptr, scene != nullptr, entities != nullptr);
            return blocks && scene && entities;
        }

        bool Import(std::uint32_t id, Texture& t)
        {
            const auto png = EncodePng(t.rgba.data(), t.w, t.h);
            auto call = reflect::Begin(Cdo(STR("/Script/Engine.Default__KismetRenderingLibrary")), STR("ImportBufferAsTexture2D"));
            if (!call)
            {
                return false;
            }
            reflect::Set(call, STR("WorldContextObject"), holder);
            std::vector<TArray<std::uint8_t>*> owned;
            SetArray(call, STR("Buffer"), png, owned);
            reflect::Invoke(call);
            UObject* tex = reflect::Get<UObject*>(call, STR("ReturnValue"));
            for (auto* a : owned)
            {
                a->~TArray();
            }
            if (!tex)
            {
                Log(L"render: texture {} ({}x{}) import failed", id, t.w, t.h);
                return false;
            }
            // Pixel art: nearest filtering (TF_Nearest), applied by recreating the GPU resource.
            if (auto* filter = tex->GetValuePtrByPropertyNameInChain<std::uint8_t>(STR("Filter")))
            {
                *filter = 0;
            }
            reinterpret_cast<void (*)(UObject*)>(Base() + sym::UTexture_UpdateResource)(tex);
            tex->SetRootSet();
            t.object = tex;
            std::erase_if(materials, [&](const auto& kv) { return std::get<0>(kv.first) == id; });
            if (id == 0)
            {
                Log(L"render: atlas {}x{} loaded", t.w, t.h);
            }
            return true;
        }

        void AddTexture(std::uint32_t id, std::uint32_t w, std::uint32_t h, const std::uint8_t* rgba)
        {
            Texture& t = textures[id];
            t.w = w;
            t.h = h;
            t.rgba.assign(rgba, rgba + std::size_t(w) * h * 4);
            Import(id, t);
        }

        void SetScalar(UObject* mid, const wchar_t* name, float value)
        {
            if (auto call = reflect::Begin(mid, STR("SetScalarParameterValue")))
            {
                reflect::Set(call, STR("ParameterName"), FName(name, FNAME_Add));
                reflect::Set(call, STR("Value"), value);
                reflect::Invoke(call);
            }
        }

        void SetVector(UObject* mid, const wchar_t* name, float r, float g, float b, float a)
        {
            if (auto call = reflect::Begin(mid, STR("SetVectorParameterValue")))
            {
                reflect::Set(call, STR("ParameterName"), FName(name, FNAME_Add));
                struct
                {
                    float r, g, b, a;
                } c{r, g, b, a};
                reflect::Set(call, STR("Value"), c);
                reflect::Invoke(call);
            }
        }

        UObject* Material(std::uint32_t texture, bool blended, std::uint16_t tint = 0xFFF)
        {
            const auto key = std::make_tuple(texture, blended, blended ? std::uint16_t(0xFFF) : tint);
            if (auto it = materials.find(key); it != materials.end())
            {
                if (UObject* mid = it->second.Get())
                {
                    return mid;
                }
                Log(L"render: material for texture {} was destroyed; recreating", texture);
                materials.erase(it);
            }
            auto tex = textures.find(texture);
            if (tex == textures.end())
            {
                return nullptr;  // not arrived yet
            }
            UObject* texObj = tex->second.object.Get();
            if (!texObj)
            {
                Log(L"render: texture {} was destroyed; re-importing", texture);
                if (!Import(texture, tex->second) || !(texObj = tex->second.object.Get()))
                {
                    return nullptr;
                }
            }
            auto call = reflect::Begin(Cdo(STR("/Script/Engine.Default__KismetMaterialLibrary")), STR("CreateDynamicMaterialInstance"));
            if (!call)
            {
                return nullptr;
            }
            reflect::Set(call, STR("WorldContextObject"), holder);
            reflect::Set(call, STR("Parent"), blended ? translucent : masked);
            reflect::Invoke(call);
            UObject* mid = reflect::Get<UObject*>(call, STR("ReturnValue"));
            if (!mid)
            {
                return nullptr;
            }
            mid->SetRootSet();  // cached across mesh sections that come and go
            const bool lit = blended ? litBlend : litOpaque;
            const wchar_t* param = !lit ? STR("SlateUI") : blended ? STR("BaseColorTexture") : STR("Texture");
            if (auto set = reflect::Begin(mid, STR("SetTextureParameterValue")))
            {
                reflect::Set(set, STR("ParameterName"), FName(param, FNAME_Add));
                reflect::Set(set, STR("Value"), texObj);
                reflect::Invoke(set);
            }
            if (lit && blended)
            {
                SetScalar(mid, STR("MetallicFactor"), 0.0f);  // default 1 would make everything metal
                SetScalar(mid, STR("BaseColorTexture_TexCoord"), 0.0f);
                SetVector(mid, STR("BaseColorFactor"), 1, 1, 1, 1);
            }
            else if (lit)
            {
                SetVector(mid, STR("DiffuseColor"), ((tint >> 8) & 15) / 15.0f, ((tint >> 4) & 15) / 15.0f, (tint & 15) / 15.0f, 0.0f);
            }
            static bool checked = false;
            if (!checked)
            {
                checked = true;
                if (auto get = reflect::Begin(mid, STR("K2_GetTextureParameterValue")))
                {
                    reflect::Set(get, STR("ParameterName"), FName(param, FNAME_Add));
                    reflect::Invoke(get);
                    Log(L"render: material texture parameter {} {}", param,
                        reflect::Get<UObject*>(get, STR("ReturnValue")) == texObj ? L"holds Minecraft's texture" : L"DID NOT TAKE (wrong parameter name?)");
                }
            }
            materials[key] = FWeakObjectPtr(mid);
            return mid;
        }

        struct Mesh
        {
            std::vector<FVec> verts, normals;
            std::vector<std::int32_t> tris;
            std::vector<FVec2> uvs;
            std::vector<FLinear> colors;  // only for the vertex-coloured (translucent) material

            void Clear()
            {
                verts.clear();
                normals.clear();
                tris.clear();
                uvs.clear();
                colors.clear();
            }
            // One Minecraft-space triangle (counter-clockwise from its front) into the game's space.
            void Triangle(const Vec3 (&p)[3], const double (&uv)[3][2], const Vec3* normal = nullptr, const FLinear* color = nullptr, bool bothSides = false)
            {
                Vec3 n;
                if (normal)
                {
                    n = *normal;
                }
                else
                {
                    const Vec3 e1{p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z}, e2{p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
                    n = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
                    const double len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                    n = len > 1e-12 ? Vec3{n.x / len, n.y / len, n.z / len} : Vec3{0, 1, 0};
                }
                for (int side = 0; side < (bothSides ? 2 : 1); ++side)
                {
                    const auto base = static_cast<std::int32_t>(verts.size());
                    const double sgn = side ? -1.0 : 1.0;
                    for (int k = 0; k < 3; ++k)
                    {
                        const Vec3 ue = McToUe(p[k]);
                        verts.push_back({ue.x, ue.y, ue.z});
                        normals.push_back({n.x * sgn, n.z * sgn, n.y * sgn});  // Minecraft (x, y, z) -> game (x, z, y)
                        uvs.push_back({uv[k][0], uv[k][1]});
                        if (color)
                        {
                            colors.push_back(color[k]);
                        }
                    }
                    const bool flip = flipWinding != (side == 1);
                    tris.insert(tris.end(), {base, flip ? base + 2 : base + 1, flip ? base + 1 : base + 2});
                }
            }
        };

        void Upload(UObject* pmc, int slot, const Mesh& mesh, UObject* material, bool collision)
        {
            if (mesh.verts.empty())
            {
                CallInt(pmc, STR("ClearMeshSection"), STR("SectionIndex"), slot);
                return;
            }
            auto call = reflect::Begin(pmc, STR("CreateMeshSection_LinearColor"));
            if (!call)
            {
                return;
            }
            std::vector<TArray<FVec>*> v;
            std::vector<TArray<std::int32_t>*> t;
            std::vector<TArray<FVec2>*> uv;
            std::vector<TArray<FLinear>*> c;
            reflect::Set(call, STR("SectionIndex"), static_cast<std::int32_t>(slot));
            SetArray(call, STR("Vertices"), mesh.verts, v);
            SetArray(call, STR("Triangles"), mesh.tris, t);
            SetArray(call, STR("Normals"), mesh.normals, v);
            SetArray(call, STR("UV0"), mesh.uvs, uv);
            if (!mesh.colors.empty())
            {
                SetArray(call, STR("VertexColors"), mesh.colors, c);
            }
            reflect::Set(call, STR("bCreateCollision"), collision);
            reflect::Invoke(call);
            for (auto* a : v) a->~TArray();
            for (auto* a : t) a->~TArray();
            for (auto* a : uv) a->~TArray();
            for (auto* a : c) a->~TArray();
            if (auto set = reflect::Begin(pmc, STR("SetMaterial")))
            {
                reflect::Set(set, STR("ElementIndex"), static_cast<std::int32_t>(slot));
                reflect::Set(set, STR("Material"), material);
                reflect::Invoke(set);
            }
        }

        int AllocSlot()
        {
            if (!freeSlots.empty())
            {
                const int s = freeSlots.back();
                freeSlots.pop_back();
                return s;
            }
            return nextSlot++;
        }

        FLinear Color(std::uint32_t rgba)
        {
            return {(rgba & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f};
        }

        // Minecraft's per-vertex tint (grass, leaves, water...) times its ambient occlusion: keep the
        // hue (normalised to its brightest channel, in 1/15 steps) for the material's tint; 0xFFF = none.
        std::uint16_t TintKey(std::uint32_t rgba)
        {
            const int r = rgba & 0xFF, g = (rgba >> 8) & 0xFF, b = (rgba >> 16) & 0xFF;
            const int hi = std::max({r, g, b}), lo = std::min({r, g, b});
            if (hi < 8 || hi - lo < 12)
            {
                return 0xFFF;
            }
            const auto q = [&](int c) { return static_cast<std::uint16_t>(std::lround(c * 15.0 / hi)); };
            return static_cast<std::uint16_t>(q(r) << 8 | q(g) << 4 | q(b));
        }

        const Vec3 kFaceNormal[7] = {{0, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};  // Direction ordinal + 1

        // A 16^3 block section: cutout triangles grouped by tint (one material each), blended ones
        // with vertex colours; each group is one mesh section.
        void OnSection(const std::uint8_t* p, std::uint32_t bytes)
        {
            proto::RenSection hdr;
            std::memcpy(&hdr, p, sizeof(hdr));
            const auto* v = reinterpret_cast<const proto::RenVertex*>(p + sizeof(hdr));
            if (sizeof(hdr) + std::size_t(hdr.vertexCount) * sizeof(proto::RenVertex) > bytes)
            {
                return;
            }
            const auto key = std::make_tuple(hdr.sx, hdr.sy, hdr.sz);
            static std::map<std::uint16_t, Mesh> solid;  // by tint
            static Mesh blended;
            for (auto& [tint, m] : solid)
            {
                m.Clear();
            }
            blended.Clear();
            const double ox = hdr.sx * 16.0, oy = hdr.sy * 16.0, oz = hdr.sz * 16.0;
            for (std::uint32_t i = 0; i + 2 < hdr.vertexCount; i += 3)
            {
                const Vec3 pts[3] = {{ox + v[i].x, oy + v[i].y, oz + v[i].z}, {ox + v[i + 1].x, oy + v[i + 1].y, oz + v[i + 1].z},
                                     {ox + v[i + 2].x, oy + v[i + 2].y, oz + v[i + 2].z}};
                const double uv[3][2] = {{v[i].u, v[i].v}, {v[i + 1].u, v[i + 1].v}, {v[i + 2].u, v[i + 2].v}};
                const auto face = (v[i].flags >> 4) & 7;
                const Vec3* normal = face && face <= 6 ? &kFaceNormal[face] : nullptr;
                if (v[i].flags & 2)
                {
                    const FLinear colors[3] = {Color(v[i].color), Color(v[i + 1].color), Color(v[i + 2].color)};
                    blended.Triangle(pts, uv, normal, colors);
                }
                else
                {
                    solid[TintKey(v[i].color)].Triangle(pts, uv, normal);
                }
            }
            auto& slots = sections[key];
            std::size_t used = 0;
            const auto put = [&](const Mesh& m, UObject* material, bool collision) {
                if (m.verts.empty() || !material)
                {
                    return;
                }
                if (used == slots.size())
                {
                    slots.push_back(AllocSlot());
                }
                Upload(blocks, slots[used++], m, material, collision);
            };
            for (const auto& [tint, m] : solid)
            {
                put(m, Material(0, false, tint), true);
            }
            put(blended, Material(0, true), false);
            for (std::size_t i = used; i < slots.size(); ++i)
            {
                CallInt(blocks, STR("ClearMeshSection"), STR("SectionIndex"), slots[i]);
                freeSlots.push_back(slots[i]);
            }
            slots.resize(used);
            if (slots.empty())
            {
                sections.erase(key);
            }
        }

        // Everything else Minecraft drew this frame (block entities, mobs, particles), relative to an origin.
        void OnScene(const std::uint8_t* p, std::uint32_t bytes)
        {
            proto::RenScene hdr;
            std::memcpy(&hdr, p, sizeof(hdr));
            const auto* batches = reinterpret_cast<const proto::RenBatch*>(p + sizeof(hdr));
            const auto* v = reinterpret_cast<const proto::RenVertex*>(batches + hdr.batchCount);
            if (sizeof(hdr) + hdr.batchCount * sizeof(proto::RenBatch) + std::size_t(hdr.vertexCount) * sizeof(proto::RenVertex) > bytes)
            {
                return;
            }
            static Mesh m;
            int used = 0;
            for (std::uint32_t b = 0; b < hdr.batchCount; ++b)
            {
                const auto& batch = batches[b];
                if (std::size_t(batch.first) + batch.count > hdr.vertexCount)
                {
                    continue;
                }
                const bool blended = batch.flags & 1;
                UObject* material = Material(batch.texture, blended);
                if (!material)
                {
                    continue;  // its texture hasn't arrived yet
                }
                m.Clear();
                for (std::uint32_t i = batch.first; i + 2 < batch.first + batch.count; i += 3)
                {
                    const Vec3 pts[3] = {{hdr.originX + v[i].x, hdr.originY + v[i].y, hdr.originZ + v[i].z},
                                         {hdr.originX + v[i + 1].x, hdr.originY + v[i + 1].y, hdr.originZ + v[i + 1].z},
                                         {hdr.originX + v[i + 2].x, hdr.originY + v[i + 2].y, hdr.originZ + v[i + 2].z}};
                    const double uv[3][2] = {{v[i].u, v[i].v}, {v[i + 1].u, v[i + 1].v}, {v[i + 2].u, v[i + 2].v}};
                    const FLinear colors[3] = {Color(v[i].color), Color(v[i + 1].color), Color(v[i + 2].color)};
                    m.Triangle(pts, uv, nullptr, blended ? colors : nullptr);
                }
                Upload(scene, used++, m, material, false);
            }
            for (int s = used; s < sceneSections; ++s)
            {
                CallInt(scene, STR("ClearMeshSection"), STR("SectionIndex"), s);
            }
            sceneSections = used;
        }

        void DestroyLights(std::vector<Light>& list)
        {
            for (auto& l : list)
            {
                if (UObject* c = l.component.Get())
                {
                    if (auto call = reflect::Begin(c, STR("K2_DestroyComponent")))
                    {
                        reflect::Set(call, STR("Object"), c);
                        reflect::Invoke(call);
                    }
                }
            }
            list.clear();
        }

        // A section's light-emitting blocks (sent after its mesh): torches, lava, glowstone, ...
        void OnLights(const std::uint8_t* p, std::uint32_t bytes)
        {
            proto::RenLights hdr;
            std::memcpy(&hdr, p, sizeof(hdr));
            if (sizeof(hdr) + std::size_t(hdr.count) * sizeof(proto::RenLight) > bytes)
            {
                return;
            }
            auto& list = lights[std::make_tuple(hdr.sx, hdr.sy, hdr.sz)];
            DestroyLights(list);
            static UClass* lightClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.PointLightComponent"));
            const auto* l = reinterpret_cast<const proto::RenLight*>(p + sizeof(hdr));
            for (std::uint32_t i = 0; i < hdr.count && lightClass; ++i)
            {
                const Vec3 ue = McToUe({hdr.sx * 16.0 + l[i].x + 0.5, hdr.sy * 16.0 + l[i].y + 0.5, hdr.sz * 16.0 + l[i].z + 0.5});
                auto add = reflect::Begin(holder, STR("AddComponentByClass"));
                if (!add)
                {
                    break;
                }
                reflect::Set(add, STR("Class"), static_cast<UObject*>(lightClass));
                TransformAt(add, STR("RelativeTransform"), {ue.x, ue.y, ue.z});
                reflect::Invoke(add);
                UObject* c = reflect::Get<UObject*>(add, STR("ReturnValue"));
                if (!c)
                {
                    continue;
                }
                // ponytail: intensity/reach by eye (unitless 5000 is the engine default); no shadows for cost.
                const float level = l[i].level / 15.0f;
                const float intensity = 6000.0f * level;
                if (auto call = reflect::Begin(c, STR("SetIntensity")))
                {
                    reflect::Set(call, STR("NewIntensity"), intensity);
                    reflect::Invoke(call);
                }
                if (auto call = reflect::Begin(c, STR("SetAttenuationRadius")))
                {
                    reflect::Set(call, STR("NewRadius"), l[i].level * 100.0f + 100.0f);
                    reflect::Invoke(call);
                }
                if (auto call = reflect::Begin(c, STR("SetLightColor")))
                {
                    const std::uint32_t rgb = l[i].color;
                    const struct
                    {
                        float r, g, b, a;
                    } col{(rgb & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, ((rgb >> 16) & 0xFF) / 255.0f, 1.0f};
                    reflect::Set(call, STR("NewLightColor"), col);
                    reflect::Set(call, STR("bSRGB"), true);
                    reflect::Invoke(call);
                }
                if (auto call = reflect::Begin(c, STR("SetCastShadows")))
                {
                    reflect::Set(call, STR("bNewValue"), false);
                    reflect::Invoke(call);
                }
                list.push_back({FWeakObjectPtr(c), intensity, static_cast<std::uint8_t>((l[i].color >> 24) & 0xF), float(i) * 1.7f});
            }
            if (list.empty())
            {
                lights.erase(std::make_tuple(hdr.sx, hdr.sy, hdr.sz));
            }
        }

        // Flames flicker, lava breathes (20 Hz).
        void AnimateLights()
        {
            static std::uint64_t next = 0;
            const auto now = ::GetTickCount64();
            if (now < next)
            {
                return;
            }
            next = now + 50;
            const double t = now / 1000.0;
            for (auto& [key, list] : lights)
            {
                for (auto& l : list)
                {
                    if (l.kind == proto::kLightSteady)
                    {
                        continue;
                    }
                    UObject* c = l.component.Get();
                    auto call = c ? reflect::Begin(c, STR("SetIntensity")) : reflect::Call{};
                    if (!call)
                    {
                        continue;
                    }
                    const double f = l.kind == proto::kLightFlame
                                         ? 0.85 + 0.1 * std::sin(t * 13.0 + l.phase) + 0.05 * std::sin(t * 31.0 + l.phase * 2.3)
                                         : 0.9 + 0.1 * std::sin(t * 1.5 + l.phase);
                    reflect::Set(call, STR("NewIntensity"), static_cast<float>(l.intensity * f));
                    reflect::Invoke(call);
                }
            }
        }

        // Arrows, tridents, dropped and thrown items: crossed two-sided quads with their item icon,
        // like Minecraft's own flat item rendering; dropped blocks as small cubes.
        void BuildEntities()
        {
            static proto::WorldEntities we;
            if (!Link::Get().ReadWorldEntities(we))
            {
                return;
            }
            static Mesh m;
            m.Clear();
            const auto quad = [&](const Vec3& c, const Vec3& a, const Vec3& b, const float* uv, bool bothSides) {
                // c centre, a and b half-extent vectors (Minecraft space); uv {u0, v0, u1, v1}
                const Vec3 p[4] = {{c.x - a.x - b.x, c.y - a.y - b.y, c.z - a.z - b.z}, {c.x + a.x - b.x, c.y + a.y - b.y, c.z + a.z - b.z},
                                   {c.x + a.x + b.x, c.y + a.y + b.y, c.z + a.z + b.z}, {c.x - a.x + b.x, c.y - a.y + b.y, c.z - a.z + b.z}};
                const double us[4] = {uv[0], uv[2], uv[2], uv[0]}, vs[4] = {uv[3], uv[3], uv[1], uv[1]};
                for (const auto& t : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}})
                {
                    const Vec3 pts[3] = {p[t[0]], p[t[1]], p[t[2]]};
                    const double tuv[3][2] = {{us[t[0]], vs[t[0]]}, {us[t[1]], vs[t[1]]}, {us[t[2]], vs[t[2]]}};
                    m.Triangle(pts, tuv, nullptr, nullptr, bothSides);
                }
            };
            constexpr double kDeg = 3.14159265358979 / 180.0;
            for (std::uint32_t i = 0; i < we.count; ++i)
            {
                const auto& e = we.entities[i];
                const Vec3 c{e.x, e.y, e.z};
                const double cy = std::cos(e.yaw * kDeg), sy = std::sin(e.yaw * kDeg), cp = std::cos(e.pitch * kDeg), sp = std::sin(e.pitch * kDeg);
                if (e.kind == proto::kWeArrow || e.kind == proto::kWeTrident)
                {
                    // Along its flight direction, two planes crossed along that axis.
                    const double len = e.kind == proto::kWeTrident ? 0.8 : 0.4, wid = 0.08;
                    const Vec3 fwd{-sy * cp * len, -sp * len, cy * cp * len};
                    const Vec3 side{cy * wid, 0, sy * wid}, up{-sy * sp * wid, cp * wid, cy * sp * wid};
                    quad(c, fwd, side, e.uv[0], true);
                    quad(c, fwd, up, e.uv[0], true);
                }
                else if (e.kind == proto::kWeItem)
                {
                    const double s = std::max(0.25f, e.scale) * 0.5;
                    const Vec3 a{cy * s, 0, sy * s}, up{0, s, 0}, a2{-sy * s, 0, cy * s};
                    quad({c.x, c.y + s, c.z}, a, up, e.uv[0], true);
                    quad({c.x, c.y + s, c.z}, a2, up, e.uv[0], true);
                }
                else if (e.kind == proto::kWeBlock)
                {
                    const double s = std::max(0.1f, e.scale) * 0.5;
                    const Vec3 cc{c.x, c.y + s, c.z};
                    const Vec3 ax{cy * s, 0, sy * s}, az{-sy * s, 0, cy * s}, ay{0, s, 0};
                    // ponytail: cube faces drawn two-sided rather than wound per face.
                    quad({cc.x + az.x, cc.y, cc.z + az.z}, ax, ay, e.uv[0], true);
                    quad({cc.x - az.x, cc.y, cc.z - az.z}, ax, ay, e.uv[0], true);
                    quad({cc.x + ax.x, cc.y, cc.z + ax.z}, az, ay, e.uv[0], true);
                    quad({cc.x - ax.x, cc.y, cc.z - ax.z}, az, ay, e.uv[0], true);
                    quad({cc.x, cc.y + s, cc.z}, ax, az, e.uv[1], true);
                    quad({cc.x, cc.y - s, cc.z}, ax, az, e.uv[2], true);
                }
            }
            if (m.verts.empty() && !entitiesShown)
            {
                return;
            }
            entitiesShown = !m.verts.empty();
            if (UObject* material = Material(0, false))
            {
                Upload(entities, 0, m, material, false);
            }
        }
    }

    void RenderTick(UObject* pawn, std::uint32_t epoch)
    {
        auto& link = Link::Get();
        const bool ready = pawn && EnsureHolder(pawn, epoch);
        // Textures that came before there was a world to draw in (main menu): kept until there is.
        struct Pending
        {
            std::uint32_t id, w, h;
            std::vector<std::uint8_t> rgba;
        };
        static std::vector<Pending> pending;
        if (ready && !pending.empty())
        {
            for (const auto& t : pending)
            {
                AddTexture(t.id, t.w, t.h, t.rgba.data());
            }
            pending.clear();
        }
        const auto texture = [&](std::uint32_t id, std::uint32_t w, std::uint32_t h, const std::uint8_t* rgba) {
            if (ready)
            {
                AddTexture(id, w, h, rgba);
                return;
            }
            std::erase_if(pending, [&](const Pending& t) { return t.id == id; });
            pending.push_back({id, w, h, std::vector<std::uint8_t>(rgba, rgba + std::size_t(w) * h * 4)});
        };
        // Always drained (Minecraft stalls on a full ring); applied once there's somewhere to draw.
        link.DrainRender(
            [&](std::uint32_t type, const std::uint8_t* p, std::uint32_t bytes) {
                if (type == proto::kRenAtlas)
                {
                    proto::RenAtlas a;
                    std::memcpy(&a, p, sizeof(a));
                    if (sizeof(a) + std::size_t(a.width) * a.height * 4 <= bytes)
                    {
                        texture(0, a.width, a.height, p + sizeof(a));
                    }
                    return;
                }
                if (type == proto::kRenTexture)
                {
                    proto::RenTexture t;
                    std::memcpy(&t, p, sizeof(t));
                    if (sizeof(t) + std::size_t(t.width) * t.height * 4 <= bytes)
                    {
                        texture(t.id, t.width, t.height, p + sizeof(t));
                    }
                    return;
                }
                if (!ready)
                {
                    return;
                }
                switch (type)
                {
                case proto::kRenSection:
                    OnSection(p, bytes);
                    break;
                case proto::kRenScene:
                    OnScene(p, bytes);
                    break;
                case proto::kRenLights:
                    OnLights(p, bytes);
                    break;
                case proto::kRenClearAll:
                    for (auto& [key, list] : lights)
                    {
                        DestroyLights(list);
                    }
                    lights.clear();
                    for (const auto& [key, slots] : sections)
                    {
                        for (int s : slots)
                        {
                            CallInt(blocks, STR("ClearMeshSection"), STR("SectionIndex"), s);
                        }
                    }
                    sections.clear();
                    freeSlots.clear();
                    nextSlot = 0;
                    break;
                default:
                    break;  // avatar, lights, animated sprites, solids, dug: not yet
                }
            },
            32ull << 20);
        if (ready && ::GetTickCount64() >= nextEntities)
        {
            nextEntities = ::GetTickCount64() + 33;
            BuildEntities();
        }
        if (ready)
        {
            AnimateLights();
        }
    }
}
