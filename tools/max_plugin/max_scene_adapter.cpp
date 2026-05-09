// ============================================================================
// WhiteoutDex Max Scene Adapter — Implements IModelSource for 3ds Max scenes
// Refactored from extract.cpp. All Max SDK coupling lives here.
// ============================================================================

#include "max_scene_adapter.h"
#include "common_types.h"
#include "io/team_glow_data.h"
#include "io/content_provider.h"
#include "io/texture_image_usage.h"
#include "renderer/coordinate_system.h"
#include "renderer/model/model_source_utils.h"

#include <maxscript/maxscript.h>
#include <maxscript/foundation/numbers.h>
#include <Windows.h>
#include <chrono>
#include <algorithm>
#include <cwchar>
#include <cstring>
#include <numbers>
#include <fstream>

using namespace whiteout::flakes;
using namespace whiteout::flakes::io;
using namespace whiteout::flakes::renderer;
using namespace whiteout::flakes::renderer::model;
using namespace whiteout::flakes::renderer::effects;

static Point3 GetVNormal(Mesh& mesh, i32 faceIdx, i32 vertIdx);

// ----------------------------------------------------------------------------
// File-local helpers:
//   * Coord-space bridge — 3ds Max is always Max-space; these lift Max
//     positions/directions into the renderer-native default. No-op when
//     WDX_DEFAULT_COORD_SPACE=Max.
//   * Traversal / material / texture primitives shared by every phase.
// ----------------------------------------------------------------------------
namespace {

inline Vector3f MaxPointToDefault(const Point3& p) {
    return CoordinateSystem::ToDefault(CoordSpace::Max, Vector3f{p.x, p.y, p.z});
}
inline Vector3f MaxDirToDefault(const Point3& n) {
    return CoordinateSystem::ToDefaultDir(CoordSpace::Max, Vector3f{n.x, n.y, n.z});
}

// Depth-first traversal of the scene's root children, invoking `fn(node)` on
// every descendant. Handles the "no interface available" case by returning
// early (callers treat this as an empty collection).
template <typename Fn>
void ForEachSceneNode(Fn fn) {
    Interface* ip = GetCOREInterface();
    if (!ip) return;
    std::function<void(INode*)> recurse = [&](INode* node) {
        if (!node) return;
        fn(node);
        for (i32 c = 0; c < node->NumberOfChildren(); c++) recurse(node->GetChildNode(c));
    };
    INode* root = ip->GetRootNode();
    for (i32 i = 0; i < root->NumberOfChildren(); i++) recurse(root->GetChildNode(i));
}

// Iterate Wc3Material layers: if `mtl` IS a Wc3Material, call `fn(mtl, 0)`;
// otherwise iterate sub-materials and invoke `fn(sub, layerIdx)` for each
// Wc3Material sub-material (non-Wc3 sub-materials are skipped, matching
// CollectMaterials' Wc3-only layer list).
template <typename Fn>
void ForEachWc3SubMtl(Mtl* mtl, Fn fn) {
    if (!mtl) return;
    if (mtl->ClassID() == WARCRAFT3_MAT_CLASS_ID) { fn(mtl, 0); return; }
    if (mtl->NumSubMtls() <= 0) return;
    i32 layerIdx = 0;
    for (i32 si = 0; si < mtl->NumSubMtls(); si++) {
        Mtl* sub = mtl->GetSubMtl(si);
        if (sub && sub->ClassID() == WARCRAFT3_MAT_CLASS_ID)
            fn(sub, layerIdx++);
    }
}

// Unwrap a Texmap* to its underlying BitmapTex. Returns tex itself when it's
// a native BitmapTex; for the legacy Wc3Bitmap wrapper, returns the first
// contained BitmapTex sub-reference; otherwise nullptr. Used by every path
// that needs to peek at the bitmap filename or its UVGen.
inline BitmapTex* UnwrapBitmapTex(Texmap* tex) {
    if (!tex) return nullptr;
    if (tex->ClassID() == Class_ID(BMTEX_CLASS_ID, 0))
        return static_cast<BitmapTex*>(tex);
    if (tex->ClassID() == WC3_BITMAP_CLASS_ID) {
        for (i32 r = 0; r < tex->NumRefs(); r++) {
            ReferenceTarget* ref = tex->GetReference(r);
            if (ref && ref->ClassID() == Class_ID(BMTEX_CLASS_ID, 0))
                return static_cast<BitmapTex*>(ref);
        }
    }
    return nullptr;
}

// Max 2022+ exposes GetObjectName(bool); earlier versions need GetClassName(MSTR&)
// with the MSTR holder outliving the returned pointer. Callers pass their own
// MSTR so the storage stays in scope.
inline const MCHAR* GetObjectClassName(Object* obj, [[maybe_unused]] MSTR& scratch) {
#if MAX_PRODUCT_YEAR_NUMBER >= 2022
    return obj->GetObjectName(false);
#else
    obj->GetClassName(scratch);
    return scratch.data();
#endif
}

// DFS over an Animatable's param blocks, invoking `fn(pblock, pid, paramDef)`
// for the first param whose int_name matches `name` (case-insensitive). Returns
// true if the param was found. The five typed PB2 getters share this scan.
template <typename Fn>
bool FindPB2Param(Animatable* anim, const wchar_t* name, Fn fn) {
    if (!anim) return false;
    for (i32 pb = 0; pb < anim->NumParamBlocks(); pb++) {
        IParamBlock2* pblock = anim->GetParamBlock(pb);
        if (!pblock) continue;
        for (i32 p = 0; p < pblock->NumParams(); p++) {
            ParamID pid = pblock->IndextoID(p);
            ParamDef& def = pblock->GetParamDef(pid);
            if (def.int_name && _wcsicmp(def.int_name, name) == 0) {
                fn(pblock, pid, def);
                return true;
            }
        }
    }
    return false;
}

} // namespace

// ============================================================================
// Ctor / Dtor
// ============================================================================

MaxSceneAdapter::MaxSceneAdapter() {}
MaxSceneAdapter::~MaxSceneAdapter() {}

// ============================================================================
// PackMatrix: Matrix3 (Max-space row-major) → Matrix44f (renderer-native)
// 3ds Max gives us a row-major Max-space transform; pack it, then conjugate
// into the renderer-native space so every downstream caller (bones,
// attachments, emitters, cameras) receives default-space matrices.
// ============================================================================

Matrix44f MaxSceneAdapter::PackMatrix(const Matrix3& tm) {
    Matrix44f m = Matrix44f::identity();
    for (i32 r = 0; r < 3; r++) {
        Point3 row = tm.GetRow(r);
        m.data[r][0] = row.x; m.data[r][1] = row.y; m.data[r][2] = row.z; m.data[r][3] = 0.0f;
    }
    Point3 trans = tm.GetRow(3);
    m.data[3][0] = trans.x; m.data[3][1] = trans.y; m.data[3][2] = trans.z; m.data[3][3] = 1.0f;
    return CoordinateSystem::ToDefault(CoordSpace::Max, m);
}

// ============================================================================
// PB2*Or — typed PB2 reads that collapse the `int v = def; if (PB2Int(...)) ...`
// idiom into a single call. Return `def` on miss, the read value otherwise.
// ============================================================================

i32 MaxSceneAdapter::PB2IntOr(Animatable* a, const wchar_t* name, TimeValue t, i32 def) {
    i32 v = def; PB2Int(a, name, t, v); return v;
}
f32 MaxSceneAdapter::PB2FloatOr(Animatable* a, const wchar_t* name, TimeValue t, f32 def) {
    f32 v = def; PB2Float(a, name, t, v); return v;
}
bool MaxSceneAdapter::PB2BoolOr(Animatable* a, const wchar_t* name, TimeValue t, bool def) {
    BOOL v = def ? TRUE : FALSE; PB2Bool(a, name, t, v); return v != 0;
}

// ============================================================================
// IParamBlock2 helpers — the five typed getters share FindPB2Param's scan and
// only differ in how they unpack the matched ParamDef + pblock value.
// ============================================================================

bool MaxSceneAdapter::PB2Float(Animatable* anim, const wchar_t* name, TimeValue t, f32& out) {
    return FindPB2Param(anim, name, [&](IParamBlock2* pblock, ParamID pid, ParamDef& def) {
        Interval iv = FOREVER;
        if (def.type == TYPE_INT) { i32 v = 0; pblock->GetValue(pid, t, v, iv); out = (f32)v; }
        else                      {            pblock->GetValue(pid, t, out, iv); }
    });
}

bool MaxSceneAdapter::PB2Int(Animatable* anim, const wchar_t* name, TimeValue t, i32& out) {
    return FindPB2Param(anim, name, [&](IParamBlock2* pblock, ParamID pid, ParamDef& def) {
        Interval iv = FOREVER;
        if (def.type == TYPE_FLOAT) { f32 v = 0; pblock->GetValue(pid, t, v, iv); out = (i32)v; }
        else                        {              pblock->GetValue(pid, t, out, iv); }
    });
}

bool MaxSceneAdapter::PB2Bool(Animatable* anim, const wchar_t* name, TimeValue t, BOOL& out) {
    return FindPB2Param(anim, name, [&](IParamBlock2* pblock, ParamID pid, ParamDef&) {
        Interval iv = FOREVER;
        i32 val = 0; pblock->GetValue(pid, t, val, iv);
        out = val ? TRUE : FALSE;
    });
}

bool MaxSceneAdapter::PB2Color(Animatable* anim, const wchar_t* name, TimeValue t, Color& out) {
    return FindPB2Param(anim, name, [&](IParamBlock2* pblock, ParamID pid, ParamDef&) {
        // If any channel > 1 the value is in [0-255] (TYPE_RGBA / scripted
        // #color); otherwise it's already [0-1] (TYPE_POINT3 / colorswatch).
        Color cv = pblock->GetColor(pid, t);
        if (cv.r > 1.0f || cv.g > 1.0f || cv.b > 1.0f)
            out = Color(cv.r / 255.0f, cv.g / 255.0f, cv.b / 255.0f);
        else
            out = cv;
    });
}

bool MaxSceneAdapter::PB2Texmap(Animatable* anim, const wchar_t* name, Texmap*& out) {
    return FindPB2Param(anim, name, [&](IParamBlock2* pblock, ParamID pid, ParamDef&) {
        Interval iv = FOREVER;
        pblock->GetValue(pid, 0, out, iv);
    });
}

Object* MaxSceneAdapter::GetBaseObject(INode* node) {
    if (!node) return nullptr;
    Object* obj = node->GetObjectRef();
    while (obj && obj->SuperClassID() == GEN_DERIVOB_CLASS_ID)
        obj = static_cast<IDerivedObject*>(obj)->GetObjRef();
    return obj;
}

Modifier* MaxSceneAdapter::FindSkinModifier(INode* node) { return FindModifierByClassID(node, SKIN_CLASSID); }

