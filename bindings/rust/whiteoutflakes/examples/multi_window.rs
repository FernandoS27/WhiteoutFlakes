// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Two windows, two swap chains, two cameras, one renderer — the question
// being whether a Rust UI can host several independent renderer viewports
// the way tools/model_explorer does with ImGui.
//
//   cargo run --example multi_window -- path/to/model.mdx
//
// What this proves and what it does not:
//
//   * Multiple swap chains from one Renderer: YES. Each window gets its
//     own target from create_swap_chain_target, and each frame renders
//     both with render_viewport(target, camera) then presents both.
//   * Independent cameras per viewport: YES. Renderer::create_camera adds
//     to the scene's camera set; camera_at drives each one separately.
//   * Independent *scenes* per viewport: NOT THROUGH THIS API. Every
//     viewport draws the same scene from a different angle. The engine
//     supports it (Viewport carries a SceneId and RenderViewport swaps the
//     active scene), but the public facade exposes no scene handles. See
//     BINDINGS.md §16 for the exact gap.
//
// Windows-only, same as examples/viewer.

#[cfg(not(windows))]
fn main() {
    eprintln!("examples/multi_window is Windows-only.");
}

#[cfg(windows)]
fn main() -> Result<(), Box<dyn std::error::Error>> {
    imp::run()
}

#[cfg(windows)]
mod imp {
    use std::collections::HashMap;
    use std::path::PathBuf;
    use std::time::Instant;

