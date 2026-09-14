// Phase-2 prefetch — synchronous-ReadFile assets (BLS shaders, IBL probe,
// DNC unit, event-data SLKs) that wf_init reads before any model loads.
// Bytes go into FetchContentProvider via _wf_provider_put.

const VS_SHADERS = ['foliage', 'gritty_hd', 'hd', 'imgui', 'popcornfx',
                    'sd_highspec', 'sd_on_hd', 'sprite', 'terrain', 'toon_hd'];
// bloomextract/bloomcombine back PostProcessService's bloom and depthoffield
// backs DofService; without them the engine disables both at init.
const PS_SHADERS = ['bloomcombine', 'bloomextract', 'crystal', 'depthoffield',
                    'distortion', 'foliage', 'gritty_hd', 'hd', 'imgui',
                    'popcornfx', 'sd', 'sd_on_hd', 'sprite', 'terrain', 'tonemap', 'toon_hd'];

// Engine assets that must skip the computable direct URL and resolve
// through /casc-contents/ only. The IBL probes are mode-agnostic files
// that live in one layer, so the mode-derived direct base would be wrong
// half the time; the backstop finds them either way. (The DNC rig is also
// backstop-only but has its own path — see DNC_VARIANTS.)
const CASC_ONLY = new Set([
    'Environment/EnvironmentMap/Portraits/PortraitDefault_IBL.dds',
    // Day/Night IBL probes — used by IblMode::DayNight (the default probe set).
    'Environment/EnvironmentMap/LordaeronSummer/Day_IBL.dds',
    'Environment/EnvironmentMap/LordaeronSummer/Night_IBL.dds',
]);

// DNC rig — the directional sun + ambient for BOTH pipelines.
//
// DncService pins its catalog path to a mod layer before asking the provider
// (`war3.w3mod:` in SD, `war3.w3mod:_hd.w3mod:` in HD — dnc_service.cpp's
// ReacquireAsset), so the provider must be keyed WITH the mount or the lookup
// misses and the scene loses its light entirely. The two layers are different
// files with different curves (HD ambientIntensity 0 vs SD 0.3), and the
// renderer switches between them with the render mode, so both are fetched.
const DNC_UNIT = 'Environment/DNC/DNCLordaeron/DNCLordaeronUnit/DNCLordaeronUnit.mdl';
const DNC_VARIANTS = [
    { context: 'sd', mount: 'war3.w3mod:' },
    { context: 'hd', mount: 'war3.w3mod:_hd.w3mod:' },
];

const ENGINE_ASSETS = [
    // IBL probes (portrait default + day/night; CASC-only — see above). The
    // day/night probes back IblMode::DayNight, the viewer's default probe set.
    'Environment/EnvironmentMap/Portraits/PortraitDefault_IBL.dds',
    'Environment/EnvironmentMap/LordaeronSummer/Day_IBL.dds',
    'Environment/EnvironmentMap/LordaeronSummer/Night_IBL.dds',
    // Replaceable tree textures (IDs 31-36). The renderer's
    // ReplaceableTextureManager resolves these via ContentProvider::Request
    // (synchronous, not the AssetManager needs pump that JS drains), so
    // they have to be pre-pushed or every tree-textured weapon / club
    // shows up with the white stub. Canonical paths from
    // src/io/replaceable_paths.cpp::ReplaceableCanonicalPath. Small files
    // (~50 KB each) — cheap to fetch all six upfront.
    'ReplaceableTextures/LordaeronTree/LordaeronSummerTree.blp',
    'ReplaceableTextures/AshenvaleTree/AshenTree.blp',
    'ReplaceableTextures/BarrensTree/BarrensTree.blp',
    'ReplaceableTextures/NorthrendTree/NorthTree.blp',
    'ReplaceableTextures/Mushroom/MushroomTree.blp',
    'ReplaceableTextures/RuinsTree/RuinsTree.blp',
    // Event-data SLKs — SPN/SPL/UBR resolution + SndEntry lookups.
    'Splats/SpawnData.slk',
    'Splats/SplatData.slk',
    'Splats/UberSplatData.slk',
    'UI/SoundInfo/DialogueCreepsBase.slk',
    'UI/SoundInfo/DialogueDemonBase.slk',
    'UI/SoundInfo/DialogueHumanBase.slk',
    'UI/SoundInfo/DialogueNagaBase.slk',
    'UI/SoundInfo/DialogueNightElfBase.slk',
    'UI/SoundInfo/DialogueOrcBase.slk',
    'UI/SoundInfo/DialogueUndeadBase.slk',
    'UI/SoundInfo/SoundAssetCombat.slk',
    'UI/SoundInfo/UnitAckSounds.slk',
    'UI/SoundInfo/UnitCombatSounds.slk',
    'UI/SoundInfo/UISounds.slk',
    'UI/SoundInfo/AmbienceSounds.slk',
    'UI/SoundInfo/AnimSounds.slk',
    'UI/SoundInfo/AbilitySounds.slk',
    'UI/SoundInfo/DialogSounds.slk',
    'UI/SoundInfo/AmbientMusic.slk',
    'UI/SoundInfo/Music.slk',
];