Modifier* MaxSceneAdapter::FindModifierByClassID(INode* node, Class_ID cid) {
    if (!node) return nullptr;
    Object* objRef = node->GetObjectRef();
    while (objRef && objRef->SuperClassID() == GEN_DERIVOB_CLASS_ID) {
        IDerivedObject* dobj = static_cast<IDerivedObject*>(objRef);
        for (i32 i = 0; i < dobj->NumModifiers(); i++) {
            Modifier* mod = dobj->GetModifier(i);
            if (mod && mod->ClassID() == cid) return mod;
        }
        objRef = dobj->GetObjRef();
    }
    return nullptr;
}

Modifier* MaxSceneAdapter::FindModifierByClassName(INode* node,
                                                   const wchar_t* const* nameSubstrings) {
    if (!node || !nameSubstrings) return nullptr;
    Object* obj = node->GetObjectRef();
    i32 safety = 32;  // bound the IDerivedObject chain so a self-referential
                      // stack can't hang the walker (mirrors the exporter).
    while (obj && safety-- > 0) {
        if (obj->SuperClassID() != GEN_DERIVOB_CLASS_ID) break;
        IDerivedObject* dobj = static_cast<IDerivedObject*>(obj);
        const i32 n = dobj->NumModifiers();
        if (n < 0 || n > 256) break;
        for (i32 i = 0; i < n; i++) {
            Modifier* mod = dobj->GetModifier(i);
            if (!mod) continue;
            MSTR cname;
            mod->GetClassName(cname);
            const wchar_t* nm = cname.data();
            if (!nm) continue;
            for (i32 k = 0; nameSubstrings[k]; k++) {
                if (wcsstr(nm, nameSubstrings[k]) != nullptr) return mod;
            }
        }
        Object* next = dobj->GetObjRef();
        if (next == obj) break;  // self-reference guard
        obj = next;
    }
    return nullptr;
}

std::wstring MaxSceneAdapter::GetMaxFilePath() {
    Interface* ip = GetCOREInterface();
    if (!ip) return L"";
#if MAX_PRODUCT_YEAR_NUMBER >= 2022
    const MCHAR* fp = ip->GetCurFilePath().data();
#else
    const MCHAR* fp = ip->GetCurFilePath();
#endif
    if (!fp || !fp[0]) return L"";
    std::wstring path(fp);
    usize pos = path.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? path.substr(0, pos + 1) : L"";
}

// ============================================================================
// Texture loading (stores pixel data for GetTextures() instead of renderer calls)
// ============================================================================

// Read raw bytes from a wide-string disk path; ext receives the lowercased extension.
static bool ReadFileBytesFromDisk(const std::wstring& filePath,
                                  std::vector<u8>& out, std::string& ext) {
    std::filesystem::path p(filePath);
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    if (out.empty()) return false;
    ext = ExtensionLower(p);
    return true;
}

// ============================================================================
// RegisterTexture — single entry point for stashing a decoded RGBA8 buffer
// into both loadedTextures_ and texEntries_. All loader paths funnel here so
// there's only one definition of "what an entry looks like".
// ============================================================================

i32 MaxSceneAdapter::RegisterTexture(const std::wstring& key, i32 replaceableId,
                                      std::vector<u8>&& pixels, i32 width, i32 height,
                                      const std::wstring& displayPath,
                                      std::string sharedKey) {
    i32 id = nextTexId_++;
    if (!key.empty()) texPathToId_[key] = id;
    loadedTextures_.push_back({id, replaceableId, std::move(pixels), width, height,
                                std::move(sharedKey)});
    TextureEntry te;
    te.textureId     = id;
    te.replaceableId = replaceableId;
    te.filePath      = displayPath.empty() ? key : displayPath;
    texEntries_.push_back(te);
    return id;
}

// EnsureHdTeamColorSentinel removed — adapters now set
// MaterialLayerData::teamColorMapId = kHdTeamColorActive when the HD layer
// flags its team-colour slot as live-driven. ReplaceableTextureManager owns
// the live swatch texture; no per-model sentinel allocation is needed.

// ============================================================================
// ResolveBitmapPath — resolve a BitmapTex filename from a Mtl's named texmap
// slot. Accepts either a native BitmapTex directly or the legacy Wc3Bitmap
// wrapper (which keeps a real BitmapTex as its first matching sub-reference).
// Returns the empty string when the slot is empty, isn't a BitmapTex, or has
// no filename assigned.
// ============================================================================

std::wstring MaxSceneAdapter::ResolveBitmapPath(Mtl* mtl, const wchar_t* paramName) {
    Texmap* tex = nullptr;
    if (!PB2Texmap(mtl, paramName, tex)) return {};
    BitmapTex* bmt = UnwrapBitmapTex(tex);
    if (!bmt) return {};
    const MCHAR* fn = bmt->GetMapName();
    return (fn && fn[0]) ? std::wstring(fn) : std::wstring{};
}

// ============================================================================
// ReadWc3MaterialFlags — pack the six Wc3Material bool toggles into the
// renderer's compact MaterialLayerData::flags bitmask.
// ============================================================================

i32 MaxSceneAdapter::ReadWc3MaterialFlags(Mtl* mtl) {
    static constexpr struct { const wchar_t* name; i32 bit; } kFlags[] = {
        { L"twoSided",      1  },
        { L"unshaded",      2  },
        { L"unfogged",      4  },
        { L"noDepthTest",   8  },
        { L"noDepthSet",   16  },
        { L"constantColor",32  },
    };
    i32 flags = 0;
    for (auto& f : kFlags) {
        if (PB2BoolOr(mtl, f.name, 0, false)) flags |= f.bit;
    }
    return flags;
}

// ============================================================================
// SnapshotMaterial / UpdateMaterialSnapshots — drive the material-change
// detection used by RefreshMaterials. Both CollectScene and RefreshMaterials
// previously duplicated this property read; now they share one implementation.
// ============================================================================

MaxSceneAdapter::MaterialSnapshot MaxSceneAdapter::SnapshotMaterial(Mtl* mtl) {
    MaterialSnapshot snap;
    snap.filterMode         = MapFilterMode(PB2IntOr(mtl, L"filterMode", 0, 1) - 1);
    snap.flags              = ReadWc3MaterialFlags(mtl);
    snap.replaceableTexture = std::max(0, PB2IntOr(mtl, L"replaceableId", 0, 1) - 1);
    // Shader dropdown is 1-based (1=SD, 2=HD, 3=SDOnHD, 4=Crystal → renderer 24).
    i32 shaderType = PB2IntOr(mtl, L"shaderType", 0, 1);
    snap.shaderId           = (shaderType == 4) ? 24 : std::max(0, shaderType - 1);
    snap.sortOrder          = std::max(0, PB2IntOr(mtl, L"sortOrder", 0, 1) - 1);
    snap.priorityPlane      = PB2IntOr(mtl, L"priorityPlane", 0, 0);
    snap.texturePath        = ResolveBitmapPath(mtl, L"diffuseMap");
    snap.normalTexPath      = ResolveBitmapPath(mtl, L"normalMap");
    snap.ormTexPath         = ResolveBitmapPath(mtl, L"ormMap");
    snap.emissiveTexPath    = ResolveBitmapPath(mtl, L"emissiveMap");
    snap.teamColorTexPath   = ResolveBitmapPath(mtl, L"teamColorMap");
    return snap;
}

void MaxSceneAdapter::UpdateMaterialSnapshots() {
    matSnapshots_.clear();
    for (auto& mi : materials_) {
        ForEachWc3SubMtl(mi.mtl, [&](Mtl* sub, i32 layerIdx) {
            // Single Wc3Material → key by materialId; composite → combined key.
            const i32 key = (sub == mi.mtl) ? mi.materialId
                                            : (mi.materialId * 1000 + layerIdx);
            matSnapshots_[key] = SnapshotMaterial(sub);
        });
    }
}

// Load a texture from the FileContentProvider (CASC/MPQ fallback).
// archivePath should be a WC3-relative path like "Textures\\Dirt.blp"
// (forward or back slashes are both accepted by the provider).
// Returns a texture id on success, or -1 if not found.
i32 MaxSceneAdapter::LoadTextureFromContentProvider(const std::string& archivePath, i32 replaceableId) {
    if (archivePath.empty()) return -1;

    // Use the wide-string key so we share the texPathToId_ cache.
    std::wstring wkey(archivePath.begin(), archivePath.end());
    auto cached = texPathToId_.find(wkey);
    if (cached != texPathToId_.end()) return cached->second;

    // Cross-model dedup: skip the CASC extraction + decode entirely when
    // the renderer has the archive path cached from a previous model load.
    std::string sharedKey = NormalizeTextureKey(archivePath);
    if (IsTextureCached(sharedKey)) {
        mprintf(_M("    [ContentProvider] cache hit, skipping decode: '%S'\n"), archivePath.c_str());
        return RegisterTexture(wkey, replaceableId, {}, 0, 0, /*displayPath*/L"",
                               std::move(sharedKey));
    }

    std::string foundExt;
    auto data = contentProvider_.ReadFile(archivePath, &foundExt);
    if (!data || data->empty()) {
        mprintf(_M("    [ContentProvider] not found: '%S'\n"), archivePath.c_str());
        return -1;
    }
    if (foundExt.empty()) foundExt = ExtensionLower(std::filesystem::path(archivePath));

    std::vector<u8> pixels; i32 w = 0, h = 0;
    if (!DecodeToRGBA8(*data, foundExt, pixels, w, h)) {
        mprintf(_M("    [ContentProvider] parse failed: '%S'\n"), archivePath.c_str());
        return -1;
    }
    i32 id = RegisterTexture(wkey, replaceableId, std::move(pixels), w, h,
                             /*displayPath*/L"", std::move(sharedKey));
    mprintf(_M("    [ContentProvider] loaded '%S' as texId=%d (%dx%d)\n"), archivePath.c_str(), id, w, h);
    return id;
}

// Decode an entire Max Bitmap into a tight RGBA8 buffer (and optionally sum
// per-channel totals for diagnostics). The caller owns the returned pixels.
static std::vector<u8> BitmapToRGBA8(Bitmap* bmp, i32 w, i32 h,
                                          long long* sumR = nullptr,
                                          long long* sumG = nullptr,
                                          long long* sumB = nullptr,
                                          long long* sumA = nullptr) {
    std::vector<u8> rgba(usize(w) * usize(h) * 4);
    std::vector<BMM_Color_64> line(w);
    long long r = 0, g = 0, b = 0, a = 0;
    for (i32 y = 0; y < h; y++) {
        bmp->GetPixels(0, y, w, line.data());
        for (i32 x = 0; x < w; x++) {
            i32 idx = (y * w + x) * 4;
            rgba[idx    ] = (u8)(line[x].r >> 8);
            rgba[idx + 1] = (u8)(line[x].g >> 8);
            rgba[idx + 2] = (u8)(line[x].b >> 8);
            rgba[idx + 3] = (u8)(line[x].a >> 8);
            r += rgba[idx]; g += rgba[idx + 1]; b += rgba[idx + 2]; a += rgba[idx + 3];
        }
    }
    if (sumR) *sumR = r; if (sumG) *sumG = g; if (sumB) *sumB = b; if (sumA) *sumA = a;
    return rgba;
}

