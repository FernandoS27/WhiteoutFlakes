// Phase-2 prefetch — synchronous-ReadFile assets (BLS shaders, IBL probe,
// DNC unit, event-data SLKs) that wf_init reads before any model loads.
// Bytes go into FetchContentProvider via _wf_provider_put.

const VS_SHADERS = ['foliage', 'gritty_hd', 'hd', 'imgui', 'popcornfx',
                    'sd_highspec', 'sd_on_hd', 'sprite', 'terrain', 'toon_hd'];
const PS_SHADERS = ['crystal', 'distortion', 'foliage', 'gritty_hd', 'hd', 'imgui',
                    'popcornfx', 'sd', 'sd_on_hd', 'sprite', 'terrain', 'tonemap', 'toon_hd'];

const ENGINE_ASSETS = [
    // IBL probe + DNC unit MDX.
    'Environment/EnvironmentMap/Portraits/PortraitDefault_IBL.dds',
    'Environment/DNC/DNCLordaeron/DNCLordaeronUnit/DNCLordaeronUnit.mdx',
    'Environment/DNC/DNCLordaeron/DNCLordaeronUnit/DNCLordaeronUnit.mdl',
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

// Tries `engineAssetRoot` (./ by default), falls back to viewer.cascUrl.
export async function prefetchEngineAssets(viewer) {
    const root = viewer.engineAssetRoot || './';
    await Promise.all(ENGINE_ASSETS.map(async (p) => {
        let res = await fetchResult(root + p);
        if (!res) res = await fetchResult(viewer.cascUrl(p));
        if (res) putBytes(viewer, pathWithServedExt(p, res.finalUrl), res.bytes);
    }));
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
