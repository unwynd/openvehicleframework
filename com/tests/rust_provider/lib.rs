// SPDX-License-Identifier: Apache-2.0

#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_void};
use std::ptr;

const OK: i32 = 0;
const INVALID_ARGUMENT: i32 = 1;
const INVALID_STATE: i32 = 4;
const UNSUPPORTED: i32 = 6;
const ABI_V1: u32 = 1;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct StringView {
    data: *const c_char,
    size: usize,
}
#[repr(C)]
pub struct HostApi {
    struct_size: u32,
    user: *mut c_void,
    log: *const c_void,
    dispatch: *const c_void,
    monotonic_time: *const c_void,
}
#[repr(C)]
pub struct Config {
    struct_size: u32,
    instance: StringView,
    configuration: StringView,
    max_endpoints: u32,
    max_operations: u32,
}
#[repr(C)]
pub struct Capabilities {
    struct_size: u32,
    features: u64,
    isolation: i32,
    max_endpoints: u32,
    max_subscriptions: u32,
    max_operations: u32,
    max_iovecs: u32,
    max_payload: u64,
    max_loan: u64,
    max_history: u32,
}

type Unary = unsafe extern "C" fn(*mut Transport) -> i32;
type Caps = unsafe extern "C" fn(*mut Transport, *mut Capabilities) -> i32;
type F3 = unsafe extern "C" fn(*mut Transport, *const c_void, *mut u64) -> i32;
type F5 = unsafe extern "C" fn(*mut Transport, u64, *const c_void, *mut c_void, *mut u64) -> i32;
type H1 = unsafe extern "C" fn(*mut Transport, u64) -> i32;
type Pub = unsafe extern "C" fn(*mut Transport, u64, Bytes) -> i32;
type IovPub = unsafe extern "C" fn(*mut Transport, u64, *const c_void, usize) -> i32;
type Loan = unsafe extern "C" fn(*mut Transport, u64, usize, *mut c_void) -> i32;
type LoanPub = unsafe extern "C" fn(*mut Transport, u64, u64, usize) -> i32;
type Req = unsafe extern "C" fn(
    *mut Transport,
    u64,
    Bytes,
    u64,
    *const c_void,
    *mut c_void,
    *mut u64,
) -> i32;
type Handler = unsafe extern "C" fn(*mut Transport, u64, *const c_void, *mut c_void) -> i32;
type Respond = unsafe extern "C" fn(*mut Transport, u64, i32, Bytes) -> i32;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct Bytes {
    data: *const u8,
    size: usize,
}

#[repr(C)]
pub struct Transport {
    struct_size: u32,
    abi_version: u32,
    implementation: *mut c_void,
    name: StringView,
    start: Option<Unary>,
    stop: Option<Unary>,
    capabilities: Option<Caps>,
    watch_start: Option<F5>,
    watch_stop: Option<H1>,
    endpoint_create: Option<F3>,
    endpoint_destroy: Option<H1>,
    subscribe: Option<F5>,
    unsubscribe: Option<H1>,
    publish: Option<Pub>,
    publish_iov: Option<IovPub>,
    loan_acquire: Option<Loan>,
    loan_publish: Option<LoanPub>,
    loan_release: Option<H1>,
    request: Option<Req>,
    cancel: Option<H1>,
    set_handler: Option<Handler>,
    respond: Option<Respond>,
}

#[repr(C)]
pub struct Factory {
    struct_size: u32,
    abi_version: u32,
    name: StringView,
    create: Option<unsafe extern "C" fn(*const HostApi, *const Config, *mut *mut Transport) -> i32>,
    destroy: Option<unsafe extern "C" fn(*mut Transport)>,
}

struct State {
    api: Transport,
    running: bool,
}
static NAME: &[u8] = b"rust-conformance";