// Load a file through the Max bitmap manager and return its contents decoded
// to RGBA8. Returns nullopt if the load fails or throws — callers fall back
// to direct-decode and content-provider paths on failure. Optional sum*
// out-params receive per-channel totals for diagnostic logging.
struct MaxBitmapRGBA { std::vector<u8> rgba; i32 width = 0, height = 0; };
static std::optional<MaxBitmapRGBA> LoadMaxBitmapRGBA(const std::wstring& filePath,
                                                     const wchar_t* logPrefix,
                                                     long long* sumR = nullptr,
                                                     long long* sumG = nullptr,
                                                     long long* sumB = nullptr,
                                                     long long* sumA = nullptr) {
    Bitmap* bmp = nullptr;
    BMMRES status = BMMRES_IOERROR;
    try {
        BitmapInfo bi; bi.SetName(filePath.c_str());
        bmp = TheManager->Load(&bi, &status);
    } catch (...) {
        mprintf(_M("  %s [exception in TheManager->Load] '%s'\n"), logPrefix, filePath.c_str());
    }
    if (!bmp || status != BMMRES_SUCCESS) {
        mprintf(_M("  %s [Max bitmap failed, status=%d] '%s'\n"),
                logPrefix, (i32)status, filePath.c_str());
        return std::nullopt;
    }
    MaxBitmapRGBA out;
    out.width  = bmp->Width();
    out.height = bmp->Height();
    out.rgba   = BitmapToRGBA8(bmp, out.width, out.height, sumR, sumG, sumB, sumA);
    bmp->DeleteThis();
    return out;
}

i32 MaxSceneAdapter::LoadTexture(const std::wstring& filePath, i32 replaceableId) {
    if (replaceableId != 0) {
        // Replaceable slot: adapter only declares the id. The renderer-
        // side ReplaceableTextureManager::RegisterModelSlot path bakes
        // pixels (TeamColor/TeamGlow ids 1/2) or loads canonical CASC
        // assets (higher ids) once the actor is staged. We dedupe by
        // a synthetic key so two emitters declaring the same id share
        // one slot.
        wchar_t buf[32];
        swprintf_s(buf, L"__REPL_%d__", replaceableId);
        std::wstring key = buf;
        auto it = texPathToId_.find(key);
        if (it != texPathToId_.end()) return it->second;
        return RegisterTexture(key, replaceableId, {}, 0, 0);
    }

    if (filePath.empty()) return -1;
    auto it = texPathToId_.find(filePath);
    if (it != texPathToId_.end()) return it->second;

    // Cross-model dedup: if the renderer's shared cache already has this
    // path, reserve a borrow slot WITHOUT decoding. UploadStagedTextures
    // sees the empty rgba + non-empty sharedKey and binds via
    // TextureAssetManager::BindShared. This skips the entire bitmap-
    // manager / disk / CASC pipeline for textures another model loaded.
    std::string sharedKey = NormalizeTextureKey(filePath);
    if (IsTextureCached(sharedKey)) {
        mprintf(_M("  Texture: [cache hit, skipping decode] '%s'\n"), filePath.c_str());
        return RegisterTexture(filePath, 0, {}, 0, 0, /*displayPath*/L"",
                               std::move(sharedKey));
    }

    // Primary path: delegate to 3ds Max's bitmap manager.
    long long sumR = 0, sumG = 0, sumB = 0, sumA = 0;
    if (auto bmp = LoadMaxBitmapRGBA(filePath, L"Texture:", &sumR, &sumG, &sumB, &sumA)) {
        const i32 total = bmp->width * bmp->height;
        i32 id = RegisterTexture(filePath, 0, std::move(bmp->rgba), bmp->width, bmp->height,
                                 /*displayPath*/L"", sharedKey);
        mprintf(_M("  Texture %d: %dx%d avgRGBA=[%d,%d,%d,%d] '%s'\n"),
                id, bmp->width, bmp->height,
                (i32)(sumR / total), (i32)(sumG / total),
                (i32)(sumB / total), (i32)(sumA / total), filePath.c_str());
        return id;
    }

    mprintf(_M("  Texture: trying direct decode for '%s'\n"), filePath.c_str());

    // Fallback 1: direct decode with our own parsers.
    {
        std::vector<u8> fileBytes; std::string ext;
        std::vector<u8> pixels; i32 pw = 0, ph = 0;
        if (ReadFileBytesFromDisk(filePath, fileBytes, ext) &&
            DecodeToRGBA8(fileBytes, ext, pixels, pw, ph)) {
            i32 id = RegisterTexture(filePath, replaceableId, std::move(pixels), pw, ph,
                                     /*displayPath*/L"", sharedKey);
            mprintf(_M("  Texture %d: %dx%d [direct decode] '%s'\n"), id, pw, ph, filePath.c_str());
            return id;
        }
    }

    // Fallback 2: CASC/MPQ by filename.
    {
        std::string narrowName = std::filesystem::path(filePath).filename().string();
        mprintf(_M("  Texture: [ContentProvider fallback] '%S'\n"), narrowName.c_str());
        i32 id = LoadTextureFromContentProvider(narrowName, replaceableId);
        if (id >= 0) {
            texPathToId_[filePath] = id;  // alias the wide path to the archive id
            return id;
        }
    }

    // All fallbacks exhausted — magenta placeholder. No sharedKey: a missing
    // file should NOT poison the dedup cache for everyone else.
    std::vector<u8> rgba;
    FillSolidRGBA(rgba, 4, 4, 255, 0, 255, 255);
    i32 id = RegisterTexture(filePath, 0, std::move(rgba), 4, 4);
    mprintf(_M("  Texture %d: [missing] %s\n"), id, filePath.c_str());
    return id;
}

// LoadTextureWithTeamColor removed — WC3's SD engine ignores the
// authored diffuse for TEAMCOLOR layers and binds a flat swatch at t0.
// The TEAMCOLOR branch now funnels through LoadTexture(L"", 1) so the
// slot is populated by ReplaceableTextureManager::BakeSlot from the
// current swatch (and re-baked live on SetTeamColor, matching HD).

// GenerateTeamGlowTexture removed — the TEAMGLOW branch in
// ExtractWc3MaterialLayer now calls LoadTexture(L"", 2), which reserves a
// textureId with replaceableId=2 and leaves the pixel bake to
// ReplaceableTextureManager on first RegisterModelSlot.

// ============================================================================
// CollectScene — called once on main thread before Get*()
// ============================================================================

void MaxSceneAdapter::CollectScene() {
    nextTexId_ = 0; nextMatId_ = 0; texPathToId_.clear(); mtlToId_.clear();
    loadedTextures_.clear(); texEntries_.clear();
    bones_.clear(); boneNameToIdx_.clear(); geosets_.clear();
    materials_.clear(); particles_.clear(); pe1Emitters_.clear();
    attachments_.clear(); ribbons_.clear(); collisions_.clear();

    CollectGeometry();
    CollectMaterials();
    CollectBones();
    CollectAttachments();
    CollectParticleEmitters();
    CollectRibbonEmitters();
    CollectCollisionShapes();

    // Capture the initial material-state snapshot so RefreshMaterials can
    // detect per-property changes on subsequent frames.
    UpdateMaterialSnapshots();
}

// ============================================================================
// Collect geometry (identical logic to old extract.cpp)
// ============================================================================

void MaxSceneAdapter::CollectGeometry() {
    geosets_.clear();
    i32 geosetId = 0;

    ForEachSceneNode([&](INode* node) {
        if (node->IsNodeHidden()) return;
        Object* baseObj = GetBaseObject(node);
        if (!baseObj) return;
        if (baseObj->SuperClassID() != GEOMOBJECT_CLASS_ID) return;
        // PE2 / ribbon helpers are geometry at the SDK level but handled
        // separately by the emitter collectors.
        if (baseObj->ClassID() == WC3PARTICLES2_CLASS_ID ||
            baseObj->ClassID() == WC3RIBBON_CLASS_ID)    return;
        if (!baseObj->CanConvertToType(triObjectClassID)) return;

        MSTR classNameBuf;
        const MCHAR* className = GetObjectClassName(baseObj, classNameBuf);
        if (_wcsicmp(className, L"Editable Mesh") != 0 &&
            _wcsicmp(className, L"Editable Poly") != 0) return;

        // Skip geometry whose only material slot is a high-index replaceable
        // with no actual bitmap bound — these are placeholder meshes.
        Mtl* mtl = node->GetMtl();
        if (mtl && mtl->ClassID() == WARCRAFT3_MAT_CLASS_ID) {
            Texmap* diffTex = nullptr; PB2Texmap(mtl, L"diffuseMap", diffTex);
            i32 retex = 0;
            if (diffTex && diffTex->ClassID() == WC3_BITMAP_CLASS_ID) {
                retex = std::max(0, PB2IntOr(diffTex, L"replaceableId", 0, 1) - 1);
            }
            if (retex >= 4 && !diffTex) {
                mprintf(_M("  [Skipped replaceable %d geoset '%s']\n"), retex, node->GetName());
                return;
            }
        }

        GeosetInfo gi; gi.geosetId = geosetId++; gi.node = node;
        geosets_.push_back(gi);
    });
}

// ============================================================================
// Extract a single Wc3Material into a MaterialLayerInfo
// ============================================================================

