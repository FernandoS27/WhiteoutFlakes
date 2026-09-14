#pragma once

// ============================================================================
// ViewerApp — GLFW + Dear ImGui frontend for WhiteoutFlakes.
//
// Replaces the previous Win32 + Common Controls RenderWindow. Single-threaded:
// `Tick(dt)` polls GLFW, builds the ImGui frame, runs the engine's per-frame
// update + RenderFrame + Present, all on the calling thread. test_main owns
// the outer loop.
//
// All host-side UI policy (camera presets dropdown, sequence dropdown, focus
// actor, tileset radio) lives in ViewerUI (sibling .cpp), keeping ViewerApp
// focused on lifetime + dispatch.
// ============================================================================

#include "export_recipe.h"
#include "export_runner.h"
#include "io/load_task.h"
#include "model/actor_manager.h"
#include "render_target.h"
#include "whiteout/flakes/enums.h" // RenderMode
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

// The WEM profile axis, for the interchange entry points below.
#include <whiteout/models/wem/profile.h>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace whiteout::flakes::io {
class IContentProvider;
class FileContentProvider;
} // namespace whiteout::flakes::io

namespace whiteout::flakes::renderer {
class RenderService;
using SceneId = u32;
} // namespace whiteout::flakes::renderer

namespace whiteout::flakes::tools {
class StorageExplorer;
} // namespace whiteout::flakes::tools

namespace whiteout::flakes::io {
struct WemDocument;
} // namespace whiteout::flakes::io

namespace whiteout::flakes {

using namespace whiteout::flakes::renderer;
using namespace whiteout::flakes::renderer::model;

class ViewerUI;

// ---------------------------------------------------------------------------
// What File > Open (and the startup picker) accept, as NFD filter specs.
//
// Shared by viewer_ui.cpp and test_main.cpp because they pop the same dialog
// from two entry points and had drifted into two copies of the same literal.
//
// `.m2` / `.m3` appear only when the renderer was built with them
// (WDX_ENABLE_M2 / WDX_ENABLE_M3): offering a file type the loader will then
// refuse is worse than not listing it. `kHasForeignModelFilter` is how a call
// site sizes its filter array.
//
// `.wem` is in every one of them: the module is in whiteout_lib whatever this
// build enables, and which profiles a given file can be opened as is a question
// the file answers at open time (io/wem/wem_profiles.h), not one the build
// flags answer here. `.gltf`/`.glb` ride the same reasoning — a glTF import is
// a Generic-profile document that opens through the WEM machinery
// (GLTF_DESIGN §2), so the module is as unconditional as WEM's.
#if WDX_ENABLE_M2 && WDX_ENABLE_M3
inline constexpr const char* kOpenAllExtensions = "mdx,mdl,pkb,pkfx,wem,gltf,glb,m2,m3";
inline constexpr const char* kForeignModelExtensions = "m2,m3";
inline constexpr bool kHasForeignModelFilter = true;
#elif WDX_ENABLE_M2
inline constexpr const char* kOpenAllExtensions = "mdx,mdl,pkb,pkfx,wem,gltf,glb,m2";
inline constexpr const char* kForeignModelExtensions = "m2";
inline constexpr bool kHasForeignModelFilter = true;
#elif WDX_ENABLE_M3
inline constexpr const char* kOpenAllExtensions = "mdx,mdl,pkb,pkfx,wem,gltf,glb,m3";
inline constexpr const char* kForeignModelExtensions = "m3";
inline constexpr bool kHasForeignModelFilter = true;
#else
inline constexpr const char* kOpenAllExtensions = "mdx,mdl,pkb,pkfx,wem,gltf,glb";
inline constexpr const char* kForeignModelExtensions = "";
inline constexpr bool kHasForeignModelFilter = false;
#endif

// The export recipe, the schedule built from it, and the output formats all
// live in export_recipe.h — a device-free header the ini writer and the G0
// test share with the viewer.

class ViewerApp {
public:
    explicit ViewerApp(RenderService& service);
    ~ViewerApp();

    ViewerApp(const ViewerApp&) = delete;
    ViewerApp& operator=(const ViewerApp&) = delete;

    /// `visible == false` creates the GLFW window hidden: the full app runs
    /// (device, swap chain, tick loop) but nothing appears on screen — what
    /// scripted export/attach runs use so a corpus sweep never flashes windows.
    bool Open(i32 width, i32 height, gfx::GfxApi api, bool visible = true);
    void Close();

    bool ShouldClose() const;
    void Tick(f32 dt);

    // Where every load long enough to freeze the window goes. One thread,
    // tasks in submission order; the UI polls it once a frame and draws the
    // modal. See tools/common/progress_dialog.h.
    io::LoadTaskRunner& Tasks() {
        return tasks_;
    }

    // Open the active game's storages as a background task. No-op when they
    // are already open, so it is safe to call on every settings commit and
    // every document open.
    // @param onDone Runs on the host thread once the storages are up (or
    //        failed); empty when the caller only wants the open.
    void OpenStoragesAsync(std::function<void(bool ok)> onDone = {});

    // The storage open on its own, for any provider — the Storage Explorer
    // panel has its own, pointed at whatever the user is browsing.
    // OpenStoragesAsync is this plus the asset retry and the table prewarm,
    // which belong to the provider the scenes read through.
    void RunStorageOpenTask(io::FileContentProvider& provider, std::function<void(bool ok)> onDone);

    // Everything a document needs in place before the host thread can open
    // it without stalling: the install, and — for World of Warcraft — the
    // client databases the look passes read during the spawn.
    //
    // ONE task, so the bar covers both. Splitting them would put the
    // database reads back on the host thread: the spawn calls
    // WowReplaceableTextures::Apply synchronously, and a prewarm merely
    // *queued* behind it has not finished by then.
    void PreloadForDocumentAsync(std::function<void()> then);

