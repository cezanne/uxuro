/*******************************************************************************
    Copyright (c) 2015-2019 NVIDIA Corporation

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

*******************************************************************************/

#ifndef __UVM8_VA_SPACE_H__
#define __UVM8_VA_SPACE_H__

#include "uvm8_processors.h"
#include "uvm8_global.h"
#include "uvm8_gpu.h"
#include "uvm8_range_tree.h"
#include "uvm8_range_group.h"
#include "uvm8_forward_decl.h"
#include "uvm8_mmu.h"
#include "uvm_linux.h"
#include "uvm_common.h"
#include "nv-kref.h"
#include "nv-linux.h"
#include "uvm8_perf_events.h"
#include "uvm8_perf_module.h"
#include "uvm8_va_block_types.h"
#include "uvm8_va_block.h"
#include "uvm8_hmm.h"
#include "uvm8_test_ioctl.h"
#include "uvm8_ats_ibm.h"
#include "uvm8_va_space_mm.h"

typedef struct uvm_nvmgpu_va_space_t
{
    bool is_initailized;

    // number of blocks to be trashed at a time
    unsigned long trash_nr_blocks; 
    // number of pages reserved for the system 
    unsigned long trash_reserved_nr_pages; 
    // init flags that dictate the optimization behaviors
    unsigned short flags;

    // pending fd for dragon map
    int fd_pending;
    struct task_struct *reducer;
    uvm_mutex_t lock;
    uvm_mutex_t lock_blocks;

    struct list_head lru_head;
} uvm_nvmgpu_va_space_t;

// uvm_deferred_free_object provides a mechanism for building and later freeing
// a list of objects which are owned by a VA space, but can't be freed while the
// VA space lock is held.

typedef enum
{
    UVM_DEFERRED_FREE_OBJECT_TYPE_CHANNEL,
    UVM_DEFERRED_FREE_OBJECT_GPU_VA_SPACE,
    UVM_DEFERRED_FREE_OBJECT_TYPE_EXTERNAL_ALLOCATION,
    UVM_DEFERRED_FREE_OBJECT_TYPE_COUNT
} uvm_deferred_free_object_type_t;

typedef struct
{
    uvm_deferred_free_object_type_t type;
    struct list_head list_node;
} uvm_deferred_free_object_t;

static void uvm_deferred_free_object_add(struct list_head *list,
                                         uvm_deferred_free_object_t *object,
                                         uvm_deferred_free_object_type_t type)
{
    object->type = type;
    list_add_tail(&object->list_node, list);
}

// Walks the list of pending objects and frees each one as appropriate to its
// type.
//
// LOCKING: May take the GPU isr_lock and the RM locks.
void uvm_deferred_free_object_list(struct list_head *deferred_free_list);

typedef enum
{
    // The GPU VA space has been initialized but not yet inserted into the
    // parent VA space.
    UVM_GPU_VA_SPACE_STATE_INIT = 0,

    // The GPU VA space is active in the VA space.
    UVM_GPU_VA_SPACE_STATE_ACTIVE,

    // The GPU VA space is no longer active in the VA space. This state can be
    // observed when threads retain the gpu_va_space then drop the VA space
    // lock. After re-taking the VA space lock, the state must be inspected to
    // see if another thread unregistered the gpu_va_space in the meantime.
    UVM_GPU_VA_SPACE_STATE_DEAD,

    UVM_GPU_VA_SPACE_STATE_COUNT
} uvm_gpu_va_space_state_t;

struct uvm_gpu_va_space_struct
{
    // Parent pointers
    uvm_va_space_t *va_space;
    uvm_gpu_t *gpu;

    uvm_gpu_va_space_state_t state;

    // Handle to the duped GPU VA space
    // to be used for all further GPU VA space related UVM-RM interactions.
    uvmGpuAddressSpaceHandle duped_gpu_va_space;
    bool did_set_page_directory;

    uvm_page_tree_t page_tables;

    // List of all uvm_user_channel_t's under this GPU VA space
    struct list_head registered_channels;

    // List of all uvm_va_range_t's under this GPU VA space with type ==
    // UVM_VA_RANGE_TYPE_CHANNEL. Used at channel registration time to find
    // shareable VA ranges without having to iterate through all VA ranges in
    // the VA space.
    struct list_head channel_va_ranges;

    // Boolean which is 1 if no new channel registration is allowed. This is set
    // when all the channels under the GPU VA space have been stopped to prevent
    // new ones from entering after we drop the VA space lock. It is an atomic_t
    // because multiple threads may set it to 1 concurrently.
    atomic_t disallow_new_channels;