MaterialLayerInfo MaxSceneAdapter::ExtractWc3MaterialLayer(Mtl* mtl) {
    MaterialLayerInfo layer;

    layer.filterMode         = MapFilterMode(PB2IntOr(mtl, L"filterMode", 0, 1) - 1);
    layer.alpha              = std::min(PB2FloatOr(mtl, L"opacity", 0, 100.0f) / 100.0f, 1.0f);
    layer.replaceableTexture = std::max(0, PB2IntOr(mtl, L"replaceableId", 0, 1) - 1);
    // Shader dropdown is 1-based (1=SD, 2=HD, 3=SDOnHD, 4=Crystal → renderer 24).
    i32 shaderType           = PB2IntOr(mtl, L"shaderType", 0, 1);
    layer.shaderId           = (shaderType == 4) ? 24 : std::max(0, shaderType - 1);
    layer.flags              = ReadWc3MaterialFlags(mtl);

    // Reforged PBR knobs. The fresnelColor trio is only written when at least
    // one component was authored — zero defaults mean "classic MDX, no fresnel".
    layer.emissiveGain     = PB2FloatOr(mtl, L"emissiveGain",   0, layer.emissiveGain);
    layer.fresnelOpacity   = PB2FloatOr(mtl, L"fresnelOpacity", 0, layer.fresnelOpacity);
    layer.fresnelTeamColor = PB2FloatOr(mtl, L"fresnelTeamCol", 0, layer.fresnelTeamColor);
    {
        f32 fr = 0, fg = 0, fb = 0;
        const bool hasR = PB2Float(mtl, L"fresnelR", 0, fr);
        const bool hasG = PB2Float(mtl, L"fresnelG", 0, fg);
        const bool hasB = PB2Float(mtl, L"fresnelB", 0, fb);
        if (hasR || hasG || hasB) layer.fresnelColor = {fr, fg, fb};
    }

    // HD subtexture slots. Missing paths stay at -1; the renderer treats
    // negative ids as "slot absent".
    auto loadSlot = [&](const wchar_t* paramName) -> i32 {
        std::wstring path = ResolveBitmapPath(mtl, paramName);
        return path.empty() ? -1 : LoadTexture(path, 0);
    };
    layer.normalMapId    = loadSlot(L"normalMap");
    layer.ormMapId       = loadSlot(L"ormMap");
    layer.emissiveMapId  = loadSlot(L"emissiveMap");

    // teamColorMap is special — the slot accepts both:
    //   (a) a Wc3 replaceable=1 placeholder (Wc3Bitmap with replaceableId=1,
    //       usually no path) → renderer fills with the live UI swatch at
    //       draw time. We map this to kHdTeamColorActive so the HD draw
    //       knows to bind the per-actor HD swatch at t4.
    //   (b) any other Wc3Bitmap (custom mask BLP / DDS the artist dropped
    //       in this slot) → carry the real loaded texture id through and
    //       let the HD draw bind it like a regular material slot.
    //
    // We read the assigned Texmap's replaceableId directly so a custom
    // mask plus replaceableId=1 still picks the swatch (engine convention
    // is "replaceableId is authoritative for this slot's binding").
    {
        Texmap* tcTex = nullptr;
        const bool hasTexmap = PB2Texmap(mtl, L"teamColorMap", tcTex) && tcTex;
        const bool isWc3Bitmap = hasTexmap && tcTex->ClassID() == WC3_BITMAP_CLASS_ID;
        const i32  tcReplId    = isWc3Bitmap
            ? std::max(0, PB2IntOr(tcTex, L"replaceableId", 0, 1) - 1)
            : 0;

        if (isWc3Bitmap && tcReplId == 1) {
            // Live swatch placeholder.
            layer.teamColorMapId = kHdTeamColorActive;
        } else {
            // Try to load whatever path the user assigned; -1 if none.
            layer.teamColorMapId = loadSlot(L"teamColorMap");
            // Fallback: HD layer with a Texmap at this slot but no path
            // (e.g. an empty BitmapTex). Treat as the swatch placeholder
            // so the slot still drives team-colour blending at draw time.
            if (layer.teamColorMapId < 0 && hasTexmap
                && (layer.shaderId == 1 || layer.shaderId == 24)) {
                layer.teamColorMapId = kHdTeamColorActive;
            }
        }
    }

    const std::wstring baseTexPath = ResolveBitmapPath(mtl, L"diffuseMap");
    i32 baseTexId = -1;
    if (!baseTexPath.empty()) {
        mprintf(_M("    \x2192 diffuse path: '%s'\n"), baseTexPath.c_str());
        baseTexId = LoadTexture(baseTexPath, 0);
    } else {
        // Diagnostic: distinguish "no texmap" from "texmap but not a BitmapTex".
        Texmap* texmap = nullptr;
        if (PB2Texmap(mtl, L"diffuseMap", texmap) && texmap)
            mprintf(_M("    \x2192 diffuseMap texmap is NOT a BitmapTexture (e.g. Mix/Composite)\n"));
        else
            mprintf(_M("    \x2192 NO texmap found for 'diffuseMap' property\n"));
    }

    // TeamColor / TeamGlow / replaceable-id resolution.
    if (layer.replaceableTexture == 1 && !baseTexPath.empty()) {
        if (layer.shaderId == 1 || layer.shaderId == 24) {
            // HD: team color comes from the live UI swatch at t4 (teamColorMapId
            // acts as the enable flag). Keep diffuse as-is; baking a static
            // composite here would be wrong.
            mprintf(_M("    \x2192 HD TEAMCOLOR branch: diffuse='%s', shaderId=%d\n"),
                    baseTexPath.c_str(), layer.shaderId);
            layer.textureId = baseTexId;
            if (layer.teamColorMapId < 0) layer.teamColorMapId = kHdTeamColorActive;
        } else {
            // SD TEAMCOLOR: WC3's engine ignores whatever BLP the MDX/Max
            // material points at for this slot and binds a flat team-colour
            // swatch at t0 — the SD shader samples the swatch verbatim, no
            // alpha-mask composite. Reserve a replaceableId=1 slot here and
            // ReplaceableTextureManager::BakeSlot fills the pixels from the
            // current swatch (and rebakes on SetTeamColor for live retint).
            mprintf(_M("    \x2192 TEAMCOLOR branch: live swatch slot (source BLP ignored: '%s')\n"),
                    baseTexPath.c_str());
            layer.textureId  = LoadTexture(L"", 1);
            layer.filterMode = 0;
            layer.alpha      = 1.0f;
        }
    } else if (layer.replaceableTexture == 2) {
        mprintf(_M("    \x2192 TEAMGLOW branch: registering live slot\n"));
        // Renderer-side ReplaceableTextureManager owns the TGA bake now —
        // the adapter just reserves a textureId with replaceableId=2.
        layer.textureId = LoadTexture(L"", 2);
    } else if (layer.replaceableTexture >= 1 && baseTexId >= 0) {
        mprintf(_M("    \x2192 REPLACEABLE branch: replTex=%d, baseTexId=%d\n"),
                layer.replaceableTexture, baseTexId);
        layer.textureId  = baseTexId;
        layer.filterMode = 0;
        layer.alpha      = 1.0f;
    } else if (layer.replaceableTexture >= 1 && baseTexId < 0) {
        mprintf(_M("    \x2192 SOLID REPLACEABLE branch: replTex=%d\n"), layer.replaceableTexture);
        layer.textureId = LoadTexture(L"", layer.replaceableTexture);
    } else {
        layer.textureId = baseTexId;
    }

    return layer;
}

// ============================================================================
// Collect materials — supports single Wc3Material and Composite materials
// ============================================================================

void MaxSceneAdapter::CollectMaterials() {
    materials_.clear(); mtlToId_.clear(); nextMatId_ = 0;

    // Copy sortOrder/priorityPlane from a Wc3Material onto MaterialInfo.
    auto fillMaterialSort = [&](MaterialInfo& mi, Mtl* src) {
        mi.sortOrder     = std::max(0, PB2IntOr(src, L"sortOrder", 0, 1) - 1);
        mi.priorityPlane = PB2IntOr(src, L"priorityPlane", 0, 0);
    };
    // Generic fallback: treat the material as a single-layer Std mat, binding
    // its first SubTexmap if it's a BitmapTex. Shared by the "composite with no
    // Wc3 sub-materials" and "plain non-Wc3 material" code paths.
    auto pushGenericLayer = [&](MaterialInfo& mi, Mtl* src) {
        MaterialLayerInfo layer;
        layer.flags = 1;
        if (src->NumSubTexmaps() > 0) {
            if (BitmapTex* bmt = UnwrapBitmapTex(src->GetSubTexmap(0))) {
                const MCHAR* fname = bmt->GetMapName();
                if (fname && fname[0]) layer.textureId = LoadTexture(std::wstring(fname), 0);
            }
        }
        mi.layers.push_back(layer);
    };

    for (auto& gs : geosets_) {
        Mtl* mtl = gs.node->GetMtl();
        if (!mtl) continue;
        if (auto it = mtlToId_.find(mtl); it != mtlToId_.end()) {
            gs.materialId = it->second;
            continue;
        }

        MaterialInfo mi; mi.materialId = nextMatId_++; mi.mtl = mtl;
        gs.materialId = mi.materialId; mtlToId_[mtl] = mi.materialId;

        if (mtl->ClassID() == WARCRAFT3_MAT_CLASS_ID) {
            // Single Wc3Material — one layer, material-level sort comes from
            // the same Wc3Material.
            mprintf(_M("  Mat %d '%s': Wc3Material\n"), mi.materialId, gs.node->GetName());
            fillMaterialSort(mi, mtl);
            mi.layers.push_back(ExtractWc3MaterialLayer(mtl));
        } else if (mtl->NumSubMtls() > 0) {
            // Composite / multi-material: take sort + priority from the first
            // Wc3 sub-material; subsequent Wc3 layers contribute per-layer blend.
            // Non-Wc3 sub-materials are skipped (matches the snapshot iteration).
            const i32 numSubs = mtl->NumSubMtls();
            mprintf(_M("  Mat %d '%s': Composite (%d sub-materials)\n"),
                    mi.materialId, gs.node->GetName(), numSubs);
            for (i32 si = 0; si < numSubs; si++) {
                Mtl* subMtl = mtl->GetSubMtl(si);
                if (!subMtl) continue;
                if (subMtl->ClassID() != WARCRAFT3_MAT_CLASS_ID) {
                    mprintf(_M("    Layer %d: non-Wc3 material '%s' (skipped)\n"),
                            si, subMtl->GetName().data());
                    continue;
                }
                mprintf(_M("    Layer %d: Wc3Material '%s'\n"), si, subMtl->GetName().data());
                if (mi.layers.empty()) fillMaterialSort(mi, subMtl);
                mi.layers.push_back(ExtractWc3MaterialLayer(subMtl));
            }
            // No Wc3Material sub-materials found — treat the composite itself
            // as a generic one-layer Std material.
            if (mi.layers.empty()) pushGenericLayer(mi, mtl);
        } else {
            // Generic non-Wc3 material — single-layer Std fallback.
            pushGenericLayer(mi, mtl);
        }
        materials_.push_back(mi);
    }
}

// ============================================================================
// Collect bones
// ============================================================================

void MaxSceneAdapter::CollectBones() {
    bones_.clear(); boneNameToIdx_.clear();

    std::vector<INode*> allBones;
    std::unordered_map<INode*, bool> boneSet;
    ForEachSceneNode([&](INode* node) {
        Modifier* skinMod = FindSkinModifier(node);
        if (!skinMod) return;
        ISkin* skin = (ISkin*)skinMod->GetInterface(I_SKIN);
        if (!skin) return;
        for (i32 b = 0; b < skin->GetNumBones(); b++) {
            INode* bn = skin->GetBone(b);
            if (bn && !boneSet[bn]) { boneSet[bn] = true; allBones.push_back(bn); }
        }
    });

    for (i32 i = 0; i < (i32)allBones.size(); i++) {
        BoneInfo bi; bi.node = allBones[i]; bi.index = i;
        bones_.push_back(bi);
        boneNameToIdx_[std::wstring(allBones[i]->GetName())] = i;
    }
}

// ============================================================================
// Collect attachments (Wc3_AttachPoint helpers)
// ============================================================================