unsafe fn state<'a>(transport: *mut Transport) -> &'a mut State {
    &mut *((*transport).implementation as *mut State)
}
unsafe extern "C" fn start(transport: *mut Transport) -> i32 {
    let state = state(transport);
    if state.running {
        INVALID_STATE
    } else {
        state.running = true;
        OK
    }
}
unsafe extern "C" fn stop(transport: *mut Transport) -> i32 {
    state(transport).running = false;
    OK
}
unsafe extern "C" fn capabilities(_: *mut Transport, out: *mut Capabilities) -> i32 {
    if out.is_null() || (*out).struct_size < std::mem::size_of::<Capabilities>() as u32 {
        return INVALID_ARGUMENT;
    }
    *out = Capabilities {
        struct_size: std::mem::size_of::<Capabilities>() as u32,
        features: 0,
        isolation: 0,
        max_endpoints: 1,
        max_subscriptions: 0,
        max_operations: 0,
        max_iovecs: 1,
        max_payload: 0,
        max_loan: 0,
        max_history: 0,
    };
    OK
}
unsafe extern "C" fn h1(_: *mut Transport, _: u64) -> i32 {
    UNSUPPORTED
}
unsafe extern "C" fn f3(_: *mut Transport, _: *const c_void, _: *mut u64) -> i32 {
    UNSUPPORTED
}
unsafe extern "C" fn f5(
    _: *mut Transport,
    _: u64,
    _: *const c_void,
    _: *mut c_void,
    _: *mut u64,
) -> i32 {
    UNSUPPORTED
}
unsafe extern "C" fn publish(_: *mut Transport, _: u64, _: Bytes) -> i32 {
    UNSUPPORTED
}
unsafe extern "C" fn publish_iov(_: *mut Transport, _: u64, _: *const c_void, _: usize) -> i32 {
    UNSUPPORTED
}
unsafe extern "C" fn loan(_: *mut Transport, _: u64, _: usize, _: *mut c_void) -> i32 {
    UNSUPPORTED
}
unsafe extern "C" fn loan_publish(_: *mut Transport, _: u64, _: u64, _: usize) -> i32 {
    UNSUPPORTED
}
unsafe extern "C" fn request(
    _: *mut Transport,
    _: u64,
    _: Bytes,
    _: u64,
    _: *const c_void,
    _: *mut c_void,
    _: *mut u64,
) -> i32 {
    UNSUPPORTED
}
unsafe extern "C" fn handler(_: *mut Transport, _: u64, _: *const c_void, _: *mut c_void) -> i32 {
    UNSUPPORTED
}
unsafe extern "C" fn respond(_: *mut Transport, _: u64, _: i32, _: Bytes) -> i32 {
    UNSUPPORTED
}

unsafe extern "C" fn create(
    host: *const HostApi,
    config: *const Config,
    out: *mut *mut Transport,
) -> i32 {
    if host.is_null() || config.is_null() || out.is_null() {
        return INVALID_ARGUMENT;
    }
    let mut state = Box::new(State {
        api: Transport {
            struct_size: std::mem::size_of::<Transport>() as u32,
            abi_version: ABI_V1,
            implementation: ptr::null_mut(),
            name: StringView {
                data: NAME.as_ptr() as *const c_char,
                size: NAME.len(),
            },
            start: Some(start),
            stop: Some(stop),
            capabilities: Some(capabilities),
            watch_start: Some(f5),
            watch_stop: Some(h1),
            endpoint_create: Some(f3),
            endpoint_destroy: Some(h1),
            subscribe: Some(f5),
            unsubscribe: Some(h1),
            publish: Some(publish),
            publish_iov: Some(publish_iov),
            loan_acquire: Some(loan),
            loan_publish: Some(loan_publish),
            loan_release: Some(h1),
            request: Some(request),
            cancel: Some(h1),
            set_handler: Some(handler),
            respond: Some(respond),
        },
        running: false,
    });
    let raw: *mut State = &mut *state;
    state.api.implementation = raw as *mut c_void;
    *out = &mut state.api;
    let _ = Box::into_raw(state);
    OK
}
unsafe extern "C" fn destroy(transport: *mut Transport) {
    if !transport.is_null() {
        drop(Box::from_raw((*transport).implementation as *mut State));
    }
}

static FACTORY: Factory = Factory {
    struct_size: std::mem::size_of::<Factory>() as u32,
    abi_version: ABI_V1,
    name: StringView {
        data: NAME.as_ptr() as *const c_char,
        size: NAME.len(),
    },
    create: Some(create),
    destroy: Some(destroy),
};
unsafe impl Sync for Factory {}

#[no_mangle]
pub extern "C" fn ovf_com_rust_conformance_query_v1() -> *const Factory {
    &FACTORY
}