    // On VMA destruction, the fault buffer needs to be flushed for all the GPUs
    // registered in the VA space to avoid leaving stale entries of the VA range
    // that is going to be destroyed. Otherwise, these fault entries can be
    // attributed to new VA ranges reallocated at the same addresses. However,
    // uvm_vm_close is called with mm->mmap_sem taken and we cannot take the ISR
    // lock. Therefore, we use a flag no notify the GPU fault handler that the
    // fault buffer needs to be flushed, before servicing the faults that belong
    // to the va_space.
    bool needs_fault_buffer_flush;

    // Node for the deferred free list where this GPU VA space is stored upon
    // being unregistered.
    uvm_deferred_free_object_t deferred_free;

    // Reference count for this gpu_va_space. This only protects the memory
    // object itself, for use in cases when the gpu_va_space needs to be
    // accessed across dropping and re-acquiring the VA space lock.
    nv_kref_t kref;

    struct
    {
        // Each GPU VA space can have ATS enabled or disabled in its hardware
        // state. This is controlled by user space when it allocates that GPU VA
        // space object from RM. This flag indicates the mode user space
        // requested when allocating this GPU VA space.
        bool enabled;

#if UVM_ATS_IBM_SUPPORTED_IN_KERNEL()
        struct npu_context *npu_context;
#endif

        // Used on the teardown path to know what to clean up. npu_context acts
        // as the equivalent flag for kernel-provided support.
        bool did_ibm_driver_init;
    } ats;
};

typedef struct
{
    int                  numa_node;

    uvm_processor_mask_t gpus;
} uvm_cpu_gpu_affinity_t;

struct uvm_va_space_struct
{
    // Mask of gpus registered with the va space
    uvm_processor_mask_t registered_gpus;

    // Array of pointers to the uvm_gpu_t objects that correspond to the
    // uvm_processor_id_t index.







    uvm_gpu_t *registered_gpus_table[UVM_ID_MAX_GPUS];

    // Mask of processors registered with the va space that support replayable faults
    uvm_processor_mask_t faultable_processors;

    // Semaphore protecting the state of the va space
    uvm_rw_semaphore_t lock;

    // Lock taken prior to taking the VA space lock in write mode, or prior to
    // taking the VA space lock in read mode on a path which will call in RM.
    // See UVM_LOCK_ORDER_VA_SPACE_SERIALIZE_WRITERS in uvm8_lock.h.
    uvm_mutex_t serialize_writers_lock;

    // Lock taken to serialize down_reads on the VA space lock with up_writes in
    // other threads. See
    // UVM_LOCK_ORDER_VA_SPACE_READ_ACQUIRE_WRITE_RELEASE_LOCK in uvm8_lock.h.
    uvm_mutex_t read_acquire_write_release_lock;

    // Tree of uvm_va_range_t's
    uvm_range_tree_t va_range_tree;

    // Kernel mapping structure passed to unmap_mapping range to unmap CPU PTEs
    // in this process.
    struct address_space mapping;

    // Storage in g_uvm_global.va_spaces.list
    struct list_head list_node;

    // Monotonically increasing counter for range groups IDs
    atomic64_t range_group_id_counter;

    // Range groups
    struct radix_tree_root range_groups;
    uvm_range_tree_t range_group_ranges;

    // Peer to peer table
    // A bitmask of peer to peer pairs enabled in this va_space
    // indexed by a peer_table_index returned by uvm_gpu_peer_table_index().
    DECLARE_BITMAP(enabled_peers, UVM_MAX_UNIQUE_GPU_PAIRS);

    // Temporary copy of the above state used to avoid allocation during VA
    // space destroy.
    DECLARE_BITMAP(enabled_peers_teardown, UVM_MAX_UNIQUE_GPU_PAIRS);

    // Interpreting these processor masks:
    //      uvm_processor_mask_test(foo[A], B)
    // ...should be read as "test if A foo B." For example:
    //      uvm_processor_mask_test(accessible_from[B], A)
    // means "test if B is accessible_from A."

    // Pre-computed masks that contain, for each processor, a mask of processors
    // which that processor can directly access. In other words, this will test
    // whether A has direct access to B:
    //      uvm_processor_mask_test(can_access[A], B)
    uvm_processor_mask_t can_access[UVM_ID_MAX_PROCESSORS];

    // Pre-computed masks that contain, for each processor memory, a mask with
    // the processors that have direct access enabled to its memory. This is the
    // opposite direction as can_access. In other words, this will test whether
    // A has direct access to B:
    //      uvm_processor_mask_test(accessible_from[B], A)
    uvm_processor_mask_t accessible_from[UVM_ID_MAX_PROCESSORS];

    // Pre-computed masks that contain, for each processor memory, a mask with
    // the processors that can directly copy to and from its memory. This is
    // almost the same as accessible_from masks, but also requires peer identity
    // mappings to be supported for peer access.
    uvm_processor_mask_t can_copy_from[UVM_ID_MAX_PROCESSORS];