void MaxSceneAdapter::CollectAttachments() {
    attachments_.clear();
    i32 idx = 0;

    ForEachSceneNode([&](INode* node) {
        Object* baseObj = GetBaseObject(node);
        if (!baseObj || baseObj->ClassID() != WC3ATTACHPOINT_CLASS_ID) return;

        AttachmentInfo ai;
        ai.index        = idx++;
        ai.node         = node;
        ai.attachmentId = PB2IntOr(baseObj, L"attachmentId", 0, 0);

        if (PB2BoolOr(baseObj, L"usesExternalModel", 0, false)) {
            // TYPE_STRING isn't covered by the typed PB2Or helpers; pull the
            // value directly through the FindPB2Param scaffold.
            FindPB2Param(static_cast<Animatable*>(baseObj), L"externalModelPath",
                [&](IParamBlock2* pblock, ParamID pid, ParamDef&) {
                    const MCHAR* sv = nullptr;
                    Interval iv = FOREVER;
                    pblock->GetValue(pid, 0, sv, iv);
                    if (sv && sv[0]) {
                        std::wstring wp(sv);
                        ai.modelPath = std::string(wp.begin(), wp.end());
                    }
                });
        }
        if (!ai.modelPath.empty()) {
            mprintf(_M("  [Attachment '%s'] id=%d model='%S'\n"),
                    node->GetName(), ai.attachmentId, ai.modelPath.c_str());
        }
        attachments_.push_back(ai);
    });
}

// ============================================================================
// Collect particle emitters
// ============================================================================

void MaxSceneAdapter::CollectParticleEmitters() {
    particles_.clear();
    pe1Emitters_.clear();
    i32 emitterId = 0;
    i32 pe1EmitterId = 0;
    const std::wstring basePath = GetMaxFilePath();

    // True when the path exists on disk.
    auto existsOnDisk = [](const std::wstring& p) {
        return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    };

    ForEachSceneNode([&](INode* node) {
        if (node->IsNodeHidden()) return;
        Object* baseObj = GetBaseObject(node);
        if (!baseObj) return;

        if (baseObj->ClassID() == WC3PARTICLES2_CLASS_ID) {
            ParticleEmitterInfo pi; pi.emitterId = emitterId++; pi.node = node;

            // Texture path/prefix come from cross-DLL GetInterface calls.
            std::wstring texPath, texFile;
            if (auto* p = static_cast<const MSTR*>(baseObj->GetInterface(WC3P2_TEXTURE_PATH_IID));   p && p->Length() > 0) texFile = p->data();
            if (auto* p = static_cast<const MSTR*>(baseObj->GetInterface(WC3P2_TEXTURE_PREFIX_IID)); p && p->Length() > 0) texPath = p->data();
            const i32 replId = PB2IntOr(baseObj, L"ReplaceableId", 0, 0);

            if (!texFile.empty()) {
                mprintf(_M("  [Particle '%s'] prefix='%s' file='%s'\n"),
                        node->GetName(), texPath.c_str(), texFile.c_str());

                // Try disk paths in order: basePath+prefix+file, basePath\Textures\file, bare filename.
                std::wstring fp = basePath + texPath + texFile;
                mprintf(_M("    Try1: '%s' %s\n"), fp.c_str(),
                        existsOnDisk(fp) ? _M("FOUND") : _M("not found"));
                if (!existsOnDisk(fp)) {
                    fp = basePath + L"Textures\\" + texFile;
                    mprintf(_M("    Try2: '%s' %s\n"), fp.c_str(),
                            existsOnDisk(fp) ? _M("FOUND") : _M("not found"));
                }
                if (!existsOnDisk(fp)) {
                    fp = texFile;
                    mprintf(_M("    Try3 (filename only): '%s'\n"), fp.c_str());
                }

                if (existsOnDisk(fp)) {
                    pi.textureId = LoadTexture(fp, replId);
                } else {
                    // Fall back to CASC/MPQ. Normalise separators for archive keys.
                    std::string narrowPrefix(texPath.begin(), texPath.end());
                    std::string narrowFile  (texFile.begin(), texFile.end());
                    for (auto& c : narrowPrefix) if (c == '/') c = '\\';
                    for (auto& c : narrowFile)   if (c == '/') c = '\\';
                    std::string archivePath = narrowPrefix + narrowFile;
                    mprintf(_M("    Try4 (ContentProvider): '%S'\n"), archivePath.c_str());
                    pi.textureId = LoadTextureFromContentProvider(archivePath, replId);
                    if (pi.textureId < 0) {
                        mprintf(_M("    Try5 (ContentProvider, no prefix): '%S'\n"), narrowFile.c_str());
                        pi.textureId = LoadTextureFromContentProvider(narrowFile, replId);
                    }
                    if (pi.textureId < 0) pi.textureId = LoadTexture(fp, replId); // magenta placeholder
                }
                pi.replaceableId = replId;
                mprintf(_M("    \x2192 texId=%d\n"), pi.textureId);
            } else if (replId > 0) {
                // No file set, but replaceable (TeamColor / TeamGlow).
                pi.textureId     = LoadTexture(L"", replId);
                pi.replaceableId = replId;
            } else {
                mprintf(_M("  [Particle '%s'] NO texture file set!\n"), node->GetName());
            }
            particles_.push_back(pi);
            return;
        }

        if (baseObj->ClassID() == WC3PARTICLES1_CLASS_ID) {
            PE1EmitterInfo pi;
            pi.emitterId = pe1EmitterId++;
            pi.node      = node;
            if (auto* p = static_cast<const MSTR*>(baseObj->GetInterface(WC3P1_MODEL_PATH_IID));
                p && p->Length() > 0) {
                std::wstring wp = p->data();
                pi.modelPath = std::string(wp.begin(), wp.end());
            }
            mprintf(_M("  [PE1 '%s'] model='%S'\n"), node->GetName(), pi.modelPath.c_str());
            if (!pi.modelPath.empty()) pe1Emitters_.push_back(pi);
        }
    });
}

// ============================================================================
// Collect ribbon emitters
// ============================================================================

void MaxSceneAdapter::CollectRibbonEmitters() {
    ribbons_.clear();
    i32 emitterId = 0;

    ForEachSceneNode([&](INode* node) {
        if (node->IsNodeHidden()) return;
        Object* baseObj = GetBaseObject(node);
        if (!baseObj || baseObj->ClassID() != WC3RIBBON_CLASS_ID) return;

        RibbonEmitterInfo ri; ri.emitterId = emitterId++; ri.node = node;

        // Wc3Ribbon stores its material in PB2 param "Material" (pb_material=7).
        // Fall back to the node material if the emitter didn't wire one.
        Mtl* mtl = nullptr;
        for (i32 pb = 0; pb < baseObj->NumParamBlocks(); pb++) {
            IParamBlock2* pblock = static_cast<Animatable*>(baseObj)->GetParamBlock(pb);
            if (!pblock) continue;
            mtl = pblock->GetMtl(7, 0);
            if (mtl) break;
        }
        if (!mtl) mtl = node->GetMtl();

        if (mtl) {
            std::wstring path;
            if (mtl->ClassID() == WARCRAFT3_MAT_CLASS_ID) {
                path = ResolveBitmapPath(mtl, L"diffuseMap");
            } else if (BitmapTex* bmt = UnwrapBitmapTex(mtl->GetSubTexmap(ID_DI))) {
                // Non-Wc3 material: the Std diffuse slot's BitmapTex.
                const MCHAR* fname = bmt->GetMapName();
                if (fname && fname[0]) path = fname;
            }
            if (!path.empty()) ri.textureId = LoadTexture(path, 0);
        }
        ribbons_.push_back(ri);
    });
}

// ============================================================================
// Collect collision shapes
// ============================================================================

void MaxSceneAdapter::CollectCollisionShapes() {
    collisions_.clear();
    ForEachSceneNode([&](INode* node) {
        if (node->IsNodeHidden()) return;
        Object* baseObj = GetBaseObject(node);
        if (!baseObj || baseObj->SuperClassID() != HELPER_CLASS_ID) return;

        MSTR cnBuf;
        const MCHAR* cn = GetObjectClassName(baseObj, cnBuf);
        if (wcsstr(cn, L"CollisionSphere") || wcsstr(cn, L"Wc3CollisionSphere"))
            collisions_.push_back({1, node});
        else if (wcsstr(cn, L"CollisionBox") || wcsstr(cn, L"Wc3CollisionBox"))
            collisions_.push_back({0, node});
    });
}

// ============================================================================
// GetVNormal helper
// ============================================================================

static Point3 GetVNormal(Mesh& mesh, i32 faceIdx, i32 vertIdx) {
    DWORD smGroup = mesh.faces[faceIdx].getSmGroup();
    if (smGroup == 0) return mesh.getFaceNormal(faceIdx);
    Point3 n(0,0,0);
    for (i32 f = 0; f < mesh.getNumFaces(); f++) {
        if (mesh.faces[f].getSmGroup() & smGroup)
            for (i32 v = 0; v < 3; v++)
                if (mesh.faces[f].v[v] == (DWORD)vertIdx) { n += mesh.getFaceNormal(f); break; }
    }
    return Normalize(n);
}

// ============================================================================
// IModelSource::GetMeshes()
// ============================================================================

std::vector<MeshData> MaxSceneAdapter::GetMeshes() {
    std::vector<MeshData> result;
    for (auto& gs : geosets_) {
        mprintf(_M("    mesh[%d] '%s' ...\n"), gs.geosetId, gs.node->GetName());

        Modifier* skinMod = FindSkinModifier(gs.node);
        if (skinMod) skinMod->DisableMod();
        ObjectState os = gs.node->EvalWorldState(0);

        if (!os.obj || !os.obj->CanConvertToType(triObjectClassID)) {
            if (skinMod) skinMod->EnableMod();
            continue;
        }
        TriObject* triObj = static_cast<TriObject*>(os.obj->ConvertToType(0, triObjectClassID));
        if (!triObj) { if (skinMod) skinMod->EnableMod(); continue; }

        Mesh& mesh = triObj->GetMesh();
        mesh.buildNormals();
        i32 numFaces = mesh.getNumFaces();
        i32 numVerts = numFaces * 3;
        gs.expandedVertCount = numVerts;
        gs.faceVertMap.resize(numVerts);

        MeshData md;
        md.geosetId   = gs.geosetId;
        md.materialId = gs.materialId;
        md.positions.resize(numVerts);
        md.normals.resize(numVerts);
        md.uvs.resize(numVerts);
        md.indices.resize(numFaces * 3);

        bool hasUVs = mesh.getNumMapVerts(1) > 0;
        MeshNormalSpec* specN = mesh.GetSpecifiedNormals();
        bool hasSpecN = specN && specN->GetNumNormals() > 0;

        for (i32 f = 0; f < numFaces; f++) {
            Face& face = mesh.faces[f];
            for (i32 v = 0; v < 3; v++) {
                i32 outIdx = f*3+v;
                i32 origV = face.v[v];
                gs.faceVertMap[outIdx] = origV;

                Point3 pos = mesh.verts[origV];
                md.positions[outIdx] = MaxPointToDefault(pos);

                Point3 n = hasSpecN ? specN->GetNormal(f,v) : GetVNormal(mesh,f,origV);
                n = Normalize(n);
                md.normals[outIdx] = MaxDirToDefault(n);

                if (hasUVs) {
                    TVFace& tvf = mesh.mapFaces(1)[f];
                    UVVert uv = mesh.mapVerts(1)[tvf.t[v]];
                    md.uvs[outIdx] = {uv.x, 1.0f - uv.y};
                } else {
                    md.uvs[outIdx] = {0, 0};
                }
                md.indices[f*3+v] = (u32)outIdx;
            }
        }

        if (triObj != os.obj) triObj->DeleteThis();
        if (skinMod) skinMod->EnableMod();

        // Validate skin vertex count
        if (skinMod) {
            ISkin* skin = (ISkin*)skinMod->GetInterface(I_SKIN);
            ISkinContextData* ctx = skin ? skin->GetContextInterface(gs.node) : nullptr;
            i32 skinVerts = ctx ? ctx->GetNumPoints() : -1;
            i32 meshOrigVerts = mesh.getNumVerts();
            if (skinVerts >= 0 && skinVerts != meshOrigVerts)
                mprintf(_M("  *** WARNING Geoset %d: mesh=%d, ISkin=%d MISMATCH!\n"),
                        gs.geosetId, meshOrigVerts, skinVerts);
        }

        mprintf(_M("  Geoset %d: %d expanded, %d faces '%s'%s\n"),
                gs.geosetId, numVerts, numFaces, gs.node->GetName(),
                skinMod ? _M(" [skinned]") : _M(""));

        result.push_back(std::move(md));
    }
    return result;
}

