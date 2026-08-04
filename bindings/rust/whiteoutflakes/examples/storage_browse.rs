// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Browse a storage — a CASC install, an MPQ archive, or a directory — and
// render what you pick.
//
//   cargo run --example storage_browse -- "C:\Program Files\Warcraft III"
//   cargo run --example storage_browse -- <install> units\nightelf\druid
//
// The two halves that make this useful together:
//
//   * `StorageBrowser` presents any of the three as one folder tree of models
//     and effects, which is what makes it usable as an open-dialog backend.
//     It is standalone — no Renderer, no gfx device — so a tool can list and
//     filter without bringing the engine up.
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
        .ok_or("usage: storage_browse <warcraft-iii-install> [folder\\path]")?;
    let folder = args.next();
    // all (default) | models | effects
    let filter = match args.next().as_deref() {
        None | Some("all") => whiteoutflakes::StorageFileFilter::All,
        Some("models") => whiteoutflakes::StorageFileFilter::ModelsOnly,
        Some("effects") => whiteoutflakes::StorageFileFilter::EffectsOnly,
        Some(other) => return Err(format!("unknown filter {other:?} (all|models|effects)").into()),
    };

    let mut br = whiteoutflakes::StorageBrowser::new();
    // Let the browser work out what it was handed: a CASC install, an MPQ
    // archive, or a plain directory to walk.
    if !br.open_auto(&install) {
        return Err(format!("open failed: {}", br.last_error()).into());
    }
    println!("opened {} as {:?}", br.root(), br.kind());
    br.set_filter(filter);

    if let Some(f) = &folder {
        br.navigate_to(f);
        if br.current_path().is_empty() && !f.is_empty() {
            return Err(format!("no such folder: {f}").into());
        }
    }

    let folders = br.folders();
    let files = br.files();
    let here = br.current_path();
    let total = br.unfiltered_file_count();
    println!(
        "\n{}  —  {} folders, {} files{}",
        if here.is_empty() { "<root>" } else { &here },
        folders.len(),
        files.len(),
        // Say what the filter hid, so an empty listing does not read as an
        // empty folder.
        if files.len() as i32 == total {
            String::new()
        } else {
            format!("  ({total} before the {:?} filter)", br.filter())
        }
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
        if total > 0 {
            println!("\nEverything here was filtered out — try `all`.");
        } else {
            println!("\nNo files here — pass a folder, e.g. units\\nightelf\\druidoftheclaw");
        }
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

    // Point the provider at whatever the browser opened, so `path` resolves.
    match br.kind() {
        whiteoutflakes::StorageKind::Casc => {
            r.scene().expect("live").set_casc_install_path(&install)
        }
        whiteoutflakes::StorageKind::Mpq => {
            r.scene().expect("live").set_mpq_list(&[install.as_str()])
        }
        // A folder's entries are absolute paths already; the provider reads
        // them straight off disk, and PE1 base path is set below.
        whiteoutflakes::StorageKind::Folder => {}
    }
    // HD and SD are different layers of the archive; picking the wrong one
    // resolves textures to the other art set.
    r.scene()
        .expect("live")
        .set_hd_mode(path.contains("_hd.w3mod"));

    if br.kind() == whiteoutflakes::StorageKind::Folder {
        if let Some(dir) = std::path::Path::new(&path).parent() {
            r.scene()
                .expect("live")
                .set_pe1_base_path(&dir.to_string_lossy());
        }
    }

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
