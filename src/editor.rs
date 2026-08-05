use super::*;

pub(crate) fn editor_ui(ctx: &egui::Context, app: &mut App) {
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
            ui.checkbox(&mut app.snap_to_grid, "Snap");
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
            ui.horizontal(|ui| {
                ui.menu_button("Add Object", |ui| {
                    if ui.button("Cube").clicked() {
                        app.add_primitive(PrimitiveKind::Cube);
                        ui.close_menu();
                    }
                    if ui.button("Plane").clicked() {
                        app.add_primitive(PrimitiveKind::Plane);
                        ui.close_menu();
                    }
                    if ui.button("Sphere").clicked() {
                        app.add_primitive(PrimitiveKind::Sphere);
                        ui.close_menu();
                    }
                });
                ui.menu_button("Add Light", |ui| {
                    if ui.button("Directional").clicked() {
                        app.add_light(LightKind::Directional);
                        ui.close_menu();
                    }
                    if ui.button("Point").clicked() {
                        app.add_light(LightKind::Point);
                        ui.close_menu();
                    }
                    if ui.button("Spot").clicked() {
                        app.add_light(LightKind::Spot);
                        ui.close_menu();
                    }
                });
                if ui.button("Delete").clicked() {
                    app.delete_selection();
                }
            });
            ui.separator();
            ui.collapsing("Actors", |ui| {
                for index in 0..app.scene.actors.len() {
                    let actor = &app.scene.actors[index];
                    let selected = app.scene.selected_actor == Some(index);
                    let label = format!("{} [{}]", actor.name, actor.kind_name());
                    if ui.selectable_label(selected, label).clicked() {
                        app.scene.select_actor(index);
                    }
                }
            });
        });

    egui::SidePanel::right("details")
        .default_width(320.0)
        .show(ctx, |ui| {
            ui.heading("Details");
            ui.separator();
            if let Some(actor) = app.scene.selected_actor_mut() {
                actor_details(ui, actor);
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

fn actor_details(ui: &mut egui::Ui, actor: &mut Actor) {
    ui.label(format!("Actor: {}", actor.name));
    ui.label(format!("Type: {}", actor.kind_name()));
    ui.separator();
    ui.label("Transform");
    vec3_drag(
        ui,
        "Position",
        &mut actor.root.local_transform.translation,
        0.05,
    );
    let mut rotation_degrees = Vec3::new(
        actor.root.local_transform.rotation.x.to_degrees(),
        actor.root.local_transform.rotation.y.to_degrees(),
        actor.root.local_transform.rotation.z.to_degrees(),
    );
    if vec3_drag(ui, "Rotation", &mut rotation_degrees, 0.4) {
        actor.root.local_transform.rotation = Vec3::new(
            rotation_degrees.x.to_radians(),
            rotation_degrees.y.to_radians(),
            rotation_degrees.z.to_radians(),
        );
    }
    if actor.static_mesh_component().is_some() {
        vec3_drag(ui, "Scale", &mut actor.root.local_transform.scale, 0.02);
    }

    for component in &mut actor.components {
        match component {
            Component::StaticMesh(mesh) => mesh_details(ui, &mut mesh.parts),
            Component::Light(light) => light_details(ui, &mut light.light),
            Component::Script(_) => {}
            Component::Audio(_) => {}
            Component::Animation(_) => {}
            Component::Input(_) => {}
        }
    }
}

fn light_details(ui: &mut egui::Ui, light: &mut Light) {
    ui.separator();
    ui.label("Light");
    ui.checkbox(&mut light.enabled, "Enabled");
    let mut color = [light.color.x, light.color.y, light.color.z];
    if ui.color_edit_button_rgb(&mut color).changed() {
        light.color = Vec3::new(color[0], color[1], color[2]);
    }
    ui.add(egui::Slider::new(&mut light.intensity, 0.0..=4.0).text("Intensity"));
    if !matches!(light.kind, LightKind::Directional) {
        ui.add(egui::Slider::new(&mut light.range, 0.1..=20.0).text("Range"));
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

fn mesh_details(ui: &mut egui::Ui, parts: &mut [MeshPart]) {
    ui.separator();
    ui.label("Static Mesh");
    ui.label(format!("Mesh parts: {}", parts.len()));
    if parts.len() > 1 {
        ui.collapsing("Parts", |ui| {
            for part in parts.iter() {
                ui.label(&part.name);
            }
        });
    }
    if let Some(material) = parts.first().map(|part| part.mesh.material) {
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
        ui.label("Applies to all mesh parts in this actor.");
        if changed {
            for part in parts {
                part.mesh.material = material;
            }
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
