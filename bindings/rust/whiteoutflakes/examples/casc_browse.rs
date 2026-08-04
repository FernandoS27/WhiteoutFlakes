// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Browse an installed Warcraft III's CASC storage and render what you pick.
//
//   cargo run --example casc_browse -- "C:\Program Files\Warcraft III"
//   cargo run --example casc_browse -- <install> units\nightelf\druid
//
// The two halves that make this useful together:
//
//   * `CascBrowser` walks the archive as a folder tree of models and effects.
//     It is standalone — no Renderer, no gfx device — so a tool can list and
//     filter an install without bringing the engine up.
//   * `SceneView::set_casc_install_path` points a scene's content provider at
//     the same archive, so the paths the browser hands back load directly.
//     `child_path` returns the original archive path for exactly that.
//
// With no folder argument this prints the root listing and stops. Given one,
// it descends there, renders the first file it finds off-screen, and reports
// what came out — enough to prove the browse-then-load path end to end
// without a window.

fn main() {
    if let Err(e) = run() {
        eprintln!("error: {e}");
        std::process::exit(1);
    }
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let install = args
        .next()
        .ok_or("usage: casc_browse <warcraft-iii-install> [folder\\path]")?;
    let folder = args.next();

    let mut br = whiteoutflakes::CascBrowser::new();
    if !br.open(&install) {
        return Err(format!("open failed: {}", br.last_error()).into());
    }
    println!("opened {}", br.root());

    if let Some(f) = &folder {
        br.navigate_to(f);
        if br.current_path().is_empty() && !f.is_empty() {
            return Err(format!("no such folder: {f}").into());
        }
    }

    let folders = br.folders();
    let files = br.files();
    let here = br.current_path();
    println!(
        "\n{}  —  {} folders, {} files",
        if here.is_empty() { "<root>" } else { &here },
        folders.len(),
        files.len()
    );
    for d in folders.iter().take(20) {
        println!("  [dir]  {d}");
    }
    if folders.len() > 20 {
        println!("  … {} more folders", folders.len() - 20);
    }
    for f in files.iter().take(20) {
        let kind = if br.is_effect(f) { "fx " } else { "mdl" };
        println!("  [{kind}]  {f}");
    }
    if files.len() > 20 {
        println!("  … {} more files", files.len() - 20);
    }

    let Some(first) = files.first() else {
        println!("\nNo files here — pass a folder to render one, e.g. units\\nightelf\\druid");
        return Ok(());
    };

    // ---- render the first file, off-screen ----
    let path = br.child_path(first);
    let is_effect = br.is_effect(first);
    println!("\nrendering {first}\n  archive path: {path}");

    let mut r = whiteoutflakes::Renderer::new();
    if !r.use_bundled_shaders() {
        return Err("no bundled shader pack — rebuild with a backend feature".into());
    }
    let api = r.preferred_backend().ok_or("no usable gfx backend")?;
    r.pipeline().expect("live").init_device(api);
    if !r.pipeline().expect("live").is_device_ready() {
        return Err(format!("InitDevice failed for {api:?}").into());
    }

    // The same install the browser is reading, so `path` resolves.
    r.scene().expect("live").set_casc_install_path(&install);
    // HD and SD are different layers of the archive; picking the wrong one
    // resolves textures to the other art set.
    r.scene()
        .expect("live")
        .set_hd_mode(path.contains("_hd.w3mod"));

    let mut loader = r.loader().expect("live");
    let handle = if is_effect {
        loader.spawn_effect(&path)
    } else {
        loader.spawn_unit(&path)
    };
    drop(loader);
    if handle == 0 {
        return Err(format!("spawn failed for {path}").into());
    }

    if !is_effect {
        let preferred = r.actor(handle).expect("live").preferred_render_mode();
        r.settings().expect("live").set_render_mode(preferred);
    }
    if let Some(mut cam) = r.camera_at(0) {
        cam.reset();
        cam.set_target(0.0, 0.0, 60.0);
        cam.set_distance(400.0);
    }

    let target = r
        .pipeline()
        .expect("live")
        .create_offscreen_target(256, 256);
    // A few frames: the template resolves over the first couple of ticks, and
    // an effect needs a moment to spawn its first particles.
    for _ in 0..30 {
        r.advance(1.0 / 60.0);
        r.pipeline().expect("live").render_viewport(target, 0);
    }

    let stats = r.pipeline().expect("live").frame_stats().expect("live");
    println!(
        "  drew {} geosets, {} textures, {} corn particles",
        stats.geosets(),
        stats.textures(),
        stats.corn_particles()
    );
    if stats.geosets() == 0 && stats.corn_particles() == 0 {
        return Err("nothing rendered — the archive path did not resolve".into());
    }

    r.pipeline().expect("live").shutdown();
    println!("\nOK");
    Ok(())
}