    // The World of Warcraft client tables have been read (or tried) for the
    // install now configured. Not asked of the tables themselves: an install
    // that cannot serve them leaves them un-Loaded forever, and that would
    // re-run the prewarm on every model opened.
    bool wowTablesPrewarmed_ = false;

    // Read World of Warcraft's client databases as a background task, so
    // the first character model of a session does not pay for fourteen
    // CASC reads on the render thread. No-op for other products, and for a
    // session that has already done it. Restyles whatever is loaded when it
    // finishes, so a model that arrived first picks the tables up.
    void PrewarmWowTablesAsync();

    // The same, for StarCraft II / Heroes: which `.m3a` a model wants is in the
    // game's GameData catalog, and reading it is ~5,700 CASC reads. Without
    // this the first `.m3` of a session pays for them inside its own load.
    bool sc2CatalogPrewarmed_ = false;
    void PrewarmSc2CatalogAsync();

    // Where the Diablo III item registry build stands. Viewer-side state
    // rather than a question to the registry, because during Building the
    // registry belongs to the task thread and the host must not even ask.
    enum class D3ItemsBuild : u8 { NotStarted, Building, Ready };
    D3ItemsBuild d3ItemsBuild_ = D3ItemsBuild::NotStarted;

    // Build the item registry (~3.4k Actor reads) as a background task; the
    // equip popup draws a progress bar meanwhile. Not cancellable: the
    // batched build is seconds, and a cancelled half-registry would need a
    // retry state machine the popup has no honest way to draw.
    void BuildD3ItemRegistryAsync();

private:
    // Re-apply the World of Warcraft look passes to the model on screen.
    // Called when the client databases finish loading behind a model that
    // was spawned before they were there. The ACTIVE document only:
    // RestyleWowModel resolves its actor through the active scene, so a
    // background tab has nothing for it to find. Those pick the tables up
    // on their next restyle instead.
    void RestyleLoadedWowModels();

public:
    // Load an MDX from disk into a NEW document (tab) and make it active.
    // Each open document owns its own scene, so previously-loaded models stay
    // resident and switchable; nothing is cleared. Dispatches .pkb / .pkfx
    // paths to LoadEffect. Returns false (and opens no tab) on failure.
    // Synchronous. Keeps the CLI and headless harnesses working the way they
    // always have: --attach-anim runs immediately after this returns and
    // --export-anim counts a fixed number of ticks, so neither can tolerate
    // a load that completes later. Interactive callers want OpenModelAsync.
    bool LoadModel(const std::filesystem::path& path);

    // What File ▸ Open uses. Identical to LoadModel when the storages this
    // model needs are already up; otherwise it opens them as a task first —
    // behind the progress modal — and opens the document when that lands.
    //
    // The freeze it removes: an `.m2` picked from the dialog switches the
    // provider to World of Warcraft, and the document's first read then
    // triggers the install open on a provider worker while the host thread
    // sits in Wait() with nothing on screen to say why.
    //
    // Returns false only for a path that cannot be opened at all; true means
    // opened OR queued.
    bool OpenModelAsync(const std::filesystem::path& path);

    // Open @p path once the frame loop is running, via OpenModelAsync.
    //
    // The startup picker needs this and File ▸ Open does not: the picker
    // runs before the first frame exists, so a load started there has no
    // frame to report in and simply freezes the window until it finishes —
    // which is the freeze this whole feature is about, reached one step
    // earlier than the dialog can cover.
    //
    // Queued paths open one per frame, each behind its own bar.
    void QueueInitialOpen(const std::filesystem::path& path);

    // Load a standalone PopcornFX effect (.pkb / .pkfx) into a NEW document and
    // play it. Unlike a model, there's no animation list — the effect just
    // runs. Becomes the active document; other open documents are untouched.
    bool LoadEffect(const std::filesystem::path& path);

    // "Reforged Graphics": force the HD render pipeline for every model,
    // overriding the per-model SD/HD auto-detection (the LoadModel HD probe).
    // Toggling reloads the active document so its assets re-resolve under the
    // new (HD vs detected) CASC overlay.
    bool ForceHd() const {
        return forceHd_;
    }
    void SetForceHd(bool on);

    // Which Warcraft III art overlay reads resolve through, globally. Nullopt
    // means "whatever the render mode implies" — the behaviour before 3.0.0
    // added a third tier. Setting it reloads the active document so its
    // textures and child models re-resolve, the same reason SetForceHd does.
    void SetArtTier(std::optional<Wc3ArtTier> tier);

    // ---- Open documents (tabs) ----
    // Each loaded file is one document, backed by its own RenderService scene.
    // The viewer renders / ticks only the active document; the rest stay frozen
    // but resident, so switching tabs is instant (no reload).
    i32 DocumentCount() const {
        return static_cast<i32>(documents_.size());
    }
    i32 ActiveDocumentIndex() const {
        return activeDoc_;
    }
    // Tab label for document `idx` (the file's stem). Empty for out-of-range.
    const std::string& DocumentTitle(i32 idx) const;
    // When the active document changes from app code (a file opened on the CLI /
    // via File > Open, or a neighbour taking over after a close), the ImGui tab
    // bar must be told which tab to select — otherwise it defaults to the first
    // tab and snaps the active document back. Returns that index once, then -1.
    i32 ConsumePendingTabSelect();
    // Make document `idx` active: publish its scene, restore its host state
    // (sequences, camera presets, focus actor) and re-apply its render mode.
    void SetActiveDocument(i32 idx);
    // Close document `idx`, destroying its scene + actors. If it was active,
    // a neighbouring tab becomes active; closing the last tab leaves the viewer
    // empty.
    void CloseDocument(i32 idx);