    // Pre-computed masks that contain, for each processor, a mask of processors
    // to which that processor has NVLINK access. In other words, this will test
    // whether A has NVLINK access to B:
    //      uvm_processor_mask_test(has_nvlink[A], B)
    // This is a subset of can_access.
    uvm_processor_mask_t has_nvlink[UVM_ID_MAX_PROCESSORS];

    // Pre-computed masks that contain, for each processor memory, a mask with
    // the processors that have direct access to its memory and native support
    // for atomics in HW. This is a subset of accessible_from.
    uvm_processor_mask_t has_native_atomics[UVM_ID_MAX_PROCESSORS];

    // Pre-computed masks that contain, for each processor memory, a mask with
    // the processors that are indirect peers. Indirect peers can access each
    // other's memory like regular peers, but with additional latency and/or bw
    // penalty.
    uvm_processor_mask_t indirect_peers[UVM_ID_MAX_PROCESSORS];

    // Mask of gpu_va_spaces registered with the va space
    // indexed by gpu->id
    uvm_processor_mask_t registered_gpu_va_spaces;

    // Mask of GPUs which have temporarily dropped the VA space lock mid-
    // unregister. Used to make other paths return an error rather than
    // corrupting state.
    uvm_processor_mask_t gpu_unregister_in_progress;

    // Mask of processors that are participating in system-wide atomics
    uvm_processor_mask_t system_wide_atomics_enabled_processors;

    // Mask of GPUs where access counters are enabled on this VA space
    uvm_processor_mask_t access_counters_enabled_processors;

    // Array with information regarding CPU/GPU NUMA affinity. There is one
    // entry per CPU NUMA node. Entries in the array are populated sequentially
    // as new CPU NUMA nodes are discovered on GPU registration. Each entry
    // contains a CPU NUMA node id, and a mask with the GPUs attached to it.
    // Since each GPU can only be attached to one CPU node id, the array can
    // contain information for up to UVM_ID_MAX_GPUS nodes. The information is
    // stored in the VA space to avoid taking the global lock.
    uvm_cpu_gpu_affinity_t gpu_cpu_numa_affinity[UVM_ID_MAX_GPUS];

    // Array of GPU VA spaces
    uvm_gpu_va_space_t *gpu_va_spaces[UVM_ID_MAX_GPUS];

    // Tracking of GPU VA spaces which have dropped the VA space lock and are
    // pending destruction. uvm_va_space_mm_shutdown has to wait for those
    // destroy operations to be completely done.
    struct
    {
        atomic_t num_pending;
        wait_queue_head_t wait_queue;
    } gpu_va_space_deferred_free;

    // Per-va_space event notification information for performance heuristics
    uvm_perf_va_space_events_t perf_events;

    uvm_perf_module_data_desc_t perf_modules_data[UVM_PERF_MODULE_TYPE_COUNT];

    // Array of modules that are loaded in the va_space, indexed by module type
    uvm_perf_module_t *perf_modules[UVM_PERF_MODULE_TYPE_COUNT];

    // Lists of counters listening for events on this VA space
    // Protected by lock
    struct
    {
        bool enabled;

        uvm_rw_semaphore_t lock;

        // Lists of counters listening for events on this VA space
        struct list_head counters[UVM_TOTAL_COUNTERS];
        struct list_head queues[UvmEventNumTypesAll];

        // Node for this va_space in global subscribers list
        struct list_head node;
    } tools;

    // Boolean which is 1 if all user channels have been already stopped. This
    // is an atomic_t because multiple threads may call
    // uvm_va_space_stop_all_user_channels concurrently.
    atomic_t user_channels_stopped;

    // Prevent future registrations of any kind (GPU, GPU VA space, channel).
    // This is used when the associated va_space_mm is torn down, which has to
    // prevent any new work from being started in this VA space.
    bool disallow_new_registers;

    bool user_channel_stops_are_immediate;

    // Block context used for GPU unmap operations so that allocation is not
    // required on the teardown path. This can only be used while the VA space
    // lock is held in write mode. Access using uvm_va_space_block_context().
    uvm_va_block_context_t va_block_context;

    // UVM_INITIALIZE has been called. Until this is set, the VA space is
    // inoperable. Use uvm_va_space_initialized() to check whether the VA space
    // has been initialized.
    atomic_t initialized;
    NvU64 initialization_flags;

    // The mm currently associated with this VA space, if any.
    uvm_va_space_mm_t va_space_mm;

    struct
    {
#if UVM_IBM_NPU_SUPPORTED()
        // These are the active NPUs in this VA space, that is, all NPUs with
        // GPUs that have GPU VA spaces registered in this VA space.
        //
        // If a bit is clear in npu_active_mask then the corresponding entry of
        // npu_ref_counts is 0. If a bit is set then the corresponding entry of
        // npu_ref_counts is greater than 0.
        NvU32 npu_ref_counts[NV_MAX_NPUS];
        DECLARE_BITMAP(npu_active_mask, NV_MAX_NPUS);
#endif

