#[allow(dead_code)]
pub(crate) struct Engine {
    pub input: InputSystem,
    pub assets: AssetSystem,
    pub audio: AudioSystem,
    pub animation: AnimationSystem,
    pub scripting: ScriptingSystem,
    pub physics: PhysicsSystem,
    pub collision: CollisionSystem,
    pub navigation: NavigationSystem,
    pub ai: AiSystem,
    pub ui: UiSystem,
    pub networking: NetworkingSystem,
    pub save_games: SaveGameSystem,
    pub localization: LocalizationSystem,
    pub diagnostics: DiagnosticsSystem,
    pub post_process: PostProcessSystem,
    pub settings: EngineSettings,
}

impl Default for Engine {
    fn default() -> Self {
        Self {
            input: InputSystem,
            assets: AssetSystem,
            audio: AudioSystem,
            animation: AnimationSystem,
            scripting: ScriptingSystem,
            physics: PhysicsSystem,
            collision: CollisionSystem,
            navigation: NavigationSystem,
            ai: AiSystem,
            ui: UiSystem,
            networking: NetworkingSystem,
            save_games: SaveGameSystem,
            localization: LocalizationSystem,
            diagnostics: DiagnosticsSystem,
            post_process: PostProcessSystem,
            settings: EngineSettings::default(),
        }
    }
}

#[allow(dead_code)]
pub(crate) struct InputSystem;

#[allow(dead_code)]
pub(crate) struct AssetSystem;

#[allow(dead_code)]
pub(crate) struct AudioSystem;

#[allow(dead_code)]
pub(crate) struct AnimationSystem;

#[allow(dead_code)]
pub(crate) struct ScriptingSystem;

#[allow(dead_code)]
pub(crate) struct PhysicsSystem;

#[allow(dead_code)]
pub(crate) struct CollisionSystem;

#[allow(dead_code)]
pub(crate) struct NavigationSystem;

#[allow(dead_code)]
pub(crate) struct AiSystem;

#[allow(dead_code)]
pub(crate) struct UiSystem;

#[allow(dead_code)]
pub(crate) struct NetworkingSystem;

#[allow(dead_code)]
pub(crate) struct SaveGameSystem;

#[allow(dead_code)]
pub(crate) struct LocalizationSystem;

#[allow(dead_code)]
pub(crate) struct DiagnosticsSystem;

#[allow(dead_code)]
pub(crate) struct PostProcessSystem;

#[allow(dead_code)]
pub(crate) struct EngineSettings {
    pub project_name: String,
    pub target_frame_rate: Option<u32>,
}

impl Default for EngineSettings {
    fn default() -> Self {
        Self {
            project_name: "ZZZ Style Engine Prototype".to_string(),
            target_frame_rate: None,
        }
    }
}