    // ---- Storage Explorer (Tools ▸ Storage Explorer) ----
    // The embedded CASC browser panel. Created lazily on first open; defaults to
    // the install path so it lands on the game storage without a folder pick.
    bool StorageExplorerOpen() const {
        return storageExplorerOpen_;
    }
    void SetStorageExplorerOpen(bool on);
    // Build the panel window inside the host's ImGui frame (called by ViewerUI).
    void BuildStorageExplorerWindow();

    // ---- Current profile ----
    // Which game the user is working with. A PROFILE, not a storage: it names a
    // settings page and an ini section, and nothing about it opens anything.
    // The provider's active product is a separate fact, moved only by content
    // that needs it (FollowModelGame) — the two agree most of the time and are
    // allowed not to.
    ProductId SettingsProfile() const {
        return settingsProfile_;
    }
    // Picked by the user in Settings. Persisted, because next launch should
    // come back to the game they were working with; applied to nothing.
    void SetSettingsProfile(ProductId game);

    // Load `game`'s ini settings into the provider's slot for it, and make it
    // the product the provider serves. ONCE per product per session unless
    // `force`: a slot keeps its configuration across a switch, so re-applying
    // buys nothing and costs everything — it overwrites session-only state and
    // invalidates a storage that is open and correct, and the install dies with
    // the last reference to it. `force` is for an explicit settings edit, which
    // must reach the live storage precisely so it reopens.
    void ApplyProfile(ProductId game, bool force);

    // ---- Animation frame export ----
    // Queue an animation export (see ExportRecipe). Deferred: the UI calls
    // this from inside the ImGui frame, and Tick() runs the actual render
    // loop on the next tick (outside ImGui frame building).
    void RequestAnimationExport(ExportRecipe recipe);

    // The last run's outcome, for the dialog's footer. ConsumeExportFinished
    // returns true exactly once per completed export, so the dialog can latch
    // the report without polling a timestamp.
    const ExportReport& LastExportReport() const {
        return lastExportReport_;
    }
    bool ConsumeExportFinished();

    // True while RunExport owns the frame loop. The export builds the viewer's
    // own ImGui frame when capturing the UI overlay, so anything in that frame
    // that would drive the model has to stand down for the duration.
    bool IsExportRunning() const {
        return exportRunning_;
    }

    // Pose the model (and, in orbit mode, the camera) at one frame of a
    // recipe. Drives both the export dialog's timeline scrubber and its live
    // preview — the same schedule the recording uses, so what they show is
    // what the export will capture.
    bool ScrubExportRecipe(const ExportRecipe& recipe, i32 frameIndex, bool applyCamera);

    // ---- Host policy (toggled by the UI, read by the per-frame tick) ----
    bool LoopNonLoopingPolicy() const {
        return loopNonLoopingPolicy_;
    }
    void SetLoopNonLoopingPolicy(bool on);

    // ---- Camera presets ----
    void ActivateCameraPreset(i32 idx);
    i32 ActiveCameraPresetIdx() const {
        return activeCameraPresetIdx_;
    }
    const std::vector<CameraPreset>& CameraPresets() const {
        return cameraPresets_;
    }
    // UTF-8 view of CameraPreset::name (which is std::wstring), refreshed
    // on every LoadModel. ImGui takes UTF-8 only, so the UI consumes this
    // mirror instead of re-converting per frame.
    const std::vector<std::string>& CameraPresetNamesUtf8() const {
        return cameraPresetNamesUtf8_;
    }
    bool CameraLocked() const {
        return cameraLocked_;
    }

    // ---- Transport ----
    //
    // On the ACTIVE document's scene, so each tab holds its own pause. Nothing
    // here is per-format: the scene clock is what every profile's animation,
    // particles, ribbons and corn-fx advance on, so one pair of buttons drives
    // a `.mdx`, an `.m2`, an `.m3` and a `.prt` alike.
    bool IsPaused() const;
    void SetPaused(bool paused);
    void TogglePaused() {
        SetPaused(!IsPaused());
    }
    // Back to frame zero and playing, whatever the transport was.
    void RestartPlayback();

    // ---- Sequences (per focus actor) ----
    const std::vector<std::string>& SequenceNames() const {
        return sequenceNames_;
    }
    const std::vector<SequenceInfo>& SequenceRanges() const {
        return sequenceRanges_;
    }

    // ---- StarCraft II external animation files (`.m3a`) ----
    //
    // SC2 ships a model's animations in separate files and names them on the
    // model's catalog entry, never inside the `.m3` itself. With no catalog
    // here the user picks the file, and the merge below it is the game's:
    // sequences append to the model's and bind by animId.
    struct AttachedAnimationInfo {
        std::string label;
        std::size_t sequenceCount = 0;
        std::size_t firstSequence = 0;
    };
    // True only when the focus actor is an `.m3` — nothing else takes one.
    bool CanAttachAnimations() const;
    std::vector<AttachedAnimationInfo> AttachedAnimations() const;
    // Reads @p path, merges it, and refreshes the sequence dropdown. False on
    // a parse failure, a file with no sequences, or one already attached.
    bool AttachAnimationFile(const std::filesystem::path& path);
    bool DetachAnimationFile(std::size_t index);

