// Copyright 2026 Anuj Attri
// SPDX-License-Identifier: Apache-2.0
//
// Thin C ABI wrapper around the `pco` crate for use from C++.
// Exported symbols: pco_compress_f64, pco_decompress_f64, pco_free, pco_free_f64.

use std::slice;

/// Compress n f64 values. Sets *out_ptr and *out_len on success. Returns 0 on success, 1 on error.
/// Caller must free *out_ptr with pco_free(*out_ptr, *out_len).
#[no_mangle]
pub unsafe extern "C" fn pco_compress_f64(
    data_ptr: *const f64,
    n: usize,
    out_ptr: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    if data_ptr.is_null() || n == 0 { return 1; }
    let data = slice::from_raw_parts(data_ptr, n);
    let config = pco::ChunkConfig::default().with_compression_level(pco::DEFAULT_COMPRESSION_LEVEL);
    match pco::standalone::simple_compress(data, &config) {
        Ok(compressed) => {
            let mut boxed = compressed.into_boxed_slice();
            let len = boxed.len();
            let ptr = boxed.as_mut_ptr();
            std::mem::forget(boxed);
            *out_ptr = ptr;
            *out_len = len;
            0
        }
        Err(_) => 1,
    }
}

/// Decompress bytes into f64 values. Sets *out_ptr and *out_n on success. Returns 0 on success.
/// Caller must free *out_ptr with pco_free_f64(*out_ptr, *out_n).
#[no_mangle]
pub unsafe extern "C" fn pco_decompress_f64(
    compressed_ptr: *const u8,
    compressed_len: usize,
    out_ptr: *mut *mut f64,
    out_n: *mut usize,
) -> i32 {
    if compressed_ptr.is_null() || compressed_len == 0 { return 1; }
    let compressed = slice::from_raw_parts(compressed_ptr, compressed_len);
    match pco::standalone::simple_decompress::<f64>(compressed) {
        Ok(values) => {
            let mut boxed = values.into_boxed_slice();
            let n = boxed.len();
            let ptr = boxed.as_mut_ptr();
            std::mem::forget(boxed);
            *out_ptr = ptr;
            *out_n   = n;
            0
        }
        Err(_) => 1,
    }
}

/// Free a byte buffer allocated by pco_compress_f64.
#[no_mangle]
pub unsafe extern "C" fn pco_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() && len > 0 {
        drop(Box::from_raw(slice::from_raw_parts_mut(ptr, len)));
    }
}

/// Free an f64 buffer allocated by pco_decompress_f64.
#[no_mangle]
pub unsafe extern "C" fn pco_free_f64(ptr: *mut f64, n: usize) {
    if !ptr.is_null() && n > 0 {
        drop(Box::from_raw(slice::from_raw_parts_mut(ptr, n)));
    }
}