        // Lock protecting npu_ref_counts and npu_active_mask. Invalidations
        // take this lock for read. GPU VA space register and unregister take
        // this lock for write. Since all invalidations take the lock for read
        // for the duration of the invalidate, taking the lock for write also
        // flushes all invalidates.
        //
        // This is a spinlock because the invalidation code paths may be called
        // with interrupts disabled, so those paths can't take the VA space
        // lock. We could use a normal exclusive spinlock instead, but a reader/
        // writer lock is preferred to allow concurrent invalidates in the same
        // VA space.
        uvm_rwlock_irqsave_t rwlock;
    } ats;

#if UVM_IS_CONFIG_HMM()
    // HMM information about this VA space.
    uvm_hmm_va_space_t hmm_va_space;
#endif

    struct
    {
        bool  page_prefetch_enabled;

        atomic_t migrate_vma_allocation_fail_nth;

        uvm_thread_context_wrapper_t *dummy_thread_context_wrappers;
        size_t num_dummy_thread_context_wrappers;
    } test;

    // Queue item for deferred f_ops->release() handling
    nv_kthread_q_item_t deferred_release_q_item;

    uvm_nvmgpu_va_space_t nvmgpu_va_space;
};

static uvm_gpu_t *uvm_va_space_get_gpu(uvm_va_space_t *va_space, uvm_gpu_id_t gpu_id)
{
    uvm_gpu_t *gpu;

    UVM_ASSERT(uvm_processor_mask_test(&va_space->registered_gpus, gpu_id));

    gpu = va_space->registered_gpus_table[uvm_id_gpu_index(gpu_id)];

    UVM_ASSERT(gpu);
    UVM_ASSERT(uvm_gpu_get(gpu->global_id) == gpu);

    return gpu;
}

static const char *uvm_va_space_processor_name(uvm_va_space_t *va_space, uvm_processor_id_t id)
{
    if (UVM_ID_IS_CPU(id))
        return "0: CPU";
    else
        return uvm_va_space_get_gpu(va_space, id)->name;
}

static void uvm_va_space_processor_uuid(uvm_va_space_t *va_space, NvProcessorUuid *uuid, uvm_processor_id_t id)
{
    if (UVM_ID_IS_CPU(id)) {
        memcpy(uuid, &NV_PROCESSOR_UUID_CPU_DEFAULT, sizeof(*uuid));
    }
    else {
        uvm_gpu_t *gpu = uvm_va_space_get_gpu(va_space, id);
        UVM_ASSERT(gpu);
        memcpy(uuid, &gpu->uuid, sizeof(*uuid));
    }
}

static bool uvm_va_space_processor_has_memory(uvm_va_space_t *va_space, uvm_processor_id_t id)
{
    if (UVM_ID_IS_CPU(id))
        return true;

    return uvm_va_space_get_gpu(va_space, id)->mem_info.size > 0;
}

// Checks if the VA space has been fully initialized (UVM_INITIALIZE has been
// called). Returns NV_OK if so, NV_ERR_ILLEGAL_ACTION otherwise.
//
// Locking: No requirements. The VA space lock does NOT need to be held when
//          calling this function, though it is allowed.
static NV_STATUS uvm_va_space_initialized(uvm_va_space_t *va_space)
{
    // The common case by far is for the VA space to have already been
    // initialized. This combined with the fact that some callers may never hold
    // the VA space lock means we don't want the VA space lock to be taken to
    // perform this check.
    //
    // Instead of locks, we rely on acquire/release memory ordering semantics.
    // The release is done at the end of uvm_api_initialize() when the
    // UVM_INITIALIZE ioctl completes. That opens the gate for any other
    // threads.
    //
    // Using acquire semantics as opposed to a normal read will add slight
    // overhead to every entry point on platforms with relaxed ordering. Should
    // that overhead become noticeable we could have UVM_INITIALIZE use
    // on_each_cpu to broadcast memory barriers.
    if (likely(atomic_read_acquire(&va_space->initialized)))
        return NV_OK;

    return NV_ERR_ILLEGAL_ACTION;
}

NV_STATUS uvm_va_space_create(struct inode *inode, struct file *filp);
void uvm_va_space_destroy(uvm_va_space_t *va_space);

// All VA space locking should be done with these wrappers. They're macros so
// lock assertions are attributed to line numbers correctly.

#define uvm_va_space_down_write(__va_space)                             \
    do {                                                                \
        uvm_mutex_lock(&(__va_space)->serialize_writers_lock);          \
        uvm_mutex_lock(&(__va_space)->read_acquire_write_release_lock); \
        uvm_down_write(&(__va_space)->lock);                            \
    } while (0)

