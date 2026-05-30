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

async function fetchBytes(url) {
    try {
        const r = await fetch(url);
        if (!r.ok) return null;
        return new Uint8Array(await r.arrayBuffer());
    } catch (_) { return null; }
}

// Tries `engineAssetRoot` (./ by default), falls back to viewer.cascUrl.
export async function prefetchEngineAssets(viewer) {
    const root = viewer.engineAssetRoot || './';
    await Promise.all(ENGINE_ASSETS.map(async (p) => {
        let bytes = await fetchBytes(root + p);
        if (!bytes) bytes = await fetchBytes(viewer.cascUrl(p));
        if (bytes) putBytes(viewer, p, bytes);
    }));
}

export async function prefetchShaders(viewer) {
    // Push under the path shape the BLS cache queries.
    const paths = [
        ...VS_SHADERS.map(n => `shaders/webgpu/vs/${n}.bls`),
        ...PS_SHADERS.map(n => `shaders/webgpu/ps/${n}.bls`),
    ];
    await Promise.all(paths.map(async (path) => {
        const bytes = await fetchBytes('./' + path);
        if (!bytes) { console.warn('[wf] prefetch FAIL ' + path); return; }
        putBytes(viewer, path, bytes);
    }));
}