    // ---- Animation tracks ----
    //
    // The sequence dropdown drives one play; a StarCraft II model routinely
    // needs several at once, because a sequence is split across sub-track
    // containers and the game layers them from separate brackets. The Marine's
    // combat shield is `Cover_Shield`, one container of a 33 ms `Cover`
    // sequence, held under whatever the unit is otherwise doing. This is that
    // stack, minus the primary play, which stays the dropdown's.
    struct AnimTrackInfo {
        i32 sequence = 0;
        // -1 ⇒ every sub-track of the sequence, which is what a plain play does.
        i32 subtrack = -1;
        f32 weight = 1.0f;
        f32 speed = 1.0f;
        bool loop = true;
    };
    const std::vector<AnimTrackInfo>& AnimTracks() const {
        return animTracks_;
    }
    // Appends a track on the sequence the dropdown is showing. False when the
    // focus actor has no animation source.
    bool AddAnimTrack();
    // Applies @p t to track @p index. Weight / speed / loop retune the live
    // play; changing the sequence or sub-track restarts it.
    void SetAnimTrack(std::size_t index, const AnimTrackInfo& t);
    void RemoveAnimTrack(std::size_t index);

    // One row per sub-track container of `sequence`, in the order
    // AnimTrackInfo::subtrack indexes them. Empty for anything but an `.m3`.
    struct SubtrackInfo {
        std::string name;
        u16 priority = 0;
        bool concurrent = false;
        std::size_t trackCount = 0;
    };
    std::vector<SubtrackInfo> SubtracksOf(i32 sequence) const;

    // ---- Global loops ----
    //
    // Sequences the model plays by itself, forever, over everything else —
    // StarCraft II's `GLstand` / `GLbirth`. On by default because that is what
    // the engine does; the toggle exists because a viewer is also for looking
    // at one thing at a time.
    struct GlobalLoopInfo {
        i32 sequence = 0;
        std::string name;
        bool enabled = true;
    };
    std::vector<GlobalLoopInfo> GlobalLoops() const;
    void SetGlobalLoopEnabled(i32 sequence, bool on);

    // ---- Focus actor (the one driven by the sequence dropdown, team
    //      colour swatch, etc.) ----
    ActorId FocusActor() const {
        return focusActor_;
    }
    model::Actor* FocusActorPtr() const;

    RenderService& Service() {
        return service_;
    }
    const RenderService& Service() const {
        return service_;
    }

    GLFWwindow* Window() const {
        return window_;
    }
    gfx::GfxApi Backend() const {
        return backend_;
    }

    // Path of the active document's model (or empty when no tab is open). The
    // load paths set this internally; Save As + the export naming read it back.
    const std::filesystem::path& CurrentModelPath() const {
        return currentModelPath_;
    }

    // True when the active document is a model from another Blizzard game
    // (`.m2` / `.m3`). Geometry-only, and in particular there is no MDX to
    // write, so Save As has nothing to offer for one.
    bool CurrentModelIsForeign() const;

    // ---- WEM interchange (WEM_INTEGRATION_DESIGN.md) -----------------------
    //
    // Two halves that do not mirror each other, and deliberately so. Writing a
    // `.wem` needs nothing from the user: the model on screen decides which
    // converter runs. *Reading* one needs a profile, because a document can
    // carry a material set per profile over one geometry — so the open is two
    // steps, and the dialog sits between them.

    // Parse @p path far enough to ask it what profiles it offers. Null when it
    // is not a WEM file, which is also how a host decides whether to show the
    // dialog at all. The parse is not thrown away: hand the same pointer to
    // @ref OpenWemAs and the file is read once.
    std::shared_ptr<io::WemDocument> PeekWemDocument(const std::filesystem::path& path);

    // The profile a `.wem` opens as when nobody picks one — the CLI's
    // `--wem-profile`. `ProfileId::Count` (the default) leaves the choice to
    // the document, which is what every other entry point wants.
    void SetPreferredWemProfile(::whiteout::models::wem::ProfileId profile) {
        preferredWemProfile_ = profile;
    }

    // Open @p document in a NEW tab as @p profile, exactly as LoadModel opens
    // any other model. `ProfileId::Count` means @ref SetPreferredWemProfile's
    // answer, and failing that the document's own default.
    bool OpenWemAs(const std::filesystem::path& path, std::shared_ptr<io::WemDocument> document,
                   ::whiteout::models::wem::ProfileId profile);

    // Whether the active document can be written back as MDX / MDL.
    //
    // Not the same question as "is it a Warcraft III file": a `.wem` opened as
    // either Warcraft III profile IS an `mdx::Model` in memory and writes out
    // fine, and a document with no model at all is not. Asked of the source, so
    // the menu item and the write cannot disagree.
    bool CanSaveAsMdx() const;

    // Whether the active document came from a format WEM can be written from.
    // What the menu item greys out on; it asks the same question the export
    // answers, so the two cannot disagree.
    bool CanExportWem() const;

    // Write the active document to @p outPath as a `.wem`.
    bool ExportWem(const std::filesystem::path& outPath);

    // Whether the active document can be written as a Warcraft III `.mdx`
    // through WEM — that is, whether it came from a format WEM reads and is not
    // Warcraft III already. A Warcraft III model has Save As, which writes it
    // back without a lossy round trip through a derived material set.
    bool CanExportMdx() const;

    // Convert the active document to Warcraft III and write it to @p outPath.
    // Textures are written beside it when @p exportTextures, converted to the
    // container the profile reads. Returns false and logs on failure.
    bool ExportMdx(const std::filesystem::path& outPath, ::whiteout::models::wem::ProfileId profile,
                   bool exportTextures);

    // Whether the active document can be written as a StarCraft II `.m3`
    // through WEM — the same question as CanExportMdx with the formats
    // swapped: it must come from a format WEM reads and not be `.m3` already.
    bool CanExportM3() const;