    use raw_window_handle::{HasWindowHandle, RawWindowHandle};
    use winit::application::ApplicationHandler;
    use winit::event::WindowEvent;
    use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop};
    use winit::window::{Window, WindowId};

    use whiteoutflakes::Renderer;

    /// One hosted viewport: its window, its swap chain, its camera.
    struct Pane {
        window: Window,
        target: u32,
        camera: u32,
        /// Yaw applied per second, so the two panes visibly differ.
        spin: f32,
        yaw: f32,
    }

    pub fn run() -> Result<(), Box<dyn std::error::Error>> {
        let mut frames = None;
        let mut positional = Vec::new();
        let mut it = std::env::args().skip(1);
        while let Some(a) = it.next() {
            if a == "--frames" {
                frames = Some(
                    it.next()
                        .ok_or("--frames needs a count")?
                        .parse::<u32>()
                        .map_err(|e| format!("--frames: {e}"))?,
                );
            } else {
                positional.push(a);
            }
        }
        let model = positional.into_iter().next().map(PathBuf::from);

        let event_loop = EventLoop::new()?;
        event_loop.set_control_flow(ControlFlow::Poll);
        let mut app = App {
            model,
            renderer: None,
            panes: Vec::new(),
            by_id: HashMap::new(),
            last: Instant::now(),
            frames_left: frames,
            closing: false,
            result: Ok(()),
        };
        event_loop.run_app(&mut app)?;
        app.result
    }

    struct App {
        model: Option<PathBuf>,
        renderer: Option<Renderer>,
        panes: Vec<Pane>,
        by_id: HashMap<WindowId, usize>,
        last: Instant,
        frames_left: Option<u32>,
        closing: bool,
        result: Result<(), Box<dyn std::error::Error>>,
    }

    impl App {
        fn start(
            &mut self,
            event_loop: &ActiveEventLoop,
        ) -> Result<(), Box<dyn std::error::Error>> {
            let mut renderer = Renderer::new();
            if !renderer.use_bundled_shaders() {
                return Err("no bundled shader pack".into());
            }
            let api = renderer
                .preferred_backend()
                .ok_or("no usable gfx backend in this build")?;
            renderer.pipeline().expect("live").init_device(api);
            if !renderer.pipeline().expect("live").is_device_ready() {
                return Err(format!("InitDevice failed for {api:?}").into());
            }
            println!("backend: {api:?}");

            renderer
                .settings()
                .expect("live")
                .set_background_color(24, 26, 32);

            if let Some(path) = self.model.clone() {
                let dir = path.parent().unwrap_or(std::path::Path::new("."));
                renderer
                    .scene()
                    .expect("live")
                    .set_pe1_base_path(&dir.to_string_lossy());
                let handle = renderer
                    .loader()
                    .expect("live")
                    .spawn_unit(&path.to_string_lossy());
                if handle == 0 {
                    return Err(format!("SpawnUnit failed for {}", path.display()).into());
                }
                let preferred = renderer
                    .actor(handle)
                    .expect("live")
                    .preferred_render_mode();
                renderer
                    .settings()
                    .expect("live")
                    .set_render_mode(preferred);
            }

            // Two windows, each with its own swap chain and camera. Camera
            // 0 already exists (the scene's default); the second is added
            // to the scene's camera set.
            for (i, (title, spin)) in [
                ("viewport A — orbiting", 25.0f32),
                ("viewport B — still", 0.0),
            ]
            .into_iter()
            .enumerate()
            {
                let window = event_loop.create_window(
                    Window::default_attributes()
                        .with_title(title)
                        .with_inner_size(winit::dpi::LogicalSize::new(640, 480)),
                )?;
                let hwnd = match window.window_handle()?.as_raw() {
                    RawWindowHandle::Win32(h) => isize::from(h.hwnd) as usize,
                    other => return Err(format!("unexpected handle: {other:?}").into()),
                };
                let size = window.inner_size();
                // SAFETY: the window outlives the target — panes are torn
                // down before the windows drop.
                let target = unsafe {
                    renderer.pipeline().expect("live").create_swap_chain_target(
                        hwnd,
                        size.width.max(1) as i32,
                        size.height.max(1) as i32,
                    )
                };
                if target == 0 {
                    return Err(format!("CreateSwapChainTarget failed for window {i}").into());
                }
                let camera = if i == 0 { 0 } else { renderer.create_camera() };
                if let Some(mut cam) = renderer.camera_at(camera) {
                    cam.reset();
                    cam.set_target(0.0, 0.0, 60.0);
                    cam.set_distance(if i == 0 { 400.0 } else { 700.0 });
                    cam.set_pitch(if i == 0 { 0.35 } else { 0.9 });
                }
                println!("pane {i}: target={target} camera={camera}");
                self.by_id.insert(window.id(), self.panes.len());
                self.panes.push(Pane {
                    window,
                    target,
                    camera,
                    spin,
                    yaw: 0.0,
                });
            }

            self.renderer = Some(renderer);
            self.last = Instant::now();
            Ok(())
        }

        fn frame(&mut self) {
            if self.closing {
                return;
            }
            let Some(renderer) = self.renderer.as_mut() else {
                return;
            };
            let now = Instant::now();
            let dt = (now - self.last).as_secs_f32().min(0.1);
            self.last = now;

            // One tick drives the shared scene; each viewport then renders
            // it through its own camera into its own swap chain.
            renderer.tick(dt);

            for pane in &mut self.panes {
                if pane.spin != 0.0 {
                    pane.yaw += pane.spin.to_radians() * dt;
                    if let Some(mut cam) = renderer.camera_at(pane.camera) {
                        cam.set_yaw(pane.yaw);
                    }
                }
                let mut pipeline = renderer.pipeline().expect("live");
                pipeline.render_viewport(pane.target, pane.camera);
            }
            for pane in &self.panes {
                renderer.pipeline().expect("live").present(pane.target);
            }
        }

        fn shutdown(&mut self) {
            self.closing = true;
            if let Some(renderer) = self.renderer.as_mut() {
                let mut pipeline = renderer.pipeline().expect("live");
                if pipeline.is_device_ready() {
                    pipeline.shutdown();
                }
            }
        }
    }

    impl ApplicationHandler for App {
        fn resumed(&mut self, event_loop: &ActiveEventLoop) {
            if self.renderer.is_some() {
                return;
            }
            if let Err(e) = self.start(event_loop) {
                self.result = Err(e);
                event_loop.exit();
            }
        }

        fn window_event(&mut self, event_loop: &ActiveEventLoop, id: WindowId, event: WindowEvent) {
            match event {
                // Closing any window ends the run — this is a probe, not a
                // window manager.
                WindowEvent::CloseRequested => {
                    self.shutdown();
                    event_loop.exit();
                }
                WindowEvent::Resized(size) => {
                    if self.closing {
                        return;
                    }
                    // Only the primary target can be resized through the
                    // public API — see BINDINGS.md §16. Point it at the
                    // window being resized first.
                    if let (Some(&idx), Some(renderer)) =
                        (self.by_id.get(&id), self.renderer.as_mut())
                    {
                        let target = self.panes[idx].target;
                        let mut pipeline = renderer.pipeline().expect("live");
                        pipeline.set_primary_target(target);
                        pipeline.resize_primary_target(
                            size.width.max(1) as i32,
                            size.height.max(1) as i32,
                        );
                    }
                }
                WindowEvent::RedrawRequested => {
                    self.frame();
                    if let Some(left) = self.frames_left.as_mut() {
                        *left = left.saturating_sub(1);
                        if *left == 0 {
                            if let Some(renderer) = self.renderer.as_mut() {
                                let stats = renderer
                                    .pipeline()
                                    .expect("live")
                                    .frame_stats()
                                    .expect("live");
                                println!(
                                    "last frame: {} geosets, {} textures across {} viewports",
                                    stats.geosets(),
                                    stats.textures(),
                                    self.panes.len()
                                );
                            }
                            self.shutdown();
                            event_loop.exit();
                        }
                    }
                }
                _ => {}
            }
        }

        fn about_to_wait(&mut self, _event_loop: &ActiveEventLoop) {
            for pane in &self.panes {
                pane.window.request_redraw();
            }
        }

        fn exiting(&mut self, _event_loop: &ActiveEventLoop) {
            self.shutdown();
        }
    }
}
