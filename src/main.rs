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
    status: String,
}

impl App {
    unsafe fn new(gl: Rc<glow::Context>, viewport: PhysicalSize<u32>) -> Result<Self, String> {
        gl.enable(glow::DEPTH_TEST);
        gl.enable(glow::CULL_FACE);
        gl.clear_color(0.075, 0.085, 0.08, 1.0);
        let renderer = Renderer::new(gl.clone())?;
        let mut scene = Scene::default();
        scene.objects.push(SceneObject {
            name: "Starter cube".to_string(),
            mesh: MeshGpu::from_cpu(gl.clone(), &MeshCpu::cube())?,
            transform: Transform::default(),
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
            status: "Drop a Sketchfab-style .glb/.gltf/.obj to import".to_string(),
        })
    }

    fn try_import_arg(&mut self) {
        if let Some(path) = std::env::args_os().nth(1).map(PathBuf::from) {
            self.import_model(path);
        }
    }

    fn import_model(&mut self, path: PathBuf) {
        match MeshCpu::load(&path) {
            Ok(meshes) => {
                let mut imported = 0;
                for mesh in meshes {
                    match unsafe { MeshGpu::from_cpu(self.gl.clone(), &mesh) } {
                        Ok(gpu_mesh) => {
                            let scale = mesh.suggested_scale();
                            self.scene.objects.push(SceneObject {
                                name: mesh.name,
                                mesh: gpu_mesh,
                                transform: Transform {
                                    translation: Vec3::new(imported as f32 * 1.8 - 0.9, 0.0, 0.0),
                                    rotation_y: 0.0,
                                    scale: Vec3::splat(scale),
                                },
                            });
                            imported += 1;
                        }
                        Err(error) => self.status = format!("GPU upload failed: {error}"),
                    }
                }
                if imported > 0 {
                    self.scene.selected = self.scene.objects.len().saturating_sub(1);
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
                if let Some(object) = self.scene.objects.get_mut(self.scene.selected) {
                    object.transform.translation = point + self.drag_offset;
                }
            }
        }
    }

    fn mouse_left(&mut self, pressed: bool) {
        self.dragging = pressed;
        if pressed {
            if let Some(point) = self.cursor_ground_point() {
                if let Some(object) = self.scene.objects.get(self.scene.selected) {
                    self.drag_offset = object.transform.translation - point;
                }
            }
        }
    }

    fn reset_scene(&mut self) {
        self.scene.selected = 0;
        for (index, object) in self.scene.objects.iter_mut().enumerate() {
            object.transform.translation = Vec3::new(index as f32 * 1.7, 0.0, 0.0);
            object.transform.rotation_y = 0.0;
        }
        self.status = "Scene reset".to_string();
    }

    fn camera(&self) -> Camera {
        Camera {
            eye: Vec3::new(4.5, 3.4, 6.0),
            target: Vec3::new(0.0, 0.85, 0.0),
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
            "{WINDOW_TITLE} | style: {} | selected: {} | objects: {} | {}",
            self.style.name(),
            self.scene
                .objects
                .get(self.scene.selected)
                .map(|object| object.name.as_str())
                .unwrap_or("none"),
            self.scene.objects.len(),
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
            camera_position: camera.eye,
            light_direction: Vec3::new(-0.35, -0.82, -0.45).normalize(),
            elapsed,
            style: self.style,
        };

        self.renderer.draw_grid(&frame);
        for (index, object) in self.scene.objects.iter_mut().enumerate() {
            object.transform.rotation_y += 0.002;
            self.renderer
                .draw_object(object, index == self.scene.selected, &frame);
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
    camera_position: Vec3,
    light_direction: Vec3,
    elapsed: f32,
    style: StylePreset,
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
    selected: usize,
}

struct SceneObject {
    name: String,
    mesh: MeshGpu,
    transform: Transform,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Vertex {
    position: [f32; 3],
    normal: [f32; 3],
    color: [f32; 4],
}

unsafe impl bytemuck::Zeroable for Vertex {}
unsafe impl bytemuck::Pod for Vertex {}

struct MeshCpu {
    name: String,
    vertices: Vec<Vertex>,
    indices: Vec<u32>,
}

impl MeshCpu {
    fn load(path: &Path) -> Result<Vec<Self>, String> {
        match path
            .extension()
            .and_then(|extension| extension.to_str())
            .map(str::to_ascii_lowercase)
            .as_deref()
        {
            Some("obj") => Self::load_obj(path),
            Some("glb") | Some("gltf") => Self::load_gltf(path),
            _ => Err(format!(
                "unsupported asset type for {}. Use .glb, .gltf, or .obj",
                path.display()
            )),
        }
    }

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
                });
            }
            indices.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
        }
        Self {
            name: "Starter cube".to_string(),
            vertices,
            indices,
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
                    });
                }
                Ok(Self {
                    name: model.name,
                    vertices,
                    indices: mesh.indices,
                })
            })
            .collect()
    }

    fn load_gltf(path: &Path) -> Result<Vec<Self>, String> {
        let (document, buffers, _) = gltf::import(path)
            .map_err(|error| format!("glTF read error for {}: {error}", path.display()))?;
        let mut meshes = Vec::new();
        for mesh in document.meshes() {
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
                let color = primitive
                    .material()
                    .pbr_metallic_roughness()
                    .base_color_factor();
                let vertices = positions
                    .into_iter()
                    .enumerate()
                    .map(|(index, position)| Vertex {
                        position,
                        normal: normals.get(index).copied().unwrap_or([0.0, 1.0, 0.0]),
                        color,
                    })
                    .collect::<Vec<_>>();
                let indices = reader
                    .read_indices()
                    .map(|indices| indices.into_u32().collect())
                    .unwrap_or_else(|| (0..vertices.len() as u32).collect());
                meshes.push(Self {
                    name: mesh.name().unwrap_or("glTF mesh").to_string(),
                    vertices,
                    indices,
                });
            }
        }
        if meshes.is_empty() {
            Err(format!("no triangle meshes found in {}", path.display()))
        } else {
            Ok(meshes)
        }
    }

    fn suggested_scale(&self) -> f32 {
        let mut max_extent = 0.001_f32;
        for vertex in &self.vertices {
            let position = Vec3::from_array(vertex.position);
            max_extent = max_extent.max(position.length());
        }
        (1.7 / max_extent).clamp(0.01, 20.0)
    }
}

