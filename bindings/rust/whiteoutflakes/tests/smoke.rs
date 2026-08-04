// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Headless smoke tests. Everything here runs without a gfx device, which
// is what makes them CI-able: `Renderer::new` only constructs the scene
// and service objects, and the views exercised below never reach the
// pipeline.
//
// Not everything can be tested this way. `Renderer::tick`, every
// `AssetsView` method, and `ReplaceablesView` dereference subsystems the
// C++ side only creates inside `init_device` — calling them on a
// device-less renderer segfaults. That is a precondition of the C++ API
// rather than anything the bindings introduce, so those paths belong in
// a device-backed example, not here.

use whiteoutflakes::{IblMode, LightingMode, RenderMode, Renderer, ShadowParams};

#[test]
fn constructs_and_hands_out_views() {
    let mut r = Renderer::new();
    assert!(r.pipeline().is_some());
    assert!(r.scene().is_some());
    assert!(r.camera().is_some());
    assert!(r.settings().is_some());
    assert!(r.loader().is_some());
    assert!(r.assets().is_some());
}

#[test]
fn settings_round_trip() {
    let mut r = Renderer::new();
    let mut s = r.settings().unwrap();

    s.set_lighting_mode(LightingMode::Glue);
    assert_eq!(s.lighting_mode(), LightingMode::Glue);

    s.set_ibl_mode(IblMode::Dungeon);
    assert_eq!(s.ibl_mode(), IblMode::Dungeon);

    s.set_tonemap_exposure(1.5);
    assert!((s.tonemap_exposure() - 1.5).abs() < 1e-6);

    s.set_lod_override(2);
    assert_eq!(s.lod_override(), 2);

    s.set_background_color(10, 20, 30);
    assert_eq!(s.background_color_raw(), 30 << 16 | 20 << 8 | 10);

    // Set once, then read the one-shot dirty flag it raises.
    s.set_render_mode(RenderMode::HD);
    assert!(s.consume_render_mode_dirty());
    assert!(!s.consume_render_mode_dirty());
}

#[test]
fn display_flags_round_trip() {
    let mut r = Renderer::new();
    let mut s = r.settings().unwrap();

    let mut flags = s.display_flags().unwrap();
    flags.set_show_grid(false);
    flags.set_show_particles(false);
    s.set_display_flags(&flags);

    let read_back = s.display_flags().unwrap();
    assert!(!read_back.show_grid());
    assert!(!read_back.show_particles());
}

#[test]
fn camera_orbit_controls_apply() {
    let mut r = Renderer::new();
    let mut cam = r.camera().unwrap();

    cam.set_distance(250.0);
    assert!((cam.distance() - 250.0).abs() < 1e-3);

    // `target` and `set_direct_pose` come from the hand-written shim, so
    // this also covers the flat-float marshalling in
    // bindings/c/whiteout_flakes_shims.cpp.
    cam.set_target(1.0, 2.0, 3.0);
    assert_eq!(cam.target(), [1.0, 2.0, 3.0]);

    cam.set_direct_pose([10.0, 0.0, 0.0], [0.0, 0.0, 0.0], 0.25);
    cam.set_orbital_mode();
}

#[test]
fn shadow_params_round_trip() {
    let mut r = Renderer::new();
    let mut shadow = r.shadow().unwrap();

    // No shadow service exists until the device is up, so `set_params` is
    // a no-op and `params` reports the C++ defaults. Asserting on the
    // default is the point: it proves the struct crosses both ways
    // intact, defaults included.
    assert_eq!(shadow.params(), ShadowParams::default());
    assert_eq!(ShadowParams::default().cascade_count, 3);
    assert!(!ShadowParams::default().enabled);

    shadow.set_params(&ShadowParams {
        cascade_count: 2,
        ..ShadowParams::default()
    });
}

#[test]
fn frame_stats_start_empty() {
    let mut r = Renderer::new();
    let stats = r.pipeline().unwrap().frame_stats().unwrap();
    assert_eq!(stats.geosets(), 0);
    assert_eq!(stats.textures(), 0);
}

#[test]
fn unknown_actor_handle_is_invalid() {
    let mut r = Renderer::new();
    let actor = r.actor(9999).unwrap();
    assert!(!actor.is_valid());
    assert_eq!(actor.handle(), 9999);
    assert!(actor.sequences().unwrap().is_empty());
    assert!(actor.camera_presets().unwrap().is_empty());
    assert!(actor.child_model_paths().is_empty());
}

#[test]
fn scene_clock_is_settable() {
    let mut r = Renderer::new();
    let mut scene = r.scene().unwrap();
    scene.set_animation_time_ms(1234);
    assert_eq!(scene.animation_time_ms(), 1234);
}

#[test]
fn casc_browser_starts_closed_and_unfiltered() {
    let br = whiteoutflakes::CascBrowser::new();
    assert!(!br.is_open());
    assert!(br.root().is_empty());
    assert!(br.last_error().is_empty());
    assert_eq!(br.filter(), whiteoutflakes::CascFileFilter::All);
    // Nothing to list, and nothing that pretends otherwise.
    assert!(br.files().is_empty());
    assert!(br.folders().is_empty());
    assert_eq!(br.unfiltered_file_count(), 0);
}

#[test]
fn casc_browser_classifies_effects_by_extension() {
    // The same predicate backs `is_effect` and the ModelsOnly/EffectsOnly
    // filter, so pinning it here pins the filter too — and it must be
    // case-insensitive, since archive paths keep whatever case the authoring
    // tool wrote.
    let br = whiteoutflakes::CascBrowser::new();
    for name in ["fog.pkb", "fog.pkfx", "FOG.PKB", "Fog.PkFx"] {
        assert!(br.is_effect(name), "{name} should be an effect");
    }
    for name in ["druid.mdx", "druid.mdl", "DRUID.MDX", "pkb.mdx", "notpkb"] {
        assert!(!br.is_effect(name), "{name} should not be an effect");
    }
}

#[test]
fn casc_browser_filter_round_trips() {
    use whiteoutflakes::CascFileFilter::*;
    let mut br = whiteoutflakes::CascBrowser::new();
    for f in [ModelsOnly, EffectsOnly, All] {
        br.set_filter(f);
        assert_eq!(br.filter(), f);
    }
}
