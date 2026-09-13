// SPDX-License-Identifier: MIT
// AfriyieOS — ELF64 loader

#ifndef AFRIYIE_ELF_H
#define AFRIYIE_ELF_H

#include "types.h"
#include "status.h"
#include "fs.h"

// Loads an ELF64 executable from a FAT32 volume into the current address space
// and returns its entry point.
//
// The segments are mapped with the permissions the file asks for, so a
// non-writable text segment really is non-writable once running.
af_status_t elf_load(fat32_volume_t *vol, const char *path,
                     af_vaddr *out_entry, af_vaddr *out_stack_top);

// Loads an ELF and enters it in ring 3. Never returns to the caller: the
// program's only way back is a system call, and its exit terminates the thread.
void elf_exec(fat32_volume_t *vol, const char *path);

#endif // AFRIYIE_ELF_H
