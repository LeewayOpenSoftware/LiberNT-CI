//! Build the vendored C special-pool engine (Dwight's `special.c`) for the LRM-RS differential
//! fuzz targets, so each target can call the C engine as the oracle beside the Rust engine.
//!
//! `special.c` is compiled UNMODIFIED against the host `poolenv.h` stand-in, assembled into the
//! same `include/{poolspecial.h,poolenv.h}` + `special/special.c` layout that
//! `lrm-rs/difftest/run-lrm-diff.sh` uses, so `special.c`'s relative `../include/poolspecial.h`
//! and `poolspecial.h`'s `poolenv.h` resolve. Pinned to the vendored copies under
//! `lrm-rs/difftest/dw`, not the CI-fetched engine source (which pulls in `<ntoskrnl.h>`).

use std::{env, fs, path::PathBuf};

fn main() {
    // Only the LRM-RS differential targets need the C engine; the lnrspoc targets do not. Building
    // the C object unconditionally is harmless (the other targets just never reference its symbols).
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let dw = manifest.join("../lrm-rs/difftest");
    let special_c = dw.join("dw/special/special.c");
    let poolspecial_h = dw.join("dw/include/poolspecial.h");
    let poolenv_h = dw.join("poolenv.h");

    // If the vendored engine is absent (e.g. a sparse checkout), skip quietly; the LRM-RS targets
    // will fail to link, which is the right signal, while lnrspoc targets keep building.
    if !special_c.exists() {
        println!("cargo:warning=lrm-rs vendored special.c not found at {}; LRM-RS fuzz targets will not link", special_c.display());
        return;
    }

    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let inc = out.join("cref/include");
    let src = out.join("cref/special");
    fs::create_dir_all(&inc).unwrap();
    fs::create_dir_all(&src).unwrap();
    fs::copy(&poolspecial_h, inc.join("poolspecial.h")).unwrap();
    fs::copy(&poolenv_h, inc.join("poolenv.h")).unwrap();
    fs::copy(&special_c, src.join("special.c")).unwrap();

    cc::Build::new()
        .file(src.join("special.c"))
        .include(&inc)
        .flag_if_supported("-std=c11")
        .warnings(true)
        .compile("lrm_special_cref");

    for p in [&special_c, &poolspecial_h, &poolenv_h] {
        println!("cargo:rerun-if-changed={}", p.display());
    }
}