#define uvm_va_space_up_write(__va_space)                                   \
    do {                                                                    \
        uvm_up_write(&(__va_space)->lock);                                  \
        uvm_mutex_unlock(&(__va_space)->read_acquire_write_release_lock);   \
        uvm_mutex_unlock(&(__va_space)->serialize_writers_lock);            \
    } while (0)

#define uvm_va_space_downgrade_write(__va_space)                                        \
    do {                                                                                \
        uvm_downgrade_write(&(__va_space)->lock);                                       \
        uvm_mutex_unlock_out_of_order(&(__va_space)->read_acquire_write_release_lock);  \
        uvm_mutex_unlock_out_of_order(&(__va_space)->serialize_writers_lock);           \
    } while (0)

// Call this when holding the VA space lock for write in order to downgrade to
// read on a path which also needs to make RM calls.
#define uvm_va_space_downgrade_write_rm(__va_space)                                     \
    do {                                                                                \
        uvm_assert_mutex_locked(&(__va_space)->serialize_writers_lock);                 \
        uvm_downgrade_write(&(__va_space)->lock);                                       \
        uvm_mutex_unlock_out_of_order(&(__va_space)->read_acquire_write_release_lock);  \
    } while (0)

#define uvm_va_space_down_read(__va_space)                                              \
    do {                                                                                \
        uvm_mutex_lock(&(__va_space)->read_acquire_write_release_lock);                 \
        uvm_down_read(&(__va_space)->lock);                                             \
        uvm_mutex_unlock_out_of_order(&(__va_space)->read_acquire_write_release_lock);  \
    } while (0)

// Call this if RM calls need to be made while holding the VA space lock in read
// mode. Note that taking read_acquire_write_release_lock is unnecessary since
// the down_read is serialized with another thread's up_write by the
// serialize_writers_lock.
#define uvm_va_space_down_read_rm(__va_space)                           \
    do {                                                                \
        uvm_mutex_lock(&(__va_space)->serialize_writers_lock);          \
        uvm_down_read(&(__va_space)->lock);                             \
    } while (0)

#define uvm_va_space_up_read(__va_space) uvm_up_read(&(__va_space)->lock)

#define uvm_va_space_up_read_rm(__va_space)                             \
    do {                                                                \
        uvm_up_read(&(__va_space)->lock);                               \
        uvm_mutex_unlock(&(__va_space)->serialize_writers_lock);        \
    } while (0)

// Initialize the VA space with the user-provided flags, enabling ioctls and
// mmap.
NV_STATUS uvm_va_space_initialize(uvm_va_space_t *va_space, NvU64 flags);

// Get a registered gpu by uuid. This restricts the search for GPUs, to those that
// have been registered with a va_space. This returns NULL if the GPU is not present, or not
// registered with the va_space.
//
// LOCKING: The VA space lock must be held.
uvm_gpu_t *uvm_va_space_get_gpu_by_uuid(uvm_va_space_t *va_space, const NvProcessorUuid *gpu_uuid);

// Like uvm_va_space_get_gpu_by_uuid, but also returns NULL if the GPU does
// not have a GPU VA space registered in the UVM va_space.
//
// LOCKING: The VA space lock must be held.
uvm_gpu_t *uvm_va_space_get_gpu_by_uuid_with_gpu_va_space(uvm_va_space_t *va_space, const NvProcessorUuid *gpu_uuid);

// Same as uvm_va_space_get_gpu_by_uuid but it also retains the GPU. The caller
// cannot assume that the GPU is still registered in the VA space after the
// function returns.
//
// LOCKING: The function takes and releases the VA space lock in read mode.
uvm_gpu_t *uvm_va_space_retain_gpu_by_uuid(uvm_va_space_t *va_space, const NvProcessorUuid *gpu_uuid);

// Returns whether read-duplication is supported
// If gpu is NULL, returns the current state.
// otherwise, it retuns what the result would be once the gpu's va space is added or removed
// (by inverting the gpu's current state)
bool uvm_va_space_can_read_duplicate(uvm_va_space_t *va_space, uvm_gpu_t *changing_gpu);

// Register a gpu in the va space
// Note that each gpu can be only registered once in a va space
//
// This call returns whether the GPU memory is a NUMA node in the kernel and the
// corresponding node id.
NV_STATUS uvm_va_space_register_gpu(uvm_va_space_t *va_space,
                                    const NvProcessorUuid *gpu_uuid,
                                    const uvm_rm_user_object_t *user_rm_va_space,
                                    NvBool *numa_enabled,
                                    NvS32 *numa_node_id);

// Unregister a gpu from the va space
NV_STATUS uvm_va_space_unregister_gpu(uvm_va_space_t *va_space, const NvProcessorUuid *gpu_uuid);