    // Convert the active document to StarCraft II and write it to @p outPath,
    // textures beside it as `.dds` when @p exportTextures. The Warcraft III
    // knobs (WC3_SD_MATERIAL_TO_SC2_DESIGN.md §5): a composite section for
    // every approximate pass fold, the keyed-alpha bake, and naming War3
    // (Mod)'s copy of a texture instead of writing one.
    bool ExportM3(const std::filesystem::path& outPath, ::whiteout::models::wem::ProfileId profile,
                  bool exportTextures, bool exactPasses = false, bool sharpenTeamKey = false,
                  bool reuseWar3ModTextures = false, bool crossEffects = true,
                  bool standardRefs = true);

    // Whether the active document can be written as glTF through WEM — the
    // widest of the export questions: glTF export takes any carried profile
    // (GLTF_DESIGN §2), so this is exactly CanExportWem.
    bool CanExportGltf() const;

    // Convert the active document to glTF 2.0 and write it to @p outPath —
    // a self-contained `.glb` when @p binary, a `.gltf` + `.bin` + images
    // otherwise. Textures embed (or land beside the file) when
    // @p exportTextures, re-encoded as PNG.
    bool ExportGltf(const std::filesystem::path& outPath, bool binary, bool exportTextures);

    // Whether the active document IS a `.m3` and can therefore be written back
    // in its own format. The mirror of CanExportM3, which answers for every
    // model except this one: a StarCraft II model has nothing to derive, so it
    // saves rather than exports.
    bool CanSaveM3() const;

    // Write the active `.m3` to @p outPath through the M3 writer. @p
    // mergeAnimations folds the attached `.m3a` files into the model's own
    // animation chunks; @p convertToSc2 retargets a Heroes model so StarCraft
    // II will load it (MODL v29, MADD reversed); @p exportTextures copies the
    // textures the layers reference beside it, under their own relative paths.
    //
    // Returns false and logs on failure, filling @p error when given. Worth
    // asking for: a shader-graph material with no StandardMaterial form blocks
    // the retarget outright — 9 of 40 Heroes models sampled — and "nothing
    // happened" is not an answer a dialog can leave the user with.
    bool SaveM3(const std::filesystem::path& outPath, bool mergeAnimations, bool convertToSc2,
                bool exportTextures = false, std::string* error = nullptr);

    // The skins the active `.m2` can wear, and which one it is wearing. Empty
    // when the model is not a creature, or with `.m2` compiled out — a UI
    // asking should hide the control rather than offer an empty one. Setting it
    // re-dresses the focus actor in place; the animation and the camera do not
    // move.
    std::vector<std::string> WowSkinNames() const;
    u32 WowSkin() const;
    void SetWowSkin(u32 skin);

    // The customisation a character `.m2` offers — "Skin Color", "Hair Style",
    // one entry per ChrCustomizationOption the client databases list for this
    // model. Empty when the model is not a character, when the databases are
    // out of reach, or with `.m2` compiled out.
    //
    // Same contract as the skin above, and the same in-place restyle: stepping
    // an option re-cuts the geosets and re-composites the body sheet on the
    // actor that is already standing there.
    struct WowCharacterOption {
        std::string name;
        u32 optionId = 0;
        u32 choiceCount = 0;
        u32 selected = 0;
    };
    std::vector<WowCharacterOption> WowCharacterOptions() const;
    void SetWowCharacterChoice(u32 optionId, u32 choiceIndex);

    // What a Diablo III player character is wearing. The same idea as the two
    // above and a different shape, because D3 states it differently: a `.m2`
    // character leaves its geosets and its body sheet blank for the game to
    // fill, and a `.app` ships every armour variant at once for the game to
    // pick between. So this offers pieces out of a wardrobe rather than
    // choices out of a database.
    //
    // Empty when the focus actor is not a D3 player character, or with `.acr`
    // compiled out. Setting anything re-dresses the actor where it stands.
    struct D3CharacterSlot {
        std::string name;               ///< "Torso", "Legs", "Boots", "Gloves", "Hair".
        i32 slot = 0;                   ///< native::LookSlot, opaque to the UI.
        std::vector<std::string> items; ///< "Naked", "Heavy A", "Medium B (CLS)".
        u32 selectedItem = 0;
        u32 lookIndex = 0; ///< Index into D3LookNames().
        /// True for the slots the item registry dresses (everything but
        /// Hair). With the registry live these rows are redundant with the
        /// outfit rows above them and the UI hides them.
        bool registryDriven = false;
    };
    std::vector<D3CharacterSlot> D3CharacterSlots() const;
    void SetD3CharacterItem(i32 slot, u32 itemIndex);
    void SetD3CharacterSlotLook(i32 slot, u32 lookIndex);

    /// The material sets the appearance ships — "A", "Unique25", "A_skeleton".
    /// This is what an equipped item names through tag 0x10401, so putting one
    /// on every slot at once is what wearing an armour *set* means.
    std::vector<std::string> D3LookNames() const;
    void SetD3CharacterLookForAll(u32 lookIndex);

    /// The sub-objects no equipment slot claims: death bodies, skill meshes,
    /// the merged `oneBatch` LOD. Hidden unless asked for.
    struct D3CharacterExtra {
        std::string name;
        u32 geoset = 0;
        bool shown = false;
    };
    std::vector<D3CharacterExtra> D3CharacterExtras() const;
    void SetD3CharacterExtra(u32 geoset, bool shown);

