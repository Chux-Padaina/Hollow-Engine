#![allow(deprecated, unsafe_op_in_unsafe_fn)]

use std::ffi::CString;
use std::num::NonZeroU32;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::Instant;

use glam::{Mat4, Vec2, Vec3, Vec4};
use glow::HasContext;
use glutin::config::{ConfigTemplateBuilder, GlConfig};
use glutin::context::{ContextApi, ContextAttributesBuilder};
use glutin::display::{GetGlDisplay, GlDisplay};
use glutin::prelude::*;
use glutin::surface::{GlSurface, SurfaceAttributesBuilder, WindowSurface};
use glutin_winit::DisplayBuilder;
use raw_window_handle::HasWindowHandle;
use winit::dpi::{PhysicalPosition, PhysicalSize};
use winit::event::{ElementState, Event, MouseButton, WindowEvent};
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
        Rc::new(glow::Context::from_loader_function(|symbol| {
            let symbol = CString::new(symbol).expect("GL symbol has interior nul");
            gl_display.get_proc_address(&symbol).cast()
        }))
    };

    let mut app = unsafe { App::new(gl, size)? };
    app.try_import_arg();

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
                    if let Err(error) = surface.swap_buffers(&gl_context) {
                        eprintln!("swap buffers: {error}");
                        target.exit();
                    }
                }
                WindowEvent::DroppedFile(path) => app.import_model(path),
                WindowEvent::CursorMoved { position, .. } => app.cursor_moved(position),
                WindowEvent::MouseInput { state, button, .. } => {
                    if button == MouseButton::Left {
                        app.mouse_left(state == ElementState::Pressed);
                    }
                }
                WindowEvent::KeyboardInput { event, .. } => {
                    if event.state == ElementState::Pressed {
                        match event.physical_key {
                            PhysicalKey::Code(KeyCode::Escape) => target.exit(),
                            PhysicalKey::Code(KeyCode::Digit1) => {
                                app.style = StylePreset::Character
                            }
                            PhysicalKey::Code(KeyCode::Digit2) => app.style = StylePreset::Street,
                            PhysicalKey::Code(KeyCode::Digit3) => app.style = StylePreset::Combat,
                            PhysicalKey::Code(KeyCode::F1) => app.debug_view = DebugView::Final,
                            PhysicalKey::Code(KeyCode::F2) => app.debug_view = DebugView::Albedo,
                            PhysicalKey::Code(KeyCode::F3) => app.debug_view = DebugView::Normals,
                            PhysicalKey::Code(KeyCode::F4) => {
                                app.debug_view = DebugView::LightingOnly
                            }
                            PhysicalKey::Code(KeyCode::KeyR) => app.reset_scene(),
                            _ => {}
                        }
                    }
                }
                _ => {}
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
    gl: Rc<glow::Context>,
    renderer: Renderer,
    scene: Scene,
    viewport: PhysicalSize<u32>,
    cursor: Vec2,
    dragging: bool,
    drag_offset: Vec3,
    start_time: Instant,
    style: StylePreset,
    debug_view: DebugView,
    status: String,
}

