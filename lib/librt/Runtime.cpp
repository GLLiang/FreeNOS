/*
 * Copyright (C) 2009 Niek Linnenbank
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <FreeNOS/System.h>
#include <Types.h>
#include <Macros.h>
#include <Array.h>
#include <FileSystemClient.h>
#include <ChannelClient.h>
#include <PoolAllocator.h>
#include <FileSystemMount.h>
#include <FileDescriptor.h>
#include <MemoryMap.h>
#include <Memory.h>
#include "PageAllocator.h"
#include "KernelLog.h"
#include "Runtime.h"

/** List of constructors. */
extern void (*CTOR_LIST)();

/** List of destructors. */
extern void (*DTOR_LIST)();

/** Initial logging instance */
static KernelLog log;

void * __dso_handle = 0;

extern C void __aeabi_unwind_cpp_pr0()
{
}

extern C int __cxa_atexit(void (*func) (void *),
                          void * arg, void * dso_handle)
{
    return (0);
}

extern C int __aeabi_atexit()
{
    return 0;
}

extern C void __cxa_pure_virtual()
{
}

extern C void __stack_chk_fail(void)
{
}

extern C int raise(int sig)
{
    return 0;
}

void runConstructors()
{
    for (void (**ctor)() = &CTOR_LIST; ctor && *ctor; ctor++)
    {
        (*ctor)();
    }
}

void runDestructors()
{
    for (void (**dtor)() = &DTOR_LIST; dtor && *dtor; dtor++)
    {
        (*dtor)();
    }
}

void setupHeap()
{
    Arch::MemoryMap map;
    Memory::Range heap = map.range(MemoryMap::UserHeap);
    PageAllocator *pageAlloc;
    PoolAllocator *poolAlloc;
    const Allocator::Range pageRange = { heap.virt, heap.size, PAGESIZE };

    // Allocate one page to store the allocators themselves
    Memory::Range range;
    range.size   = PAGESIZE;
    range.access = Memory::User | Memory::Readable | Memory::Writable;
    range.virt   = heap.virt;
    range.phys   = ZERO;
    const API::Result result = VMCtl(SELF, MapContiguous, &range);
    if (result != API::Success)
    {
        PrivExec(WriteConsole, (Address) ("failed to allocate pages for heap: terminating"));
        ProcessCtl(SELF, KillPID);
    }

    // Allocate instance copy on vm pages itself
    pageAlloc = new (heap.virt) PageAllocator(pageRange);
    poolAlloc = new (heap.virt + sizeof(PageAllocator)) PoolAllocator(pageAlloc);

    // Set default allocator
    Allocator::setDefault(poolAlloc);
}

void setupChannels()
{
    ChannelClient *client = new ChannelClient();
    (void) client;

    ChannelClient::instance->setRegistry(new ChannelRegistry());
}

void setupMappings()
{
    FileSystemClient filesystem;

    // Map user program arguments
    Arch::MemoryMap map;
    Memory::Range argRange = map.range(MemoryMap::UserArgs);

    // First page is the argc+argv (skip here)
    // Second page is the current working directory
    filesystem.setCurrentDirectory(new String((char *) argRange.virt + PAGESIZE, false));

    // Third page and above contain the file descriptors table
    setFiles((FileDescriptor *) (argRange.virt + (PAGESIZE * 2)));

    // Inherit file descriptors table from parent (if any).
    // Without a parent, just clear the file descriptors
    if (ProcessCtl(SELF, GetParent) == 0)
    {
        MemoryBlock::set(getFiles(), 0, argRange.size - (PAGESIZE * 2));
        filesystem.setCurrentDirectory(String("/"));
    }
}

extern C void SECTION(".entry") _entry()
{
    int ret, argc;
    char *arguments;
    char **argv;
    SystemInformation info;
    Arch::MemoryMap map;

    // Clear BSS
    clearBSS();

    // Setup the heap, C++ constructors and default mounts
    setupHeap();
    runConstructors();
    setupChannels();
    setupMappings();

    // Allocate buffer for arguments
    argc = 0;
    argv = new char*[ARGV_COUNT];
    arguments = (char *) map.range(MemoryMap::UserArgs).virt;

    // Fill in arguments list
    while (argc < ARGV_COUNT && *arguments)
    {
        argv[argc] = arguments;
        arguments += ARGV_SIZE;
        argc++;
    }

    // Pass control to the program
    ret = main(argc, argv);

    // Terminate execution
    runDestructors();
    ProcessCtl(SELF, KillPID, ret);
}
