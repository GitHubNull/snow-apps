//! Windows-only overlay probe; the workload uses Win32 layers and input.

#[cfg(windows)]
#[path = "overlay_windows.rs"]
mod workload;

#[cfg(windows)]
fn main() -> snow_ui_selector::SelectorResult<()> {
    workload::main()
}

#[cfg(not(windows))]
fn main() {
    eprintln!("overlay requires Windows");
}
