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
