// Minecraft's world content as procedural meshes in the game's world (Phase 4).
//
// No editor, so no custom materials: the engine's Widget3DPassThrough materials (shipped with the
// game for widget components) draw a texture parameter "SlateUI" onto mesh UVs. They're unlit, so
// Minecraft content looks like it does in Minecraft rather than lit by the game's lights.
// ponytail: unlit + no vertex tint (grass/leaves stay grey) + no animated sprites; a lit material
// with vertex colours (game material discovery, or a cooked pak) is the upgrade.
#include "Render.hpp"

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

        FWeakObjectPtr holderRef;
        UObject* blocks = nullptr;    // procedural mesh components on the holder actor
        UObject* scene = nullptr;
        UObject* entities = nullptr;
        UObject* masked = nullptr;    // Widget3DPassThrough materials
        UObject* translucent = nullptr;
        std::uint32_t holderEpoch = ~0u;
        bool broken = false;

        std::unordered_map<std::uint32_t, UObject*> textures;        // Minecraft texture id (0 = atlas) -> UTexture2D
        std::map<std::pair<std::uint32_t, bool>, UObject*> materials;  // (texture, translucent) -> MID

        struct SectionSlots
        {
            int solid = -1, blended = -1;
        };
        std::map<std::tuple<int, int, int>, SectionSlots> sections;
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

        bool EnsureHolder(UObject* pawn, std::uint32_t epoch)
        {
            if (broken)
            {
                return false;
            }
            if (holderRef.Get() && holderEpoch == epoch)
            {
                return true;
            }
            if (!SymbolsMatch())
            {
                broken = true;
                return false;
            }
            // A new map: everything Minecraft sent for the old one is gone with its actor.
            sections.clear();
            freeSlots.clear();
            nextSlot = 0;
            sceneSections = 0;
            materials.clear();  // MIDs were made for the old world (still rooted; a few bytes each)
            holderEpoch = epoch;

            masked = LoadObject(STR("/Engine/EngineMaterials/Widget3DPassThrough_Masked.Widget3DPassThrough_Masked"));
            translucent = LoadObject(STR("/Engine/EngineMaterials/Widget3DPassThrough_Translucent.Widget3DPassThrough_Translucent"));
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
            UObject* holder = reflect::Get<UObject*>(spawn, STR("ReturnValue"));
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

        void AddTexture(std::uint32_t id, std::uint32_t w, std::uint32_t h, const std::uint8_t* rgba, UObject* worldContext)
        {
            const auto png = EncodePng(rgba, w, h);
            auto call = reflect::Begin(Cdo(STR("/Script/Engine.Default__KismetRenderingLibrary")), STR("ImportBufferAsTexture2D"));
            if (!call)
            {
                return;
            }
            reflect::Set(call, STR("WorldContextObject"), worldContext);
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
                Log(L"render: texture {} ({}x{}) import failed", id, w, h);
                return;
            }
            // Pixel art: nearest filtering (TF_Nearest), applied by recreating the GPU resource.
            if (auto* filter = tex->GetValuePtrByPropertyNameInChain<std::uint8_t>(STR("Filter")))
            {
                *filter = 0;
            }
            reinterpret_cast<void (*)(UObject*)>(Base() + sym::UTexture_UpdateResource)(tex);
            tex->SetRootSet();  // we hold it until it's replaced; nothing else may reference it yet
            textures[id] = tex;
            std::erase_if(materials, [&](const auto& kv) { return kv.first.first == id; });
            if (id == 0)
            {
                Log(L"render: atlas {}x{} loaded", w, h);
            }
        }

        UObject* Material(std::uint32_t texture, bool blended, UObject* worldContext)
        {
            const auto key = std::make_pair(texture, blended);
            if (auto it = materials.find(key); it != materials.end())
            {
                return it->second;
            }
            auto tex = textures.find(texture);
            auto call = reflect::Begin(Cdo(STR("/Script/Engine.Default__KismetMaterialLibrary")), STR("CreateDynamicMaterialInstance"));
            if (tex == textures.end() || !call)
            {
                return nullptr;
            }
            reflect::Set(call, STR("WorldContextObject"), worldContext);
            reflect::Set(call, STR("Parent"), blended ? translucent : masked);
            reflect::Invoke(call);
            UObject* mid = reflect::Get<UObject*>(call, STR("ReturnValue"));
            if (!mid)
            {
                return nullptr;
            }
            mid->SetRootSet();  // cached across mesh sections that come and go
            if (auto set = reflect::Begin(mid, STR("SetTextureParameterValue")))
            {
                reflect::Set(set, STR("ParameterName"), FName(STR("SlateUI"), FNAME_Add));
                reflect::Set(set, STR("Value"), tex->second);
                reflect::Invoke(set);
            }
            static bool checked = false;
            if (!checked)
            {
                checked = true;
                if (auto get = reflect::Begin(mid, STR("K2_GetTextureParameterValue")))
                {
                    reflect::Set(get, STR("ParameterName"), FName(STR("SlateUI"), FNAME_Add));
                    reflect::Invoke(get);
                    Log(L"render: material texture parameter SlateUI {}", reflect::Get<UObject*>(get, STR("ReturnValue")) == tex->second ? L"holds Minecraft's texture" : L"DID NOT TAKE (wrong parameter name?)");
                }
            }
            materials[key] = mid;
            return mid;
        }

        struct Mesh
        {
            std::vector<FVec> verts;
            std::vector<std::int32_t> tris;
            std::vector<FVec2> uvs;

            void Clear()
            {
                verts.clear();
                tris.clear();
                uvs.clear();
            }
            // A Minecraft-space vertex (blocks), into the game's space.
            void Add(double x, double y, double z, double u, double v)
            {
                const Vec3 ue = McToUe({x, y, z});
                verts.push_back({ue.x, ue.y, ue.z});
                uvs.push_back({u, v});
            }
            // Minecraft triangle lists; swapping Y/Z mirrors space, so the winding flips too.
            void TriangleList()
            {
                for (std::int32_t i = 0; i + 2 < static_cast<std::int32_t>(verts.size()); i += 3)
                {
                    tris.insert(tris.end(), {i, i + 2, i + 1});
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
            reflect::Set(call, STR("SectionIndex"), static_cast<std::int32_t>(slot));
            SetArray(call, STR("Vertices"), mesh.verts, v);
            SetArray(call, STR("Triangles"), mesh.tris, t);
            SetArray(call, STR("UV0"), mesh.uvs, uv);
            reflect::Set(call, STR("bCreateCollision"), collision);
            reflect::Invoke(call);
            for (auto* a : v) a->~TArray();
            for (auto* a : t) a->~TArray();
            for (auto* a : uv) a->~TArray();
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

        // A 16^3 block section: solid (cutout) and blended triangles in two mesh sections.
        void OnSection(const std::uint8_t* p, std::uint32_t bytes, UObject* pawn)
        {
            proto::RenSection hdr;
            std::memcpy(&hdr, p, sizeof(hdr));
            const auto* v = reinterpret_cast<const proto::RenVertex*>(p + sizeof(hdr));
            if (sizeof(hdr) + std::size_t(hdr.vertexCount) * sizeof(proto::RenVertex) > bytes)
            {
                return;
            }
            const auto key = std::make_tuple(hdr.sx, hdr.sy, hdr.sz);
            static Mesh solid, blended;
            solid.Clear();
            blended.Clear();
            const double ox = hdr.sx * 16.0, oy = hdr.sy * 16.0, oz = hdr.sz * 16.0;
            for (std::uint32_t i = 0; i + 2 < hdr.vertexCount; i += 3)
            {
                Mesh& m = (v[i].flags & 2) ? blended : solid;
                for (int k = 0; k < 3; ++k)
                {
                    m.Add(ox + v[i + k].x, oy + v[i + k].y, oz + v[i + k].z, v[i + k].u, v[i + k].v);
                }
            }
            solid.TriangleList();
            blended.TriangleList();
            auto& slots = sections[key];
            if (hdr.vertexCount == 0)
            {
                for (int* s : {&slots.solid, &slots.blended})
                {
                    if (*s >= 0)
                    {
                        CallInt(blocks, STR("ClearMeshSection"), STR("SectionIndex"), *s);
                        freeSlots.push_back(*s);
                    }
                }
                sections.erase(key);
                return;
            }
            if (slots.solid < 0)
            {
                slots.solid = AllocSlot();
            }
            if (slots.blended < 0)
            {
                slots.blended = AllocSlot();
            }
            Upload(blocks, slots.solid, solid, Material(0, false, pawn), true);
            Upload(blocks, slots.blended, blended, Material(0, true, pawn), false);
        }

        // Everything else Minecraft drew this frame (block entities, mobs, particles), relative to an origin.
        void OnScene(const std::uint8_t* p, std::uint32_t bytes, UObject* pawn)
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
                UObject* material = Material(batch.texture, batch.flags & 1, pawn);
                if (!material)
                {
                    continue;  // its texture hasn't arrived yet
                }
                m.Clear();
                for (std::uint32_t i = batch.first; i < batch.first + batch.count; ++i)
                {
                    m.Add(hdr.originX + v[i].x, hdr.originY + v[i].y, hdr.originZ + v[i].z, v[i].u, v[i].v);
                }
                m.TriangleList();
                Upload(scene, used++, m, material, false);
            }
            for (int s = used; s < sceneSections; ++s)
            {
                CallInt(scene, STR("ClearMeshSection"), STR("SectionIndex"), s);
            }
            sceneSections = used;
        }

        // Arrows, tridents, dropped and thrown items: crossed quads with their item icon, like
        // Minecraft's own flat item rendering; dropped blocks as small cubes.
        void BuildEntities(UObject* pawn)
        {
            static proto::WorldEntities we;
            if (!Link::Get().ReadWorldEntities(we))
            {
                return;
            }
            static Mesh m;
            m.Clear();
            const auto quad = [&](const Vec3& c, const Vec3& a, const Vec3& b, const float* uv) {
                // c centre, a and b half-extent vectors (Minecraft space); uv {u0, v0, u1, v1}
                const Vec3 p[4] = {{c.x - a.x - b.x, c.y - a.y - b.y, c.z - a.z - b.z}, {c.x + a.x - b.x, c.y + a.y - b.y, c.z + a.z - b.z},
                                   {c.x + a.x + b.x, c.y + a.y + b.y, c.z + a.z + b.z}, {c.x - a.x + b.x, c.y - a.y + b.y, c.z - a.z + b.z}};
                const double us[4] = {uv[0], uv[2], uv[2], uv[0]}, vs[4] = {uv[3], uv[3], uv[1], uv[1]};
                for (int i : {0, 1, 2, 0, 2, 3})
                {
                    m.Add(p[i].x, p[i].y, p[i].z, us[i], vs[i]);
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
                    quad(c, fwd, side, e.uv[0]);
                    quad(c, fwd, up, e.uv[0]);
                }
                else if (e.kind == proto::kWeItem)
                {
                    const double s = std::max(0.25f, e.scale) * 0.5;
                    const Vec3 a{cy * s, 0, sy * s}, up{0, s, 0}, a2{-sy * s, 0, cy * s};
                    quad({c.x, c.y + s, c.z}, a, up, e.uv[0]);
                    quad({c.x, c.y + s, c.z}, a2, up, e.uv[0]);
                }
                else if (e.kind == proto::kWeBlock)
                {
                    const double s = std::max(0.1f, e.scale) * 0.5;
                    const Vec3 cc{c.x, c.y + s, c.z};
                    const Vec3 ax{cy * s, 0, sy * s}, az{-sy * s, 0, cy * s}, ay{0, s, 0};
                    quad({cc.x + az.x, cc.y, cc.z + az.z}, ax, ay, e.uv[0]);
                    quad({cc.x - az.x, cc.y, cc.z - az.z}, ax, ay, e.uv[0]);
                    quad({cc.x + ax.x, cc.y, cc.z + ax.z}, az, ay, e.uv[0]);
                    quad({cc.x - ax.x, cc.y, cc.z - ax.z}, az, ay, e.uv[0]);
                    quad({cc.x, cc.y + s, cc.z}, ax, az, e.uv[1]);
                    quad({cc.x, cc.y - s, cc.z}, ax, az, e.uv[2]);
                }
            }
            m.TriangleList();
            if (m.verts.empty() && !entitiesShown)
            {
                return;
            }
            entitiesShown = !m.verts.empty();
            if (UObject* material = Material(0, false, pawn))
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
                AddTexture(t.id, t.w, t.h, t.rgba.data(), pawn);
            }
            pending.clear();
        }
        const auto texture = [&](std::uint32_t id, std::uint32_t w, std::uint32_t h, const std::uint8_t* rgba) {
            if (ready)
            {
                AddTexture(id, w, h, rgba, pawn);
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
                    OnSection(p, bytes, pawn);
                    break;
                case proto::kRenScene:
                    OnScene(p, bytes, pawn);
                    break;
                case proto::kRenClearAll:
                    for (const auto& [key, slots] : sections)
                    {
                        for (int s : {slots.solid, slots.blended})
                        {
                            if (s >= 0)
                            {
                                CallInt(blocks, STR("ClearMeshSection"), STR("SectionIndex"), s);
                            }
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
            BuildEntities(pawn);
        }
    }
}
