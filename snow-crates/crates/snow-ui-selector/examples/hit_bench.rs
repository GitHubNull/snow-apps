//! Windows-only hit-testing benchmark; the workload uses Win32 windows.

#[cfg(windows)]
#[path = "hit_bench_windows.rs"]
mod workload;

#[cfg(windows)]
fn main() -> snow_ui_selector::SelectorResult<()> {
    workload::main()
}

#[cfg(not(windows))]
fn main() {
    eprintln!("hit_bench requires Windows");
}