    // The outfit: dressing by in-game ITEM name, on top of the wardrobe
    // above. One row per engine visual slot; the armour rows dress the model
    // now, the attachment rows (helm/weapons/shoulders) are listed but wait
    // on the attachment phase. Names resolve through the GameBalance item
    // registry, built lazily from the active storage on first use.
    struct D3OutfitRow {
        std::string name;          ///< "Torso", "Right hand", ...
        i32 visualSlot = 0;        ///< native::EVisualSlot, opaque to the UI.
        std::string equipped;      ///< Item name (the stem), empty when nothing is.
        std::string equippedLabel; ///< Its display name; the stem when unnamed.
        i32 dye = 0;
        bool armour = false; ///< True for the four rows that dress today.
    };
    /// Empty when the focus actor is not a D3 player character, or while the
    /// registry is not Ready.
    std::vector<D3OutfitRow> D3OutfitSlots() const;
    /// True once the registry holds items — false means it is still building
    /// (see D3ItemRegistryBuilding) or the storage ships no GameBalance
    /// tables. The first call kicks the build as a background task (a few
    /// thousand small reads, once per storage).
    bool D3ItemRegistryReady() const;
    /// The background build is running — the popup should draw its progress
    /// rather than "no tables".
    bool D3ItemRegistryBuilding() const {
        return d3ItemsBuild_ == D3ItemsBuild::Building;
    }
    /// One picker row: the in-game display name to show, the record stem that
    /// equips it. The stem stays the key everywhere that persists (presets,
    /// scenario tokens); the label is presentation only.
    struct D3OutfitItemEntry {
        std::string label;
        std::string stem;
    };
    /// Up to @p max items offered for @p visualSlot whose display name OR
    /// stem contains @p filter (case-insensitive), sorted by label. The offer
    /// is restricted to what the focused character's class can wear —
    /// class-neutral items always pass — unless @p allClasses. One display
    /// name is one row: the art-test dupes ship a name on many records, and
    /// the group's survivor is the equipped record when one is (so the picker
    /// still shows the selection), else the first stem. A filter that spells
    /// a stem still reaches its record — stems are unique, so a group
    /// filtered down to one record survives as that record.
    std::vector<D3OutfitItemEntry> D3OutfitItemEntries(i32 visualSlot, std::string_view filter,
                                                       usize max, bool allClasses) const;
    /// The item sets whose pieces the focused class can wear (same neutrality
    /// and @p allClasses rules), sorted by display name.
    struct D3OutfitSetEntry {
        std::string label; ///< "Firebird's Finery".
        std::string key;   ///< The ItemSets.stl key — the stable identity.
        usize pieces = 0;  ///< Wearable pieces (rings and amulets excluded).
    };
    std::vector<D3OutfitSetEntry> D3OutfitSetEntries(std::string_view filter,
                                                     bool allClasses) const;
    /// Equip every wearable piece of the set @p key into its slot (weapons
    /// fill right hand then left; other slots keep what they wore). False
    /// when the key names no set.
    bool EquipD3OutfitSet(std::string_view key);
    /// Equip @p itemName (empty = unequip). False when the name is unknown.
    bool SetD3OutfitItem(i32 visualSlot, std::string_view itemName);
    void SetD3OutfitDye(i32 visualSlot, i32 dye);
    /// Sheathe or draw the equipped weapons (moves them between the hand and
    /// the sheath hardpoints).
    bool D3OutfitSheathed() const;
    void SetD3OutfitSheathed(bool sheathed);
    /// One line about an item for the picker's tooltip: gbid, actor SNO and
    /// the cracked type name. Empty for an unknown name.
    std::string D3OutfitItemTip(std::string_view itemName) const;
    /// Outfit presets: whole outfits by name (item NAMES, dyes, sheathe),
    /// stored beside the viewer settings in `d3_outfits.ini`. Loading
    /// tolerates unknown item names: it reports and skips them.
    std::vector<std::string> D3OutfitPresetNames() const;
    bool SaveD3OutfitPreset(std::string_view name);
    bool LoadD3OutfitPreset(std::string_view name);

    /// @name Ragdoll
    ///
    /// D3 has no model-level "collapse now": the client builds the rig on a
    /// gameplay event and stops the actor animating in the same call. Nothing
    /// in a model file carries that event, so the host supplies it — which is
    /// why the switch is here and not in RenderService.
    /// @{
    /// @brief Whether the focus actor has a rig that could collapse at all.
    bool HasD3Ragdoll() const;
    bool D3Ragdoll() const;
    void SetD3Ragdoll(bool on);
    /// @}

private:
    /// Re-dress the focus actor after a wardrobe change, reloading only if it
    /// cannot be done in place.
    void RestyleD3();

    void InitImGui();
    void ShutdownImGui();

    // Runs a queued animation export synchronously: drives the focus actor
    // through the recipe's schedule one frame at a time, captures each
    // composited frame and hands it to the output sink. The loop itself lives
    // in export_runner.cpp; this wires it to the viewer.
    void RunAnimationExport(const ExportRecipe& recipe);
    // Fills the callback bundle export_runner.cpp drives the viewer through.
    ExportHost MakeExportHost();

    void OnFramebufferResize(i32 w, i32 h);

    // Renders one frame from inside a GLFW callback. Windows runs a modal
    // message loop while the user drags a border or the title bar, so the
    // main loop's Tick() doesn't run for the whole drag — the window would
    // sit unpainted at its old size while the swap chain has already grown.
    // The resize / refresh callbacks do fire from inside that loop, so we
    // paint from there. Re-entrant calls are dropped.
    void RedrawFromCallback();

    void OnMouseButton(i32 button, i32 action);
    void OnCursorPos(f64 x, f64 y);
    void OnScroll(f64 yoffset);
    void UpdateCameraPresetAnimator();
    // Frames the orbital camera on a freshly-loaded model — targets the centre
    // of its bounding box, three-quarter angle, distance to fit.
    void FrameCameraToModel(model::Actor* hero);

    // Frames the orbital camera on a standalone .pkb effect using its live
    // particle cloud (a .pkb has no mesh bounds). Returns true once it found
    // particles and reframed; false while the effect hasn't spawned any yet.
    // Driven by the deferred warm-up in Tick — particle spread isn't known
    // until the sim has run a few frames.
    bool FrameCameraToEffect();

