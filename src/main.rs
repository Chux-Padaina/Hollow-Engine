#![allow(deprecated, unsafe_op_in_unsafe_fn)]

use std::ffi::CString;
use std::num::NonZeroU32;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Instant;

use glam::{Mat4, Quat, Vec2, Vec3, Vec4};
use glow::HasContext;
use glutin::config::{ConfigTemplateBuilder, GlConfig};
use glutin::context::{ContextApi, ContextAttributesBuilder};
use glutin::display::{GetGlDisplay, GlDisplay};
use glutin::prelude::*;
use glutin::surface::{GlSurface, SurfaceAttributesBuilder, WindowSurface};
use glutin_winit::DisplayBuilder;
use raw_window_handle::HasWindowHandle;
use winit::dpi::{PhysicalPosition, PhysicalSize};
use winit::event::{ElementState, Event, MouseButton, MouseScrollDelta, WindowEvent};
use winit::event_loop::{ControlFlow, EventLoop};
use winit::keyboard::{KeyCode, PhysicalKey};
use winit::window::{Window, WindowAttributes};

const WINDOW_TITLE: &str = "ZZZ Style Engine Prototype - drop .glb/.gltf/.obj, left-drag to move";
const MAX_POINT_LIGHTS: usize = 8;
const GROUND_CLEARANCE: f32 = 0.035;

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
    }
}

fn run() -> Result<(), String> {
    let event_loop = EventLoop::new().map_err(|error| format!("event loop: {error}"))?;
    let (window, gl_config) = create_window(&event_loop)?;
    let raw_window_handle = window
        .window_handle()
        .map_err(|error| format!("window handle: {error}"))?
        .as_raw();

    let gl_display = gl_config.display();
    let context_attributes = ContextAttributesBuilder::new()
        .with_context_api(ContextApi::OpenGl(None))
        .build(Some(raw_window_handle));
    let not_current_context = unsafe {
        gl_display
            .create_context(&gl_config, &context_attributes)
            .map_err(|error| format!("create OpenGL context: {error}"))?
    };

    let size = window.inner_size();
    let attrs = SurfaceAttributesBuilder::<WindowSurface>::new().build(
        raw_window_handle,
        non_zero(size.width),
        non_zero(size.height),
    );
    let surface = unsafe {
        gl_display
            .create_window_surface(&gl_config, &attrs)
            .map_err(|error| format!("create window surface: {error}"))?
    };
    let gl_context = not_current_context
        .make_current(&surface)
        .map_err(|error| format!("make OpenGL context current: {error}"))?;

    let gl = unsafe {
        Arc::new(glow::Context::from_loader_function(|symbol| {
            let symbol = CString::new(symbol).expect("GL symbol has interior nul");
            gl_display.get_proc_address(&symbol).cast()
        }))
    };

    let mut app = unsafe { App::new(gl.clone(), size)? };
    app.try_import_arg();
    let mut editor = unsafe { EditorLayer::new(gl.clone(), &window)? };

    event_loop.set_control_flow(ControlFlow::Poll);
    event_loop
        .run(move |event, target| match event {
            Event::WindowEvent { event, .. } => match event {
                WindowEvent::CloseRequested => target.exit(),
                WindowEvent::Resized(size) => {
                    surface.resize(&gl_context, non_zero(size.width), non_zero(size.height));
                    app.resize(size);
                }
                WindowEvent::RedrawRequested => {
                    app.update_title(&window);
                    unsafe {
                        app.render();
                    }
                    editor.run(&window, &mut app);
                    unsafe {
                        editor.paint(&window);
                    }
                    if let Err(error) = surface.swap_buffers(&gl_context) {
                        eprintln!("swap buffers: {error}");
                        target.exit();
                    }
                }
                _ => {
                    let egui_response = editor.on_window_event(&window, &event);
                    if let WindowEvent::DroppedFile(path) = &event {
                        app.import_model(path.clone());
                    }
                    if !egui_response.consumed {
                        match event {
                            WindowEvent::CursorMoved { position, .. } => app.cursor_moved(position),
                            WindowEvent::MouseInput { state, button, .. } => {
                                app.mouse_button(button, state == ElementState::Pressed)
                            }
                            WindowEvent::MouseWheel { delta, .. } => app.mouse_wheel(delta),
                            WindowEvent::KeyboardInput { event, .. } => {
                                if event.state == ElementState::Pressed {
                                    match event.physical_key {
                                        PhysicalKey::Code(KeyCode::Escape) => target.exit(),
                                        PhysicalKey::Code(KeyCode::Digit1) => {
                                            app.style = StylePreset::Character
                                        }
                                        PhysicalKey::Code(KeyCode::Digit2) => {
                                            app.style = StylePreset::Street
                                        }
                                        PhysicalKey::Code(KeyCode::Digit3) => {
                                            app.style = StylePreset::Combat
                                        }
                                        PhysicalKey::Code(KeyCode::F1) => {
                                            app.debug_view = DebugView::Final
                                        }
                                        PhysicalKey::Code(KeyCode::F2) => {
                                            app.debug_view = DebugView::Albedo
                                        }
                                        PhysicalKey::Code(KeyCode::F3) => {
                                            app.debug_view = DebugView::Normals
                                        }
                                        PhysicalKey::Code(KeyCode::F4) => {
                                            app.debug_view = DebugView::LightingOnly
                                        }
                                        PhysicalKey::Code(KeyCode::F5) => {
                                            app.debug_view = DebugView::Roughness
                                        }
                                        PhysicalKey::Code(KeyCode::F6) => {
                                            app.debug_view = DebugView::Metallic
                                        }
                                        PhysicalKey::Code(KeyCode::KeyQ) => {
                                            app.editor_tool = EditorTool::Select
                                        }
                                        PhysicalKey::Code(KeyCode::KeyW) => {
                                            app.editor_tool = EditorTool::Move
                                        }
                                        PhysicalKey::Code(KeyCode::KeyE) => {
                                            app.editor_tool = EditorTool::Rotate
                                        }
                                        PhysicalKey::Code(KeyCode::KeyR) => {
                                            app.editor_tool = EditorTool::Scale
                                        }
                                        PhysicalKey::Code(KeyCode::Home) => app.reset_scene(),
                                        _ => {}
                                    }
                                }
                            }
                            _ => {}
                        }
                    }
                }
            },
            Event::AboutToWait => window.request_redraw(),
            _ => {}
        })
        .map_err(|error| format!("run loop: {error}"))
}

fn create_window(event_loop: &EventLoop<()>) -> Result<(Window, glutin::config::Config), String> {
    let template = ConfigTemplateBuilder::new()
        .with_depth_size(24)
        .with_stencil_size(8)
        .with_alpha_size(8);
    let attributes = WindowAttributes::default()
        .with_title(WINDOW_TITLE)
        .with_inner_size(PhysicalSize::new(1280, 720));
    let display_builder = DisplayBuilder::new().with_window_attributes(Some(attributes));
    let (window, gl_config) = display_builder
        .build(event_loop, template, |configs| {
            configs
                .max_by_key(|config| config.num_samples())
                .expect("at least one OpenGL config")
        })
        .map_err(|error| format!("build window: {error}"))?;
    let window = window.ok_or_else(|| "window was not created".to_string())?;
    Ok((window, gl_config))
}

fn non_zero(value: u32) -> NonZeroU32 {
    NonZeroU32::new(value.max(1)).expect("max(1) is non-zero")
}

struct App {
    gl: Arc<glow::Context>,
    renderer: Renderer,
    scene: Scene,
    viewport: PhysicalSize<u32>,
    cursor: Vec2,
    dragging: bool,
    orbiting_camera: bool,
    panning_camera: bool,
    last_cursor: Vec2,
    drag_offset: Vec3,
    active_gizmo: Option<GizmoHandle>,
    hovered_gizmo: Option<GizmoHandle>,
    drag_start_cursor: Vec2,
    drag_start_transform: Option<Transform>,
    rotate_drag_last_angle: Option<f32>,
    rotate_drag_amount: f32,
    start_time: Instant,
    style: StylePreset,
    debug_view: DebugView,
    editor_tool: EditorTool,
    transform_space: TransformSpace,
    camera: EditorCamera,
    status: String,
}

struct EditorLayer {
    egui_ctx: egui::Context,
    egui_state: egui_winit::State,
    painter: egui_glow::Painter,
    clipped_primitives: Vec<egui::ClippedPrimitive>,
    textures_delta: egui::TexturesDelta,
    pixels_per_point: f32,
}

impl EditorLayer {
    unsafe fn new(gl: Arc<glow::Context>, window: &Window) -> Result<Self, String> {
        let egui_ctx = egui::Context::default();
        let painter = egui_glow::Painter::new(gl, "", None, true)
            .map_err(|error| format!("egui painter: {error}"))?;
        let egui_state = egui_winit::State::new(
            egui_ctx.clone(),
            egui::ViewportId::ROOT,
            window,
            None,
            window.theme(),
            Some(painter.max_texture_side()),
        );
        Ok(Self {
            egui_ctx,
            egui_state,
            painter,
            clipped_primitives: Vec::new(),
            textures_delta: egui::TexturesDelta::default(),
            pixels_per_point: 1.0,
        })
    }

    fn on_window_event(
        &mut self,
        window: &Window,
        event: &WindowEvent,
    ) -> egui_winit::EventResponse {
        self.egui_state.on_window_event(window, event)
    }

    fn run(&mut self, window: &Window, app: &mut App) {
        let raw_input = self.egui_state.take_egui_input(window);
        let output = self.egui_ctx.run(raw_input, |ctx| {
            editor_ui(ctx, app);
        });
        self.egui_state
            .handle_platform_output(window, output.platform_output);
        self.pixels_per_point = output.pixels_per_point;
        self.clipped_primitives = self
            .egui_ctx
            .tessellate(output.shapes, output.pixels_per_point);
        self.textures_delta.append(output.textures_delta);
    }

    unsafe fn paint(&mut self, window: &Window) {
        let size = window.inner_size();
        let screen_size = [size.width, size.height];
        self.painter.paint_and_update_textures(
            screen_size,
            self.pixels_per_point,
            &self.clipped_primitives,
            &self.textures_delta,
        );
        self.clipped_primitives.clear();
        self.textures_delta = egui::TexturesDelta::default();
    }
}

fn editor_ui(ctx: &egui::Context, app: &mut App) {
    egui::TopBottomPanel::top("toolbar").show(ctx, |ui| {
        ui.horizontal(|ui| {
            ui.label("Tool");
            tool_button(ui, app, EditorTool::Select, "Q Select");
            tool_button(ui, app, EditorTool::Move, "W Move");
            tool_button(ui, app, EditorTool::Rotate, "E Rotate");
            tool_button(ui, app, EditorTool::Scale, "R Scale");
            ui.separator();
            if ui.button(app.transform_space.name()).clicked() {
                app.transform_space = app.transform_space.toggle();
            }
            ui.separator();
            ui.label(format!("View: {}", app.debug_view.name()));
            ui.label(format!("Style: {}", app.style.name()));
        });
    });

    egui::SidePanel::left("outliner")
        .default_width(260.0)
        .show(ctx, |ui| {
            ui.heading("Outliner");
            ui.separator();
            ui.collapsing("Objects", |ui| {
                let mut groups = Vec::<(u32, String, usize)>::new();
                for object in &app.scene.objects {
                    if let Some((_, _, count)) =
                        groups.iter_mut().find(|(id, _, _)| *id == object.group_id)
                    {
                        *count += 1;
                    } else {
                        groups.push((object.group_id, object.name.clone(), 1));
                    }
                }
                for (group_id, name, count) in groups {
                    let selected =
                        app.scene.selected_light.is_none() && app.scene.selected_group == group_id;
                    let label = if count > 1 {
                        format!("{name} ({count} meshes)")
                    } else {
                        name
                    };
                    if ui.selectable_label(selected, label).clicked() {
                        app.scene.select_group(group_id);
                    }
                }
            });
            ui.collapsing("Lights", |ui| {
                for index in 0..app.scene.lights.len() {
                    let light = &app.scene.lights[index];
                    let selected = app.scene.selected_light == Some(index);
                    if ui
                        .selectable_label(
                            selected,
                            format!("{} [{}]", light.name, light.kind.name()),
                        )
                        .clicked()
                    {
                        app.scene.select_light(index);
                    }
                }
            });
        });

    egui::SidePanel::right("details")
        .default_width(320.0)
        .show(ctx, |ui| {
            ui.heading("Details");
            ui.separator();
            if let Some(light_index) = app.scene.selected_light {
                light_details(ui, &mut app.scene.lights[light_index]);
            } else {
                object_details(ui, &mut app.scene);
            }
            ui.separator();
            camera_details(ui, &mut app.camera);
        });
}

fn tool_button(ui: &mut egui::Ui, app: &mut App, tool: EditorTool, label: &str) {
    if ui
        .selectable_label(app.editor_tool == tool, label)
        .on_hover_text(tool.name())
        .clicked()
    {
        app.editor_tool = tool;
    }
}

fn object_details(ui: &mut egui::Ui, scene: &mut Scene) {
    ui.label(format!("Selected group: {}", scene.selected_group));
    let count = scene
        .objects
        .iter()
        .filter(|object| object.group_id == scene.selected_group)
        .count();
    ui.label(format!("Meshes: {count}"));
    if let Some(transform) = scene
        .selected_group()
        .first()
        .map(|object| object.transform)
    {
        let mut transform = transform;
        let mut changed = false;
        ui.separator();
        ui.label("Transform");
        changed |= vec3_drag(ui, "Position", &mut transform.translation, 0.05);
        let mut rotation_degrees = Vec3::new(
            transform.rotation.x.to_degrees(),
            transform.rotation.y.to_degrees(),
            transform.rotation.z.to_degrees(),
        );
        changed |= vec3_drag(ui, "Rotation", &mut rotation_degrees, 0.4);
        transform.rotation = Vec3::new(
            rotation_degrees.x.to_radians(),
            rotation_degrees.y.to_radians(),
            rotation_degrees.z.to_radians(),
        );
        changed |= vec3_drag(ui, "Scale", &mut transform.scale, 0.02);
        if changed {
            for object in scene.selected_group_mut() {
                object.transform = transform;
            }
        }
    }
    if let Some(material) = scene
        .selected_group()
        .first()
        .map(|object| object.mesh.material)
    {
        let mut material = material;
        let mut changed = false;
        ui.separator();
        ui.label("Material");
        changed |= ui
            .add(egui::Slider::new(&mut material.metallic, 0.0..=1.0).text("Metallic"))
            .changed();
        changed |= ui
            .add(egui::Slider::new(&mut material.roughness, 0.04..=1.0).text("Roughness"))
            .changed();
        ui.label("Applies to all meshes in the selected group.");
        if changed {
            for object in scene.selected_group_mut() {
                object.mesh.material = material;
            }
        }
    }
}