impl App {
    unsafe fn new(gl: Rc<glow::Context>, viewport: PhysicalSize<u32>) -> Result<Self, String> {
        gl.enable(glow::DEPTH_TEST);
        gl.enable(glow::CULL_FACE);
        gl.clear_color(0.075, 0.085, 0.08, 1.0);
        let renderer = Renderer::new(gl.clone())?;
        let mut scene = Scene::default();
        scene.next_group_id = 1;
        scene.lights = default_showroom_lights();
        scene.objects.push(SceneObject {
            name: "Starter cube".to_string(),
            mesh: MeshGpu::from_cpu(gl.clone(), &MeshCpu::cube())?,
            group_id: 0,
            transform: Transform {
                translation: Vec3::new(0.0, 0.5 + GROUND_CLEARANCE, 0.0),
                ..Transform::default()
            },
        });
        Ok(Self {
            gl,
            renderer,
            scene,
            viewport,
            cursor: Vec2::ZERO,
            dragging: false,
            drag_offset: Vec3::ZERO,
            start_time: Instant::now(),
            style: StylePreset::Street,
            debug_view: DebugView::Final,
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
                                    rotation_y: 0.0,
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
        self.cursor = Vec2::new(position.x as f32, position.y as f32);
        if self.dragging {
            if let Some(point) = self.cursor_ground_point() {
                let translation = point + self.drag_offset;
                for object in self.scene.selected_group_mut() {
                    object.transform.translation = translation;
                }
            }
        }
    }

    fn mouse_left(&mut self, pressed: bool) {
        self.dragging = pressed;
        if pressed {
            if let Some(point) = self.cursor_ground_point() {
                if let Some(object) = self.scene.selected_group().first() {
                    self.drag_offset = object.transform.translation - point;
                }
            }
        }
    }

    fn reset_scene(&mut self) {
        self.scene.selected = 0;
        self.scene.selected_group = 0;
        for (index, object) in self.scene.objects.iter_mut().enumerate() {
            object.transform.translation = if object.group_id == 0 {
                Vec3::new(0.0, 0.5 + GROUND_CLEARANCE, 0.0)
            } else {
                Vec3::new(index as f32 * 0.02, GROUND_CLEARANCE, 0.0)
            };
            object.transform.rotation_y = 0.0;
        }
        self.status = "Scene reset".to_string();
    }

    fn camera(&self) -> Camera {
        Camera {
            eye: Vec3::new(3.8, 2.45, 4.6),
            target: Vec3::new(0.0, 0.72, 0.0),
            fov_y_radians: 48.0_f32.to_radians(),
        }
    }

    fn cursor_ground_point(&self) -> Option<Vec3> {
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
        let denom = ray_dir.y;
        if denom.abs() < 0.0001 {
            return None;
        }
        let t = -ray_origin.y / denom;
        (t >= 0.0).then_some(ray_origin + ray_dir * t)
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
            light_direction: self.scene.primary_directional_light_direction(),
            point_lights: self.scene.point_light_packet(elapsed),
            elapsed,
            style: self.style,
            debug_view: self.debug_view,
        };

        for object in &mut self.scene.objects {
            object.transform.rotation_y += 0.002;
        }
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

#[derive(Clone, Copy)]
enum DebugView {
    Final,
    Albedo,
    Normals,
    LightingOnly,
}

impl DebugView {
    fn name(self) -> &'static str {
        match self {
            Self::Final => "final",
            Self::Albedo => "albedo",
            Self::Normals => "normals",
            Self::LightingOnly => "lighting-only",
        }
    }

    fn shader_id(self) -> i32 {
        match self {
            Self::Final => 0,
            Self::Albedo => 1,
            Self::Normals => 2,
            Self::LightingOnly => 3,
        }
    }
}

#[derive(Clone, Copy)]
struct RenderState {
    depth_write: bool,
    blend: bool,
    cull_face: Option<u32>,
}

impl RenderPass {
    fn state(self) -> RenderState {
        match self {
            Self::Environment => RenderState {
                depth_write: true,
                blend: false,
                cull_face: None,
            },
            Self::Decals => RenderState {
                depth_write: false,
                blend: true,
                cull_face: None,
            },
            Self::Opaque => RenderState {
                depth_write: true,
                blend: false,
                cull_face: Some(glow::BACK),
            },
            Self::Outlines => RenderState {
                depth_write: false,
                blend: false,
                cull_face: Some(glow::FRONT),
            },
            Self::Gizmos => RenderState {
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
    light_direction: Vec3,
    point_lights: [GpuPointLight; MAX_POINT_LIGHTS],
    elapsed: f32,
    style: StylePreset,
    debug_view: DebugView,
}

#[derive(Clone, Copy)]
struct GpuPointLight {
    position: Vec3,
    color: Vec3,
    range: f32,
    intensity: f32,
}

impl Default for GpuPointLight {
    fn default() -> Self {
        Self {
            position: Vec3::ZERO,
            color: Vec3::ZERO,
            range: 1.0,
            intensity: 0.0,
        }
    }
}

enum LightKind {
    Directional,
    Point,
    Spot,
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

    fn pulsing(mut self, amount: f32) -> Self {
        self.pulse = amount;
        self
    }

    fn animated_intensity(&self, time: f32) -> f32 {
        self.intensity * (1.0 + self.pulse * time.sin())
    }
}

fn default_showroom_lights() -> Vec<Light> {
    vec![
        Light::directional(
            "Key Directional",
            Vec3::new(-0.35, -0.82, -0.45),
            Vec3::new(1.0, 0.96, 0.88),
            1.0,
        ),
        Light::point(
            "Ceiling Warm",
            Vec3::new(0.0, 3.65, 0.0),
            Vec3::new(1.0, 0.96, 0.82),
            8.5,
            0.92,
        ),
        Light::point(
            "Ceiling Cool Right",
            Vec3::new(3.8, 3.25, 2.6),
            Vec3::new(0.95, 0.98, 1.0),
            5.8,
            0.62,
        ),
        Light::point(
            "Ceiling Cool Left",
            Vec3::new(-3.8, 3.25, 2.6),
            Vec3::new(0.95, 0.98, 1.0),
            5.8,
            0.62,
        ),
        Light::point(
            "Floor Cyan Right",
            Vec3::new(4.8, 0.45, 0.2),
            Vec3::new(0.0, 0.95, 0.88),
            4.3,
            0.95,
        )
        .pulsing(0.04),
        Light::point(
            "Floor Cyan Left",
            Vec3::new(-4.8, 0.45, 0.2),
            Vec3::new(0.0, 0.95, 0.88),
            4.3,
            0.95,
        )
        .pulsing(0.04),
        Light::spot(
            "Amber Spot Right",
            Vec3::new(2.7, 0.45, -4.0),
            Vec3::new(-0.35, 0.9, 0.35),
            Vec3::new(1.0, 0.45, 0.12),
            4.0,
            0.82,
            36.0_f32.to_radians(),
        ),
        Light::spot(
            "Amber Spot Left",
            Vec3::new(-2.7, 0.45, -4.0),
            Vec3::new(0.35, 0.9, 0.35),
            Vec3::new(1.0, 0.45, 0.12),
            4.0,
            0.82,
            36.0_f32.to_radians(),
        ),
        Light::point(
            "Rear Gold",
            Vec3::new(0.0, 2.8, -5.5),
            Vec3::new(1.0, 0.88, 0.45),
            5.2,
            0.48,
        ),
    ]
}

struct Camera {
    eye: Vec3,
    target: Vec3,
    fov_y_radians: f32,
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
    rotation_y: f32,
    scale: Vec3,
}

impl Default for Transform {
    fn default() -> Self {
        Self {
            translation: Vec3::ZERO,
            rotation_y: 0.0,
            scale: Vec3::ONE,
        }
    }
}

impl Transform {
    fn matrix(self) -> Mat4 {
        Mat4::from_translation(self.translation)
            * Mat4::from_rotation_y(self.rotation_y)
            * Mat4::from_scale(self.scale)
    }
}

#[derive(Default)]
struct Scene {
    objects: Vec<SceneObject>,
    lights: Vec<Light>,
    selected: usize,
    selected_group: u32,
    next_group_id: u32,
}

struct SceneObject {
    name: String,
    mesh: MeshGpu,
    group_id: u32,
    transform: Transform,
}

impl Scene {
    fn primary_directional_light_direction(&self) -> Vec3 {
        self.lights
            .iter()
            .find(|light| light.enabled && matches!(light.kind, LightKind::Directional))
            .map(|light| light.direction)
            .unwrap_or(Vec3::new(-0.35, -0.82, -0.45).normalize())
    }

    fn point_light_packet(&self, time: f32) -> [GpuPointLight; MAX_POINT_LIGHTS] {
        let mut packet = [GpuPointLight::default(); MAX_POINT_LIGHTS];
        let mut count = 0;
        for light in &self.lights {
            if !light.enabled || matches!(light.kind, LightKind::Directional) {
                continue;
            }
            if count == MAX_POINT_LIGHTS {
                break;
            }
            let spot_focus = if matches!(light.kind, LightKind::Spot) {
                0.84 + light.spot_angle_radians.cos().abs() * 0.08
            } else {
                1.0
            };
            packet[count] = GpuPointLight {
                position: light.position,
                color: light.color,
                range: light.range,
                intensity: light.animated_intensity(time) * spot_focus,
            };
            count += 1;
        }
        packet
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
                    color: [0.82, 0.86, 0.76, 1.0],
                    tex_coord: [0.0, 0.0],
                });
            }
            indices.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
        }
        Self {
            name: "Starter cube".to_string(),
            vertices,
            indices,
            texture: None,
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
    bounds_radius: f32,
    index_count: i32,
}

impl MeshGpu {
    unsafe fn from_cpu(gl: Rc<glow::Context>, mesh: &MeshCpu) -> Result<Self, String> {
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
            bounds_radius,
            index_count: mesh.indices.len() as i32,
        })
    }
}

struct Renderer {
    gl: Rc<glow::Context>,
    object_program: glow::NativeProgram,
    outline_program: glow::NativeProgram,
    showroom_program: glow::NativeProgram,
    unlit_program: glow::NativeProgram,
    contact_shadow_program: glow::NativeProgram,
    showroom: MeshGpu,
    light_gizmo: MeshGpu,
    contact_shadow: MeshGpu,
}

impl Renderer {
    unsafe fn new(gl: Rc<glow::Context>) -> Result<Self, String> {
        let object_program = compile_program(&gl, OBJECT_VERTEX, OBJECT_FRAGMENT)?;
        let outline_program = compile_program(&gl, OUTLINE_VERTEX, OUTLINE_FRAGMENT)?;
        let showroom_program = compile_program(&gl, SHOWROOM_VERTEX, SHOWROOM_FRAGMENT)?;
        let unlit_program = compile_program(&gl, UNLIT_VERTEX, UNLIT_FRAGMENT)?;
        let contact_shadow_program =
            compile_program(&gl, CONTACT_SHADOW_VERTEX, CONTACT_SHADOW_FRAGMENT)?;
        let showroom = MeshGpu::from_cpu(gl.clone(), &showroom_mesh())?;
        let light_gizmo = MeshGpu::from_cpu(gl.clone(), &MeshCpu::cube())?;
        let contact_shadow = MeshGpu::from_cpu(gl.clone(), &contact_shadow_mesh())?;
        Ok(Self {
            gl,
            object_program,
            outline_program,
            showroom_program,
            unlit_program,
            contact_shadow_program,
            showroom,
            light_gizmo,
            contact_shadow,
        })
    }