    static void FramebufferSizeCallback(GLFWwindow* w, int width, int height);
    static void WindowRefreshCallback(GLFWwindow* w);
    static void MouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* w, double x, double y);
    static void ScrollCallback(GLFWwindow* w, double xoff, double yoff);

    // Per-document host state. The "flat" working members below mirror the
    // ACTIVE document; SaveActiveDocState / LoadActiveDocState shuttle that
    // mirror in and out of these slots on a tab switch. The scene itself
    // (actors, camera, clock) lives in the RenderService under `scene`.
    struct Document {
        SceneId scene = 0;
        std::string title; // tab label (file stem)
        std::filesystem::path modelPath;
        ActorId focusActor = 0;
        std::vector<std::string> sequenceNames;
        std::vector<SequenceInfo> sequenceRanges;
        std::vector<AnimTrackInfo> animTracks;
        std::vector<u32> animTrackHandles;
        // Global loops the user switched off, by sequence index. Cleared
        // whenever the sequence table is rebuilt, since the indices are then
        // meaningless — the same reason the sequence *selection* resets.
        std::vector<i32> silencedGlobals;
        std::vector<CameraPreset> cameraPresets;
        std::vector<std::string> cameraPresetNamesUtf8;
        i32 activeCameraPresetIdx = -1;
        bool cameraLocked = false;
        i32 walkDriftPrevSeqIdx = -1;
        f32 walkDriftAccumulated = 0.0f;
        i32 effectFrameTicks = -1;
        i32 lastParentTimeMs = 0;
        // Render mode is genuinely per scene now (SceneManager::SetRenderMode)
        // — the document's scene carries it, so there is no mirror here.
    };

    // Scene of the active document, or the default scene when none is open.
    SceneId ActiveSceneId() const;
    // Publish ActiveSceneId() as the RenderService's active scene so input
    // callbacks + every Scene() read this frame target the active document.
    void PublishActiveScene();
    // Lazily build (and return) the shared content provider every document
    // scene uses — the default scene's configured FileContentProvider, wrapped
    // in a non-owning shared_ptr so all tabs see the same CASC/MPQ/install set.
    std::shared_ptr<io::IContentProvider> SharedProvider();
    // Copy the flat working members into / out of documents_[activeDoc_].
    void SaveActiveDocState();
    void LoadActiveDocState();
    // Reset the flat working members to "no model loaded".
    void ClearWorkingState();
    // Point the shared RenderSettings (+ content-provider HD mode, splat /
    // event-data caches) at `wanted`, running the side effects only when the
    // mode actually changes. Called at load and whenever a document with a
    // different mode becomes active, so each scene draws in its own HD/SD mode.
    void ApplyRenderMode(RenderMode wanted);
    // Create a new document scene bound to `provider`, run `loadBody` (which
    // spawns into the now-active scene and fills the flat working state,
    // returning false on failure), and register it as the active tab. On failure
    // the scene is destroyed and the previously-active tab restored. The single
    // place that owns document create / rollback / register.
    bool OpenDocumentScene(std::shared_ptr<io::IContentProvider> provider, std::string title,
                           const std::function<bool()>& loadBody);
    // Restore the active document after a failed open: re-activate `prevDoc`
    // (its saved state + scene), or clear to the empty/default state if none.
    void RestoreActiveAfterFailedOpen(i32 prevDoc);

    // Re-point the shared provider at the game a model file belongs to, so its
    // textures have a storage to resolve against. See the definition.
    void FollowModelGame(const std::filesystem::path& path);
    void AdoptNearbyWowKeys(const std::filesystem::path& modelPath);

    // Shared body of LoadModel/LoadEffect: opens `path` (from the shared game
    // provider) as a new document. `effect` selects the .pkb path.
    bool OpenDocument(const std::filesystem::path& path, bool effect);

    // Open a model/effect from an arbitrary content provider (e.g. the Storage
    // Explorer's CASC provider) as a new document. Unlike OpenDocument, the doc
    // scene uses `provider` instead of the shared game provider, and HD-ness is
    // derived from the archive path (no filesystem MDX probe). `archivePath` is
    // a provider-native path (CASC ':'/'\\' separators).
    // The document open itself, once whatever storage it needs is up.
    bool OpenStorageDocumentNow(const std::string& archivePath, bool effect,
                                std::shared_ptr<io::IContentProvider> provider);
    bool OpenStorageDocument(const std::string& archivePath, bool effect,
                             std::shared_ptr<io::IContentProvider> provider);
    // Common post-spawn flat-state fill (sequences, camera framing, presets)
    // shared by the model load paths. `hero` may be null (caller handles).
    void FillModelDocState(model::Actor* hero, const std::filesystem::path& path);
    // Re-read the focus actor's sequence table into the dropdown mirrors,
    // keeping the current selection when it is still in range. Shared by the
    // load path and by attaching / detaching an animation file.
    void RefreshSequenceCache(model::Actor* hero, bool resetSelection);
    // Hand the focus actor's playlist the global loops minus silencedGlobals_.
    void PublishGlobalLoops();
    // Re-play every track after something rebuilt the playlist (a Bind), which
    // leaves every handle we hold pointing at nothing.
    void ReassertAnimTracks();
    // Post-spawn flat-state fill for a standalone effect (placeholder sequence,
    // provisional camera, deferred reframe). Caller sets currentModelPath_.
    void FillEffectDocState(model::Actor* hero);
    // Body of OpenDocument after a fresh scene is active: spawns into the
    // active scene and fills the flat working state. Returns false on spawn
    // failure (caller tears the scene down).
    bool LoadModelIntoActiveScene(const std::filesystem::path& path);
    bool LoadEffectIntoActiveScene(const std::filesystem::path& path);