fn light_details(ui: &mut egui::Ui, light: &mut Light) {
    ui.label(format!("Light: {}", light.name));
    ui.label(format!("Type: {}", light.kind.name()));
    ui.checkbox(&mut light.enabled, "Enabled");
    let mut color = [light.color.x, light.color.y, light.color.z];
    if ui.color_edit_button_rgb(&mut color).changed() {
        light.color = Vec3::new(color[0], color[1], color[2]);
    }
    ui.add(egui::Slider::new(&mut light.intensity, 0.0..=4.0).text("Intensity"));
    if !matches!(light.kind, LightKind::Directional) {
        vec3_drag(ui, "Position", &mut light.position, 0.05);
        ui.add(egui::Slider::new(&mut light.range, 0.1..=20.0).text("Range"));
    }
    if matches!(light.kind, LightKind::Directional | LightKind::Spot) {
        vec3_drag(ui, "Direction", &mut light.direction, 0.02);
        light.direction = light.direction.normalize_or_zero();
    }
    if matches!(light.kind, LightKind::Spot) {
        let mut angle = light.spot_angle_radians.to_degrees();
        if ui
            .add(egui::Slider::new(&mut angle, 5.0..=90.0).text("Spot angle"))
            .changed()
        {
            light.spot_angle_radians = angle.to_radians();
        }
    }
}

fn camera_details(ui: &mut egui::Ui, camera: &mut EditorCamera) {
    ui.label("Camera");
    ui.label("RMB orbit/look, MMB pan, wheel zoom");
    vec3_drag(ui, "Target", &mut camera.target, 0.05);
    ui.add(egui::Slider::new(&mut camera.distance, 1.2..=30.0).text("Distance"));
}

fn vec3_drag(ui: &mut egui::Ui, label: &str, value: &mut Vec3, speed: f64) -> bool {
    let mut changed = false;
    ui.label(label);
    ui.horizontal(|ui| {
        changed |= ui
            .add(egui::DragValue::new(&mut value.x).speed(speed).prefix("X "))
            .changed();
        changed |= ui
            .add(egui::DragValue::new(&mut value.y).speed(speed).prefix("Y "))
            .changed();
        changed |= ui
            .add(egui::DragValue::new(&mut value.z).speed(speed).prefix("Z "))
            .changed();
    });
    changed
}

impl App {
    unsafe fn new(gl: Arc<glow::Context>, viewport: PhysicalSize<u32>) -> Result<Self, String> {
        gl.enable(glow::DEPTH_TEST);
        gl.enable(glow::CULL_FACE);
        gl.clear_color(0.075, 0.085, 0.08, 1.0);
        let renderer = Renderer::new(gl.clone())?;
        let mut scene = Scene::default();
        scene.next_group_id = 1;
        scene.lights = default_editor_lights();
        scene.objects.push(SceneObject {
            name: "Cube A".to_string(),
            mesh: MeshGpu::from_cpu(
                gl.clone(),
                &MeshCpu::cube_with_material(
                    "Cube A",
                    [0.82, 0.66, 0.42, 1.0],
                    MaterialProperties {
                        metallic: 0.0,
                        roughness: 0.72,
                    },
                ),
            )?,
            group_id: 0,
            transform: Transform {
                translation: Vec3::new(-1.2, 0.5 + GROUND_CLEARANCE, 0.0),
                ..Transform::default()
            },
        });
        scene.objects.push(SceneObject {
            name: "Cube B".to_string(),
            mesh: MeshGpu::from_cpu(
                gl.clone(),
                &MeshCpu::cube_with_material(
                    "Cube B",
                    [0.42, 0.58, 0.76, 1.0],
                    MaterialProperties {
                        metallic: 1.0,
                        roughness: 0.28,
                    },
                ),
            )?,
            group_id: 1,
            transform: Transform {
                translation: Vec3::new(1.2, 0.5 + GROUND_CLEARANCE, 0.0),
                scale: Vec3::splat(0.8),
                ..Transform::default()
            },
        });
        scene.objects.push(SceneObject {
            name: "Floor".to_string(),
            mesh: MeshGpu::from_cpu(
                gl.clone(),
                &MeshCpu::cube_with_material(
                    "Floor",
                    [0.34, 0.35, 0.33, 1.0],
                    MaterialProperties {
                        metallic: 0.0,
                        roughness: 0.88,
                    },
                ),
            )?,
            group_id: 2,
            transform: Transform {
                translation: Vec3::new(0.0, -0.04, 0.0),
                scale: Vec3::new(8.0, 0.08, 8.0),
                ..Transform::default()
            },
        });
        scene.next_group_id = 3;
        Ok(Self {
            gl,
            renderer,
            scene,
            viewport,
            cursor: Vec2::ZERO,
            dragging: false,
            orbiting_camera: false,
            panning_camera: false,
            last_cursor: Vec2::ZERO,
            drag_offset: Vec3::ZERO,
            active_gizmo: None,
            hovered_gizmo: None,
            drag_start_cursor: Vec2::ZERO,
            drag_start_transform: None,
            rotate_drag_last_angle: None,
            rotate_drag_amount: 0.0,
            start_time: Instant::now(),
            style: StylePreset::Street,
            debug_view: DebugView::Final,
            editor_tool: EditorTool::Move,
            transform_space: TransformSpace::Global,
            camera: EditorCamera::default(),
            status: "Drop a Sketchfab-style .glb/.gltf/.obj to import".to_string(),
        })
    }

    fn try_import_arg(&mut self) {
        if let Some(path) = std::env::args_os().nth(1).map(PathBuf::from) {
            self.import_model(path);
        }
    }

    fn import_model(&mut self, path: PathBuf) {
        match ImportedModel::load(&path) {
            Ok(model) => {
                let mut imported = 0;
                let group_id = self.scene.next_group_id;
                self.scene.next_group_id += 1;
                for mesh in model.meshes {
                    match unsafe { MeshGpu::from_cpu(self.gl.clone(), &mesh) } {
                        Ok(gpu_mesh) => {
                            self.scene.objects.push(SceneObject {
                                name: mesh.name,
                                mesh: gpu_mesh,
                                group_id,
                                transform: Transform {
                                    translation: Vec3::new(0.0, GROUND_CLEARANCE, 0.0),
                                    rotation: Vec3::ZERO,
                                    scale: Vec3::ONE,
                                },
                            });
                            imported += 1;
                        }
                        Err(error) => self.status = format!("GPU upload failed: {error}"),
                    }
                }
                if imported > 0 {
                    self.scene.selected = self.scene.objects.len().saturating_sub(1);
                    self.scene.selected_group = group_id;
                    self.scene.selected_light = None;
                    self.status = format!("Imported {imported} mesh(es) from {}", path.display());
                }
            }
            Err(error) => self.status = format!("Import failed: {error}"),
        }
    }

    fn resize(&mut self, viewport: PhysicalSize<u32>) {
        self.viewport = viewport;
    }

    fn cursor_moved(&mut self, position: PhysicalPosition<f64>) {
        let previous = self.cursor;
        self.cursor = Vec2::new(position.x as f32, position.y as f32);
        let delta = self.cursor - previous;
        self.last_cursor = previous;
        if self.orbiting_camera {
            self.camera.orbit(delta);
            return;
        }
        if self.panning_camera {
            self.camera.pan(delta);
            return;
        }
        if !self.dragging {
            self.hovered_gizmo = self.cursor_ray().and_then(|ray| self.hit_test_gizmo(ray));
        }
        if self.dragging {
            self.drag_active_gizmo(delta);
        }
    }

    fn mouse_button(&mut self, button: MouseButton, pressed: bool) {
        match button {
            MouseButton::Left => {
                self.dragging = pressed;
                if pressed {
                    self.begin_tool_drag();
                } else {
                    self.active_gizmo = None;
                    self.drag_start_transform = None;
                    self.rotate_drag_last_angle = None;
                    self.rotate_drag_amount = 0.0;
                    self.hovered_gizmo = self.cursor_ray().and_then(|ray| self.hit_test_gizmo(ray));
                }
            }
            MouseButton::Right => self.orbiting_camera = pressed,
            MouseButton::Middle => self.panning_camera = pressed,
            _ => {}
        }
    }

    fn begin_tool_drag(&mut self) {
        self.drag_start_cursor = self.cursor;
        self.drag_offset = Vec3::ZERO;
        self.drag_start_transform = self
            .scene
            .selected_group()
            .first()
            .map(|object| object.transform);
        let ray = self.cursor_ray();
        if let Some(ray) = ray {
            if let Some(handle) = self.hit_test_gizmo(ray) {
                self.active_gizmo = Some(handle);
                if matches!(self.editor_tool, EditorTool::Select) {
                    self.editor_tool = EditorTool::Move;
                }
            } else if let Some(group_id) = self.pick_object(ray) {
                self.scene.select_group(group_id);
                self.drag_start_transform = self
                    .scene
                    .selected_group()
                    .first()
                    .map(|object| object.transform);
                self.active_gizmo = Some(GizmoHandle::Center);
            } else {
                self.active_gizmo = None;
            }
        }

        if matches!(self.editor_tool, EditorTool::Rotate) && self.active_gizmo.is_some() {
            self.rotate_drag_last_angle = self.cursor_angle_around_selected();
            self.rotate_drag_amount = 0.0;
        }

        if matches!(self.editor_tool, EditorTool::Select | EditorTool::Move) {
            if let Some(object) = self.scene.selected_group().first() {
                let origin = object.transform.translation;
                let hit_point = match self.active_gizmo {
                    Some(GizmoHandle::Plane(plane)) => self.cursor_ray().and_then(|ray| {
                        let (a, b) = self.gizmo_plane_axes(plane, object.transform);
                        ray.intersect_plane(origin, a.cross(b).normalize_or_zero())
                    }),
                    Some(GizmoHandle::Axis(Axis::Y)) => None,
                    Some(GizmoHandle::Ring(_)) => None,
                    _ => self.cursor_ground_point(),
                };
                if let Some(point) = hit_point {
                    self.drag_offset = origin - point;
                }
            }
        }
    }

    fn drag_active_gizmo(&mut self, _delta: Vec2) {
        let Some(handle) = self.active_gizmo else {
            return;
        };
        let start_transform = self
            .drag_start_transform
            .or_else(|| {
                self.scene
                    .selected_group()
                    .first()
                    .map(|object| object.transform)
            })
            .unwrap_or_default();
        let mut transform = start_transform;
        let total_delta = self.cursor - self.drag_start_cursor;

        match self.editor_tool {
            EditorTool::Select | EditorTool::Move => match handle {
                GizmoHandle::Center => {
                    if let Some(point) = self.cursor_ground_point() {
                        transform.translation = point + self.drag_offset;
                    }
                }
                GizmoHandle::Axis(axis) => {
                    let axis_vec = self.gizmo_axis_vector(axis, start_transform);
                    let amount = self.screen_axis_drag_amount(
                        start_transform.translation,
                        axis_vec,
                        total_delta,
                    );
                    transform.translation = start_transform.translation + axis_vec * amount;
                    if matches!(self.transform_space, TransformSpace::Global) && axis == Axis::Y {
                        transform.translation.y = transform.translation.y.max(GROUND_CLEARANCE);
                    }
                }
                GizmoHandle::Plane(plane) => {
                    let (a, b) = self.gizmo_plane_axes(plane, start_transform);
                    let normal = a.cross(b).normalize_or_zero();
                    if let Some(point) = self
                        .cursor_ray()
                        .and_then(|ray| ray.intersect_plane(start_transform.translation, normal))
                    {
                        let target = point + self.drag_offset;
                        let delta = target - start_transform.translation;
                        transform.translation =
                            start_transform.translation + a * delta.dot(a) + b * delta.dot(b);
                    }
                }
                GizmoHandle::Ring(_) => {}
            },
            EditorTool::Rotate => {
                if matches!(handle, GizmoHandle::Center) {
                    transform.rotation = start_transform.rotation
                        + Vec3::new(total_delta.y * 0.01, total_delta.x * 0.01, 0.0);
                    for object in self.scene.selected_group_mut() {
                        object.transform = transform;
                    }
                    return;
                }
                let axis = match handle {
                    GizmoHandle::Ring(axis) | GizmoHandle::Axis(axis) => axis,
                    GizmoHandle::Plane(plane) => plane.normal(),
                    GizmoHandle::Center => Axis::Y,
                };
                let amount = if let Some(angle) = self.cursor_angle_around_selected() {
                    let previous = self.rotate_drag_last_angle.unwrap_or(angle);
                    let delta_angle = wrap_angle(angle - previous);
                    self.rotate_drag_last_angle = Some(angle);
                    self.rotate_drag_amount += delta_angle
                        * self.rotate_axis_screen_sign(axis, start_transform.translation);
                    self.rotate_drag_amount
                } else {
                    (total_delta.x - total_delta.y) * 0.012
                };
                let axis_vec = self.gizmo_axis_vector(axis, start_transform);
                let start_rotation = start_transform.rotation_quat();
                let delta_rotation = Quat::from_axis_angle(axis_vec.normalize_or_zero(), amount);
                transform.set_rotation_quat(delta_rotation * start_rotation);
            }
            EditorTool::Scale => {
                let factor =
                    (1.0 + total_delta.x * 0.006 - total_delta.y * 0.004).clamp(0.05, 20.0);
                match handle {
                    GizmoHandle::Center => transform.scale = start_transform.scale * factor,
                    GizmoHandle::Axis(axis) => {
                        transform.scale = start_transform.scale;
                        axis.set_component(
                            &mut transform.scale,
                            (axis.component(start_transform.scale) * factor).max(0.02),
                        );
                    }
                    GizmoHandle::Plane(plane) => {
                        transform.scale = start_transform.scale;
                        let (a, b) = plane.axes();
                        a.set_component(
                            &mut transform.scale,
                            (a.component(start_transform.scale) * factor).max(0.02),
                        );
                        b.set_component(
                            &mut transform.scale,
                            (b.component(start_transform.scale) * factor).max(0.02),
                        );
                    }
                    GizmoHandle::Ring(_) => {}
                }
                transform.scale = transform.scale.clamp(Vec3::splat(0.02), Vec3::splat(50.0));
            }
        }

        for object in self.scene.selected_group_mut() {
            object.transform = transform;
        }
    }