// Registers a GPU VA space with the UVM VA space.
NV_STATUS uvm_va_space_register_gpu_va_space(uvm_va_space_t *va_space,
                                             uvm_rm_user_object_t *user_rm_va_space,
                                             const NvProcessorUuid *gpu_uuid);

// Unregisters a GPU VA space from the UVM VA space.
NV_STATUS uvm_va_space_unregister_gpu_va_space(uvm_va_space_t *va_space, const NvProcessorUuid *gpu_uuid);

// Stop all user channels
//
// This function sets a flag in the VA space indicating that all the channels
// have been already stopped and should only be used when no new user channels
// can be registered.
//
// LOCKING: The VA space lock must be held in read mode, not write.
void uvm_va_space_stop_all_user_channels(uvm_va_space_t *va_space);

// Calls uvm_user_channel_detach on all user channels in a VA space.
//
// The detached channels are added to the input list. The caller is expected to
// drop the VA space lock and call uvm_deferred_free_object_list to complete the
// destroy operation.
//
// LOCKING: The owning VA space must be locked in write mode.
void uvm_va_space_detach_all_user_channels(uvm_va_space_t *va_space, struct list_head *deferred_free_list);

// Returns whether peer access between these two GPUs has been enabled in this
// VA space. Both GPUs must be registered in the VA space.
bool uvm_va_space_peer_enabled(uvm_va_space_t *va_space, uvm_gpu_t *gpu1, uvm_gpu_t *gpu2);

static uvm_va_space_t *uvm_va_space_get(struct file *filp)
{
    UVM_ASSERT(uvm_file_is_nvidia_uvm(filp));
    UVM_ASSERT_MSG(filp->private_data != NULL, "filp: 0x%llx", (NvU64)filp);

    return (uvm_va_space_t *)filp->private_data;
}

static uvm_va_block_context_t *uvm_va_space_block_context(uvm_va_space_t *va_space)
{
    uvm_assert_rwsem_locked_write(&va_space->lock);

    uvm_va_block_context_init(&va_space->va_block_context);

    return &va_space->va_block_context;
}

// Retains the GPU VA space memory object. destroy_gpu_va_space and
// uvm_gpu_va_space_release drop the count. This is used to keep the GPU VA
// space object allocated when dropping and re-taking the VA space lock. If
// another thread called remove_gpu_va_space in the meantime,
// gpu_va_space->state will be UVM_GPU_VA_SPACE_STATE_DEAD.
static inline void uvm_gpu_va_space_retain(uvm_gpu_va_space_t *gpu_va_space)
{
    nv_kref_get(&gpu_va_space->kref);
}

// This only frees the GPU VA space object itself, so it must have been removed
// from its VA space and destroyed prior to the final release.
void uvm_gpu_va_space_release(uvm_gpu_va_space_t *gpu_va_space);

// Wrapper for nvUvmInterfaceUnsetPageDirectory
void uvm_gpu_va_space_unset_page_dir(uvm_gpu_va_space_t *gpu_va_space);

static uvm_gpu_va_space_state_t uvm_gpu_va_space_state(uvm_gpu_va_space_t *gpu_va_space)
{
    UVM_ASSERT(gpu_va_space->gpu);

    switch (gpu_va_space->state) {
        case UVM_GPU_VA_SPACE_STATE_INIT:
            UVM_ASSERT(!gpu_va_space->va_space);
            break;

        case UVM_GPU_VA_SPACE_STATE_ACTIVE:
        case UVM_GPU_VA_SPACE_STATE_DEAD:
            UVM_ASSERT(gpu_va_space->va_space);
            break;

        default:
            UVM_ASSERT_MSG(0, "Invalid state: %u\n", gpu_va_space->state);
            break;
    }

    return gpu_va_space->state;
}

static uvm_gpu_va_space_t *uvm_gpu_va_space_get(uvm_va_space_t *va_space, uvm_gpu_t *gpu)
{
    uvm_gpu_va_space_t *gpu_va_space;

    uvm_assert_rwsem_locked(&va_space->lock);

    if (!gpu || !uvm_processor_mask_test(&va_space->registered_gpu_va_spaces, gpu->id))
        return NULL;

    gpu_va_space = va_space->gpu_va_spaces[uvm_id_gpu_index(gpu->id)];
    UVM_ASSERT(uvm_gpu_va_space_state(gpu_va_space) == UVM_GPU_VA_SPACE_STATE_ACTIVE);
    UVM_ASSERT(gpu_va_space->va_space == va_space);
    UVM_ASSERT(gpu_va_space->gpu == gpu);
    return gpu_va_space;
}