// ============================================================================
// IModelSource::GetTextures()
// ============================================================================

std::vector<TextureData> MaxSceneAdapter::GetTextures() {
    std::vector<TextureData> result;
    result.reserve(loadedTextures_.size());
    for (auto& lt : loadedTextures_) {
        TextureData td;
        td.textureId    = lt.textureId;
        td.replaceableId = lt.replaceableId;
        td.pixels        = std::move(lt.rgba);
        // MaxSceneAdapter always produces RGBA8 (3ds Max bitmaps are
        // decoded to 32-bit on import). Apply the engine's filename-
        // suffix-driven sRGB / linear policy on top — `_Diffuse` /
        // `_Emissive` / `_IBL` / default get `_SRGB` SRVs, `_Normal` /
        // `_ORM` / `Textures/Normal` / `Textures/ORM` stay linear.
        // The original asset path is carried on lt.sharedKey for
        // file-backed textures; procedural / sentinel textures have
        // an empty key and get the default-sRGB policy. Mirrors
        // `CImageFile::DetermineImageUsage` @ Preview 0x7ff609bad260.
        td.format        = ApplyTextureSrgbPolicy(gfx::Format::R8G8B8A8_UNORM,
                                                  lt.sharedKey);
        td.width         = lt.width;
        td.height        = lt.height;
        // sharedKey was stamped at LoadTexture time (file-backed slots
        // only — replaceable / sentinel paths leave it empty so they
        // stay per-model). When pixels are also empty the renderer treats
        // this as a borrow-only entry and goes through BindShared.
        td.sharedKey     = std::move(lt.sharedKey);
        result.push_back(std::move(td));
    }
    loadedTextures_.clear();
    return result;
}

// ============================================================================
// IModelSource::GetMaterials()
// ============================================================================

std::vector<MaterialData> MaxSceneAdapter::GetMaterials() {
    std::vector<MaterialData> result;
    for (auto& mi : materials_) {
        MaterialData md;
        md.materialId    = mi.materialId;
        md.priorityPlane = mi.priorityPlane;
        md.sortOrder     = mi.sortOrder;
        for (auto& li : mi.layers) {
            MaterialLayerData ld;
            ld.filterMode      = li.filterMode;
            ld.textureId       = li.textureId;
            ld.alpha           = li.alpha;
            ld.flags           = li.flags;
            ld.shaderId        = li.shaderId;
            ld.normalMapId     = li.normalMapId;
            ld.ormMapId        = li.ormMapId;
            ld.emissiveMapId   = li.emissiveMapId;
            ld.teamColorMapId  = li.teamColorMapId;
            ld.emissiveGain    = li.emissiveGain;
            ld.fresnelOpacity  = li.fresnelOpacity;
            ld.fresnelTeamColor = li.fresnelTeamColor;
            ld.fresnelColor    = li.fresnelColor;
            md.layers.push_back(ld);
        }
        result.push_back(std::move(md));
    }
    return result;
}

// ============================================================================
// IModelSource::GetSkeleton()
// ============================================================================

SkeletonData MaxSceneAdapter::GetSkeleton() {
    SkeletonData sd;
    sd.nodeCount = (i32)bones_.size();
    sd.inverseBindMatrices.resize(sd.nodeCount);
    sd.billboardFlags.resize(sd.nodeCount, 0);
    sd.nodeParents.assign(sd.nodeCount, -1);

    // Helper: "Billboarded" & family are ints written via SetUserProp; treat
    // a non-zero value as on.
    auto userFlag = [](INode* node, const TCHAR* name) {
        i32 val = 0;
        return node->GetUserPropInt(name, val) && val != 0;
    };

    for (i32 i = 0; i < sd.nodeCount; i++) {
        INode* node              = bones_[i].node;
        Matrix3 inv              = Inverse(node->GetNodeTM(0));
        bones_[i].inverseBind    = inv;
        sd.inverseBindMatrices[i] = PackMatrix(inv);

        sd.billboardFlags[i] = PackBillboardFlags(
            userFlag(node, _T("Billboarded")),
            userFlag(node, _T("BillboardedLockX")),
            userFlag(node, _T("BillboardedLockY")),
            userFlag(node, _T("BillboardedLockZ")),
            userFlag(node, _T("CameraAnchored")));
    }
    return sd;
}

// ============================================================================
// IModelSource::GetSkinWeights()
// ============================================================================

std::vector<SkinWeightData> MaxSceneAdapter::GetSkinWeights() {
    std::vector<SkinWeightData> result;

    for (auto& gs : geosets_) {
        if (gs.expandedVertCount == 0 || gs.faceVertMap.empty()) continue;
        Modifier* skinMod = FindSkinModifier(gs.node);
        if (!skinMod) { mprintf(_M("  Geoset %d: no Skin modifier\n"), gs.geosetId); continue; }
        ISkin* skin = (ISkin*)skinMod->GetInterface(I_SKIN);
        ISkinContextData* ctx = skin ? skin->GetContextInterface(gs.node) : nullptr;
        if (!ctx) { mprintf(_M("  Geoset %d: ISkin context failed\n"), gs.geosetId); continue; }

        i32 origVC = ctx->GetNumPoints();
        struct OW { i32 bi[4]={0,0,0,0}; f32 wt[4]={0,0,0,0}; };
        std::vector<OW> ow(origVC);
        i32 zeroWeightVerts = 0;
        for (i32 v = 0; v < origVC; v++) {
            i32 nw = ctx->GetNumAssignedBones(v);
            f32 totalW = 0;
            for (i32 b = 0; b < std::min(nw, 4); b++) {
                INode* bn = skin->GetBone(ctx->GetAssignedBone(v,b));
                i32 idx = 0;
                if (bn) { auto it = boneNameToIdx_.find(std::wstring(bn->GetName())); if (it != boneNameToIdx_.end()) idx = it->second; }
                ow[v].bi[b] = idx;
                ow[v].wt[b] = ctx->GetBoneWeight(v, b);
                totalW += ow[v].wt[b];
            }
            if (totalW < 0.001f) zeroWeightVerts++;
        }

        i32 ec = gs.expandedVertCount;
        SkinWeightData sw;
        sw.geosetId = gs.geosetId;
        sw.influences.resize(ec);
        i32 outOfRange = 0, mapped = 0, maxFVM = 0;
        for (i32 vi = 0; vi < ec; vi++) {
            i32 origV = gs.faceVertMap[vi];
            if (origV > maxFVM) maxFVM = origV;
            if (origV >= 0 && origV < origVC) {
                for (i32 j = 0; j < 4; j++) {
                    sw.influences[vi].boneIdx[j] = ow[origV].bi[j];
                    sw.influences[vi].weight[j]  = ow[origV].wt[j];
                }
                mapped++;
            } else {
                outOfRange++;
            }
        }
        mprintf(_M("  Geoset %d: skin %d expanded, %d ISkin, maxIdx=%d, mapped=%d, OOB=%d, zeroW=%d\n"),
                gs.geosetId, ec, origVC, maxFVM, mapped, outOfRange, zeroWeightVerts);

        // Build per-geoset compact subset palette required by the new
        // per-geoset palette system (commit 6c23683). boneIdx values above
        // are global node indices; remap them to LOCAL slots so that
        // GeosetPaletteSize() > 0 and bonePaletteCb gets created.
        {
            std::unordered_map<i32, i32> globalToLocal;
            for (auto& inf : sw.influences) {
                for (i32 j = 0; j < 4; j++) {
                    if (inf.weight[j] > 0.0f) {
                        i32 g = inf.boneIdx[j];
                        if (globalToLocal.find(g) == globalToLocal.end()) {
                            i32 local = (i32)sw.subsetNodeIndices.size();
                            globalToLocal.emplace(g, local);
                            sw.subsetNodeIndices.push_back(g);
                        }
                    }
                }
            }
            for (auto& inf : sw.influences) {
                for (i32 j = 0; j < 4; j++) {
                    auto it = globalToLocal.find(inf.boneIdx[j]);
                    inf.boneIdx[j] = (it != globalToLocal.end()) ? it->second : 0;
                }
            }
        }

        result.push_back(std::move(sw));
    }
    return result;
}

// ============================================================================
// IModelSource::GetParticleConfigs()
// ============================================================================