    RenderService& service_;
    GLFWwindow* window_ = nullptr;
    gfx::GfxApi backend_ = gfx::GfxApi::D3D12;
    RenderTargetId targetId_ = 0;
    bool imguiInitialised_ = false;

    i32 lastFbW_ = 0;
    i32 lastFbH_ = 0;

    // Set while RedrawFromCallback is driving Tick() — suppresses the nested
    // glfwPollEvents (we're already inside event dispatch) and guards against
    // a callback firing recursively out of that frame.
    bool inCallbackRedraw_ = false;

    std::unique_ptr<ViewerUI> ui_;

    // "Reforged Graphics" — when set, every model loads/renders in HD
    // regardless of its detected SD/HD preference (see LoadModelIntoActiveScene
    // + SetForceHd).
    bool forceHd_ = false;

    // ---- Open documents (tabs) ----
    std::vector<Document> documents_;
    i32 activeDoc_ = -1; // index into documents_, or -1 when none are open
    // Tab index the UI should force-select next frame after an app-driven active
    // change, or -1 (see ConsumePendingTabSelect).
    i32 pendingTabSelect_ = -1;
    // Non-owning shared_ptr aliasing the default scene's content provider; one
    // instance, lazily built by SharedProvider(), handed to every document
    // scene so they all resolve assets through the same configured provider.
    std::shared_ptr<io::IContentProvider> sharedProvider_;

    // Save the panel's ini section once its state stops changing.
    void PollStorageExplorerState(f32 dt);

    // Embedded Storage Explorer panel (Tools ▸ Storage Explorer), created lazily.
    std::unique_ptr<tools::StorageExplorer> storageExplorer_;
    bool storageExplorerOpen_ = false;
    // What the panel's `[StorageExplorer]` ini section last held, and how long
    // until it is rewritten. The panel's state moves continuously while a
    // splitter or a zoom slider is being dragged, so the write waits for it to
    // settle rather than rewriting the whole settings file every frame.
    std::string explorerStateKey_;
    float explorerSaveDelay_ = 0.0f;

    // The profile the Settings panel is on. Seeded from the ini at startup.
    ProductId settingsProfile_ = ProductId::Wc3;
    // Which products have had their ini settings loaded into their slot this
    // session, indexed by ProductId. See ApplyProfile.
    std::array<bool, 5> ioProfileApplied_{};

    // ---- Host state (mirror of the ACTIVE document) ----
    bool loopNonLoopingPolicy_ = true;
    ActorId focusActor_ = 0;
    // Deferred camera-framing for a standalone .pkb: LoadEffect arms this, Tick
    // counts sim frames and reframes once the particle cloud has developed.
    // <0 means inactive.
    i32 effectFrameTicks_ = -1;
    std::vector<CameraPreset> cameraPresets_;
    std::vector<std::string> cameraPresetNamesUtf8_;
    std::vector<std::string> sequenceNames_;
    std::vector<SequenceInfo> sequenceRanges_;
    // The extra plays the Animation window owns, and the playlist handles they
    // were started with — parallel, one handle per track. A handle the playlist
    // has already retired reads back as "not found" and the track simply
    // restarts, which is what makes a non-looping track re-armable.
    std::vector<AnimTrackInfo> animTracks_;
    std::vector<u32> animTrackHandles_;
    std::vector<i32> silencedGlobals_;
    i32 activeCameraPresetIdx_ = -1;
    bool cameraLocked_ = false;
    std::filesystem::path currentModelPath_;

    // The WEM document the next open will spawn, and the profile it was picked
    // at. Set by OpenWemAs (from the dialog) and by LoadModel (from the file,
    // at its own default) — both before FollowModelGame runs, because the
    // profile is what tells it which game's storage to point at.
    std::shared_ptr<io::WemDocument> pendingWemDocument_;
    /// What the CLI asked for, or `Count`.
    ::whiteout::models::wem::ProfileId preferredWemProfile_ =
        ::whiteout::models::wem::ProfileId::Count;
    ::whiteout::models::wem::ProfileId pendingWemProfile_ =
        ::whiteout::models::wem::ProfileId::Count;

    // ---- Input state ----
    bool lmbDown_ = false;
    bool rmbDown_ = false;
    bool mmbDown_ = false;
    f64 lastMouseX_ = 0.0;
    f64 lastMouseY_ = 0.0;

    // FPS counter (title bar)
    f64 fpsAccum_ = 0.0;
    i32 fpsFrames_ = 0;

    // Walk-drift state — used by Tick to advance the actor along
    // walk-cycle sequences, mirroring the old test_main logic.
    i32 walkDriftPrevSeqIdx_ = -1;
    f32 walkDriftAccumulated_ = 0.0f;

    // Last animation-time sample, for computing parentDt without a
    // duplicate animation-clock tick.
    i32 lastParentTimeMs_ = 0;

    // Paths handed over before the frame loop started (the startup picker),
    // opened one per frame by Tick so each gets a drawable progress bar.
    std::vector<std::filesystem::path> pendingInitialOpens_;

    // Pending animation export — filled by RequestAnimationExport, consumed
    // by the next Tick().
    bool exportPending_ = false;
    ExportRecipe pendingExport_;
    ExportReport lastExportReport_;
    bool exportFinished_ = false;
    bool exportRunning_ = false;

    // LAST member, deliberately. Members are destroyed in reverse declaration
    // order, so declaring it here is what makes it the FIRST thing torn down —
    // and its destructor cancels the running task and joins the thread. A task
    // body captures the Storage Explorer, the provider and this object; every
    // one of them is still alive while that join happens, because every one of
    // them is declared above.
    io::LoadTaskRunner tasks_;

    friend class ViewerUI;
};

} // namespace whiteout::flakes