#define for_each_gpu_va_space(__gpu_va_space, __va_space)                                                   \
    for (__gpu_va_space =                                                                                   \
            uvm_gpu_va_space_get(                                                                           \
                __va_space,                                                                                 \
                uvm_processor_mask_find_first_va_space_gpu(&__va_space->registered_gpu_va_spaces, va_space) \
            );                                                                                              \
         __gpu_va_space;                                                                                    \
         __gpu_va_space =                                                                                   \
            uvm_gpu_va_space_get(                                                                           \
                __va_space,                                                                                 \
                __uvm_processor_mask_find_next_va_space_gpu(&__va_space->registered_gpu_va_spaces,          \
                                                            va_space,                                       \
                                                            __gpu_va_space->gpu)                            \
            )                                                                                               \
        )

// Return the first GPU set in the given mask or NULL. The caller must ensure
// that the GPUs set in the mask are registered in the VA space and cannot be
// unregistered during this call.
static uvm_gpu_t *uvm_processor_mask_find_first_va_space_gpu(const uvm_processor_mask_t *mask, uvm_va_space_t *va_space)
{
    uvm_gpu_t *gpu;
    uvm_gpu_id_t gpu_id;

    UVM_ASSERT(uvm_processor_mask_subset(mask, &va_space->registered_gpus));

    gpu_id = uvm_processor_mask_find_first_gpu_id(mask);
    if (UVM_ID_IS_INVALID(gpu_id))
        return NULL;

    gpu = uvm_va_space_get_gpu(va_space, gpu_id);
    UVM_ASSERT_MSG(gpu, "gpu_id %u\n", uvm_id_value(gpu_id));

    return gpu;
}

static uvm_gpu_t *uvm_va_space_find_first_gpu(uvm_va_space_t *va_space)
{
    uvm_assert_rwsem_locked(&va_space->lock);

    return uvm_processor_mask_find_first_va_space_gpu(&va_space->registered_gpus, va_space);
}

// Same as uvm_processor_mask_find_next_va_space_gpu below, but gpu cannot be
// NULL
static uvm_gpu_t *__uvm_processor_mask_find_next_va_space_gpu(const uvm_processor_mask_t *mask,
                                                              uvm_va_space_t *va_space,
                                                              uvm_gpu_t *gpu)
{
    uvm_gpu_id_t gpu_id;

    UVM_ASSERT(gpu != NULL);
    UVM_ASSERT(uvm_processor_mask_subset(mask, &va_space->registered_gpus));

    gpu_id = uvm_processor_mask_find_next_id(mask, uvm_gpu_id_next(gpu->id));
    if (UVM_ID_IS_INVALID(gpu_id))
        return NULL;

    gpu = uvm_va_space_get_gpu(va_space, gpu_id);
    UVM_ASSERT_MSG(gpu, "gpu_id %u\n", uvm_id_value(gpu_id));

    return gpu;
}

// Return the next GPU with an id larger than gpu->id set in the given mask.
// The function returns NULL if gpu is NULL. The caller must ensure that the
// GPUs set in the mask are registered in the VA space and cannot be
// unregistered during this call.
static uvm_gpu_t *uvm_processor_mask_find_next_va_space_gpu(const uvm_processor_mask_t *mask,
                                                            uvm_va_space_t *va_space,
                                                            uvm_gpu_t *gpu)
{
    if (gpu == NULL)
        return NULL;

    return __uvm_processor_mask_find_next_va_space_gpu(mask, va_space, gpu);
}

#define for_each_va_space_gpu_in_mask(gpu, va_space, mask)                                       \
    for (({uvm_assert_rwsem_locked(&(va_space)->lock);                                           \
           gpu = uvm_processor_mask_find_first_va_space_gpu(mask, va_space);});                  \
           gpu != NULL;                                                                          \
           gpu = __uvm_processor_mask_find_next_va_space_gpu(mask, va_space, gpu))

// Helper to iterate over all GPUs registered in a UVM VA space
#define for_each_va_space_gpu(gpu, va_space) \
    for_each_va_space_gpu_in_mask(gpu, va_space, &(va_space)->registered_gpus)

static void uvm_va_space_global_gpus_in_mask(uvm_va_space_t *va_space,
                                             uvm_global_processor_mask_t *global_mask,
                                             const uvm_processor_mask_t *mask)
{
    uvm_gpu_t *gpu;

    uvm_global_processor_mask_zero(global_mask);

    for_each_va_space_gpu_in_mask(gpu, va_space, mask)
        uvm_global_processor_mask_set(global_mask, gpu->global_id);
}

static void uvm_va_space_global_gpus(uvm_va_space_t *va_space, uvm_global_processor_mask_t *global_mask)
{
    uvm_va_space_global_gpus_in_mask(va_space, global_mask, &va_space->registered_gpus);
}

