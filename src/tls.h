#pragma once

#include "pe_loader.h"

// Implicit TLS (__declspec(thread) / thread_local) for a manually mapped image.
//
// Windows only sets up implicit TLS for modules the OS loader knows about, so we:
//  1. pick a free slot index past every slot the OS loader handed out,
//  2. write it into the image's _tls_index,
//  3. give the current thread (and every thread created later, via the host's own
//     TLS callback) a ThreadLocalStoragePointer array that includes our block,
//  4. run the image's TLS callbacks with the right reason codes.
bool tls_setup(LoadedImage& img);

// Runs the image's TLS callbacks with DLL_PROCESS_ATTACH on the current thread.
void tls_run_process_attach();