std::vector<ParticleEmitterConfig> MaxSceneAdapter::GetParticleConfigs() {
    std::vector<ParticleEmitterConfig> result;
    for (auto& pi : particles_) {
        Object* obj = GetBaseObject(pi.node);
        if (!obj) continue;

        ParticleEmitterConfig cfg;
        cfg.textureId     = pi.textureId >= 0 ? pi.textureId : 0;
        cfg.replaceableId = pi.replaceableId;
        cfg.filterMode    = MapPE2BlendMode(PB2IntOr(obj, L"BlendMode", 0, 0));

        cfg.rows        = PB2IntOr  (obj, L"TextureRows", 0, cfg.rows);
        cfg.cols        = PB2IntOr  (obj, L"TextureCols", 0, cfg.cols);
        cfg.unshaded    = PB2BoolOr (obj, L"Unshaded",    0, cfg.unshaded);
        cfg.lifeSpan    = PB2FloatOr(obj, L"Life",        0, cfg.lifeSpan);
        cfg.squirt      = PB2BoolOr (obj, L"Squirt",      0, cfg.squirt);

        // Segment colors (Point3 in Wc3Particles2 → Vector3f).
        Color cv;
        if (PB2Color(obj, L"ColorStart", 0, cv)) cfg.startColor = {cv.r, cv.g, cv.b};
        if (PB2Color(obj, L"ColorMid",   0, cv)) cfg.midColor   = {cv.r, cv.g, cv.b};
        if (PB2Color(obj, L"ColorEnd",   0, cv)) cfg.endColor   = {cv.r, cv.g, cv.b};

        // Segment alpha (0-255 int → float) / scale / mid-time.
        cfg.startAlpha = (f32)PB2IntOr  (obj, L"AlphaStart", 0, (i32)cfg.startAlpha);
        cfg.midAlpha   = (f32)PB2IntOr  (obj, L"AlphaMid",   0, (i32)cfg.midAlpha);
        cfg.endAlpha   = (f32)PB2IntOr  (obj, L"AlphaEnd",   0, (i32)cfg.endAlpha);
        cfg.startScale =        PB2FloatOr(obj, L"ScaleStart", 0, cfg.startScale);
        cfg.midScale   =        PB2FloatOr(obj, L"ScaleMid",   0, cfg.midScale);
        cfg.endScale   =        PB2FloatOr(obj, L"ScaleEnd",   0, cfg.endScale);
        cfg.midTime    =        PB2FloatOr(obj, L"MidTime",    0, cfg.midTime);

        // ParticleType: Wc3Particles2 0=Head,1=Tail,2=Both → renderer 1=Head,2=Tail,3=Both.
        if (i32 pt; PB2Int(obj, L"ParticleType", 0, pt)) cfg.particleType = pt + 1;
        cfg.tailLength  = PB2FloatOr(obj, L"TailLength",  0, cfg.tailLength);

        cfg.modelSpace  = PB2BoolOr(obj, L"ModelSpace",  0, cfg.modelSpace);
        cfg.xyQuad      = PB2BoolOr(obj, L"XYQuad",      0, cfg.xyQuad);
        cfg.lineEmitter = PB2BoolOr(obj, L"LineEmitter", 0, cfg.lineEmitter);

        // Head/tail UV animation frames.
        cfg.headLifeStart   = PB2IntOr(obj, L"HeadLifeStart",   0, cfg.headLifeStart);
        cfg.headLifeEnd     = PB2IntOr(obj, L"HeadLifeEnd",     0, cfg.headLifeEnd);
        cfg.headLifeRepeat  = PB2IntOr(obj, L"HeadLifeRepeat",  0, cfg.headLifeRepeat);
        cfg.headDecayStart  = PB2IntOr(obj, L"HeadDecayStart",  0, cfg.headDecayStart);
        cfg.headDecayEnd    = PB2IntOr(obj, L"HeadDecayEnd",    0, cfg.headDecayEnd);
        cfg.headDecayRepeat = PB2IntOr(obj, L"HeadDecayRepeat", 0, cfg.headDecayRepeat);
        cfg.tailLifeStart   = PB2IntOr(obj, L"TailLifeStart",   0, cfg.tailLifeStart);
        cfg.tailLifeEnd     = PB2IntOr(obj, L"TailLifeEnd",     0, cfg.tailLifeEnd);
        cfg.tailLifeRepeat  = PB2IntOr(obj, L"TailLifeRepeat",  0, cfg.tailLifeRepeat);
        cfg.tailDecayStart  = PB2IntOr(obj, L"TailDecayStart",  0, cfg.tailDecayStart);
        cfg.tailDecayEnd    = PB2IntOr(obj, L"TailDecayEnd",    0, cfg.tailDecayEnd);
        cfg.tailDecayRepeat = PB2IntOr(obj, L"TailDecayRepeat", 0, cfg.tailDecayRepeat);

        cfg.sortZ         = PB2BoolOr(obj, L"SortPrimitives", 0, cfg.sortZ);
        cfg.unfogged      = PB2BoolOr(obj, L"Unfogged",       0, cfg.unfogged);
        cfg.count         = PB2IntOr (obj, L"Count",          0, cfg.count);
        cfg.priorityPlane = PB2IntOr (obj, L"PriorityPlane",  0, cfg.priorityPlane);

        mprintf(_M("  Particle %d: '%s' tex=%d fm=%d unshaded=%d\n"),
                pi.emitterId, pi.node->GetName(), pi.textureId, cfg.filterMode, (i32)cfg.unshaded);
        result.push_back(cfg);
    }
    return result;
}

// ============================================================================
// IModelSource::GetRibbonConfigs()
// ============================================================================

std::vector<RibbonEmitterConfig> MaxSceneAdapter::GetRibbonConfigs() {
    std::vector<RibbonEmitterConfig> result;
    for (auto& ri : ribbons_) {
        Object* obj = GetBaseObject(ri.node);
        if (!obj) continue;

        RibbonEmitterConfig cfg;
        cfg.textureId = ri.textureId >= 0 ? ri.textureId : 0;

        // Wc3Ribbon has no filterMode param of its own; derive from the node
        // material when it's a Wc3Material, otherwise fall back to Additive.
        if (Mtl* mtl = ri.node->GetMtl(); mtl && mtl->ClassID() == WARCRAFT3_MAT_CLASS_ID) {
            cfg.filterMode = MapFilterMode(PB2IntOr(mtl, L"filterMode", 0, 1) - 1);
        } else {
            cfg.filterMode = MapFilterMode(3);
        }

        cfg.rows     = PB2IntOr  (obj, L"Texture Rows",     0, cfg.rows);
        cfg.cols     = PB2IntOr  (obj, L"Texture Columns",  0, cfg.cols);
        cfg.emission = (f32)PB2IntOr(obj, L"Edges Per Second", 0, (i32)cfg.emission);
        cfg.life     = PB2FloatOr(obj, L"Edge Lifetime",    0, cfg.life);
        cfg.gravity  = PB2FloatOr(obj, L"Gravity",          0, cfg.gravity);
        // Wc3Ribbon has no unshaded/twosided params — engine renders ribbons
        // double-sided + unshaded by convention.
        cfg.unshaded = true;
        cfg.twoSided = true;

        mprintf(_M("  Ribbon %d: '%s' tex=%d fm=%d\n"),
                ri.emitterId, ri.node->GetName(), ri.textureId, cfg.filterMode);
        result.push_back(cfg);
    }
    return result;
}

// ============================================================================
// IModelSource::GetCollisionShapes()
// ============================================================================

std::vector<CollisionShapeData> MaxSceneAdapter::GetCollisionShapes() {
    std::vector<CollisionShapeData> result;
    for (auto& ci : collisions_) {
        Object* obj = GetBaseObject(ci.node);
        if (!obj) continue;

        CollisionShapeData cs;
        cs.type        = ci.type;
        cs.radius      = 0;
        cs.vertices[0] = {0, 0, 0};
        cs.vertices[1] = {0, 0, 0};

        if (ci.type == 1) {
            cs.radius = PB2FloatOr(obj, L"radius", 0, 10.0f);
        } else {
            // Extent parameters are full width/length/height; bounding corners
            // use half-extents in X/Y and full height in Z.
            const f32 halfW = PB2FloatOr(obj, L"width",  0, 5.0f) * 0.5f;
            const f32 halfL = PB2FloatOr(obj, L"length", 0, 5.0f) * 0.5f;
            const f32 h     = PB2FloatOr(obj, L"height", 0, 5.0f);
            cs.vertices[0] = {-halfW, -halfL, 0};
            cs.vertices[1] = { halfW,  halfL, h};
        }
        result.push_back(cs);
    }
    return result;
}

// ============================================================================
// IModelSource::GetPE1Configs()
// ============================================================================

std::vector<AttachmentConfig> MaxSceneAdapter::GetAttachmentConfigs() {
    std::vector<AttachmentConfig> result;
    for (auto& ai : attachments_) {
        AttachmentConfig cfg;
        cfg.attachmentId = ai.attachmentId;
        cfg.modelPath = ai.modelPath;
        result.push_back(cfg);
    }
    return result;
}

std::vector<PE1EmitterConfig> MaxSceneAdapter::GetPE1Configs() {
    std::vector<PE1EmitterConfig> result;
    for (auto& pi : pe1Emitters_) {
        Object* obj = GetBaseObject(pi.node);
        if (!obj) continue;
        PE1EmitterConfig cfg;
        cfg.modelPath = pi.modelPath;
        cfg.lifespan  = PB2FloatOr(obj, L"Life",  0, cfg.lifespan);
        cfg.scale     = PB2FloatOr(obj, L"Scale", 0, cfg.scale);
        result.push_back(cfg);
    }
    return result;
}

// ============================================================================
// IAnimationSource::Evaluate() — compute per-frame state from Max scene.
// Max controls the timeline, so sequenceIdx + globalTimeMs + worldTransform +
// cameraPos are unused here; only timeMs (advanced via Max's TimeValue) feeds in.
// ============================================================================