// Return the processor in the candidates mask that is "closest" to src, or
// UVM_ID_MAX_PROCESSORS if candidates is empty. The order is:
// - src itself
// - Direct NVLINK GPU peers if src is CPU or GPU (1)
// - NVLINK CPU if src is GPU
// - Indirect NVLINK GPU peers if src is GPU
// - PCIe peers if src is GPU (2)
// - CPU if src is GPU
// - Deterministic selection from the pool of candidates
//
// (1) When src is a GPU, NVLINK GPU peers are preferred over the CPU because in
//     NUMA systems the CPU processor may refer to multiple CPU NUMA nodes, and
//     the bandwidth between src and the farthest CPU node can be substantially
//     lower than the bandwidth src and its peer GPUs.
// (2) TODO: Bug 1764943: Is copying from a PCI peer always better than copying
//     from CPU?
uvm_processor_id_t uvm_processor_mask_find_closest_id(uvm_va_space_t *va_space,
                                                      const uvm_processor_mask_t *candidates,
                                                      uvm_processor_id_t src);

// Iterate over each ID in mask in order of proximity to src. This is
// destructive to mask.
#define for_each_closest_id(id, mask, src, va_space)                    \
    for (id = uvm_processor_mask_find_closest_id(va_space, mask, src);  \
         UVM_ID_IS_VALID(id);                                           \
         uvm_processor_mask_clear(mask, id), id = uvm_processor_mask_find_closest_id(va_space, mask, src))

// Return the GPU whose memory corresponds to the given node_id
static uvm_gpu_t *uvm_va_space_find_gpu_with_memory_node_id(uvm_va_space_t *va_space, int node_id)
{
    uvm_gpu_t *gpu;

    UVM_ASSERT(nv_numa_node_has_memory(node_id));

    if (!g_uvm_global.ats.supported)
        return NULL;

    for_each_va_space_gpu(gpu, va_space) {
        UVM_ASSERT(gpu->numa_info.enabled);

        if (gpu->numa_info.node_id == node_id)
            return gpu;
    }

    return NULL;
}

static bool uvm_va_space_memory_node_is_gpu(uvm_va_space_t *va_space, int node_id)
{
    return uvm_va_space_find_gpu_with_memory_node_id(va_space, node_id) != NULL;
}

// Return a processor mask with the GPUs attached to the node_id CPU memory
// node
static void uvm_va_space_get_gpus_attached_to_cpu_node(uvm_va_space_t *va_space,
                                                       int node_id,
                                                       uvm_processor_mask_t *gpus)
{
    uvm_gpu_id_t gpu_id;

    UVM_ASSERT(!uvm_va_space_memory_node_is_gpu(va_space, node_id));

    for_each_gpu_id(gpu_id) {
        const uvm_cpu_gpu_affinity_t *affinity = &va_space->gpu_cpu_numa_affinity[uvm_id_gpu_index(gpu_id)];
        if (affinity->numa_node == node_id) {
            uvm_processor_mask_copy(gpus, &affinity->gpus);
            return;
        }
    }

    uvm_processor_mask_zero(gpus);
}

// Helper that returns the first GPU in the mask returned by
// uvm_va_space_get_gpus_attached_to_cpu_node or NULL if empty
static uvm_gpu_t *uvm_va_space_find_first_gpu_attached_to_cpu_node(uvm_va_space_t *va_space, int node_id)
{
    uvm_processor_mask_t gpus;

    uvm_va_space_get_gpus_attached_to_cpu_node(va_space, node_id, &gpus);

    return uvm_processor_mask_find_first_va_space_gpu(&gpus, va_space);
}

// Obtain the user channel with the given instance_ptr. This is used during
// non-replayable fault service. This function needs to be called with the va
// space lock held in order to prevent channels from being removed.
uvm_user_channel_t *uvm_gpu_va_space_get_user_channel(uvm_gpu_va_space_t *gpu_va_space,
                                                      uvm_gpu_phys_address_t instance_ptr);

// Whether some form of pageable access (ATS, HMM) is supported by the system on
// this VA space. This does NOT check whether GPUs with pageable support are
// present, just whether system + VA space support exists.
bool uvm_va_space_pageable_mem_access_supported(uvm_va_space_t *va_space);

NV_STATUS uvm8_test_get_pageable_mem_access_type(UVM_TEST_GET_PAGEABLE_MEM_ACCESS_TYPE_PARAMS *params,
                                                 struct file *filp);
NV_STATUS uvm8_test_enable_nvlink_peer_access(UVM_TEST_ENABLE_NVLINK_PEER_ACCESS_PARAMS *params, struct file *filp);
NV_STATUS uvm8_test_disable_nvlink_peer_access(UVM_TEST_DISABLE_NVLINK_PEER_ACCESS_PARAMS *params, struct file *filp);

#endif // __UVM8_VA_SPACE_H__
