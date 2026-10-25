use std::{
    ffi::{CStr, CString, c_char},
    panic::{AssertUnwindSafe, catch_unwind},
    slice,
    sync::{
        Arc, RwLock,
        atomic::{AtomicBool, Ordering},
    },
};

use adblock::{
    Engine,
    lists::{FilterSet, ParseOptions, RuleTypes},
    request::Request,
    resources::{PermissionMask, Resource},
};

// Permission bit 0 is trusted UBO scripets
// uBOs yt rules use trusted-replace-fetch-response etc.
// Lists must be parsed with it
// All lists are in 1 buffer, so it applies to all lists in config
// Add only lists that you would trust in UBO
const TRUSTED: PermissionMask = PermissionMask::from_bits(0b1);

// Engine is shared between loader and GUI thread
// This only works with single-thread off
// Assert that the engine has send and sync
const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<Engine>();
};

pub struct FbbAdblock {
    ready: AtomicBool,
    engine: RwLock<Option<Arc<Engine>>>,
}

unsafe fn s<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        None
    } else {
        unsafe { CStr::from_ptr(p).to_str().ok() }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn fbb_adblock_new() -> *mut FbbAdblock {
    Box::into_raw(Box::new(FbbAdblock {
        ready: AtomicBool::new(false),
        engine: RwLock::new(None),
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fbb_adblock_load(
    h: *const FbbAdblock,
    rules: *const c_char,
    len: usize,
    resources: *const c_char,
    resources_len: usize,
) -> bool {
    let Some(h) = (unsafe { h.as_ref() }) else {
        return false;
    };
    if rules.is_null() {
        return false;
    }
    // Caller guarantees that rules points at len amount of readable bytes
    let bytes = unsafe { slice::from_raw_parts(rules.cast::<u8>(), len) };
    // Same for optional resources buff
    let res_bytes: &[u8] = if resources.is_null() || resources_len == 0 {
        Default::default()
    } else {
        unsafe { slice::from_raw_parts(resources.cast::<u8>(), resources_len) }
    };

    // Parser panic must not unwind into C++ and abort browser
    let built = catch_unwind(AssertUnwindSafe(|| {
        let text = String::from_utf8_lossy(bytes).into_owned();
        let mut fs = FilterSet::new(false);
        // Cosmetic and scriptlet rules must stay
        // YT ads are served by same hosts as the video and removed by scriplets that edit player resp
        fs.add_filter_list(
            text,
            ParseOptions {
                rule_types: RuleTypes::All,
                permissions: TRUSTED,
                ..ParseOptions::default()
            },
        );
        let mut engine = Engine::new_with_filter_set(fs);
        if !res_bytes.is_empty() {
            match serde_json::from_slice::<Vec<Resource>>(res_bytes) {
                Ok(list) => engine.use_resources(list),
                Err(e) => eprintln!("fbb_adblock: ignoring unreadable resources: {e}"),
            }
        }
        engine
    }));
    let Ok(engine) = built else {
        return false;
    };

    let old = {
        let mut slot = h.engine.write().unwrap_or_else(|e| e.into_inner());
        slot.replace(Arc::new(engine))
    };
    h.ready.store(true, Ordering::Release);
    // Wait for prev engine after lcok released so queries dont wait for dealloc
    drop(old);
    true
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fbb_adblock_ready(h: *const FbbAdblock) -> bool {
    unsafe { h.as_ref() }.is_some_and(|h| h.ready.load(Ordering::Acquire))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fbb_adblock_should_block(
    h: *const FbbAdblock,
    url: *const c_char,
    source: *const c_char,
    ty: *const c_char,
) -> bool {
    let Some(h) = (unsafe { h.as_ref() }) else {
        return false;
    };
    if !h.ready.load(Ordering::Acquire) {
        return false;
    }
    let (Some(url), Some(source), Some(ty)) =
        (unsafe { s(url) }, unsafe { s(source) }, unsafe { s(ty) })
    else {
        return false;
    };
    let Ok(req) = Request::new(url, source, ty, "GET") else {
        return false;
    };

    // Clone arc and drop guard, loading swapping in new engine should never wait for a query and
    // vice versa
    let engine = h.engine.read().unwrap_or_else(|e| e.into_inner()).clone();
    let Some(engine) = engine else {
        return false;
    };

    catch_unwind(AssertUnwindSafe(|| {
        engine.check_network_request(&req).should_block()
    }))
    .unwrap_or(false)
}

// Wraps the scriptlets and the hide selector CSS of one page into a single script.
fn page_script(injected: &str, css: &str) -> String {
    let mut out = String::with_capacity(injected.len() + css.len() + 512);
    out.push_str(injected);
    out.push('\n');
    if !css.is_empty() {
        // document.documentElement does not exist at doc creation so wait with a mutation observer
        // JSON string is a valid JS literal
        let lit = serde_json::to_string(css).unwrap_or_else(|_| "\"\"".to_owned());
        out.push_str("(function(){var css=");
        out.push_str(&lit);
        out.push_str(
            ";var d=document,done=false;\
             function add(){if(done)return;var p=d.head||d.documentElement;if(!p)return;\
             done=true;var s=d.createElement('style');s.textContent=css;p.appendChild(s);}\
             add();if(!done){var mo=new MutationObserver(function(){add();if(done)mo.disconnect();});\
             mo.observe(d,{childList:true,subtree:true});}})();",
        );
    }
    out
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fbb_adblock_page_script(
    h: *const FbbAdblock,
    url: *const c_char,
) -> *mut c_char {
    let none = std::ptr::null_mut();
    let Some(h) = (unsafe { h.as_ref() }) else {
        return none;
    };
    let Some(url) = (unsafe { s(url) }) else {
        return none;
    };
    if !h.ready.load(Ordering::Acquire) {
        return none;
    }
    let engine = h.engine.read().unwrap_or_else(|e| e.into_inner()).clone();
    let Some(engine) = engine else {
        return none;
    };

    let script = catch_unwind(AssertUnwindSafe(|| {
        let res = engine.url_cosmetic_resources(url);
        // Only 1 rule per selector so then invalid selector drops only self instead of whole list
        // Procedural filters unhandled
        let mut sels: Vec<&String> = res.hide_selectors.iter().collect();
        sels.sort();
        let mut css = String::new();
        for sel in sels {
            css.push_str(sel);
            css.push_str("{display:none!important}\n");
        }
        if res.injected_script.is_empty() && css.is_empty() {
            return String::new();
        }
        page_script(&res.injected_script, &css)
    }))
    .unwrap_or_default();

    if script.is_empty() {
        return none;
    }
    CString::new(script).map_or(none, CString::into_raw)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fbb_adblock_string_free(p: *mut c_char) {
    if !p.is_null() {
        unsafe {
            drop(CString::from_raw(p));
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn fbb_adblock_free(h: *mut FbbAdblock) {
    if !h.is_null() {
        unsafe {
            drop(Box::from_raw(h));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    fn blocked(h: *const FbbAdblock, url: &str, source: &str, ty: &str) -> bool {
        let (url, source, ty) = (
            CString::new(url).unwrap(),
            CString::new(source).unwrap(),
            CString::new(ty).unwrap(),
        );
        unsafe { fbb_adblock_should_block(h, url.as_ptr(), source.as_ptr(), ty.as_ptr()) }
    }

    fn load(h: *const FbbAdblock, rules: &str) -> bool {
        unsafe { fbb_adblock_load(h, rules.as_ptr().cast(), rules.len(), std::ptr::null(), 0) }
    }

    #[test]
    fn never_blocks_before_ready_then_matches_and_swaps() {
        let h = fbb_adblock_new();
        assert!(!unsafe { fbb_adblock_ready(h) });
        assert!(!blocked(
            h,
            "https://ads.example.com/a.js",
            "https://site.test/",
            "script"
        ));

        assert!(load(
            h,
            "||ads.example.com^$third-party\n@@||ads.example.com/ok.js\n||t.test^$script\n"
        ));
        assert!(unsafe { fbb_adblock_ready(h) });

        // plain match, exception, first-party exemption, resource-type option
        assert!(blocked(
            h,
            "https://ads.example.com/a.js",
            "https://site.test/",
            "script"
        ));
        assert!(!blocked(
            h,
            "https://ads.example.com/ok.js",
            "https://site.test/",
            "script"
        ));
        assert!(!blocked(
            h,
            "https://ads.example.com/a.js",
            "https://ads.example.com/",
            "script"
        ));
        assert!(blocked(
            h,
            "https://t.test/x.js",
            "https://site.test/",
            "script"
        ));
        assert!(!blocked(
            h,
            "https://t.test/x.png",
            "https://site.test/",
            "image"
        ));

        // Replacing the rule set must drop the old rules.
        assert!(load(h, "||other.test^\n"));
        assert!(!blocked(
            h,
            "https://ads.example.com/a.js",
            "https://site.test/",
            "script"
        ));
        assert!(blocked(
            h,
            "https://other.test/a.js",
            "https://site.test/",
            "script"
        ));

        unsafe { fbb_adblock_free(h) };
    }

    #[test]
    fn stray_bytes_and_cosmetic_rules_do_not_discard_valid_rules() {
        let h = fbb_adblock_new();
        let rules = b"! comment\n##.cosmetic\n\xff\xfe broken\n||bad.test^\n".to_vec();
        assert!(unsafe {
            fbb_adblock_load(h, rules.as_ptr().cast(), rules.len(), std::ptr::null(), 0)
        });
        assert!(blocked(
            h,
            "https://bad.test/x",
            "https://site.test/",
            "script"
        ));
        unsafe { fbb_adblock_free(h) };
    }

    // Not run when this was written (no toolchain available): run `cargo test` once.
    #[test]
    fn scriptlet_and_hide_rules_become_a_page_script() {
        let h = fbb_adblock_new();
        let rules = "example.com##+js(fbb-test, hello)\nexample.com###ad-slot\n";
        // template resource: window.__fbb = "{{1}}";
        let res = r#"[{"name":"fbb-test.js","aliases":["fbb-test"],"kind":"template","content":"d2luZG93Ll9fZmJiID0gInt7MX19Ijs="}]"#;
        assert!(unsafe {
            fbb_adblock_load(
                h,
                rules.as_ptr().cast(),
                rules.len(),
                res.as_ptr().cast(),
                res.len(),
            )
        });
        let url = CString::new("https://www.example.com/").unwrap();
        let p = unsafe { fbb_adblock_page_script(h, url.as_ptr()) };
        assert!(!p.is_null());
        let js = unsafe { CStr::from_ptr(p) }.to_str().unwrap().to_owned();
        unsafe { fbb_adblock_string_free(p) };
        assert!(js.contains("hello"), "scriptlet missing: {js}");
        assert!(js.contains("#ad-slot"), "hide rule missing: {js}");

        let other = CString::new("https://other.test/").unwrap();
        assert!(unsafe { fbb_adblock_page_script(h, other.as_ptr()) }.is_null());
        unsafe { fbb_adblock_free(h) };
    }

    #[test]
    fn null_and_garbage_arguments_are_safe() {
        assert!(!unsafe { fbb_adblock_ready(std::ptr::null()) });
        assert!(!unsafe {
            fbb_adblock_load(std::ptr::null(), std::ptr::null(), 0, std::ptr::null(), 0)
        });
        let h = fbb_adblock_new();
        assert!(!unsafe { fbb_adblock_load(h, std::ptr::null(), 0, std::ptr::null(), 0) });
        assert!(unsafe { fbb_adblock_page_script(h, std::ptr::null()) }.is_null());
        unsafe { fbb_adblock_string_free(std::ptr::null_mut()) };
        assert!(!blocked(h, "not a url", "", "script"));
        unsafe { fbb_adblock_free(h) };
        unsafe { fbb_adblock_free(std::ptr::null_mut()) };
    }
}