    fn mouse_wheel(&mut self, delta: MouseScrollDelta) {
        let amount = match delta {
            MouseScrollDelta::LineDelta(_, y) => y,
            MouseScrollDelta::PixelDelta(position) => position.y as f32 * 0.04,
        };
        self.camera.zoom(amount);
    }

    fn reset_scene(&mut self) {
        self.scene.selected = 0;
        self.scene.selected_group = 0;
        self.scene.selected_light = None;
        for (index, object) in self.scene.objects.iter_mut().enumerate() {
            object.transform.translation = if object.group_id == 0 {
                Vec3::new(-1.2, 0.5 + GROUND_CLEARANCE, 0.0)
            } else if object.group_id == 1 {
                Vec3::new(1.2, 0.5 + GROUND_CLEARANCE, 0.0)
            } else if object.is_floor() {
                Vec3::new(0.0, -0.04, 0.0)
            } else {
                Vec3::new(index as f32 * 0.02, GROUND_CLEARANCE, 0.0)
            };
            object.transform.rotation = Vec3::ZERO;
        }
        self.status = "Scene reset".to_string();
    }

    fn camera(&self) -> Camera {
        self.camera.to_camera()
    }

    fn cursor_ground_point(&self) -> Option<Vec3> {
        let ray = self.cursor_ray()?;
        ray.intersect_y_plane(0.0)
    }

    fn cursor_ray(&self) -> Option<Ray> {
        let width = self.viewport.width.max(1) as f32;
        let height = self.viewport.height.max(1) as f32;
        let ndc = Vec2::new(
            (self.cursor.x / width) * 2.0 - 1.0,
            1.0 - (self.cursor.y / height) * 2.0,
        );
        let camera = self.camera();
        let view = camera.view();
        let projection = camera.projection(width / height);
        let inv = (projection * view).inverse();
        let near = inv * Vec4::new(ndc.x, ndc.y, -1.0, 1.0);
        let far = inv * Vec4::new(ndc.x, ndc.y, 1.0, 1.0);
        let ray_origin = near.truncate() / near.w;
        let ray_far = far.truncate() / far.w;
        let ray_dir = (ray_far - ray_origin).normalize();
        Some(Ray {
            origin: ray_origin,
            direction: ray_dir,
        })
    }

    fn cursor_angle_around_selected(&self) -> Option<f32> {
        let origin = self.scene.selected_group().first()?.transform.translation;
        let screen_origin = self.project_world_to_screen(origin)?;
        let offset = self.cursor - screen_origin;
        (offset.length_squared() > 16.0).then_some(offset.y.atan2(offset.x))
    }

    fn project_world_to_screen(&self, world: Vec3) -> Option<Vec2> {
        let width = self.viewport.width.max(1) as f32;
        let height = self.viewport.height.max(1) as f32;
        let camera = self.camera();
        let clip = camera.projection(width / height) * camera.view() * world.extend(1.0);
        if clip.w.abs() < 0.0001 {
            return None;
        }
        let ndc = clip.truncate() / clip.w;
        Some(Vec2::new(
            (ndc.x + 1.0) * 0.5 * width,
            (1.0 - ndc.y) * 0.5 * height,
        ))
    }

    fn rotate_axis_screen_sign(&self, axis: Axis, origin: Vec3) -> f32 {
        let camera = self.camera();
        let view_to_object = (origin - camera.eye).normalize_or_zero();
        let axis_vec = self.gizmo_axis_vector(axis, self.drag_start_transform.unwrap_or_default());
        if axis_vec.dot(view_to_object) >= 0.0 {
            1.0
        } else {
            -1.0
        }
    }

    fn gizmo_axis_vector(&self, axis: Axis, transform: Transform) -> Vec3 {
        match self.transform_space {
            TransformSpace::Global => axis.vector(),
            TransformSpace::Local => transform
                .rotation_matrix()
                .transform_vector3(axis.vector())
                .normalize_or_zero(),
        }
    }

    fn gizmo_plane_axes(&self, plane: AxisPlane, transform: Transform) -> (Vec3, Vec3) {
        let (a, b) = plane.axes();
        (
            self.gizmo_axis_vector(a, transform),
            self.gizmo_axis_vector(b, transform),
        )
    }

    fn screen_axis_drag_amount(&self, origin: Vec3, axis_vec: Vec3, total_delta: Vec2) -> f32 {
        let Some(start) = self.project_world_to_screen(origin) else {
            return 0.0;
        };
        let Some(end) = self.project_world_to_screen(origin + axis_vec.normalize_or_zero()) else {
            return 0.0;
        };
        let screen_axis = (end - start).normalize_or_zero();
        if screen_axis.length_squared() < 0.001 {
            0.0
        } else {
            total_delta.dot(screen_axis) * self.camera.distance * 0.0022
        }
    }

    fn pick_object(&self, ray: Ray) -> Option<u32> {
        self.scene
            .objects
            .iter()
            .filter(|object| !object.is_floor())
            .filter_map(|object| {
                let scale = object.transform.scale.max_element().max(0.001);
                let radius = object.mesh.bounds_radius * scale;
                ray.intersect_sphere(object.transform.translation, radius)
                    .map(|distance| (distance, object.group_id))
            })
            .min_by(|a, b| a.0.total_cmp(&b.0))
            .map(|(_, group_id)| group_id)
    }

    fn hit_test_gizmo(&self, ray: Ray) -> Option<GizmoHandle> {
        let transform = self.scene.selected_group().first()?.transform;
        let origin = transform.translation;
        if ray.intersect_sphere(origin, 0.22).is_some() {
            return Some(GizmoHandle::Center);
        }
        match self.editor_tool {
            EditorTool::Select => None,
            EditorTool::Move | EditorTool::Scale => {
                let mut best = None;
                for plane in [AxisPlane::Xy, AxisPlane::Xz, AxisPlane::Yz] {
                    let (a, b) = self.gizmo_plane_axes(plane, transform);
                    if hit_plane_handle(ray, origin, a, b) {
                        return Some(GizmoHandle::Plane(plane));
                    }
                }
                for axis in [Axis::X, Axis::Y, Axis::Z] {
                    let axis_vec = self.gizmo_axis_vector(axis, transform);
                    let start = origin + axis_vec * 0.16;
                    let end = origin + axis_vec * 1.18;
                    let distance = ray.distance_to_segment(start, end);
                    if distance < 0.16 {
                        let score = distance;
                        if best.is_none_or(|(best_score, _)| score < best_score) {
                            best = Some((score, GizmoHandle::Axis(axis)));
                        }
                    }
                    if matches!(self.editor_tool, EditorTool::Scale)
                        && ray
                            .intersect_sphere(origin + axis_vec * 1.05, 0.2)
                            .is_some()
                    {
                        return Some(GizmoHandle::Axis(axis));
                    }
                }
                best.map(|(_, handle)| handle)
            }
            EditorTool::Rotate => {
                for axis in [Axis::X, Axis::Y, Axis::Z] {
                    if hit_ring(ray, origin, self.gizmo_axis_vector(axis, transform)) {
                        return Some(GizmoHandle::Ring(axis));
                    }
                }
                None
            }
        }
    }

    fn update_title(&self, window: &Window) {
        window.set_title(&format!(
            "{WINDOW_TITLE} | style: {} | view: {} | selected: {} | objects: {} | lights: {} | key: {} | {}",
            self.style.name(),
            self.debug_view.name(),
            self.scene
                .objects
                .get(self.scene.selected)
                .map(|object| object.name.as_str())
                .unwrap_or("none"),
            self.scene.objects.len(),
            self.scene.lights.len(),
            self.scene.key_light_name(),
            self.status
        ));
    }

    unsafe fn render(&mut self) {
        let elapsed = self.start_time.elapsed().as_secs_f32();
        let width = self.viewport.width.max(1) as i32;
        let height = self.viewport.height.max(1) as i32;
        self.gl.viewport(0, 0, width, height);
        self.gl
            .clear(glow::COLOR_BUFFER_BIT | glow::DEPTH_BUFFER_BIT | glow::STENCIL_BUFFER_BIT);

        let camera = self.camera();
        let frame = FrameData {
            view: camera.view(),
            projection: camera.projection(width as f32 / height as f32),
            viewport: Vec2::new(width as f32, height as f32),
            camera_position: camera.eye,
            directional_light: self.scene.primary_directional_light(),
            local_lights: self.scene.local_light_packet(elapsed),
            local_light_count: self.scene.local_light_count(),
            elapsed,
            style: self.style,
            debug_view: self.debug_view,
            editor_tool: self.editor_tool,
            transform_space: self.transform_space,
            hovered_gizmo: self.hovered_gizmo,
            active_gizmo: self.active_gizmo,
        };

        self.renderer.render_scene(&self.scene, &frame);
    }
}

#[derive(Clone, Copy)]
enum RenderPass {
    Environment,
    Decals,
    Opaque,
    Outlines,
    Gizmos,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum EditorTool {
    Select,
    Move,
    Rotate,
    Scale,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum TransformSpace {
    Global,
    Local,
}

impl TransformSpace {
    fn name(self) -> &'static str {
        match self {
            Self::Global => "Global",
            Self::Local => "Local",
        }
    }

    fn toggle(self) -> Self {
        match self {
            Self::Global => Self::Local,
            Self::Local => Self::Global,
        }
    }
}

#[derive(Clone, Copy)]
struct Ray {
    origin: Vec3,
    direction: Vec3,
}

impl Ray {
    fn intersect_y_plane(self, y: f32) -> Option<Vec3> {
        let denom = self.direction.y;
        if denom.abs() < 0.0001 {
            return None;
        }
        let t = (y - self.origin.y) / denom;
        (t >= 0.0).then_some(self.origin + self.direction * t)
    }

    fn intersect_plane(self, point: Vec3, normal: Vec3) -> Option<Vec3> {
        let denom = self.direction.dot(normal);
        if denom.abs() < 0.0001 {
            return None;
        }
        let t = (point - self.origin).dot(normal) / denom;
        (t >= 0.0).then_some(self.origin + self.direction * t)
    }

    fn intersect_sphere(self, center: Vec3, radius: f32) -> Option<f32> {
        let oc = self.origin - center;
        let b = oc.dot(self.direction);
        let c = oc.length_squared() - radius * radius;
        let h = b * b - c;
        if h < 0.0 {
            return None;
        }
        let t = -b - h.sqrt();
        (t >= 0.0).then_some(t)
    }