FrameState MaxSceneAdapter::Evaluate(i32 /*sequenceIdx*/, i32 timeMs, i32 /*globalTimeMs*/,
                                     const Matrix44f& /*worldTransform*/,
                                     const Vector3f& /*cameraPos*/) const {
    // Convert ms → Max ticks (0 if the tick rate is unavailable).
    const i32 tpf = GetTicksPerFrame(), fps = GetFrameRate();
    const TimeValue t = (tpf > 0 && fps > 0)
        ? (TimeValue)((f32)timeMs * (f32)fps / 1000.0f * (f32)tpf)
        : 0;
    constexpr f32 kDegToRad = std::numbers::pi_v<f32> / 180.0f;

    FrameState state;

    // Bone world matrices
    state.boneWorldMatrices.resize(bones_.size());
    for (usize i = 0; i < bones_.size(); i++)
        state.boneWorldMatrices[i] = PackMatrix(bones_[i].node->GetNodeTM(t));

    // Geoset transforms + visibility + per-vertex-mod color
    const i32 gc = (i32)geosets_.size();
    state.geosetTransforms.resize(gc, Matrix44f::identity());
    state.geosetAlphas    .assign(gc, 1.0f);
    state.geosetColors    .assign(gc, Vector3f{1, 1, 1});
    for (i32 i = 0; i < gc; i++) {
        INode* node = geosets_[i].node;
        if (!node) continue;
        state.geosetTransforms[i] = PackMatrix(node->GetNodeTM(t));
        // Geoset alpha carries visibility only; per-layer opacity ships
        // separately via layerAlphas so each layer can fade independently.
        state.geosetAlphas[i] = std::clamp(node->GetVisibility(t), 0.0f, 1.0f);

        // Wc3VertexMod is a MaxScript scripted plugin. Max does NOT route
        // its declared classID through Modifier::ClassID() — the C++ value
        // is some opaque internal id. The exporter hits the same wall and
        // resolves it by name (geoset_anim_extractor.cpp::isWc3VertexMod).
        static const wchar_t* kWc3VertexModNames[] = {
            L"Wc3VertexMod",          // MaxScript classOf observed
            L"Wdx_Wc3VertexMod",      // raw plugin internal name
            L"Wc3 Vertex Color",      // plugin "name:" attribute
            nullptr
        };
        if (Modifier* mod = FindModifierByClassName(node, kWc3VertexModNames)) {
            if (PB2BoolOr(mod, L"UsesColor", t, false)) {
                Color col(1, 1, 1); PB2Color(mod, L"VertexColor", t, col);
                state.geosetColors[i] = {col.r, col.g, col.b};
            }
        }
    }

    // Per-layer alpha: one entry per unique (material, layer) authored on any
    // geoset's node material.
    {
        std::vector<i32> sentMats;
        for (auto& gs : geosets_) {
            Mtl* mtl = gs.node ? gs.node->GetMtl() : nullptr;
            if (!mtl) continue;
            auto it = mtlToId_.find(mtl);
            if (it == mtlToId_.end()) continue;
            const i32 matId = it->second;
            if (std::find(sentMats.begin(), sentMats.end(), matId) != sentMats.end()) continue;
            sentMats.push_back(matId);
            ForEachWc3SubMtl(mtl, [&](Mtl* sub, i32 layerIdx) {
                const f32 a = PB2FloatOr(sub, L"opacity", t, 100.0f);
                state.layerAlphas.push_back({matId, layerIdx, std::min(a / 100.0f, 1.0f)});
            });
        }
    }

    // Particle emitter states
    for (auto& pi : particles_) {
        Object* obj = GetBaseObject(pi.node);
        if (!obj) continue;
        FrameState::ParticleFrameState ps;
        ps.emitterId    = pi.emitterId;
        ps.transform    = PackMatrix(pi.node->GetNodeTM(t));
        ps.emissionRate = PB2FloatOr(obj, L"EmissionRate", t);
        ps.speed        = PB2FloatOr(obj, L"Speed",        t);
        ps.variation    = PB2FloatOr(obj, L"Variation",    t);
        ps.coneAngle    = PB2FloatOr(obj, L"ConeAngle",    t) * kDegToRad; // deg→rad
        ps.gravity      = PB2FloatOr(obj, L"Gravity",      t);
        ps.width        = PB2FloatOr(obj, L"Width",        t);
        ps.length       = PB2FloatOr(obj, L"Height",       t);
        ps.visibility   = pi.node->GetVisibility(t);
        ps.squirting    = PB2BoolOr (obj, L"Squirt",       t, false);
        state.particleStates.push_back(ps);
    }

    // Ribbon emitter states
    for (auto& ri : ribbons_) {
        Object* obj = GetBaseObject(ri.node);
        if (!obj) continue;
        FrameState::RibbonFrameState rs;
        rs.emitterId  = ri.emitterId;
        rs.transform  = PackMatrix(ri.node->GetNodeTM(t));
        rs.above      = PB2FloatOr(obj, L"Height Above", t, 20.0f);
        rs.below      = PB2FloatOr(obj, L"Height Below", t, 20.0f);
        rs.alpha      = PB2FloatOr(obj, L"Alpha",        t, 1.0f);
        rs.visibility = ri.node->GetVisibility(t);
        rs.slot       = PB2IntOr  (obj, L"Texture Slot", t, 0);
        Color cv;
        if (PB2Color(obj, L"Color", t, cv)) rs.color = {cv.r, cv.g, cv.b};
        else                                rs.color = {1, 1, 1};
        state.ribbonStates.push_back(rs);
    }

    // Collision transforms
    for (auto& ci : collisions_)
        state.collisionTransforms.push_back(PackMatrix(ci.node->GetNodeTM(t)));

    // Attachment transforms
    for (usize i = 0; i < attachments_.size(); i++) {
        auto& ai = attachments_[i];
        state.attachmentStates.push_back({(i32)i,
                                          PackMatrix(ai.node->GetNodeTM(t)),
                                          ai.node->GetVisibility(t)});
    }

    // PE1 emitter states
    for (auto& pi : pe1Emitters_) {
        Object* obj = GetBaseObject(pi.node);
        if (!obj) continue;
        FrameState::PE1FrameState ps;
        ps.emitterId    = pi.emitterId;
        ps.transform    = PackMatrix(pi.node->GetNodeTM(t));
        ps.speed        = PB2FloatOr(obj, L"Speed",        t);
        ps.emissionRate = PB2FloatOr(obj, L"EmissionRate", t);
        ps.latitude     = PB2FloatOr(obj, L"Latitude",     t) * kDegToRad;
        ps.longitude    = PB2FloatOr(obj, L"Longitude",    t) * kDegToRad;
        ps.gravity      = PB2FloatOr(obj, L"Gravity",      t);
        ps.visibility   = pi.node->GetVisibility(t);
        state.pe1States.push_back(ps);
    }

    // Texture animations: one entry per unique (material, layer) with any
    // non-default UV animation authored.
    auto readUVAnim = [&](Mtl* mtl, i32 matId, i32 layerIdx) {
        f32 uOff = 0, vOff = 0, uTile = 1, vTile = 1, wAng = 0;

        // Primary: Wc3Material PB2 UV-anim knobs (doesn't depend on a
        // BitmapTex controller being present).
        if (PB2Float(mtl, L"anim_UOffset", t, uOff)) {
            PB2Float(mtl, L"anim_VOffset", t, vOff);
            PB2Float(mtl, L"anim_UTiling", t, uTile);
            PB2Float(mtl, L"anim_VTiling", t, vTile);
            PB2Float(mtl, L"anim_WAngle",  t, wAng);
        } else {
            // Fallback: read from BitmapTex UVGen (non-Wc3 materials).
            Texmap* texmap = nullptr;
            if (PB2Texmap(mtl, L"diffuseMap", texmap)) {
                if (BitmapTex* bmt = UnwrapBitmapTex(texmap)) {
                    if (StdUVGen* uvg = bmt->GetUVGen()) {
                        uOff  = uvg->GetUOffs(t); vOff  = uvg->GetVOffs(t);
                        uTile = uvg->GetUScl(t);  vTile = uvg->GetVScl(t);
                        wAng  = uvg->GetWAng(t);
                    }
                }
            }
        }
        if (uOff != 0 || vOff != 0 || uTile != 1 || vTile != 1 || wAng != 0) {
            FrameState::TexAnimState tas;
            tas.materialId = matId;
            tas.layerIndex = layerIdx;
            tas.uOff       = uOff;  tas.vOff  = vOff;
            tas.uTile      = uTile; tas.vTile = vTile;
            tas.rotation   = wAng;
            state.texAnims.push_back(tas);
        }
    };

    std::vector<i32> sentTA;
    for (auto& gs : geosets_) {
        Mtl* mtl = gs.node ? gs.node->GetMtl() : nullptr;
        if (!mtl) continue;
        auto it = mtlToId_.find(mtl);
        if (it == mtlToId_.end()) continue;
        const i32 matId = it->second;
        if (std::find(sentTA.begin(), sentTA.end(), matId) != sentTA.end()) continue;
        sentTA.push_back(matId);
        ForEachWc3SubMtl(mtl, [&](Mtl* sub, i32 layerIdx) { readUVAnim(sub, matId, layerIdx); });
    }

    return state;
}

// ============================================================================
// RefreshMaterials — re-read material properties, detect changes
// ============================================================================

MaxSceneAdapter::MaterialRefreshResult MaxSceneAdapter::RefreshMaterials() {
    MaterialRefreshResult result;

    // Structural comparison of two snapshots — returns true if any tracked
    // field differs (equivalent to memberwise equality).
    auto snapshotsEqual = [](const MaterialSnapshot& a, const MaterialSnapshot& b) {
        return a.filterMode         == b.filterMode
            && a.flags              == b.flags
            && a.priorityPlane      == b.priorityPlane
            && a.sortOrder          == b.sortOrder
            && a.replaceableTexture == b.replaceableTexture
            && a.shaderId           == b.shaderId
            && a.texturePath        == b.texturePath
            && a.normalTexPath      == b.normalTexPath
            && a.ormTexPath         == b.ormTexPath
            && a.emissiveTexPath    == b.emissiveTexPath
            && a.teamColorTexPath   == b.teamColorTexPath;
    };

    bool anyChanged = false;
    for (auto& mi : materials_) {
        if (anyChanged) break;
        ForEachWc3SubMtl(mi.mtl, [&](Mtl* sub, i32 layerIdx) {
            if (anyChanged) return;
            const i32 key = (sub == mi.mtl) ? mi.materialId
                                            : (mi.materialId * 1000 + layerIdx);
            MaterialSnapshot cur = SnapshotMaterial(sub);
            auto it = matSnapshots_.find(key);
            if (it == matSnapshots_.end() || !snapshotsEqual(it->second, cur))
                anyChanged = true;
        });
    }

    if (!anyChanged) return result;

    // Something changed — re-collect materials + textures from scratch.
    loadedTextures_.clear();
    texPathToId_.clear();
    texEntries_.clear();
    nextTexId_ = 0;
    CollectMaterials();
    UpdateMaterialSnapshots();

    result.materials = GetMaterials();
    result.textures  = GetTextures();
    result.changed   = true;
    return result;
}

// ============================================================================
// IModelSource::GetSequences() — Max doesn't have MDX sequences
// ============================================================================

std::vector<SequenceInfo> MaxSceneAdapter::GetSequences() const {
    return {};
}

// ============================================================================
// GetCameraPresets — collect camera objects from the Max scene
// ============================================================================

std::vector<CameraPreset> MaxSceneAdapter::GetCameraPresets() {
    std::vector<CameraPreset> presets;
    ForEachSceneNode([&](INode* node) {
        Object* obj = GetBaseObject(node);
        if (!obj || obj->SuperClassID() != CAMERA_CLASS_ID) return;

        // Max-space world transform; resolve the target from the node's
        // target link when present, otherwise project 100 units along -Z.
        Matrix3 tm      = node->GetNodeTM(0);
        Point3 rawPos   = tm.GetRow(3);
        Point3 rawTgt   = rawPos + tm.GetRow(2) * -100.0f;
        if (INode* targNode = node->GetTarget())
            rawTgt = targNode->GetNodeTM(0).GetRow(3);

        // Lift into renderer-native space before deriving orbital parameters
        // so pitch/yaw match the camera's Z-up-around-target convention
        // regardless of WDX_DEFAULT_COORD_SPACE.
        Vector3f pos = MaxPointToDefault(rawPos);
        Vector3f tgt = MaxPointToDefault(rawTgt);

        Vector3f dir{pos.x - tgt.x, pos.y - tgt.y, pos.z - tgt.z};
        f32 dist = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (dist < 0.01f) dist = 100.0f;
        dir = {dir.x / dist, dir.y / dist, dir.z / dist};

        CameraPreset cp;
        cp.name     = std::wstring(node->GetName());
        cp.pitch    = asinf(std::clamp(dir.z, -1.0f, 1.0f));
        cp.yaw      = atan2f(dir.y, dir.x);
        cp.distance = dist;
        cp.target   = tgt;
        cp.isLive   = false;
        presets.push_back(cp);
    });
    return presets;
}
