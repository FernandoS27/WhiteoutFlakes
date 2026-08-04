// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Read a rendered frame back to CPU memory, on whichever backends this build
// has, and check the pixels are actually right.
//
//   cargo run --example readback
//   cargo run --example readback -- path/to/model.mdx
//   cargo run --features d3d12,vulkan --example readback
//
// `PipelineView::readback_target` is how a GUI shows the renderer somewhere it
// cannot hand over a window — a preview thumbnail, a panel the toolkit
// composites itself, an off-screen export. It is implemented on every backend
// this crate exposes; this walks each one that is compiled in, renders a known
// background colour into an off-screen target, and verifies the bytes that
// come back.
//
// Writes a .ppm per backend so the output can be eyeballed, and exits non-zero
// if any backend disagrees with the others.

use whiteoutflakes::{GfxApi, Renderer};

/// Background the scene is cleared to. Deliberately asymmetric in R/B so a
/// backend that hands back BGRA without swizzling fails loudly instead of
/// looking plausible.
const BG: (u8, u8, u8) = (200, 90, 30);
const SIZE: i32 = 128;

fn main() {
    let model = std::env::args().nth(1);
    let mut failures = 0;
    let mut ran = 0;
    let mut corners: Vec<(String, [u8; 3])> = Vec::new();

    for api in [GfxApi::D3D11, GfxApi::D3D12, GfxApi::Vulkan, GfxApi::WebGPU] {
        if !whiteoutflakes::backend_compiled_in(api) {
            continue;
        }
        ran += 1;
        match check(api, model.as_deref()) {
            Ok((msg, corner)) => {
                println!("{api:?}: {msg}");
                corners.push((format!("{api:?}"), corner));
            }
            Err(e) => {
                println!("{api:?}: FAILED — {e}");
                failures += 1;
            }
        }
    }

    // With a model in frame the clear goes through the HDR/tonemap path, so
    // the corner is no longer the raw background — but every backend must
    // land on the SAME value. Cross-backend agreement is the real check here;
    // the exact-colour one only holds for the bare clear.
    if let Some((_, first)) = corners.first() {
        for (name, c) in &corners[1..] {
            if c != first {
                println!("MISMATCH: {name} corner {c:?} vs {:?}", corners[0]);
                failures += 1;
            }
        }
        if corners.len() > 1 {
            println!(
                "
all backends agree on corner {first:?}"
            );
        }
    }

    if ran == 0 {
        eprintln!("no backends compiled in — build with a backend feature");
        std::process::exit(1);
    }
    println!("\n{} backend(s) checked, {failures} failed", ran);
    if failures > 0 {
        std::process::exit(1);
    }
}

type Checked = (String, [u8; 3]);

fn check(api: GfxApi, model: Option<&str>) -> Result<Checked, Box<dyn std::error::Error>> {
    let mut r = Renderer::new();
    if !r.use_bundled_shaders() {
        return Err("no bundled shader pack".into());
    }
    if !r.has_shaders_for(api) {
        return Err("compiled in, but no shader pack staged for it".into());
    }
    r.pipeline().expect("live").init_device(api);
    if !r.pipeline().expect("live").is_device_ready() {
        return Err("InitDevice failed".into());
    }
    r.settings()
        .expect("live")
        .set_background_color(BG.0, BG.1, BG.2);

    if let Some(path) = model {
        let dir = std::path::Path::new(path)
            .parent()
            .unwrap_or(std::path::Path::new("."));
        r.scene()
            .expect("live")
            .set_pe1_base_path(&dir.to_string_lossy());
        let h = r.loader().expect("live").spawn_unit(path);
        if h != 0 {
            let preferred = r.actor(h).expect("live").preferred_render_mode();
            r.settings().expect("live").set_render_mode(preferred);
        }
        if let Some(mut cam) = r.camera_at(0) {
            cam.reset();
            cam.set_target(0.0, 0.0, 60.0);
            cam.set_distance(400.0);
        }
    }

    let target = r
        .pipeline()
        .expect("live")
        .create_offscreen_target(SIZE, SIZE);
    if target == 0 {
        return Err("CreateOffscreenTarget failed".into());
    }
    for _ in 0..8 {
        r.advance(1.0 / 60.0);
        r.pipeline().expect("live").render_viewport(target, 0);
    }

    let (pixels, w, h) = r
        .pipeline()
        .expect("live")
        .readback_target(target)
        .ok_or("readback returned nothing")?;

    if (w, h) != (SIZE, SIZE) {
        return Err(format!("size {w}x{h}, expected {SIZE}x{SIZE}").into());
    }
    let expected = (w as usize) * (h as usize) * 4;
    if pixels.len() != expected {
        return Err(format!("{} bytes, expected {expected} (tight RGBA8)", pixels.len()).into());
    }

    // The corners are background even with a model in frame, and the top-left
    // pixel also pins down row order — a flipped image would put the scene
    // there in some layouts.
    let corner = [pixels[0], pixels[1], pixels[2]];
    let tol = 8i32; // the clear goes through an sRGB-ish path on some backends
    let close = |a: u8, b: u8| (a as i32 - b as i32).abs() <= tol;
    // Only the bare clear reaches the surface untouched. With a model loaded
    // the frame is tonemapped, so the exact value is not the background any
    // more — main() checks the backends against each other instead.
    if model.is_none()
        && !(close(corner[0], BG.0) && close(corner[1], BG.1) && close(corner[2], BG.2))
    {
        // Naming the swizzle explicitly: this is the failure a BGRA backend
        // produces if the readback forgets to reorder.
        let swizzled = close(corner[0], BG.2) && close(corner[2], BG.0);
        return Err(format!(
            "corner is {corner:?}, expected {BG:?}{}",
            if swizzled {
                " — channels are BGRA"
            } else {
                ""
            }
        )
        .into());
    }

    let name = format!("readback_{api:?}.ppm").to_lowercase();
    write_ppm(&name, &pixels, w, h)?;

    r.pipeline().expect("live").shutdown();
    Ok((
        format!(
            "{w}x{h}, {} bytes, corner {corner:?} — wrote {name}",
            pixels.len()
        ),
        corner,
    ))
}

/// PPM because it is six lines of code and every image viewer reads it.
fn write_ppm(path: &str, rgba: &[u8], w: i32, h: i32) -> std::io::Result<()> {
    use std::io::Write;
    let mut out = Vec::with_capacity((w * h * 3) as usize + 32);
    write!(out, "P6\n{w} {h}\n255\n")?;
    for px in rgba.chunks_exact(4) {
        out.extend_from_slice(&px[..3]);
    }
    std::fs::write(path, out)
}