struct MeshGpu {
    vertex_array: glow::NativeVertexArray,
    _vertex_buffer: glow::NativeBuffer,
    _index_buffer: glow::NativeBuffer,
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
        gl.bind_vertex_array(None);

        Ok(Self {
            vertex_array,
            _vertex_buffer: vertex_buffer,
            _index_buffer: index_buffer,
            index_count: mesh.indices.len() as i32,
        })
    }
}

struct Renderer {
    gl: Rc<glow::Context>,
    object_program: glow::NativeProgram,
    outline_program: glow::NativeProgram,
    grid_program: glow::NativeProgram,
    grid: MeshGpu,
}

impl Renderer {
    unsafe fn new(gl: Rc<glow::Context>) -> Result<Self, String> {
        let object_program = compile_program(&gl, OBJECT_VERTEX, OBJECT_FRAGMENT)?;
        let outline_program = compile_program(&gl, OUTLINE_VERTEX, OUTLINE_FRAGMENT)?;
        let grid_program = compile_program(&gl, GRID_VERTEX, GRID_FRAGMENT)?;
        let grid = MeshGpu::from_cpu(gl.clone(), &grid_mesh())?;
        Ok(Self {
            gl,
            object_program,
            outline_program,
            grid_program,
            grid,
        })
    }

    unsafe fn draw_grid(&self, frame: &FrameData) {
        self.gl.disable(glow::CULL_FACE);
        self.gl.use_program(Some(self.grid_program));
        set_mat4(&self.gl, self.grid_program, "u_model", Mat4::IDENTITY);
        set_mat4(&self.gl, self.grid_program, "u_view", frame.view);
        set_mat4(
            &self.gl,
            self.grid_program,
            "u_projection",
            frame.projection,
        );
        set_f32(&self.gl, self.grid_program, "u_time", frame.elapsed);
        self.gl.bind_vertex_array(Some(self.grid.vertex_array));
        self.gl
            .draw_elements(glow::LINES, self.grid.index_count, glow::UNSIGNED_INT, 0);
        self.gl.bind_vertex_array(None);
        self.gl.enable(glow::CULL_FACE);
    }

    unsafe fn draw_object(&self, object: &SceneObject, selected: bool, frame: &FrameData) {
        let model = object.transform.matrix();
        let palette = frame.style.palette();

        self.gl.cull_face(glow::FRONT);
        self.gl.use_program(Some(self.outline_program));
        set_mat4(&self.gl, self.outline_program, "u_model", model);
        set_mat4(&self.gl, self.outline_program, "u_view", frame.view);
        set_mat4(
            &self.gl,
            self.outline_program,
            "u_projection",
            frame.projection,
        );
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
            if selected { 0.042 } else { 0.026 },
        );
        self.gl.bind_vertex_array(Some(object.mesh.vertex_array));
        self.gl.draw_elements(
            glow::TRIANGLES,
            object.mesh.index_count,
            glow::UNSIGNED_INT,
            0,
        );