    fn distance_to_segment(self, a: Vec3, b: Vec3) -> f32 {
        let u = self.direction;
        let v = b - a;
        let w0 = self.origin - a;
        let a_dot = u.dot(u);
        let b_dot = u.dot(v);
        let c_dot = v.dot(v);
        let d_dot = u.dot(w0);
        let e_dot = v.dot(w0);
        let denom = a_dot * c_dot - b_dot * b_dot;
        let mut ray_t = if denom.abs() > 0.0001 {
            (b_dot * e_dot - c_dot * d_dot) / denom
        } else {
            0.0
        };
        let mut segment_t = if denom.abs() > 0.0001 {
            (a_dot * e_dot - b_dot * d_dot) / denom
        } else {
            0.0
        };
        ray_t = ray_t.max(0.0);
        segment_t = segment_t.clamp(0.0, 1.0);
        let point_ray = self.origin + u * ray_t;
        let point_segment = a + v * segment_t;
        point_ray.distance(point_segment)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Axis {
    X,
    Y,
    Z,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum AxisPlane {
    Xy,
    Xz,
    Yz,
}

impl AxisPlane {
    fn axes(self) -> (Axis, Axis) {
        match self {
            Self::Xy => (Axis::X, Axis::Y),
            Self::Xz => (Axis::X, Axis::Z),
            Self::Yz => (Axis::Y, Axis::Z),
        }
    }

    fn normal(self) -> Axis {
        match self {
            Self::Xy => Axis::Z,
            Self::Xz => Axis::Y,
            Self::Yz => Axis::X,
        }
    }
}

impl Axis {
    fn vector(self) -> Vec3 {
        match self {
            Self::X => Vec3::X,
            Self::Y => Vec3::Y,
            Self::Z => Vec3::Z,
        }
    }

    fn component(self, value: Vec3) -> f32 {
        match self {
            Self::X => value.x,
            Self::Y => value.y,
            Self::Z => value.z,
        }
    }

    fn set_component(self, value: &mut Vec3, component: f32) {
        match self {
            Self::X => value.x = component,
            Self::Y => value.y = component,
            Self::Z => value.z = component,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum GizmoHandle {
    Center,
    Axis(Axis),
    Plane(AxisPlane),
    Ring(Axis),
}

impl EditorTool {
    fn name(self) -> &'static str {
        match self {
            Self::Select => "select",
            Self::Move => "move",
            Self::Rotate => "rotate",
            Self::Scale => "scale",
        }
    }
}

#[derive(Clone, Copy)]
enum DebugView {
    Final,
    Albedo,
    Normals,
    LightingOnly,
    Roughness,
    Metallic,
}

impl DebugView {
    fn name(self) -> &'static str {
        match self {
            Self::Final => "final",
            Self::Albedo => "albedo",
            Self::Normals => "normals",
            Self::LightingOnly => "lighting-only",
            Self::Roughness => "roughness",
            Self::Metallic => "metallic",
        }
    }

    fn shader_id(self) -> i32 {
        match self {
            Self::Final => 0,
            Self::Albedo => 1,
            Self::Normals => 2,
            Self::LightingOnly => 3,
            Self::Roughness => 4,
            Self::Metallic => 5,
        }
    }
}

#[derive(Clone, Copy)]
struct RenderState {
    depth_test: bool,
    depth_write: bool,
    blend: bool,
    cull_face: Option<u32>,
}

impl RenderPass {
    fn state(self) -> RenderState {
        match self {
            Self::Environment => RenderState {
                depth_test: true,
                depth_write: false,
                blend: true,
                cull_face: None,
            },
            Self::Decals => RenderState {
                depth_test: true,
                depth_write: false,
                blend: true,
                cull_face: None,
            },
            Self::Opaque => RenderState {
                depth_test: true,
                depth_write: true,
                blend: false,
                cull_face: Some(glow::BACK),
            },
            Self::Outlines => RenderState {
                depth_test: true,
                depth_write: false,
                blend: false,
                cull_face: Some(glow::FRONT),
            },
            Self::Gizmos => RenderState {
                depth_test: false,
                depth_write: false,
                blend: false,
                cull_face: None,
            },
        }
    }
}

#[derive(Clone, Copy)]
enum StylePreset {
    Character,
    Street,
    Combat,
}

impl StylePreset {
    fn name(self) -> &'static str {
        match self {
            Self::Character => "character",
            Self::Street => "street",
            Self::Combat => "combat",
        }
    }

    fn palette(self) -> Palette {
        match self {
            Self::Character => Palette {
                shadow: Vec3::new(0.16, 0.20, 0.25),
                mid: Vec3::new(0.66, 0.70, 0.68),
                light: Vec3::new(1.05, 1.02, 0.88),
                rim: Vec3::new(0.25, 0.92, 0.86),
                outline: Vec3::new(0.015, 0.018, 0.02),
            },
            Self::Street => Palette {
                shadow: Vec3::new(0.18, 0.23, 0.20),
                mid: Vec3::new(0.72, 0.68, 0.55),
                light: Vec3::new(1.08, 0.92, 0.62),
                rim: Vec3::new(1.0, 0.35, 0.13),
                outline: Vec3::new(0.018, 0.016, 0.014),
            },
            Self::Combat => Palette {
                shadow: Vec3::new(0.09, 0.16, 0.18),
                mid: Vec3::new(0.50, 0.64, 0.55),
                light: Vec3::new(1.12, 0.94, 0.36),
                rim: Vec3::new(0.0, 0.92, 0.76),
                outline: Vec3::new(0.01, 0.012, 0.010),
            },
        }
    }
}

#[allow(dead_code)]
struct Palette {
    shadow: Vec3,
    mid: Vec3,
    light: Vec3,
    rim: Vec3,
    outline: Vec3,
}

struct FrameData {
    view: Mat4,
    projection: Mat4,
    viewport: Vec2,
    camera_position: Vec3,
    directional_light: GpuDirectionalLight,
    local_lights: [GpuLocalLight; MAX_POINT_LIGHTS],
    local_light_count: i32,
    elapsed: f32,
    style: StylePreset,
    debug_view: DebugView,
    editor_tool: EditorTool,
    transform_space: TransformSpace,
    hovered_gizmo: Option<GizmoHandle>,
    active_gizmo: Option<GizmoHandle>,
}

#[derive(Clone, Copy)]
struct GpuDirectionalLight {
    direction: Vec3,
    color: Vec3,
    intensity: f32,
}

impl Default for GpuDirectionalLight {
    fn default() -> Self {
        Self {
            direction: Vec3::new(-0.35, -0.82, -0.45).normalize(),
            color: Vec3::new(1.0, 0.96, 0.88),
            intensity: 1.0,
        }
    }
}

#[derive(Clone, Copy)]
struct GpuLocalLight {
    position: Vec3,
    color: Vec3,
    direction: Vec3,
    range: f32,
    intensity: f32,
    inner_cos: f32,
    outer_cos: f32,
}

impl Default for GpuLocalLight {
    fn default() -> Self {
        Self {
            position: Vec3::ZERO,
            color: Vec3::ZERO,
            direction: Vec3::NEG_Y,
            range: 1.0,
            intensity: 0.0,
            inner_cos: -2.0,
            outer_cos: -2.0,
        }
    }
}

#[allow(dead_code)]
enum LightKind {
    Directional,
    Point,
    Spot,
}

impl LightKind {
    fn name(&self) -> &'static str {
        match self {
            Self::Directional => "directional",
            Self::Point => "point",
            Self::Spot => "spot",
        }
    }
}

struct Light {
    name: String,
    kind: LightKind,
    position: Vec3,
    direction: Vec3,
    color: Vec3,
    intensity: f32,
    range: f32,
    spot_angle_radians: f32,
    enabled: bool,
    pulse: f32,
}

impl Light {
    fn directional(name: &str, direction: Vec3, color: Vec3, intensity: f32) -> Self {
        Self {
            name: name.to_string(),
            kind: LightKind::Directional,
            position: Vec3::ZERO,
            direction: direction.normalize_or_zero(),
            color,
            intensity,
            range: 1.0,
            spot_angle_radians: 0.0,
            enabled: true,
            pulse: 0.0,
        }
    }

    fn point(name: &str, position: Vec3, color: Vec3, range: f32, intensity: f32) -> Self {
        Self {
            name: name.to_string(),
            kind: LightKind::Point,
            position,
            direction: Vec3::NEG_Y,
            color,
            intensity,
            range,
            spot_angle_radians: 0.0,
            enabled: true,
            pulse: 0.0,
        }
    }

    #[allow(dead_code)]
    fn spot(
        name: &str,
        position: Vec3,
        direction: Vec3,
        color: Vec3,
        range: f32,
        intensity: f32,
        spot_angle_radians: f32,
    ) -> Self {
        Self {
            name: name.to_string(),
            kind: LightKind::Spot,
            position,
            direction: direction.normalize_or_zero(),
            color,
            intensity,
            range,
            spot_angle_radians,
            enabled: true,
            pulse: 0.0,
        }
    }

    #[allow(dead_code)]
    fn pulsing(mut self, amount: f32) -> Self {
        self.pulse = amount;
        self
    }

    fn animated_intensity(&self, time: f32) -> f32 {
        self.intensity * (1.0 + self.pulse * time.sin())
    }
}

fn default_editor_lights() -> Vec<Light> {
    vec![
        Light::directional(
            "Key Directional",
            Vec3::new(-0.45, -0.9, -0.35),
            Vec3::new(1.0, 1.0, 0.96),
            1.15,
        ),
        Light::point(
            "Test Point",
            Vec3::new(0.0, 2.4, 2.2),
            Vec3::new(1.0, 0.74, 0.42),
            5.0,
            1.35,
        ),
    ]
}

struct Camera {
    eye: Vec3,
    target: Vec3,
    fov_y_radians: f32,
}

struct EditorCamera {
    target: Vec3,
    yaw: f32,
    pitch: f32,
    distance: f32,
    fov_y_radians: f32,
}

impl Default for EditorCamera {
    fn default() -> Self {
        Self {
            target: Vec3::new(0.0, 0.72, 0.0),
            yaw: 42.0_f32.to_radians(),
            pitch: 18.0_f32.to_radians(),
            distance: 6.3,
            fov_y_radians: 48.0_f32.to_radians(),
        }
    }
}

impl EditorCamera {
    fn to_camera(&self) -> Camera {
        let cp = self.pitch.cos();
        let direction = Vec3::new(self.yaw.sin() * cp, self.pitch.sin(), self.yaw.cos() * cp);
        Camera {
            eye: self.target + direction * self.distance,
            target: self.target,
            fov_y_radians: self.fov_y_radians,
        }
    }

    fn orbit(&mut self, delta: Vec2) {
        self.yaw -= delta.x * 0.006;
        self.pitch = (self.pitch + delta.y * 0.006).clamp(-1.15, 1.15);
    }

    fn pan(&mut self, delta: Vec2) {
        let camera = self.to_camera();
        let forward = (camera.target - camera.eye).normalize_or_zero();
        let right = forward.cross(Vec3::Y).normalize_or_zero();
        let up = right.cross(forward).normalize_or_zero();
        let scale = self.distance * 0.0014;
        self.target += (-right * delta.x + up * delta.y) * scale;
    }

    fn zoom(&mut self, amount: f32) {
        self.distance = (self.distance * (1.0 - amount * 0.08)).clamp(1.2, 30.0);
    }
}

impl Camera {
    fn view(&self) -> Mat4 {
        Mat4::look_at_rh(self.eye, self.target, Vec3::Y)
    }

    fn projection(&self, aspect: f32) -> Mat4 {
        Mat4::perspective_rh_gl(self.fov_y_radians, aspect, 0.05, 250.0)
    }
}

#[derive(Clone, Copy)]
struct Transform {
    translation: Vec3,
    rotation: Vec3,
    scale: Vec3,
}

impl Default for Transform {
    fn default() -> Self {
        Self {
            translation: Vec3::ZERO,
            rotation: Vec3::ZERO,
            scale: Vec3::ONE,
        }
    }
}

impl Transform {
    fn matrix(self) -> Mat4 {
        Mat4::from_translation(self.translation)
            * self.rotation_matrix()
            * Mat4::from_scale(self.scale)
    }

    fn rotation_matrix(self) -> Mat4 {
        Mat4::from_quat(self.rotation_quat())
    }

    fn rotation_quat(self) -> Quat {
        Quat::from_euler(
            glam::EulerRot::XYZ,
            self.rotation.x,
            self.rotation.y,
            self.rotation.z,
        )
    }

    fn set_rotation_quat(&mut self, rotation: Quat) {
        let (x, y, z) = rotation.normalize().to_euler(glam::EulerRot::XYZ);
        self.rotation = Vec3::new(x, y, z);
    }
}

#[derive(Default)]
struct Scene {
    objects: Vec<SceneObject>,
    lights: Vec<Light>,
    selected: usize,
    selected_group: u32,
    selected_light: Option<usize>,
    next_group_id: u32,
}

struct SceneObject {
    name: String,
    mesh: MeshGpu,
    group_id: u32,
    transform: Transform,
}

impl SceneObject {
    fn is_floor(&self) -> bool {
        self.name == "Floor"
    }
}

impl Scene {
    fn primary_directional_light(&self) -> GpuDirectionalLight {
        self.lights
            .iter()
            .find(|light| light.enabled && matches!(light.kind, LightKind::Directional))
            .map(|light| GpuDirectionalLight {
                direction: light.direction.normalize_or_zero(),
                color: light.color,
                intensity: light.intensity,
            })
            .unwrap_or_default()
    }

    fn local_light_packet(&self, time: f32) -> [GpuLocalLight; MAX_POINT_LIGHTS] {
        let mut packet = [GpuLocalLight::default(); MAX_POINT_LIGHTS];
        let mut count = 0;
        for light in &self.lights {
            if !light.enabled || matches!(light.kind, LightKind::Directional) {
                continue;
            }
            if count == MAX_POINT_LIGHTS {
                break;
            }
            let (inner_cos, outer_cos) = if matches!(light.kind, LightKind::Spot) {
                let outer = light.spot_angle_radians.max(0.01).cos();
                let inner = (light.spot_angle_radians * 0.62).max(0.01).cos();
                (inner, outer)
            } else {
                (-2.0, -2.0)
            };
            packet[count] = GpuLocalLight {
                position: light.position,
                color: light.color,
                direction: light.direction.normalize_or_zero(),
                range: light.range,
                intensity: light.animated_intensity(time),
                inner_cos,
                outer_cos,
            };
            count += 1;
        }
        packet
    }

    fn local_light_count(&self) -> i32 {
        self.lights
            .iter()
            .filter(|light| light.enabled && !matches!(light.kind, LightKind::Directional))
            .take(MAX_POINT_LIGHTS)
            .count() as i32
    }

    fn visible_gizmo_lights(&self) -> impl Iterator<Item = &Light> {
        self.lights
            .iter()
            .filter(|light| light.enabled && !matches!(light.kind, LightKind::Directional))
    }

    fn key_light_name(&self) -> &str {
        self.lights
            .iter()
            .find(|light| light.enabled && matches!(light.kind, LightKind::Directional))
            .map(|light| light.name.as_str())
            .unwrap_or("none")
    }

    fn selected_group(&self) -> Vec<&SceneObject> {
        self.objects
            .iter()
            .filter(|object| object.group_id == self.selected_group)
            .collect()
    }

    fn selected_group_mut(&mut self) -> impl Iterator<Item = &mut SceneObject> {
        let selected_group = self.selected_group;
        self.objects
            .iter_mut()
            .filter(move |object| object.group_id == selected_group)
    }

    fn select_group(&mut self, group_id: u32) {
        self.selected_group = group_id;
        self.selected_light = None;
        if let Some(index) = self
            .objects
            .iter()
            .position(|object| object.group_id == group_id)
        {
            self.selected = index;
        }
    }

    fn select_light(&mut self, index: usize) {
        if index < self.lights.len() {
            self.selected_light = Some(index);
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Vertex {
    position: [f32; 3],
    normal: [f32; 3],
    color: [f32; 4],
    tex_coord: [f32; 2],
}

unsafe impl bytemuck::Zeroable for Vertex {}
unsafe impl bytemuck::Pod for Vertex {}

struct MeshCpu {
    name: String,
    vertices: Vec<Vertex>,
    indices: Vec<u32>,
    texture: Option<TextureCpu>,
    material: MaterialProperties,
}

#[derive(Clone, Copy)]
struct MaterialProperties {
    metallic: f32,
    roughness: f32,
}

impl Default for MaterialProperties {
    fn default() -> Self {
        Self {
            metallic: 0.0,
            roughness: 0.55,
        }
    }
}

#[derive(Clone)]
struct TextureCpu {
    width: u32,
    height: u32,
    pixels: Vec<u8>,
}

struct ImportedModel {
    meshes: Vec<MeshCpu>,
}

impl ImportedModel {
    fn load(path: &Path) -> Result<Self, String> {
        match path
            .extension()
            .and_then(|extension| extension.to_str())
            .map(str::to_ascii_lowercase)
            .as_deref()
        {
            Some("obj") => Ok(Self {
                meshes: MeshCpu::load_obj(path)?,
            }),
            Some("glb") | Some("gltf") => Self::load_gltf(path),
            _ => Err(format!(
                "unsupported asset type for {}. Use .glb, .gltf, or .obj",
                path.display()
            )),
        }
    }

    fn load_gltf(path: &Path) -> Result<Self, String> {
        let (document, buffers, images) = gltf::import(path)
            .map_err(|error| format!("glTF read error for {}: {error}", path.display()))?;
        let textures = images
            .iter()
            .map(TextureCpu::from_gltf_image)
            .collect::<Vec<_>>();
        let mut meshes = Vec::new();

        for scene in document.scenes() {
            for node in scene.nodes() {
                collect_gltf_node(&node, Mat4::IDENTITY, &buffers, &textures, &mut meshes);
            }
        }

        if meshes.is_empty() {
            return Err(format!("no triangle meshes found in {}", path.display()));
        }

        normalize_meshes(&mut meshes);
        Ok(Self { meshes })
    }
}

impl MeshCpu {
    fn cube() -> Self {
        Self::cube_with_color("Starter cube", [0.82, 0.86, 0.76, 1.0])
    }

    fn cube_with_color(name: &str, color: [f32; 4]) -> Self {
        Self::cube_with_material(name, color, MaterialProperties::default())
    }

    fn cube_with_material(name: &str, color: [f32; 4], material: MaterialProperties) -> Self {
        let mut vertices = Vec::new();
        let mut indices = Vec::new();
        let faces = [
            (Vec3::Z, [0, 1, 2, 3]),
            (-Vec3::Z, [5, 4, 7, 6]),
            (Vec3::X, [1, 5, 6, 2]),
            (-Vec3::X, [4, 0, 3, 7]),
            (Vec3::Y, [3, 2, 6, 7]),
            (-Vec3::Y, [4, 5, 1, 0]),
        ];
        let corners = [
            Vec3::new(-0.5, -0.5, 0.5),
            Vec3::new(0.5, -0.5, 0.5),
            Vec3::new(0.5, 0.5, 0.5),
            Vec3::new(-0.5, 0.5, 0.5),
            Vec3::new(-0.5, -0.5, -0.5),
            Vec3::new(0.5, -0.5, -0.5),
            Vec3::new(0.5, 0.5, -0.5),
            Vec3::new(-0.5, 0.5, -0.5),
        ];
        for (face, corner_indices) in faces {
            let base = vertices.len() as u32;
            for corner_index in corner_indices {
                vertices.push(Vertex {
                    position: corners[corner_index].to_array(),
                    normal: face.to_array(),
                    color,
                    tex_coord: [0.0, 0.0],
                });
            }
            indices.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
        }
        Self {
            name: name.to_string(),
            vertices,
            indices,
            texture: None,
            material,
        }
    }

    fn load_obj(path: &Path) -> Result<Vec<Self>, String> {
        let options = tobj::LoadOptions {
            triangulate: true,
            single_index: true,
            ..Default::default()
        };
        let (models, materials) = tobj::load_obj(path, &options)
            .map_err(|error| format!("OBJ read error for {}: {error}", path.display()))?;
        let materials = materials.unwrap_or_default();
        models
            .into_iter()
            .map(|model| {
                let mesh = model.mesh;
                let mut vertices = Vec::with_capacity(mesh.positions.len() / 3);
                let color = mesh
                    .material_id
                    .and_then(|id| materials.get(id))
                    .and_then(|material| material.diffuse)
                    .unwrap_or([0.72, 0.74, 0.68]);
                for index in 0..mesh.positions.len() / 3 {
                    let normal = if mesh.normals.len() >= index * 3 + 3 {
                        [
                            mesh.normals[index * 3],
                            mesh.normals[index * 3 + 1],
                            mesh.normals[index * 3 + 2],
                        ]
                    } else {
                        [0.0, 1.0, 0.0]
                    };
                    vertices.push(Vertex {
                        position: [
                            mesh.positions[index * 3],
                            mesh.positions[index * 3 + 1],
                            mesh.positions[index * 3 + 2],
                        ],
                        normal,
                        color: [color[0], color[1], color[2], 1.0],
                        tex_coord: if mesh.texcoords.len() >= index * 2 + 2 {
                            [mesh.texcoords[index * 2], mesh.texcoords[index * 2 + 1]]
                        } else {
                            [0.0, 0.0]
                        },
                    });
                }
                Ok(Self {
                    name: model.name,
                    vertices,
                    indices: mesh.indices,
                    texture: None,
                    material: MaterialProperties::default(),
                })
            })
            .collect()
    }
}

impl TextureCpu {
    fn from_gltf_image(image: &gltf::image::Data) -> Option<Self> {
        use gltf::image::Format;

        let mut pixels = Vec::with_capacity((image.width * image.height * 4) as usize);
        match image.format {
            Format::R8 => {
                for value in &image.pixels {
                    pixels.extend_from_slice(&[*value, *value, *value, 255]);
                }
            }
            Format::R8G8 => {
                for pixel in image.pixels.chunks_exact(2) {
                    pixels.extend_from_slice(&[pixel[0], pixel[0], pixel[0], pixel[1]]);
                }
            }
            Format::R8G8B8 => {
                for pixel in image.pixels.chunks_exact(3) {
                    pixels.extend_from_slice(&[pixel[0], pixel[1], pixel[2], 255]);
                }
            }
            Format::R8G8B8A8 => pixels.extend_from_slice(&image.pixels),
            _ => return None,
        }

        Some(Self {
            width: image.width,
            height: image.height,
            pixels,
        })
    }
}

fn collect_gltf_node(
    node: &gltf::Node<'_>,
    parent_transform: Mat4,
    buffers: &[gltf::buffer::Data],
    textures: &[Option<TextureCpu>],
    meshes: &mut Vec<MeshCpu>,
) {
    let local = mat4_from_gltf(node.transform().matrix());
    let world = parent_transform * local;

    if let Some(mesh) = node.mesh() {
        for primitive in mesh.primitives() {
            let reader = primitive.reader(|buffer| Some(&buffers[buffer.index()]));
            let Some(positions) = reader.read_positions() else {
                continue;
            };
            let positions: Vec<[f32; 3]> = positions.collect();
            let normals: Vec<[f32; 3]> = reader
                .read_normals()
                .map(Iterator::collect)
                .unwrap_or_else(|| vec![[0.0, 1.0, 0.0]; positions.len()]);
            let tex_coords: Vec<[f32; 2]> = reader
                .read_tex_coords(0)
                .map(|coords| coords.into_f32().collect())
                .unwrap_or_else(|| vec![[0.0, 0.0]; positions.len()]);
            let vertex_colors: Vec<[f32; 4]> = reader
                .read_colors(0)
                .map(|colors| colors.into_rgba_f32().collect())
                .unwrap_or_else(|| vec![[1.0, 1.0, 1.0, 1.0]; positions.len()]);
            let material = primitive.material();
            let pbr = material.pbr_metallic_roughness();
            let factor = pbr.base_color_factor();
            let material_properties = MaterialProperties {
                metallic: pbr.metallic_factor().clamp(0.0, 1.0),
                roughness: pbr.roughness_factor().clamp(0.04, 1.0),
            };
            let texture = pbr
                .base_color_texture()
                .and_then(|info| textures.get(info.texture().source().index()))
                .cloned()
                .flatten();

            let vertices = positions
                .into_iter()
                .enumerate()
                .map(|(index, position)| {
                    let color = vertex_colors
                        .get(index)
                        .copied()
                        .unwrap_or([1.0, 1.0, 1.0, 1.0]);
                    let color = [
                        color[0] * factor[0],
                        color[1] * factor[1],
                        color[2] * factor[2],
                        color[3] * factor[3],
                    ];
                    let normal =
                        Vec3::from_array(normals.get(index).copied().unwrap_or([0.0, 1.0, 0.0]));
                    Vertex {
                        position: world
                            .transform_point3(Vec3::from_array(position))
                            .to_array(),
                        normal: world
                            .transform_vector3(normal)
                            .normalize_or_zero()
                            .to_array(),
                        color,
                        tex_coord: tex_coords.get(index).copied().unwrap_or([0.0, 0.0]),
                    }
                })
                .collect::<Vec<_>>();
            let indices = reader
                .read_indices()
                .map(|indices| indices.into_u32().collect())
                .unwrap_or_else(|| (0..vertices.len() as u32).collect());

            meshes.push(MeshCpu {
                name: format!(
                    "{}:{}",
                    mesh.name().unwrap_or("glTF mesh"),
                    primitive.index()
                ),
                vertices,
                indices,
                texture,
                material: material_properties,
            });
        }
    }

    for child in node.children() {
        collect_gltf_node(&child, world, buffers, textures, meshes);
    }
}

fn mat4_from_gltf(matrix: [[f32; 4]; 4]) -> Mat4 {
    Mat4::from_cols_array_2d(&matrix)
}

fn normalize_meshes(meshes: &mut [MeshCpu]) {
    let mut min = Vec3::splat(f32::INFINITY);
    let mut max = Vec3::splat(f32::NEG_INFINITY);
    for mesh in meshes.iter() {
        for vertex in &mesh.vertices {
            let position = Vec3::from_array(vertex.position);
            min = min.min(position);
            max = max.max(position);
        }
    }

    let size = max - min;
    let longest_axis = size.x.max(size.y).max(size.z).max(0.001);
    let scale = 3.8 / longest_axis;
    let center = Vec3::new((min.x + max.x) * 0.5, min.y, (min.z + max.z) * 0.5);

    for mesh in meshes {
        for vertex in &mut mesh.vertices {
            let position = Vec3::from_array(vertex.position);
            vertex.position = ((position - center) * scale).to_array();
        }
    }
}

struct MeshGpu {
    vertex_array: glow::NativeVertexArray,
    _vertex_buffer: glow::NativeBuffer,
    _index_buffer: glow::NativeBuffer,
    texture: Option<glow::NativeTexture>,
    material: MaterialProperties,
    bounds_radius: f32,
    index_count: i32,
}

impl MeshGpu {
    unsafe fn from_cpu(gl: Arc<glow::Context>, mesh: &MeshCpu) -> Result<Self, String> {
        let vertex_array = gl
            .create_vertex_array()
            .map_err(|error| error.to_string())?;
        let vertex_buffer = gl.create_buffer().map_err(|error| error.to_string())?;
        let index_buffer = gl.create_buffer().map_err(|error| error.to_string())?;

        gl.bind_vertex_array(Some(vertex_array));
        gl.bind_buffer(glow::ARRAY_BUFFER, Some(vertex_buffer));
        gl.buffer_data_u8_slice(
            glow::ARRAY_BUFFER,
            bytemuck::cast_slice(&mesh.vertices),
            glow::STATIC_DRAW,
        );
        gl.bind_buffer(glow::ELEMENT_ARRAY_BUFFER, Some(index_buffer));
        gl.buffer_data_u8_slice(
            glow::ELEMENT_ARRAY_BUFFER,
            bytemuck::cast_slice(&mesh.indices),
            glow::STATIC_DRAW,
        );

        let stride = std::mem::size_of::<Vertex>() as i32;
        gl.enable_vertex_attrib_array(0);
        gl.vertex_attrib_pointer_f32(0, 3, glow::FLOAT, false, stride, 0);
        gl.enable_vertex_attrib_array(1);
        gl.vertex_attrib_pointer_f32(1, 3, glow::FLOAT, false, stride, 12);
        gl.enable_vertex_attrib_array(2);
        gl.vertex_attrib_pointer_f32(2, 4, glow::FLOAT, false, stride, 24);
        gl.enable_vertex_attrib_array(3);
        gl.vertex_attrib_pointer_f32(3, 2, glow::FLOAT, false, stride, 40);
        gl.bind_vertex_array(None);

        let texture = if let Some(texture) = &mesh.texture {
            let gl_texture = gl.create_texture().map_err(|error| error.to_string())?;
            gl.bind_texture(glow::TEXTURE_2D, Some(gl_texture));
            gl.tex_parameter_i32(
                glow::TEXTURE_2D,
                glow::TEXTURE_MIN_FILTER,
                glow::LINEAR_MIPMAP_LINEAR as i32,
            );
            gl.tex_parameter_i32(
                glow::TEXTURE_2D,
                glow::TEXTURE_MAG_FILTER,
                glow::LINEAR as i32,
            );
            gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_S, glow::REPEAT as i32);
            gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_T, glow::REPEAT as i32);
            gl.tex_image_2d(
                glow::TEXTURE_2D,
                0,
                glow::RGBA as i32,
                texture.width as i32,
                texture.height as i32,
                0,
                glow::RGBA,
                glow::UNSIGNED_BYTE,
                glow::PixelUnpackData::Slice(Some(&texture.pixels)),
            );
            gl.generate_mipmap(glow::TEXTURE_2D);
            gl.bind_texture(glow::TEXTURE_2D, None);
            Some(gl_texture)
        } else {
            None
        };

        let bounds_radius = mesh
            .vertices
            .iter()
            .map(|vertex| Vec3::from_array(vertex.position).length())
            .fold(0.001_f32, f32::max);

        Ok(Self {
            vertex_array,
            _vertex_buffer: vertex_buffer,
            _index_buffer: index_buffer,
            texture,
            material: mesh.material,
            bounds_radius,
            index_count: mesh.indices.len() as i32,
        })
    }
}

struct Renderer {
    gl: Arc<glow::Context>,
    object_program: glow::NativeProgram,
    outline_program: glow::NativeProgram,
    editor_grid_program: glow::NativeProgram,
    unlit_program: glow::NativeProgram,
    contact_shadow_program: glow::NativeProgram,
    editor_grid: MeshGpu,
    light_gizmo: MeshGpu,
    arrow_gizmo: MeshGpu,
    rotation_gizmo: MeshGpu,
    contact_shadow: MeshGpu,
}

impl Renderer {
    unsafe fn new(gl: Arc<glow::Context>) -> Result<Self, String> {
        let object_program = compile_program(&gl, OBJECT_VERTEX, OBJECT_FRAGMENT)?;
        let outline_program = compile_program(&gl, OUTLINE_VERTEX, OUTLINE_FRAGMENT)?;
        let editor_grid_program = compile_program(&gl, EDITOR_GRID_VERTEX, EDITOR_GRID_FRAGMENT)?;
        let unlit_program = compile_program(&gl, UNLIT_VERTEX, UNLIT_FRAGMENT)?;
        let contact_shadow_program =
            compile_program(&gl, CONTACT_SHADOW_VERTEX, CONTACT_SHADOW_FRAGMENT)?;
        let editor_grid = MeshGpu::from_cpu(gl.clone(), &editor_grid_mesh())?;
        let light_gizmo = MeshGpu::from_cpu(gl.clone(), &MeshCpu::cube())?;
        let arrow_gizmo = MeshGpu::from_cpu(gl.clone(), &arrow_gizmo_mesh())?;
        let rotation_gizmo = MeshGpu::from_cpu(gl.clone(), &rotation_gizmo_mesh())?;
        let contact_shadow = MeshGpu::from_cpu(gl.clone(), &contact_shadow_mesh())?;
        Ok(Self {
            gl,
            object_program,
            outline_program,
            editor_grid_program,
            unlit_program,
            contact_shadow_program,
            editor_grid,
            light_gizmo,
            arrow_gizmo,
            rotation_gizmo,
            contact_shadow,
        })
    }

    unsafe fn render_scene(&self, scene: &Scene, frame: &FrameData) {
        self.begin_pass(RenderPass::Environment);
        self.draw_editor_grid(frame);

        self.begin_pass(RenderPass::Outlines);
        for object in &scene.objects {
            if !object.is_floor() {
                self.draw_object_outline(object, object.group_id == scene.selected_group, frame);
            }
        }

        self.begin_pass(RenderPass::Opaque);
        for object in &scene.objects {
            self.draw_object_surface(object, object.group_id == scene.selected_group, frame);
        }

        self.begin_pass(RenderPass::Decals);
        for object in &scene.objects {
            if !object.is_floor() {
                self.draw_contact_shadow(object, frame);
            }
        }

        self.begin_pass(RenderPass::Gizmos);
        self.draw_light_gizmos(scene, frame);
        self.draw_transform_gizmo(scene, frame);
        self.end_frame_state();
    }

    unsafe fn begin_pass(&self, pass: RenderPass) {
        let state = pass.state();
        if state.depth_test {
            self.gl.enable(glow::DEPTH_TEST);
            self.gl.depth_func(glow::LEQUAL);
        } else {
            self.gl.disable(glow::DEPTH_TEST);
        }
        self.gl.depth_mask(state.depth_write);
        if state.blend {
            self.gl.enable(glow::BLEND);
            self.gl
                .blend_func(glow::SRC_ALPHA, glow::ONE_MINUS_SRC_ALPHA);
        } else {
            self.gl.disable(glow::BLEND);
        }
        if let Some(face) = state.cull_face {
            self.gl.enable(glow::CULL_FACE);
            self.gl.cull_face(face);
        } else {
            self.gl.disable(glow::CULL_FACE);
        }
    }

    unsafe fn end_frame_state(&self) {
        self.gl.enable(glow::DEPTH_TEST);
        self.gl.depth_func(glow::LEQUAL);
        self.gl.depth_mask(true);
        self.gl.disable(glow::BLEND);
        self.gl.enable(glow::CULL_FACE);
        self.gl.cull_face(glow::BACK);
        self.gl.bind_vertex_array(None);
        self.gl.bind_texture(glow::TEXTURE_2D, None);
        self.gl.use_program(None);
    }

    unsafe fn draw_editor_grid(&self, frame: &FrameData) {
        self.gl.use_program(Some(self.editor_grid_program));
        set_mat4(
            &self.gl,
            self.editor_grid_program,
            "u_model",
            Mat4::IDENTITY,
        );
        set_mat4(&self.gl, self.editor_grid_program, "u_view", frame.view);
        set_mat4(
            &self.gl,
            self.editor_grid_program,
            "u_projection",
            frame.projection,
        );
        set_f32(&self.gl, self.editor_grid_program, "u_time", frame.elapsed);
        self.gl
            .bind_vertex_array(Some(self.editor_grid.vertex_array));
        self.gl.draw_elements(
            glow::TRIANGLES,
            self.editor_grid.index_count,
            glow::UNSIGNED_INT,
            0,
        );
        self.gl.bind_vertex_array(None);
    }

    unsafe fn draw_light_gizmos(&self, scene: &Scene, frame: &FrameData) {
        self.gl.use_program(Some(self.unlit_program));
        set_mat4(&self.gl, self.unlit_program, "u_view", frame.view);
        set_mat4(
            &self.gl,
            self.unlit_program,
            "u_projection",
            frame.projection,
        );
        self.gl
            .bind_vertex_array(Some(self.light_gizmo.vertex_array));
        for light in scene.visible_gizmo_lights() {
            let type_scale = match light.kind {
                LightKind::Point => Vec3::splat(0.055 + light.intensity * 0.045),
                LightKind::Spot => Vec3::new(0.07, 0.12, 0.07),
                LightKind::Directional => Vec3::splat(0.08),
            };
            let model = Mat4::from_translation(light.position) * Mat4::from_scale(type_scale);
            set_mat4(&self.gl, self.unlit_program, "u_model", model);
            set_vec3(
                &self.gl,
                self.unlit_program,
                "u_color",
                light.color * (0.55 + light.intensity * 0.65),
            );
            self.gl.draw_elements(
                glow::TRIANGLES,
                self.light_gizmo.index_count,
                glow::UNSIGNED_INT,
                0,
            );
        }
        self.gl.bind_vertex_array(None);
    }

    unsafe fn draw_transform_gizmo(&self, scene: &Scene, frame: &FrameData) {
        let Some(transform) = scene
            .selected_group()
            .first()
            .map(|object| object.transform)
        else {
            return;
        };
        let origin = transform.translation;
        let basis = gizmo_basis(frame.transform_space, transform);
        self.gl.use_program(Some(self.unlit_program));
        set_mat4(&self.gl, self.unlit_program, "u_view", frame.view);
        set_mat4(
            &self.gl,
            self.unlit_program,
            "u_projection",
            frame.projection,
        );
        self.gl
            .bind_vertex_array(Some(self.light_gizmo.vertex_array));

        match frame.editor_tool {
            EditorTool::Select => {
                self.draw_gizmo_handle_cube(
                    origin,
                    Vec3::splat(0.14),
                    Vec3::new(1.0, 0.92, 0.2),
                    GizmoHandle::Center,
                    frame,
                );
            }
            EditorTool::Move => {
                self.draw_plane_handle(
                    origin,
                    basis,
                    AxisPlane::Xy,
                    Vec3::new(1.0, 0.82, 0.18),
                    frame,
                );
                self.draw_plane_handle(
                    origin,
                    basis,
                    AxisPlane::Xz,
                    Vec3::new(1.0, 0.38, 0.18),
                    frame,
                );
                self.draw_plane_handle(
                    origin,
                    basis,
                    AxisPlane::Yz,
                    Vec3::new(0.18, 0.82, 1.0),
                    frame,
                );
                for axis in [Axis::X, Axis::Y, Axis::Z] {
                    self.draw_axis_bar(origin, basis, axis, axis_color(axis), true, frame);
                }
                self.draw_gizmo_handle_cube(
                    origin,
                    Vec3::splat(0.13),
                    Vec3::new(1.0, 0.92, 0.25),
                    GizmoHandle::Center,
                    frame,
                );
            }
            EditorTool::Scale => {
                self.draw_plane_handle(
                    origin,
                    basis,
                    AxisPlane::Xy,
                    Vec3::new(1.0, 0.82, 0.18),
                    frame,
                );
                self.draw_plane_handle(
                    origin,
                    basis,
                    AxisPlane::Xz,
                    Vec3::new(1.0, 0.38, 0.18),
                    frame,
                );
                self.draw_plane_handle(
                    origin,
                    basis,
                    AxisPlane::Yz,
                    Vec3::new(0.18, 0.82, 1.0),
                    frame,
                );
                for axis in [Axis::X, Axis::Y, Axis::Z] {
                    self.draw_axis_bar(origin, basis, axis, axis_color(axis), false, frame);
                    self.draw_gizmo_handle_box(
                        origin + basis_axis(basis, axis) * 1.08,
                        basis,
                        Vec3::splat(0.14),
                        axis_color(axis),
                        GizmoHandle::Axis(axis),
                        frame,
                    );
                }
                self.draw_gizmo_handle_cube(
                    origin,
                    Vec3::splat(0.16),
                    Vec3::new(1.0, 0.92, 0.25),
                    GizmoHandle::Center,
                    frame,
                );
            }
            EditorTool::Rotate => {
                for axis in [Axis::X, Axis::Y, Axis::Z] {
                    self.draw_rotation_ring(origin, basis, axis, axis_color(axis), frame);
                }
            }
        }
        self.gl.bind_vertex_array(None);
    }

    unsafe fn draw_axis_bar(
        &self,
        origin: Vec3,
        basis: Mat4,
        axis: Axis,
        color: Vec3,
        arrow_head: bool,
        frame: &FrameData,
    ) {
        let axis_vec = basis_axis(basis, axis);
        let handle = GizmoHandle::Axis(axis);
        let center = origin + axis_vec * 0.55;
        let scale = if axis == Axis::X {
            Vec3::new(1.05, 0.035, 0.035)
        } else if axis == Axis::Y {
            Vec3::new(0.035, 1.05, 0.035)
        } else {
            Vec3::new(0.035, 0.035, 1.05)
        };
        self.draw_gizmo_handle_box(center, basis, scale, color, handle, frame);
        if arrow_head {
            let tip = origin + axis_vec * 1.14;
            self.draw_gizmo_arrowhead(tip, basis, axis, color, handle, frame);
        }
    }

    unsafe fn draw_plane_handle(
        &self,
        origin: Vec3,
        basis: Mat4,
        plane: AxisPlane,
        color: Vec3,
        frame: &FrameData,
    ) {
        let (a, b) = plane.axes();
        let center = origin + basis_axis(basis, a) * 0.38 + basis_axis(basis, b) * 0.38;
        let scale = match plane {
            AxisPlane::Xy => Vec3::new(0.24, 0.24, 0.018),
            AxisPlane::Xz => Vec3::new(0.24, 0.018, 0.24),
            AxisPlane::Yz => Vec3::new(0.018, 0.24, 0.24),
        };
        self.draw_gizmo_handle_box(
            center,
            basis,
            scale,
            color,
            GizmoHandle::Plane(plane),
            frame,
        );
    }

    unsafe fn draw_rotation_ring(
        &self,
        origin: Vec3,
        basis: Mat4,
        axis: Axis,
        color: Vec3,
        frame: &FrameData,
    ) {
        let handle = GizmoHandle::Ring(axis);
        let color = gizmo_color(color, handle, frame);
        let emphasis = if frame.active_gizmo == Some(handle) || frame.hovered_gizmo == Some(handle)
        {
            1.055
        } else {
            1.0
        };
        let model = Mat4::from_translation(origin)
            * basis
            * ring_axis_model(axis)
            * Mat4::from_scale(Vec3::splat(emphasis));
        self.gl
            .bind_vertex_array(Some(self.rotation_gizmo.vertex_array));
        set_mat4(&self.gl, self.unlit_program, "u_model", model);
        set_vec3(&self.gl, self.unlit_program, "u_color", color);
        self.gl.draw_elements(
            glow::TRIANGLES,
            self.rotation_gizmo.index_count,
            glow::UNSIGNED_INT,
            0,
        );
    }

    unsafe fn draw_gizmo_handle_cube(
        &self,
        position: Vec3,
        scale: Vec3,
        color: Vec3,
        handle: GizmoHandle,
        frame: &FrameData,
    ) {
        self.draw_gizmo_cube(position, scale, gizmo_color(color, handle, frame));
    }

    unsafe fn draw_gizmo_handle_box(
        &self,
        position: Vec3,
        basis: Mat4,
        scale: Vec3,
        color: Vec3,
        handle: GizmoHandle,
        frame: &FrameData,
    ) {
        self.draw_gizmo_box(position, basis, scale, gizmo_color(color, handle, frame));
    }

    unsafe fn draw_gizmo_arrowhead(
        &self,
        position: Vec3,
        basis: Mat4,
        axis: Axis,
        color: Vec3,
        handle: GizmoHandle,
        frame: &FrameData,
    ) {
        let model = Mat4::from_translation(position)
            * basis
            * arrow_axis_model(axis)
            * Mat4::from_scale(Vec3::splat(0.18));
        set_mat4(&self.gl, self.unlit_program, "u_model", model);
        set_vec3(
            &self.gl,
            self.unlit_program,
            "u_color",
            gizmo_color(color, handle, frame),
        );
        self.gl
            .bind_vertex_array(Some(self.arrow_gizmo.vertex_array));
        self.gl.draw_elements(
            glow::TRIANGLES,
            self.arrow_gizmo.index_count,
            glow::UNSIGNED_INT,
            0,
        );
        self.gl
            .bind_vertex_array(Some(self.light_gizmo.vertex_array));
    }

    unsafe fn draw_gizmo_cube(&self, position: Vec3, scale: Vec3, color: Vec3) {
        self.draw_gizmo_box(position, Mat4::IDENTITY, scale, color);
    }

    unsafe fn draw_gizmo_box(&self, position: Vec3, basis: Mat4, scale: Vec3, color: Vec3) {
        set_mat4(
            &self.gl,
            self.unlit_program,
            "u_model",
            Mat4::from_translation(position) * basis * Mat4::from_scale(scale),
        );
        set_vec3(&self.gl, self.unlit_program, "u_color", color);
        self.gl.draw_elements(
            glow::TRIANGLES,
            self.light_gizmo.index_count,
            glow::UNSIGNED_INT,
            0,
        );
    }

    unsafe fn draw_contact_shadow(&self, object: &SceneObject, frame: &FrameData) {
        let radius = object.mesh.bounds_radius.clamp(0.35, 2.4);
        let light_ray = frame.directional_light.direction.normalize_or_zero();
        let object_height = object.transform.translation.y.max(0.12);
        let ray_y = light_ray.y.min(-0.08);
        let projected =
            object.transform.translation + light_ray * ((0.012 - object_height) / ray_y);
        let horizontal = Vec2::new(light_ray.x, light_ray.z);
        let stretch = 1.0 + horizontal.length() * 1.4;
        let yaw = horizontal.y.atan2(horizontal.x);
        let position = Vec3::new(projected.x, 0.012, projected.z);
        let model = Mat4::from_translation(position)
            * Mat4::from_rotation_y(-yaw)
            * Mat4::from_scale(Vec3::new(radius * 0.88 * stretch, 1.0, radius * 0.54));

        self.gl.use_program(Some(self.contact_shadow_program));
        set_mat4(&self.gl, self.contact_shadow_program, "u_model", model);
        set_mat4(&self.gl, self.contact_shadow_program, "u_view", frame.view);
        set_mat4(
            &self.gl,
            self.contact_shadow_program,
            "u_projection",
            frame.projection,
        );
        set_f32(
            &self.gl,
            self.contact_shadow_program,
            "u_strength",
            (0.34 * frame.directional_light.intensity / radius.sqrt()).clamp(0.1, 0.42),
        );
        self.gl
            .bind_vertex_array(Some(self.contact_shadow.vertex_array));
        self.gl.draw_elements(
            glow::TRIANGLES,
            self.contact_shadow.index_count,
            glow::UNSIGNED_INT,
            0,
        );
        self.gl.bind_vertex_array(None);
    }

    unsafe fn draw_object_outline(&self, object: &SceneObject, selected: bool, frame: &FrameData) {
        let model = object.transform.matrix();
        let palette = frame.style.palette();

        self.gl.use_program(Some(self.outline_program));
        set_mat4(&self.gl, self.outline_program, "u_model", model);
        set_mat4(&self.gl, self.outline_program, "u_view", frame.view);
        set_mat4(
            &self.gl,
            self.outline_program,
            "u_projection",
            frame.projection,
        );
        set_vec2(&self.gl, self.outline_program, "u_viewport", frame.viewport);
        set_vec3(
            &self.gl,
            self.outline_program,
            "u_outline_color",
            palette.outline,
        );
        set_f32(
            &self.gl,
            self.outline_program,
            "u_outline_width",
            if selected { 3.4 } else { 2.2 },
        );
        set_f32(
            &self.gl,
            self.outline_program,
            "u_ground_clip_y",
            GROUND_CLEARANCE * 0.45,
        );
        self.gl.bind_vertex_array(Some(object.mesh.vertex_array));
        self.gl.draw_elements(
            glow::TRIANGLES,
            object.mesh.index_count,
            glow::UNSIGNED_INT,
            0,
        );
        self.gl.bind_vertex_array(None);
    }

    unsafe fn draw_object_surface(&self, object: &SceneObject, selected: bool, frame: &FrameData) {
        let model = object.transform.matrix();
        self.gl.use_program(Some(self.object_program));
        self.gl.active_texture(glow::TEXTURE0);
        self.gl.bind_texture(glow::TEXTURE_2D, object.mesh.texture);
        set_mat4(&self.gl, self.object_program, "u_model", model);
        set_mat4(&self.gl, self.object_program, "u_view", frame.view);
        set_mat4(
            &self.gl,
            self.object_program,
            "u_projection",
            frame.projection,
        );
        set_vec3(
            &self.gl,
            self.object_program,
            "u_camera_position",
            frame.camera_position,
        );
        set_vec3(
            &self.gl,
            self.object_program,
            "u_directional_light_direction",
            frame.directional_light.direction,
        );
        set_vec3(
            &self.gl,
            self.object_program,
            "u_directional_light_color",
            frame.directional_light.color,
        );
        set_f32(
            &self.gl,
            self.object_program,
            "u_directional_light_intensity",
            frame.directional_light.intensity,
        );
        set_f32(
            &self.gl,
            self.object_program,
            "u_metallic",
            object.mesh.material.metallic,
        );
        set_f32(
            &self.gl,
            self.object_program,
            "u_roughness",
            object.mesh.material.roughness,
        );
        set_i32(
            &self.gl,
            self.object_program,
            "u_local_light_count",
            frame.local_light_count,
        );
        set_i32(
            &self.gl,
            self.object_program,
            "u_debug_view",
            frame.debug_view.shader_id(),
        );
        set_local_lights(&self.gl, self.object_program, &frame.local_lights);
        set_i32(&self.gl, self.object_program, "u_base_texture", 0);
        set_f32(
            &self.gl,
            self.object_program,
            "u_has_texture",
            if object.mesh.texture.is_some() {
                1.0
            } else {
                0.0
            },
        );
        set_f32(
            &self.gl,
            self.object_program,
            "u_selected",
            if selected { 1.0 } else { 0.0 },
        );
        self.gl.bind_vertex_array(Some(object.mesh.vertex_array));
        self.gl.draw_elements(
            glow::TRIANGLES,
            object.mesh.index_count,
            glow::UNSIGNED_INT,
            0,
        );
        self.gl.bind_vertex_array(None);
        self.gl.bind_texture(glow::TEXTURE_2D, None);
    }
}

fn editor_grid_mesh() -> MeshCpu {
    let mut vertices = Vec::new();
    let mut indices = Vec::new();

    let mut push_quad = |a: Vec3, b: Vec3, c: Vec3, d: Vec3, color: [f32; 4]| {
        let base = vertices.len() as u32;
        let normal = (b - a).cross(c - a).normalize_or_zero();
        vertices.push(Vertex {
            position: a.to_array(),
            normal: normal.to_array(),
            color,
            tex_coord: [0.0, 0.0],
        });
        vertices.push(Vertex {
            position: b.to_array(),
            normal: normal.to_array(),
            color,
            tex_coord: [1.0, 0.0],
        });
        vertices.push(Vertex {
            position: c.to_array(),
            normal: normal.to_array(),
            color,
            tex_coord: [1.0, 1.0],
        });
        vertices.push(Vertex {
            position: d.to_array(),
            normal: normal.to_array(),
            color,
            tex_coord: [0.0, 1.0],
        });
        indices.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
    };

    let size = 12;
    let half_width = 0.01;
    let grid_y = 0.018;
    for i in -size..=size {
        let f = i as f32;
        let color = if i == 0 {
            [0.95, 0.78, 0.28, 1.0]
        } else {
            [0.22, 0.34, 0.31, 0.82]
        };
        push_quad(
            Vec3::new(f - half_width, grid_y, -size as f32),
            Vec3::new(f + half_width, grid_y, -size as f32),
            Vec3::new(f + half_width, grid_y, size as f32),
            Vec3::new(f - half_width, grid_y, size as f32),
            color,
        );
        push_quad(
            Vec3::new(-size as f32, grid_y, f - half_width),
            Vec3::new(size as f32, grid_y, f - half_width),
            Vec3::new(size as f32, grid_y, f + half_width),
            Vec3::new(-size as f32, grid_y, f + half_width),
            color,
        );
    }

    MeshCpu {
        name: "world editor grid".to_string(),
        vertices,
        indices,
        texture: None,
        material: MaterialProperties::default(),
    }
}

fn contact_shadow_mesh() -> MeshCpu {
    let vertices = vec![
        Vertex {
            position: [-1.0, 0.0, -1.0],
            normal: [0.0, 1.0, 0.0],
            color: [0.0, 0.0, 0.0, 1.0],
            tex_coord: [0.0, 0.0],
        },
        Vertex {
            position: [1.0, 0.0, -1.0],
            normal: [0.0, 1.0, 0.0],
            color: [0.0, 0.0, 0.0, 1.0],
            tex_coord: [1.0, 0.0],
        },
        Vertex {
            position: [1.0, 0.0, 1.0],
            normal: [0.0, 1.0, 0.0],
            color: [0.0, 0.0, 0.0, 1.0],
            tex_coord: [1.0, 1.0],
        },
        Vertex {
            position: [-1.0, 0.0, 1.0],
            normal: [0.0, 1.0, 0.0],
            color: [0.0, 0.0, 0.0, 1.0],
            tex_coord: [0.0, 1.0],
        },
    ];
    MeshCpu {
        name: "contact shadow".to_string(),
        vertices,
        indices: vec![0, 1, 2, 0, 2, 3],
        texture: None,
        material: MaterialProperties::default(),
    }
}

fn arrow_gizmo_mesh() -> MeshCpu {
    let segments = 24;
    let radius = 0.42;
    let length = 1.0;
    let mut vertices = Vec::new();
    let mut indices = Vec::new();

    vertices.push(Vertex {
        position: [0.0, length, 0.0],
        normal: [0.0, 1.0, 0.0],
        color: [1.0, 1.0, 1.0, 1.0],
        tex_coord: [0.5, 1.0],
    });
    vertices.push(Vertex {
        position: [0.0, 0.0, 0.0],
        normal: [0.0, -1.0, 0.0],
        color: [1.0, 1.0, 1.0, 1.0],
        tex_coord: [0.5, 0.5],
    });

    for index in 0..segments {
        let angle = index as f32 / segments as f32 * std::f32::consts::TAU;
        let (s, c) = angle.sin_cos();
        vertices.push(Vertex {
            position: [c * radius, 0.0, s * radius],
            normal: Vec3::new(c, radius / length, s).normalize().to_array(),
            color: [1.0, 1.0, 1.0, 1.0],
            tex_coord: [index as f32 / segments as f32, 0.0],
        });
    }

    for index in 0..segments {
        let current = 2 + index as u32;
        let next = 2 + ((index + 1) % segments) as u32;
        indices.extend_from_slice(&[0, current, next]);
        indices.extend_from_slice(&[1, next, current]);
    }

    MeshCpu {
        name: "arrow gizmo".to_string(),
        vertices,
        indices,
        texture: None,
        material: MaterialProperties::default(),
    }
}

fn rotation_gizmo_mesh() -> MeshCpu {
    let ring_segments = 128;
    let tube_segments = 10;
    let major_radius = 0.92;
    let tube_radius = 0.018;
    let mut vertices = Vec::new();
    let mut indices = Vec::new();

    for ring in 0..ring_segments {
        let ring_angle = ring as f32 / ring_segments as f32 * std::f32::consts::TAU;
        let (ring_sin, ring_cos) = ring_angle.sin_cos();
        let radial = Vec3::new(ring_cos, ring_sin, 0.0);
        let center = radial * major_radius;

        for tube in 0..tube_segments {
            let tube_angle = tube as f32 / tube_segments as f32 * std::f32::consts::TAU;
            let (tube_sin, tube_cos) = tube_angle.sin_cos();
            let normal = (radial * tube_cos + Vec3::Z * tube_sin).normalize();
            let position = center + normal * tube_radius;
            vertices.push(Vertex {
                position: position.to_array(),
                normal: normal.to_array(),
                color: [1.0, 1.0, 1.0, 1.0],
                tex_coord: [0.0, 0.0],
            });
        }
    }

    for ring in 0..ring_segments {
        let next_ring = (ring + 1) % ring_segments;
        for tube in 0..tube_segments {
            let next_tube = (tube + 1) % tube_segments;
            let a = (ring * tube_segments + tube) as u32;
            let b = (next_ring * tube_segments + tube) as u32;
            let c = (next_ring * tube_segments + next_tube) as u32;
            let d = (ring * tube_segments + next_tube) as u32;
            indices.extend_from_slice(&[a, b, c, a, c, d]);
        }
    }

    MeshCpu {
        name: "rotation gizmo".to_string(),
        vertices,
        indices,
        texture: None,
        material: MaterialProperties::default(),
    }
}

fn hit_ring(ray: Ray, origin: Vec3, axis_vec: Vec3) -> bool {
    let normal = axis_vec.normalize_or_zero();
    let denom = ray.direction.dot(normal);
    if denom.abs() < 0.0001 {
        return false;
    }
    let t = (origin - ray.origin).dot(normal) / denom;
    if t < 0.0 {
        return false;
    }
    let hit = ray.origin + ray.direction * t;
    let distance = (hit - origin).length();
    (distance - 0.92).abs() < 0.16
}

fn ring_axis_model(axis: Axis) -> Mat4 {
    match axis {
        Axis::X => Mat4::from_rotation_y(std::f32::consts::FRAC_PI_2),
        Axis::Y => Mat4::from_rotation_x(-std::f32::consts::FRAC_PI_2),
        Axis::Z => Mat4::IDENTITY,
    }
}

fn arrow_axis_model(axis: Axis) -> Mat4 {
    match axis {
        Axis::X => Mat4::from_rotation_z(-std::f32::consts::FRAC_PI_2),
        Axis::Y => Mat4::IDENTITY,
        Axis::Z => Mat4::from_rotation_x(std::f32::consts::FRAC_PI_2),
    }
}

fn wrap_angle(angle: f32) -> f32 {
    let mut wrapped = angle;
    while wrapped > std::f32::consts::PI {
        wrapped -= std::f32::consts::TAU;
    }
    while wrapped < -std::f32::consts::PI {
        wrapped += std::f32::consts::TAU;
    }
    wrapped
}

fn hit_plane_handle(ray: Ray, origin: Vec3, a: Vec3, b: Vec3) -> bool {
    let center = origin + a * 0.38 + b * 0.38;
    let Some(hit) = ray.intersect_plane(center, a.cross(b).normalize_or_zero()) else {
        return false;
    };
    let rel = hit - center;
    rel.dot(a).abs() <= 0.2 && rel.dot(b).abs() <= 0.2
}

fn axis_color(axis: Axis) -> Vec3 {
    match axis {
        Axis::X => Vec3::new(1.0, 0.18, 0.14),
        Axis::Y => Vec3::new(0.26, 1.0, 0.3),
        Axis::Z => Vec3::new(0.2, 0.52, 1.0),
    }
}

fn gizmo_basis(space: TransformSpace, transform: Transform) -> Mat4 {
    match space {
        TransformSpace::Global => Mat4::IDENTITY,
        TransformSpace::Local => transform.rotation_matrix(),
    }
}

fn basis_axis(basis: Mat4, axis: Axis) -> Vec3 {
    basis.transform_vector3(axis.vector()).normalize_or_zero()
}

fn gizmo_color(base: Vec3, handle: GizmoHandle, frame: &FrameData) -> Vec3 {
    if frame.active_gizmo == Some(handle) {
        Vec3::new(1.0, 0.95, 0.18)
    } else if frame.hovered_gizmo == Some(handle) {
        (base * 1.45 + Vec3::splat(0.16)).min(Vec3::splat(1.0))
    } else {
        base
    }
}

unsafe fn compile_program(
    gl: &glow::Context,
    vertex_source: &str,
    fragment_source: &str,
) -> Result<glow::NativeProgram, String> {
    let program = gl.create_program().map_err(|error| error.to_string())?;
    let vertex = compile_shader(gl, glow::VERTEX_SHADER, vertex_source)?;
    let fragment = compile_shader(gl, glow::FRAGMENT_SHADER, fragment_source)?;
    gl.attach_shader(program, vertex);
    gl.attach_shader(program, fragment);
    gl.link_program(program);
    if !gl.get_program_link_status(program) {
        return Err(gl.get_program_info_log(program));
    }
    gl.detach_shader(program, vertex);
    gl.detach_shader(program, fragment);
    gl.delete_shader(vertex);
    gl.delete_shader(fragment);
    Ok(program)
}

unsafe fn compile_shader(
    gl: &glow::Context,
    shader_type: u32,
    source: &str,
) -> Result<glow::NativeShader, String> {
    let shader = gl
        .create_shader(shader_type)
        .map_err(|error| error.to_string())?;
    gl.shader_source(shader, source);
    gl.compile_shader(shader);
    if gl.get_shader_compile_status(shader) {
        Ok(shader)
    } else {
        Err(gl.get_shader_info_log(shader))
    }
}

unsafe fn set_mat4(gl: &glow::Context, program: glow::NativeProgram, name: &str, value: Mat4) {
    if let Some(location) = gl.get_uniform_location(program, name) {
        gl.uniform_matrix_4_f32_slice(Some(&location), false, &value.to_cols_array());
    }
}

unsafe fn set_vec3(gl: &glow::Context, program: glow::NativeProgram, name: &str, value: Vec3) {
    if let Some(location) = gl.get_uniform_location(program, name) {
        gl.uniform_3_f32(Some(&location), value.x, value.y, value.z);
    }
}

unsafe fn set_vec2(gl: &glow::Context, program: glow::NativeProgram, name: &str, value: Vec2) {
    if let Some(location) = gl.get_uniform_location(program, name) {
        gl.uniform_2_f32(Some(&location), value.x, value.y);
    }
}

unsafe fn set_local_lights(
    gl: &glow::Context,
    program: glow::NativeProgram,
    lights: &[GpuLocalLight; MAX_POINT_LIGHTS],
) {
    let mut positions = [0.0; MAX_POINT_LIGHTS * 3];
    let mut colors = [0.0; MAX_POINT_LIGHTS * 3];
    let mut directions = [0.0; MAX_POINT_LIGHTS * 3];
    let mut params = [0.0; MAX_POINT_LIGHTS * 4];
    for (index, light) in lights.iter().enumerate() {
        positions[index * 3] = light.position.x;
        positions[index * 3 + 1] = light.position.y;
        positions[index * 3 + 2] = light.position.z;
        colors[index * 3] = light.color.x;
        colors[index * 3 + 1] = light.color.y;
        colors[index * 3 + 2] = light.color.z;
        directions[index * 3] = light.direction.x;
        directions[index * 3 + 1] = light.direction.y;
        directions[index * 3 + 2] = light.direction.z;
        params[index * 4] = light.range;
        params[index * 4 + 1] = light.intensity;
        params[index * 4 + 2] = light.inner_cos;
        params[index * 4 + 3] = light.outer_cos;
    }
    if let Some(location) = gl.get_uniform_location(program, "u_local_light_positions") {
        gl.uniform_3_f32_slice(Some(&location), &positions);
    }
    if let Some(location) = gl.get_uniform_location(program, "u_local_light_colors") {
        gl.uniform_3_f32_slice(Some(&location), &colors);
    }
    if let Some(location) = gl.get_uniform_location(program, "u_local_light_directions") {
        gl.uniform_3_f32_slice(Some(&location), &directions);
    }
    if let Some(location) = gl.get_uniform_location(program, "u_local_light_params") {
        gl.uniform_4_f32_slice(Some(&location), &params);
    }
}

unsafe fn set_f32(gl: &glow::Context, program: glow::NativeProgram, name: &str, value: f32) {
    if let Some(location) = gl.get_uniform_location(program, name) {
        gl.uniform_1_f32(Some(&location), value);
    }
}

unsafe fn set_i32(gl: &glow::Context, program: glow::NativeProgram, name: &str, value: i32) {
    if let Some(location) = gl.get_uniform_location(program, name) {
        gl.uniform_1_i32(Some(&location), value);
    }
}

const OBJECT_VERTEX: &str = r#"#version 330 core
layout (location = 0) in vec3 a_position;
layout (location = 1) in vec3 a_normal;
layout (location = 2) in vec4 a_color;
layout (location = 3) in vec2 a_tex_coord;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;

out vec3 v_world_position;
out vec3 v_normal;
out vec4 v_color;
out vec2 v_tex_coord;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world_position = world.xyz;
    v_normal = normalize(mat3(transpose(inverse(u_model))) * a_normal);
    v_color = a_color;
    v_tex_coord = a_tex_coord;
    gl_Position = u_projection * u_view * world;
}
"#;

const OBJECT_FRAGMENT: &str = r#"#version 330 core
in vec3 v_world_position;
in vec3 v_normal;
in vec4 v_color;
in vec2 v_tex_coord;

uniform vec3 u_camera_position;
uniform vec3 u_directional_light_direction;
uniform vec3 u_directional_light_color;
uniform float u_directional_light_intensity;
uniform float u_selected;
uniform float u_has_texture;
uniform float u_metallic;
uniform float u_roughness;
uniform sampler2D u_base_texture;
uniform int u_debug_view;
uniform int u_local_light_count;
uniform vec3 u_local_light_positions[8];
uniform vec3 u_local_light_colors[8];
uniform vec3 u_local_light_directions[8];
uniform vec4 u_local_light_params[8];

out vec4 out_color;

const float PI = 3.14159265359;

float distributionGGX(vec3 normal, vec3 half_dir, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ndh = max(dot(normal, half_dir), 0.0);
    float ndh2 = ndh * ndh;
    float denom = ndh2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0001);
}

float geometrySchlickGGX(float ndv, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndv / max(ndv * (1.0 - k) + k, 0.0001);
}

float geometrySmith(vec3 normal, vec3 view_dir, vec3 light_dir, float roughness) {
    float ndv = max(dot(normal, view_dir), 0.0);
    float ndl = max(dot(normal, light_dir), 0.0);
    return geometrySchlickGGX(ndv, roughness) * geometrySchlickGGX(ndl, roughness);
}

vec3 fresnelSchlick(float cos_theta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

vec3 pbrLight(
    vec3 albedo,
    float metallic,
    float roughness,
    vec3 normal,
    vec3 view_dir,
    vec3 light_dir,
    vec3 radiance
) {
    vec3 half_dir = normalize(view_dir + light_dir);
    float ndl = max(dot(normal, light_dir), 0.0);
    float ndv = max(dot(normal, view_dir), 0.0);
    if (ndl <= 0.0 || ndv <= 0.0) {
        return vec3(0.0);
    }

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float d = distributionGGX(normal, half_dir, roughness);
    float g = geometrySmith(normal, view_dir, light_dir, roughness);
    vec3 f = fresnelSchlick(max(dot(half_dir, view_dir), 0.0), f0);

    vec3 numerator = d * g * f;
    float denominator = max(4.0 * ndv * ndl, 0.0001);
    vec3 specular = numerator / denominator;

    vec3 ks = f;
    vec3 kd = (1.0 - ks) * (1.0 - metallic);
    vec3 diffuse = kd * albedo / PI;

    return (diffuse + specular) * radiance * ndl;
}

void main() {
    vec3 normal = normalize(v_normal);
    vec3 view_dir = normalize(u_camera_position - v_world_position);
    vec3 key_dir = normalize(-u_directional_light_direction);
    vec3 sampled = u_has_texture > 0.5 ? texture(u_base_texture, v_tex_coord).rgb : vec3(1.0);
    vec3 albedo = pow(clamp(v_color.rgb * sampled, 0.0, 1.0), vec3(2.2));
    float metallic = clamp(u_metallic, 0.0, 1.0);
    float roughness = clamp(u_roughness, 0.04, 1.0);

    vec3 direct_light = pbrLight(
        albedo,
        metallic,
        roughness,
        normal,
        view_dir,
        key_dir,
        u_directional_light_color * max(u_directional_light_intensity, 0.0) * 3.2
    );
    for (int i = 0; i < 8; ++i) {
        if (i >= u_local_light_count) {
            break;
        }
        vec3 to_light = u_local_light_positions[i] - v_world_position;
        float distance_to_light = length(to_light);
        vec3 light_dir = to_light / max(distance_to_light, 0.001);
        vec4 params = u_local_light_params[i];
        float attenuation = 1.0 - smoothstep(params.x * 0.18, params.x, distance_to_light);
        float cone = 1.0;
        if (params.w > -1.5) {
            float spot_dot = dot(normalize(u_local_light_directions[i]), -light_dir);
            cone = smoothstep(params.w, params.z, spot_dot);
        }
        float visibility = attenuation * cone * params.y;
        direct_light += pbrLight(
            albedo,
            metallic,
            roughness,
            normal,
            view_dir,
            light_dir,
            u_local_light_colors[i] * visibility * 14.0
        );
    }

    vec3 ambient = albedo * vec3(0.03) * (1.0 - metallic);
    vec3 color = ambient + direct_light;
    color += vec3(1.0, 0.82, 0.18) * u_selected * 0.08;

    if (u_debug_view == 1) {
        out_color = vec4(pow(clamp(albedo, 0.0, 1.0), vec3(1.0 / 2.2)), v_color.a);
        return;
    }
    if (u_debug_view == 2) {
        out_color = vec4(normal * 0.5 + 0.5, 1.0);
        return;
    }
    if (u_debug_view == 3) {
        vec3 lighting_only = clamp(ambient + direct_light, 0.0, 8.0);
        out_color = vec4(pow(lighting_only / (lighting_only + vec3(1.0)), vec3(1.0 / 2.2)), 1.0);
        return;
    }
    if (u_debug_view == 4) {
        out_color = vec4(vec3(roughness), 1.0);
        return;
    }
    if (u_debug_view == 5) {
        out_color = vec4(vec3(metallic), 1.0);
        return;
    }

    float camera_distance = length(u_camera_position - v_world_position);
    vec3 fog_color = vec3(0.10, 0.11, 0.10);
    color = mix(color, fog_color, smoothstep(18.0, 38.0, camera_distance) * 0.25);
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    out_color = vec4(color, v_color.a);
}
"#;

const OUTLINE_VERTEX: &str = r#"#version 330 core
layout (location = 0) in vec3 a_position;
layout (location = 1) in vec3 a_normal;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;
uniform float u_outline_width;
uniform vec2 u_viewport;

out float v_world_y;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world_y = world.y;
    vec3 view_normal = normalize(mat3(u_view * u_model) * a_normal);
    vec4 clip = u_projection * u_view * world;
    vec2 dir = normalize(view_normal.xy + vec2(0.0001, 0.0001));
    vec2 pixel = vec2(2.0 / u_viewport.x, 2.0 / u_viewport.y);
    clip.xy += dir * pixel * u_outline_width * clip.w;
    gl_Position = clip;
}
"#;

const OUTLINE_FRAGMENT: &str = r#"#version 330 core
in float v_world_y;
uniform vec3 u_outline_color;
uniform float u_ground_clip_y;
out vec4 out_color;

void main() {
    if (v_world_y < u_ground_clip_y) {
        discard;
    }
    out_color = vec4(u_outline_color, 1.0);
}
"#;

const UNLIT_VERTEX: &str = r#"#version 330 core
layout (location = 0) in vec3 a_position;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;

void main() {
    gl_Position = u_projection * u_view * u_model * vec4(a_position, 1.0);
}
"#;

const UNLIT_FRAGMENT: &str = r#"#version 330 core
uniform vec3 u_color;
out vec4 out_color;

void main() {
    vec3 color = u_color / (u_color + vec3(1.0));
    out_color = vec4(pow(color, vec3(0.72)), 1.0);
}
"#;

const CONTACT_SHADOW_VERTEX: &str = r#"#version 330 core
layout (location = 0) in vec3 a_position;
layout (location = 3) in vec2 a_tex_coord;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;

out vec2 v_tex_coord;

void main() {
    v_tex_coord = a_tex_coord;
    gl_Position = u_projection * u_view * u_model * vec4(a_position, 1.0);
}
"#;

const CONTACT_SHADOW_FRAGMENT: &str = r#"#version 330 core
in vec2 v_tex_coord;
uniform float u_strength;
out vec4 out_color;

void main() {
    vec2 centered = v_tex_coord * 2.0 - 1.0;
    float d = dot(centered, centered);
    float alpha = smoothstep(1.0, 0.08, d) * u_strength;
    out_color = vec4(0.0, 0.0, 0.0, alpha);
}
"#;

const EDITOR_GRID_VERTEX: &str = r#"#version 330 core
layout (location = 0) in vec3 a_position;
layout (location = 2) in vec4 a_color;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;
uniform float u_time;

out vec4 v_color;

void main() {
    v_color = a_color;
    gl_Position = u_projection * u_view * u_model * vec4(a_position, 1.0);
}
"#;

const EDITOR_GRID_FRAGMENT: &str = r#"#version 330 core
in vec4 v_color;
out vec4 out_color;

void main() {
    vec3 color = v_color.rgb;
    float glow = max(max(color.r, color.g), color.b);
    color += color * smoothstep(0.55, 1.0, glow) * 0.35;
    out_color = vec4(color, v_color.a);
}
"#;