function putBytes(viewer, path, u8) {
    const M = viewer._module;
    const pathPtr = viewer._cstr(path);
    const dataPtr = M._malloc(u8.byteLength);
    M.HEAPU8.set(u8, dataPtr);
    try {
        M._wf_provider_put(viewer._handle, pathPtr, dataPtr, u8.byteLength);
    } finally {
        M._free(dataPtr);
        M._free(pathPtr);
    }
}

async function fetchResult(url) {
    try {
        const r = await fetch(url);
        if (!r.ok) return null;
        return { bytes: new Uint8Array(await r.arrayBuffer()), finalUrl: r.url };
    } catch (_) { return null; }
}

function extOf(s) {
    const dot = s.lastIndexOf('.');
    return dot >= 0 ? s.slice(dot).toLowerCase() : '';
}

// If Hive served e.g. foo.dds for our foo.blp request, store under
// the served ext so FetchContentProvider's alt-ext walk hits and the
// renderer parses with the right decoder.
function pathWithServedExt(originalPath, finalUrl) {
    let servedExt = extOf(originalPath);
    try {
        const finalPath = new URL(finalUrl).pathname.toLowerCase();
        const ext = extOf(finalPath);
        if (ext) servedExt = ext;
    } catch (_) { /* opaque URL — keep original */ }
    const dot = originalPath.lastIndexOf('.');
    return (dot >= 0 ? originalPath.slice(0, dot) : originalPath) + servedExt;
}

// Both DNC layers, each stored under the mod-pinned key DncService will ask
// for. `context` is what selects the layer server-side; the request path keeps
// the plain `war3.w3mod:` mount either way (Hive resolves the _hd.w3mod
// overlay from the context, not the path).
async function prefetchDnc(viewer) {
    await Promise.all(DNC_VARIANTS.map(async ({ context, mount }) => {
        const url = viewer.cascUrl('war3.w3mod:' + DNC_UNIT, false) + '&context=' + context;
        const res = await fetchResult(url);
        if (!res) { console.warn('[wf] prefetch FAIL dnc/' + context); return; }
        putBytes(viewer, pathWithServedExt(mount + DNC_UNIT, res.finalUrl), res.bytes);
    }));
}

// Every entry resolves through the shared Hive chain, so the startup set
// gets the same direct-URL fast path the asset pump does — 23 of these 26
// are plain mod content the direct tree serves in one round trip.
//
// There used to be a `viewer.engineAssetRoot || './'` probe ahead of the
// CASC fetch. `engineAssetRoot` was never assigned anywhere in the package
// or in any host, so the probe was unconditional and could only 404 — 26
// guaranteed misses per page load, each one blocking the real fetch behind
// it, individually measured at up to 1.5 s. If a local mirror is wanted
// again it comes back as an explicit prefix a host sets, not as a
// speculative probe.
export async function prefetchEngineAssets(viewer) {
    await Promise.all([
        prefetchDnc(viewer),
        ...ENGINE_ASSETS.map(async (p) => {
            // Mode-agnostic files: no context, and no direct arm (the
            // direct base is mode-derived and would pick a layer).
            const urls = CASC_ONLY.has(p)
                ? [viewer.cascUrl(p, false)]
                : viewer.cascCandidates(p);
            for (const url of urls) {
                const res = await fetchResult(url);
                if (res) {
                    putBytes(viewer, pathWithServedExt(p, res.finalUrl), res.bytes);
                    return;
                }
            }
            console.warn('[wf] prefetch FAIL ' + p);
        }),
    ]);
}

export async function prefetchShaders(viewer) {
    // `path` doubles as the key the BLS cache queries through the wasm
    // provider, while the URL is module-relative so the package keeps
    // working when mounted under any subpath (e.g. /whiteout-js-viewer/).
    const paths = [
        ...VS_SHADERS.map(n => `shaders/webgpu/vs/${n}.bls`),
        ...PS_SHADERS.map(n => `shaders/webgpu/ps/${n}.bls`),
    ];
    await Promise.all(paths.map(async (path) => {
        const url = new URL('./' + path, import.meta.url).href;
        const res = await fetchResult(url);
        if (!res) { console.warn('[wf] prefetch FAIL ' + path); return; }
        putBytes(viewer, path, res.bytes);
    }));
}
