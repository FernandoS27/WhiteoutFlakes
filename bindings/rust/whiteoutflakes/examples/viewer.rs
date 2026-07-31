// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// A minimal model viewer: winit for the window, WhiteoutFlakes for
// everything inside it. Roughly what tools/basic_viewer does, minus the
// ImGui chrome — the renderer skips its ImGui pass when no context
// exists, so a Rust host gets the scene and nothing else.
//
//   cargo run --example viewer -- path/to/model.mdx
//
// Windows-only. `create_swap_chain_target` wants an HWND here; on Linux
// and macOS it wants a VkSurfaceKHR / NSWindow that the host has to
// build itself, and doing that from Rust needs an `ash` (or objc)
// dependency this example deliberately does not take.
//
// The renderer resolves BLS shaders relative to the *executable*
// directory, so `shaders/` must sit next to the example binary. See
// scripts/build-rust.ps1, which stages it.

#[cfg(not(windows))]
fn main() {
    eprintln!("examples/viewer is Windows-only — see the header comment.");
}

#[cfg(windows)]
fn main() -> Result<(), Box<dyn std::error::Error>> {
    imp::run()
}

#[cfg(windows)]
mod imp {
    use std::path::PathBuf;
    use std::time::Instant;

    use raw_window_handle::{HasWindowHandle, RawWindowHandle};
    use winit::application::ApplicationHandler;
    use winit::event::{ElementState, MouseButton, MouseScrollDelta, WindowEvent};
    use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop};
    use winit::window::{Window, WindowId};

    use whiteoutflakes::{GfxApi, Renderer};

    const INITIAL_SIZE: (u32, u32) = (1280, 720);

    pub fn run() -> Result<(), Box<dyn std::error::Error>> {
        let args: Vec<String> = std::env::args().skip(1).collect();
        // `--frames N` renders N frames, prints the last frame's stats and
        // exits. Gives the example a self-checking mode that doesn't need
        // somebody to close the window.
        let mut frame_budget = None;
        let mut positional = Vec::new();
        let mut it = args.into_iter();
        while let Some(a) = it.next() {
            match a.as_str() {
                "--frames" => {
                    frame_budget = Some(
                        it.next()
                            .ok_or("--frames needs a count")?
                            .parse::<u32>()
                            .map_err(|e| format!("--frames: {e}"))?,
                    )
                }
                _ => positional.push(a),
            }
        }
        let model = positional.into_iter().next().map(PathBuf::from);
        if let Some(p) = &model {
            if !p.is_file() {
                return Err(format!("no such model: {}", p.display()).into());
            }
        } else {
            eprintln!("no model given — rendering an empty scene.");
            eprintln!("usage: cargo run --example viewer -- path/to/model.mdx");
        }

        let event_loop = EventLoop::new()?;
        // Animation runs off the clock, so drive frames continuously
        // rather than waiting for input.
        event_loop.set_control_flow(ControlFlow::Poll);
        let mut app = App::new(model, frame_budget);
        event_loop.run_app(&mut app)?;
        app.result
    }

    /// Everything that only exists once the window does.
    struct Gpu {
        renderer: Renderer,
        target: u32,
        size: (u32, u32),
    }

    struct App {
        model: Option<PathBuf>,
        window: Option<Window>,
        gpu: Option<Gpu>,
        last_frame: Instant,
        /// Drag state: (button, last cursor position).
        drag: Option<(MouseButton, (f64, f64))>,
        cursor: (f64, f64),
        frames_left: Option<u32>,
        result: Result<(), Box<dyn std::error::Error>>,
    }

    impl App {
        fn new(model: Option<PathBuf>, frames_left: Option<u32>) -> Self {
            App {
                model,
                window: None,
                gpu: None,
                last_frame: Instant::now(),
                drag: None,
                cursor: (0.0, 0.0),
                frames_left,
                result: Ok(()),
            }
        }

        /// Bring the device up and load the model. Split out from
        /// `resumed` so the `?` on each step lands in one place.
        fn start(&mut self, window: &Window) -> Result<Gpu, Box<dyn std::error::Error>> {
            let hwnd = match window.window_handle()?.as_raw() {
                RawWindowHandle::Win32(h) => isize::from(h.hwnd) as usize,
                other => return Err(format!("unexpected window handle: {other:?}").into()),
            };

            let mut renderer = Renderer::new();
            let mut pipeline = renderer.pipeline().expect("live renderer");

            pipeline.init_device(GfxApi::D3D11);
            // InitDevice reports failure through a bool the public C++
            // facade drops on the floor, so ask separately.
            if !pipeline.is_device_ready() {
                return Err("InitDevice failed — no D3D11 device".into());
            }

            let size = window.inner_size();
            let (w, h) = (size.width.max(1), size.height.max(1));
            // SAFETY: `window` outlives the target — App drops the
            // renderer before the window, and the target is destroyed
            // with it.
            let target = unsafe { pipeline.create_swap_chain_target(hwnd, w as i32, h as i32) };
            if target == 0 {
                return Err("CreateSwapChainTarget failed".into());
            }
            pipeline.set_primary_target(target);
            drop(pipeline);

            renderer
                .settings()
                .expect("live renderer")
                .set_background_color(30, 34, 42);

            if let Some(path) = self.model.clone() {
                load_model(&mut renderer, &path)?;
            }

            let mut cam = renderer.camera().expect("live renderer");
            cam.reset();
            cam.set_target(0.0, 0.0, 60.0);
            cam.set_distance(400.0);
            drop(cam);

            Ok(Gpu {
                renderer,
                target,
                size: (w, h),
            })
        }

        fn frame(&mut self) {
            let Some(gpu) = self.gpu.as_mut() else { return };

            let now = Instant::now();
            let dt = (now - self.last_frame).as_secs_f32().min(0.1);
            self.last_frame = now;

            gpu.renderer.tick(dt);
            let mut pipeline = gpu.renderer.pipeline().expect("live renderer");
            pipeline.render_frame(gpu.target);
            pipeline.present(gpu.target);
        }

        /// `--frames` epilogue: print what the last frame actually drew.
        /// A geoset count of zero with a model loaded means the scene
        /// reached the GPU empty — the one thing worth failing on.
        fn report_and_exit(&mut self, event_loop: &ActiveEventLoop) {
            if let Some(gpu) = self.gpu.as_mut() {
                let stats = gpu
                    .renderer
                    .pipeline()
                    .expect("live renderer")
                    .frame_stats();
                println!(
                    "last frame: {} geosets, {} textures, {} nodes, {} particles",
                    stats.geosets, stats.textures, stats.nodes, stats.particles
                );
                if self.model.is_some() && stats.geosets == 0 {
                    self.result = Err("model loaded but nothing was drawn".into());
                }
            }
            event_loop.exit();
        }
    }

    /// The load order the renderer expects: base path first (it also
    /// realises the disk content provider), then spawn, then match the
    /// render mode to what the model's materials actually want.
    fn load_model(
        renderer: &mut Renderer,
        path: &std::path::Path,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let dir = path.parent().unwrap_or(std::path::Path::new("."));
        renderer
            .scene()
            .expect("live renderer")
            .set_pe1_base_path(&dir.to_string_lossy());

        let utf8 = path.to_string_lossy().into_owned();
        let handle = renderer.loader().expect("live renderer").spawn_unit(&utf8);
        if handle == 0 {
            return Err(format!("SpawnUnit failed for {}", path.display()).into());
        }

        let actor = renderer.actor(handle).expect("live renderer");
        let preferred = actor.preferred_render_mode();
        let sequences = actor.sequences();
        drop(actor);

        // SD models rendered through the HD path mis-blend their
        // multi-layer materials, and vice versa.
        renderer
            .settings()
            .expect("live renderer")
            .set_render_mode(preferred);

        // Geoset / material counts are still zero here — the template
        // resolves over the next few ticks — so report them from
        // frame_stats after rendering instead.
        println!(
            "loaded {} — {} sequences, {preferred:?}",
            path.display(),
            sequences.len()
        );
        for (i, s) in sequences.iter().enumerate() {
            println!("  [{i}] {} ({}..{}ms)", s.name(), s.start_ms(), s.end_ms());
        }
        Ok(())
    }

    impl ApplicationHandler for App {
        fn resumed(&mut self, event_loop: &ActiveEventLoop) {
            if self.window.is_some() {
                return;
            }
            let attrs = Window::default_attributes()
                .with_title("WhiteoutFlakes — Rust bindings")
                .with_inner_size(winit::dpi::LogicalSize::new(INITIAL_SIZE.0, INITIAL_SIZE.1));
            let window = match event_loop.create_window(attrs) {
                Ok(w) => w,
                Err(e) => {
                    self.result = Err(e.into());
                    event_loop.exit();
                    return;
                }
            };
            match self.start(&window) {
                Ok(gpu) => self.gpu = Some(gpu),
                Err(e) => {
                    self.result = Err(e);
                    event_loop.exit();
                    return;
                }
            }
            self.window = Some(window);
            self.last_frame = Instant::now();
        }

        fn window_event(
            &mut self,
            event_loop: &ActiveEventLoop,
            _id: WindowId,
            event: WindowEvent,
        ) {
            match event {
                WindowEvent::CloseRequested => event_loop.exit(),

                WindowEvent::Resized(size) => {
                    if let Some(gpu) = self.gpu.as_mut() {
                        let (w, h) = (size.width.max(1), size.height.max(1));
                        if (w, h) != gpu.size {
                            gpu.size = (w, h);
                            gpu.renderer
                                .pipeline()
                                .expect("live renderer")
                                .resize_primary_target(w as i32, h as i32);
                        }
                    }
                }

                WindowEvent::MouseInput { state, button, .. } => match state {
                    ElementState::Pressed => self.drag = Some((button, self.cursor)),
                    ElementState::Released => self.drag = None,
                },

                WindowEvent::CursorMoved { position, .. } => {
                    let pos = (position.x, position.y);
                    if let (Some((button, last)), Some(gpu)) = (self.drag, self.gpu.as_mut()) {
                        let dx = (pos.0 - last.0) as i32;
                        let dy = (pos.1 - last.1) as i32;
                        let mut cam = gpu.renderer.camera().expect("live renderer");
                        match button {
                            MouseButton::Right => cam.pan(dx, dy),
                            _ => cam.rotate(dx, dy),
                        }
                        self.drag = Some((button, pos));
                    }
                    self.cursor = pos;
                }

                WindowEvent::MouseWheel { delta, .. } => {
                    if let Some(gpu) = self.gpu.as_mut() {
                        // Zoom takes wheel detents, which is what the
                        // line delta already is; pixel deltas (trackpads)
                        // need scaling down to the same unit.
                        let detents = match delta {
                            MouseScrollDelta::LineDelta(_, y) => y,
                            MouseScrollDelta::PixelDelta(p) => p.y as f32 / 120.0,
                        };
                        gpu.renderer
                            .camera()
                            .expect("live renderer")
                            .zoom(detents.round() as i32);
                    }
                }

                WindowEvent::RedrawRequested => {
                    self.frame();
                    if let Some(left) = self.frames_left.as_mut() {
                        *left = left.saturating_sub(1);
                        if *left == 0 {
                            self.report_and_exit(event_loop);
                        }
                    }
                }

                _ => {}
            }
        }

        fn about_to_wait(&mut self, _event_loop: &ActiveEventLoop) {
            if let Some(w) = self.window.as_ref() {
                w.request_redraw();
            }
        }

        fn exiting(&mut self, _event_loop: &ActiveEventLoop) {
            // Tear the device down while the window is still alive: the
            // swap chain holds the HWND.
            if let Some(gpu) = self.gpu.as_mut() {
                gpu.renderer.pipeline().expect("live renderer").shutdown();
            }
            self.gpu = None;
        }
    }
}