        self.gl.cull_face(glow::BACK);
        self.gl.use_program(Some(self.object_program));
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
        set_f32(
            &self.gl,
            self.object_program,
            "u_selected",
            if selected { 1.0 } else { 0.0 },
        );
        self.gl.draw_elements(
            glow::TRIANGLES,
            object.mesh.index_count,
            glow::UNSIGNED_INT,
            0,
        );
        self.gl.bind_vertex_array(None);
    }
}

fn grid_mesh() -> MeshCpu {
    let mut vertices = Vec::new();
    let mut indices = Vec::new();
    let mut push_line = |a: Vec3, b: Vec3, color: [f32; 4]| {
        let base = vertices.len() as u32;
        vertices.push(Vertex {
            position: a.to_array(),
            normal: Vec3::Y.to_array(),
            color,
        });
        vertices.push(Vertex {
            position: b.to_array(),
            normal: Vec3::Y.to_array(),
            color,
        });
        indices.extend_from_slice(&[base, base + 1]);
    };
    for i in -12..=12 {
        let color = if i == 0 {
            [0.95, 0.78, 0.28, 0.9]
        } else {
            [0.24, 0.34, 0.30, 0.45]
        };
        let f = i as f32;
        push_line(Vec3::new(f, 0.0, -12.0), Vec3::new(f, 0.0, 12.0), color);
        push_line(Vec3::new(-12.0, 0.0, f), Vec3::new(12.0, 0.0, f), color);
    }
    MeshCpu {
        name: "editor grid".to_string(),
        vertices,
        indices,
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

unsafe fn set_f32(gl: &glow::Context, program: glow::NativeProgram, name: &str, value: f32) {
    if let Some(location) = gl.get_uniform_location(program, name) {
        gl.uniform_1_f32(Some(&location), value);
    }
}

const OBJECT_VERTEX: &str = r#"#version 330 core
layout (location = 0) in vec3 a_position;
layout (location = 1) in vec3 a_normal;
layout (location = 2) in vec4 a_color;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;

out vec3 v_world_position;
out vec3 v_normal;
out vec4 v_color;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world_position = world.xyz;
    v_normal = normalize(mat3(transpose(inverse(u_model))) * a_normal);
    v_color = a_color;
    gl_Position = u_projection * u_view * world;
}
"#;

const OBJECT_FRAGMENT: &str = r#"#version 330 core
in vec3 v_world_position;
in vec3 v_normal;
in vec4 v_color;

uniform vec3 u_camera_position;
uniform vec3 u_light_direction;
uniform vec3 u_shadow;
uniform vec3 u_mid;
uniform vec3 u_light;
uniform vec3 u_rim;
uniform float u_selected;

out vec4 out_color;

void main() {
    vec3 normal = normalize(v_normal);
    float ndl = dot(normal, normalize(-u_light_direction));
    float ramp = ndl < 0.28 ? 0.0 : (ndl < 0.68 ? 0.58 : 1.0);
    vec3 ramp_color = mix(u_shadow, u_mid, step(0.35, ramp));
    ramp_color = mix(ramp_color, u_light, step(0.9, ramp));

    vec3 view_dir = normalize(u_camera_position - v_world_position);
    float rim = pow(1.0 - max(dot(view_dir, normal), 0.0), 3.0);
    rim = smoothstep(0.34, 0.88, rim);

    vec3 base = pow(v_color.rgb, vec3(0.85));
    vec3 color = base * ramp_color + u_rim * rim * 0.42;
    color += vec3(1.0, 0.82, 0.18) * u_selected * 0.08;
    color = pow(color, vec3(0.92));
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

void main() {
    vec3 pushed = a_position + normalize(a_normal) * u_outline_width;
    gl_Position = u_projection * u_view * u_model * vec4(pushed, 1.0);
}
"#;

const OUTLINE_FRAGMENT: &str = r#"#version 330 core
uniform vec3 u_outline_color;
out vec4 out_color;

void main() {
    out_color = vec4(u_outline_color, 1.0);
}
"#;

const GRID_VERTEX: &str = r#"#version 330 core
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

const GRID_FRAGMENT: &str = r#"#version 330 core
in vec4 v_color;
out vec4 out_color;

void main() {
    out_color = v_color;
}
"#;