    unsafe fn render_scene(&self, scene: &Scene, frame: &FrameData) {
        self.begin_pass(RenderPass::Environment);
        self.draw_showroom(frame);

        self.begin_pass(RenderPass::Decals);
        for object in &scene.objects {
            self.draw_contact_shadow(object, frame);
        }

        self.begin_pass(RenderPass::Outlines);
        for object in &scene.objects {
            self.draw_object_outline(object, object.group_id == scene.selected_group, frame);
        }

        self.begin_pass(RenderPass::Opaque);
        for object in &scene.objects {
            self.draw_object_surface(object, object.group_id == scene.selected_group, frame);
        }

        self.begin_pass(RenderPass::Gizmos);
        self.draw_light_gizmos(scene, frame);
        self.end_frame_state();
    }

    unsafe fn begin_pass(&self, pass: RenderPass) {
        let state = pass.state();
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
        self.gl.depth_mask(true);
        self.gl.disable(glow::BLEND);
        self.gl.enable(glow::CULL_FACE);
        self.gl.cull_face(glow::BACK);
        self.gl.bind_vertex_array(None);
        self.gl.bind_texture(glow::TEXTURE_2D, None);
        self.gl.use_program(None);
    }

    unsafe fn draw_showroom(&self, frame: &FrameData) {
        self.gl.use_program(Some(self.showroom_program));
        set_mat4(&self.gl, self.showroom_program, "u_model", Mat4::IDENTITY);
        set_mat4(&self.gl, self.showroom_program, "u_view", frame.view);
        set_mat4(
            &self.gl,
            self.showroom_program,
            "u_projection",
            frame.projection,
        );
        set_f32(&self.gl, self.showroom_program, "u_time", frame.elapsed);
        self.gl.bind_vertex_array(Some(self.showroom.vertex_array));
        self.gl.draw_elements(
            glow::TRIANGLES,
            self.showroom.index_count,
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

    unsafe fn draw_contact_shadow(&self, object: &SceneObject, frame: &FrameData) {
        let radius = object.mesh.bounds_radius.clamp(0.35, 2.4);
        let position = Vec3::new(
            object.transform.translation.x,
            0.014,
            object.transform.translation.z,
        );
        let model = Mat4::from_translation(position)
            * Mat4::from_rotation_y(object.transform.rotation_y)
            * Mat4::from_scale(Vec3::new(radius * 1.2, 1.0, radius * 0.78));

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
            (0.36 / radius.sqrt()).clamp(0.12, 0.34),
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
        let palette = frame.style.palette();

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
            "u_light_direction",
            frame.light_direction,
        );
        set_vec3(&self.gl, self.object_program, "u_shadow", palette.shadow);
        set_vec3(&self.gl, self.object_program, "u_mid", palette.mid);
        set_vec3(&self.gl, self.object_program, "u_light", palette.light);
        set_vec3(&self.gl, self.object_program, "u_rim", palette.rim);
        set_i32(
            &self.gl,
            self.object_program,
            "u_point_light_count",
            frame.point_lights.len() as i32,
        );
        set_i32(
            &self.gl,
            self.object_program,
            "u_debug_view",
            frame.debug_view.shader_id(),
        );
        set_point_lights(&self.gl, self.object_program, &frame.point_lights);
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

fn showroom_mesh() -> MeshCpu {
    let mut vertices = Vec::new();
    let mut indices = Vec::new();
    let ceiling_center = Vec3::ZERO;
    let ceiling_color = [0.92, 0.94, 0.9, 1.0];

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

    let segments = 96;
    let radius = 7.2;
    let wall_height = 4.2;
    for segment in 0..segments {
        let a0 = segment as f32 / segments as f32 * std::f32::consts::TAU;
        let a1 = (segment + 1) as f32 / segments as f32 * std::f32::consts::TAU;
        let p0 = Vec3::new(a0.cos() * radius, 0.0, a0.sin() * radius);
        let p1 = Vec3::new(a1.cos() * radius, 0.0, a1.sin() * radius);
        let tint = if segment % 12 < 6 { 0.03 } else { 0.0 };
        push_quad(
            Vec3::ZERO,
            p1,
            p0,
            Vec3::ZERO,
            [0.17 + tint, 0.18 + tint, 0.17 + tint, 1.0],
        );
        push_quad(
            p0,
            p1,
            p1 + Vec3::Y * wall_height,
            p0 + Vec3::Y * wall_height,
            [0.24, 0.25, 0.23, 1.0],
        );
    }

    for segment in 0..segments {
        let a0 = segment as f32 / segments as f32 * std::f32::consts::TAU;
        let a1 = (segment + 1) as f32 / segments as f32 * std::f32::consts::TAU;
        let p0 = Vec3::new(a0.cos() * radius, wall_height, a0.sin() * radius);
        let p1 = Vec3::new(a1.cos() * radius, wall_height, a1.sin() * radius);
        push_quad(
            ceiling_center + Vec3::Y * wall_height,
            p0,
            p1,
            ceiling_center + Vec3::Y * wall_height,
            ceiling_color,
        );
    }

    for ring in 0..16 {
        let angle = ring as f32 / 16.0 * std::f32::consts::TAU;
        let dir = Vec3::new(angle.cos(), 0.0, angle.sin());
        let side = Vec3::new(-angle.sin(), 0.0, angle.cos());
        let center = dir * 4.9 + Vec3::Y * 3.72;
        let color = match ring % 4 {
            0 => [1.00, 0.92, 0.72, 1.0],
            1 => [0.60, 0.92, 1.00, 1.0],
            2 => [1.00, 0.68, 0.42, 1.0],
            _ => [0.88, 1.00, 0.84, 1.0],
        };
        push_quad(
            center - side * 0.42 - dir * 0.12,
            center + side * 0.42 - dir * 0.12,
            center + side * 0.42 + dir * 0.12,
            center - side * 0.42 + dir * 0.12,
            color,
        );
    }

    for ring in 0..10 {
        let angle = (ring as f32 + 0.5) / 10.0 * std::f32::consts::TAU;
        let dir = Vec3::new(angle.cos(), 0.0, angle.sin());
        let side = Vec3::new(-angle.sin(), 0.0, angle.cos());
        let center = dir * 6.35 + Vec3::Y * 0.004;
        let color = if ring % 2 == 0 {
            [0.18, 0.95, 0.82, 1.0]
        } else {
            [1.0, 0.46, 0.18, 1.0]
        };
        push_quad(
            center - side * 0.14 - dir * 0.28,
            center + side * 0.14 - dir * 0.28,
            center + side * 0.14 + dir * 0.28,
            center - side * 0.14 + dir * 0.28,
            color,
        );
    }

    MeshCpu {
        name: "circular showroom".to_string(),
        vertices,
        indices,
        texture: None,
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

unsafe fn set_point_lights(
    gl: &glow::Context,
    program: glow::NativeProgram,
    lights: &[GpuPointLight; MAX_POINT_LIGHTS],
) {
    let mut positions = [0.0; MAX_POINT_LIGHTS * 3];
    let mut colors = [0.0; MAX_POINT_LIGHTS * 3];
    let mut params = [0.0; MAX_POINT_LIGHTS * 2];
    for (index, light) in lights.iter().enumerate() {
        positions[index * 3] = light.position.x;
        positions[index * 3 + 1] = light.position.y;
        positions[index * 3 + 2] = light.position.z;
        colors[index * 3] = light.color.x;
        colors[index * 3 + 1] = light.color.y;
        colors[index * 3 + 2] = light.color.z;
        params[index * 2] = light.range;
        params[index * 2 + 1] = light.intensity;
    }
    if let Some(location) = gl.get_uniform_location(program, "u_point_light_positions") {
        gl.uniform_3_f32_slice(Some(&location), &positions);
    }
    if let Some(location) = gl.get_uniform_location(program, "u_point_light_colors") {
        gl.uniform_3_f32_slice(Some(&location), &colors);
    }
    if let Some(location) = gl.get_uniform_location(program, "u_point_light_params") {
        gl.uniform_2_f32_slice(Some(&location), &params);
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
uniform vec3 u_light_direction;
uniform vec3 u_shadow;
uniform vec3 u_mid;
uniform vec3 u_light;
uniform vec3 u_rim;
uniform float u_selected;
uniform float u_has_texture;
uniform sampler2D u_base_texture;
uniform int u_debug_view;
uniform int u_point_light_count;
uniform vec3 u_point_light_positions[8];
uniform vec3 u_point_light_colors[8];
uniform vec2 u_point_light_params[8];

out vec4 out_color;

void main() {
    vec3 normal = normalize(v_normal);
    float ndl = dot(normal, normalize(-u_light_direction));
    float wrapped = clamp(ndl * 0.72 + 0.36, 0.0, 1.0);
    float mid_band = smoothstep(0.24, 0.42, wrapped);
    float high_band = smoothstep(0.62, 0.82, wrapped);
    vec3 ramp_color = mix(u_shadow, u_mid, mid_band);
    ramp_color = mix(ramp_color, u_light, high_band);

    vec3 warm_floor = vec3(1.0, 0.48, 0.16) * smoothstep(0.0, 1.8, v_world_position.y) * 0.06;
    vec3 cool_side = vec3(0.12, 0.78, 0.95) * (0.5 + 0.5 * normal.x) * 0.07;
    ramp_color += warm_floor + cool_side;

    vec3 point_light = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
        if (i >= u_point_light_count) {
            break;
        }
        vec3 to_light = u_point_light_positions[i] - v_world_position;
        float distance_to_light = length(to_light);
        vec3 light_dir = to_light / max(distance_to_light, 0.001);
        float range = u_point_light_params[i].x;
        float intensity = u_point_light_params[i].y;
        float attenuation = smoothstep(range, 0.0, distance_to_light);
        float facing = smoothstep(-0.25, 0.72, dot(normal, light_dir));
        point_light += u_point_light_colors[i] * attenuation * facing * intensity;
    }

    vec3 view_dir = normalize(u_camera_position - v_world_position);
    float rim = pow(1.0 - max(dot(view_dir, normal), 0.0), 3.0);
    rim = smoothstep(0.34, 0.88, rim);

    vec3 sampled = u_has_texture > 0.5 ? texture(u_base_texture, v_tex_coord).rgb : vec3(1.0);
    vec3 albedo = clamp(v_color.rgb * sampled, 0.0, 8.0);
    vec3 base = pow(albedo, vec3(0.85));
    vec3 lighting = ramp_color + point_light * 0.48;
    vec3 color = base * lighting + u_rim * rim * 0.42;
    color += vec3(1.0, 0.82, 0.18) * u_selected * 0.08;

    if (u_debug_view == 1) {
        out_color = vec4(pow(clamp(albedo, 0.0, 1.0), vec3(0.4545)), v_color.a);
        return;
    }
    if (u_debug_view == 2) {
        out_color = vec4(normal * 0.5 + 0.5, 1.0);
        return;
    }
    if (u_debug_view == 3) {
        vec3 lighting_only = clamp(lighting + u_rim * rim * 0.24, 0.0, 4.0);
        lighting_only = lighting_only / (lighting_only + vec3(1.0));
        out_color = vec4(pow(lighting_only, vec3(0.82)) * 1.25, 1.0);
        return;
    }

    float camera_distance = length(u_camera_position - v_world_position);
    vec3 fog_color = vec3(0.17, 0.19, 0.17);
    color = mix(color, fog_color, smoothstep(12.0, 28.0, camera_distance) * 0.42);
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(0.82)) * 1.18;
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

const SHOWROOM_VERTEX: &str = r#"#version 330 core
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

const SHOWROOM_FRAGMENT: &str = r#"#version 330 core
in vec4 v_color;
out vec4 out_color;

void main() {
    vec3 color = v_color.rgb;
    float glow = max(max(color.r, color.g), color.b);
    color += color * smoothstep(0.55, 1.0, glow) * 0.35;
    out_color = vec4(color, v_color.a);
}
"#;
