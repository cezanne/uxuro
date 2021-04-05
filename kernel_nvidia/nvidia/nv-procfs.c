/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 1999-2017 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */

#define  __NO_VERSION__
#include "nv-misc.h"

#include "os-interface.h"
#include "nv-linux.h"

#if defined(CONFIG_PROC_FS)

#include "nv-procfs.h"
#include "nv_compiler.h"
#include "nv-reg.h"
#include "conftest/patches.h"
#include "nv-ibmnpu.h"

#define NV_DEFINE_SINGLE_NVRM_PROCFS_FILE(name) \
    NV_DEFINE_SINGLE_PROCFS_FILE(name, \
                                 NV_READ_LOCK_SYSTEM_PM_LOCK_INTERRUPTIBLE, \
                                 NV_READ_UNLOCK_SYSTEM_PM_LOCK)

static const char *__README_warning = \
    "The NVIDIA graphics driver tries to detect potential problems\n"
    "with the host system and warns about them using the system's\n"
    "logging mechanisms. Important warning message are also logged\n"
    "to dedicated text files in this directory.\n";

static const char *__README_patches = \
    "The NVIDIA graphics driver's kernel interface files can be\n"
    "patched to improve compatibility with new Linux kernels or to\n"
    "fix bugs in these files. When applied, each official patch\n"
    "provides a short text file with a short description of itself\n"
    "in this directory.\n";

static struct proc_dir_entry *proc_nvidia;
static struct proc_dir_entry *proc_nvidia_warnings;
static struct proc_dir_entry *proc_nvidia_patches;
static struct proc_dir_entry *proc_nvidia_gpus;

extern char *NVreg_RegistryDwords;
extern char *NVreg_RegistryDwordsPerDevice;
extern char *NVreg_RmMsg;
extern char *NVreg_AssignGpus;
extern char *NVreg_GpuBlacklist;
extern char *NVreg_TemporaryFilePath;

static char nv_registry_keys[NV_MAX_REGISTRY_KEYS_LENGTH];

#if defined(CONFIG_PM)
static nv_pm_action_depth_t nv_pm_action_depth = NV_PM_ACTION_DEPTH_DEFAULT;
#endif

static int nv_procfs_read_registry(struct seq_file *s, void *v);

#define NV_NUMA_STATUS_MSG_LEN      (32)
#define NV_PROC_WRITE_BUFFER_SIZE   (64 * PAGE_SIZE)

/*
 * Status messages directly corresponding to states in nv_numa_states_t.
 */
static const char *nv_numa_status_messages[] =
{
    "disabled",
    "offline",
    "online_in_progress",
    "online",
    "online_failed",
    "offline_in_progress",
    "offline_failed",
};

static int
nv_procfs_read_gpu_info(
    struct seq_file *s,
    void *v
)
{
    nv_state_t *nv = s->private;
    nv_linux_state_t *nvl = NV_GET_NVL_FROM_NV_STATE(nv);
    struct pci_dev *pci_dev = nvl->pci_dev;
    char *type, *fmt, tmpstr[NV_DEVICE_NAME_LENGTH];
    NvU8 *uuid;
    NvU32 vbios_rev1, vbios_rev2, vbios_rev3, vbios_rev4, vbios_rev5;
    nvidia_stack_t *sp = NULL;

    if (nv_kmem_cache_alloc_stack(&sp) != 0)
    {
        return 0;
    }

    if (rm_get_device_name(sp, nv, pci_dev->device, pci_dev->subsystem_vendor,
                pci_dev->subsystem_device, NV_DEVICE_NAME_LENGTH,
                tmpstr) != NV_OK)
    {
        strcpy (tmpstr, "Unknown");
    }

    seq_printf(s, "Model: \t\t %s\n", tmpstr);
    seq_printf(s, "IRQ:   \t\t %d\n", nv->interrupt_line);

    if (rm_get_gpu_uuid(sp, nv, &uuid, NULL) == NV_OK)
    {
        seq_printf(s, "GPU UUID: \t %s\n", (char *)uuid);
        os_free_mem(uuid);
    }

    if (rm_get_vbios_version(sp, nv, &vbios_rev1, &vbios_rev2,
                &vbios_rev3, &vbios_rev4,
                &vbios_rev5) != NV_OK)
    {
        seq_printf(s, "Video BIOS: \t ??.??.??.??.??\n");
    }
    else
    {
        fmt = "Video BIOS: \t %02x.%02x.%02x.%02x.%02x\n";
        seq_printf(s, fmt, vbios_rev1, vbios_rev2, vbios_rev3, vbios_rev4,
                   vbios_rev5);
    }

    if (nv_find_pci_capability(pci_dev, PCI_CAP_ID_EXP))
        type = "PCIe";
    else
        type = "PCI";
    seq_printf(s, "Bus Type: \t %s\n", type);

    seq_printf(s, "DMA Size: \t %d bits\n",
     nv_count_bits(pci_dev->dma_mask));
    seq_printf(s, "DMA Mask: \t 0x%llx\n", pci_dev->dma_mask);
    seq_printf(s, "Bus Location: \t %04x:%02x:%02x.%x\n",
                   nv->pci_info.domain, nv->pci_info.bus,
                   nv->pci_info.slot, PCI_FUNC(pci_dev->devfn));
    seq_printf(s, "Device Minor: \t %u\n", nvl->minor_num);

#if defined(DEBUG)
    do
    {
        int j;
        for (j = 0; j < NV_GPU_NUM_BARS; j++)
        {
            seq_printf(s, "BAR%u: \t\t 0x%llx (%lluMB)\n",
                       j, nv->bars[j].cpu_address, (nv->bars[j].size >> 20));
        }
    } while (0);
#endif

    seq_printf(s, "Blacklisted:\t %s\n",
               ((nv->flags & NV_FLAG_BLACKLIST) != 0) ? "Yes" : "No");

    nv_kmem_cache_free_stack(sp);

    return 0;
}

NV_DEFINE_SINGLE_NVRM_PROCFS_FILE(gpu_info);

static int
nv_procfs_read_version(
    struct seq_file *s,
    void *v
)
{
    seq_printf(s, "NVRM version: %s\n", pNVRM_ID);
    seq_printf(s, "GCC version:  %s\n", NV_COMPILER);

    return 0;
}

NV_DEFINE_SINGLE_NVRM_PROCFS_FILE(version);

static void
nv_procfs_close_file(
    nv_file_private_t *nvfp
)
{
    nvidia_stack_t *sp;

    if (nvfp->data != NULL)
    {
        os_free_mem(nvfp->data);
    }

    sp  = nvfp->fops_sp[NV_FOPS_STACK_INDEX_PROCFS];
    if (sp != NULL)
    {
        nv_kmem_cache_free_stack(sp);
    }

    nv_free_file_private(nvfp);
}

static int
nv_procfs_open_file(
    struct inode *inode,
    struct file  *file,
    nv_file_private_t **nvfpp
)
{
    int retval = 0;
    NV_STATUS status;
    nv_file_private_t *nvfp = NULL;
    nvidia_stack_t *sp = NULL;

    nvfp = nv_alloc_file_private();
    if (nvfp == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate file private!\n");
        return -ENOMEM;
    }

    nvfp->proc_data = NV_PDE_DATA(inode);

    if (0 == (file->f_mode & FMODE_WRITE))
        goto done;

    retval = nv_kmem_cache_alloc_stack(&sp);
    if (retval != 0)
    {
        goto done;
    }

    status = os_alloc_mem((void **)&nvfp->data, NV_PROC_WRITE_BUFFER_SIZE);
    if (status != NV_OK)
    {
        retval = -ENOMEM;
        goto done;
    }

    os_mem_set((void *)nvfp->data, 0, NV_PROC_WRITE_BUFFER_SIZE);
    nvfp->fops_sp[NV_FOPS_STACK_INDEX_PROCFS] = sp;

done:
    if (retval < 0)
    {
        nv_procfs_close_file(nvfp);
        return retval;
    }

    *nvfpp = nvfp;

    return 0;
}

static int
nv_procfs_open_registry(
    struct inode *inode,
    struct file  *file
)
{
    nv_file_private_t *nvfp = NULL;
    int retval;

    retval = nv_procfs_open_file(inode, file, &nvfp);
    if (retval < 0)
    {
        return retval;
    }

    retval = single_open(file, nv_procfs_read_registry, nvfp);
    if (retval < 0)
    {
        nv_procfs_close_file(nvfp);
        return retval;
    }

    retval = NV_READ_LOCK_SYSTEM_PM_LOCK_INTERRUPTIBLE();
    if (retval < 0)
    {
        single_release(inode, file);
        nv_procfs_close_file(nvfp);
    }

    return retval;
}

static int
nv_procfs_close_registry(
    struct inode *inode,
    struct file  *file
)
{
    struct seq_file *s = file->private_data;
    nv_file_private_t *nvfp = s->private;
    nv_state_t *nv;
    nv_linux_state_t *nvl = NULL;
    nvidia_stack_t *sp = nvfp->fops_sp[NV_FOPS_STACK_INDEX_PROCFS];
    char *key_name, *key_value, *registry_keys;
    size_t key_len, len;
    long count;
    NV_STATUS rm_status;
    int rc = 0;

    if (0 != nvfp->off)
    {
        nv = nvfp->proc_data;
        if (nv != NULL)
            nvl = NV_GET_NVL_FROM_NV_STATE(nv);
        key_value = (char *)nvfp->data;

        key_name = strsep(&key_value, "=");

        if (NULL == key_name || NULL == key_value)
        {
            rc = -EINVAL;
            goto done;
        }

        key_len = (strlen(key_name) + 1);
        count = (nvfp->off - key_len);

        if (count <= 0)
        {
            rc = -EINVAL;
            goto done;
        }

        rm_status = rm_write_registry_binary(sp, nv, "NVreg", key_name,
                key_value, count);
        if (rm_status != NV_OK)
        {
            rc = -EFAULT;
            goto done;
        }

        registry_keys = ((nvl != NULL) ?
                nvl->registry_keys : nv_registry_keys);
        if (strstr(registry_keys, key_name) != NULL)
            goto done;
        len = strlen(registry_keys);

        if ((len + key_len + 2) <= NV_MAX_REGISTRY_KEYS_LENGTH)
        {
            if (len != 0)
                strcat(registry_keys, ", ");
            strcat(registry_keys, key_name);
        }
    }

done:
    NV_READ_UNLOCK_SYSTEM_PM_LOCK();

    single_release(inode, file);

    nv_procfs_close_file(nvfp);

    return rc;
}

static int
nv_procfs_read_params(
    struct seq_file *s,
    void *v
)
{
    unsigned int i;
    nv_parm_t *entry;

    for (i = 0; (entry = &nv_parms[i])->name != NULL; i++)
        seq_printf(s, "%s: %u\n", entry->name, *entry->data);

    seq_printf(s, "RegistryDwords: \"%s\"\n",
               (NVreg_RegistryDwords != NULL) ? NVreg_RegistryDwords : "");
    seq_printf(s, "RegistryDwordsPerDevice: \"%s\"\n",
               (NVreg_RegistryDwordsPerDevice != NULL) ? NVreg_RegistryDwordsPerDevice : "");
    seq_printf(s, "RmMsg: \"%s\"\n",
               (NVreg_RmMsg != NULL) ? NVreg_RmMsg : "");
    seq_printf(s, "AssignGpus: \"%s\"\n",
               (NVreg_AssignGpus != NULL) ? NVreg_AssignGpus : "");
    seq_printf(s, "GpuBlacklist: \"%s\"\n",
               (NVreg_GpuBlacklist != NULL) ? NVreg_GpuBlacklist : "");
    seq_printf(s, "TemporaryFilePath: \"%s\"\n",
               (NVreg_TemporaryFilePath != NULL) ? NVreg_TemporaryFilePath : "");

    return 0;
}

NV_DEFINE_SINGLE_NVRM_PROCFS_FILE(params);

static int
nv_procfs_read_registry(
    struct seq_file *s,
    void *v
)
{
    nv_file_private_t *nvfp = s->private;
    nv_state_t *nv = nvfp->proc_data;
    nv_linux_state_t *nvl = NULL;
    char *registry_keys;

    if (nv != NULL)
        nvl = NV_GET_NVL_FROM_NV_STATE(nv);
    registry_keys = ((nvl != NULL) ?
            nvl->registry_keys : nv_registry_keys);

    seq_printf(s, "Binary: \"%s\"\n", registry_keys);
    return 0;
}

static ssize_t
nv_procfs_write_file(
    struct file   *file,
    const char __user *buffer,
    size_t count,
    loff_t *pos
)
{
    int status = 0;
    struct seq_file *s = file->private_data;
    nv_file_private_t *nvfp = s->private;
    char *proc_buffer;
    unsigned long bytes_left;

    down(&nvfp->fops_sp_lock[NV_FOPS_STACK_INDEX_PROCFS]);

    bytes_left = (NV_PROC_WRITE_BUFFER_SIZE - nvfp->off - 1);

    if (count == 0)
    {
        status = -EINVAL;
        goto done;
    }
    else if ((bytes_left == 0) || (count > bytes_left))
    {
        status = -ENOSPC;
        goto done;
    }

    proc_buffer = &((char *)nvfp->data)[nvfp->off];

    if (copy_from_user(proc_buffer, buffer, count))
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to copy in proc data!\n");
        status = -EFAULT;
    }
    else
    {
        nvfp->off += count;
    }

    *pos = nvfp->off;

done:
    up(&nvfp->fops_sp_lock[NV_FOPS_STACK_INDEX_PROCFS]);

    return ((status < 0) ? status : (int)count);
}

static nv_proc_ops_t nv_procfs_registry_fops = {
     NV_PROC_OPS_SET_OWNER()
    .NV_PROC_OPS_OPEN    = nv_procfs_open_registry,
    .NV_PROC_OPS_READ    = seq_read,
    .NV_PROC_OPS_WRITE   = nv_procfs_write_file,
    .NV_PROC_OPS_LSEEK   = seq_lseek,
    .NV_PROC_OPS_RELEASE = nv_procfs_close_registry,
};

#if defined(CONFIG_PM)
static int
nv_procfs_show_suspend_depth(
    struct seq_file *m,
    void *v
)
{
    seq_printf(m, "default modeset uvm\n");
    return 0;
}

static ssize_t
nv_procfs_write_suspend_depth(
    struct file *file,
    const char __user *buf,
    size_t count,
    loff_t *pos
)
{
    char kbuf[sizeof("modeset\n")];
    unsigned i;

    if (!NV_IS_SUSER())
    {
        return -EPERM;
    }

    if (count < strlen("uvm") || count > sizeof(kbuf))
    {
        return -EINVAL;
    }

    if (copy_from_user(kbuf, buf, count))
    {
        return -EFAULT;
    }

    count = min(count, sizeof(kbuf) - 1);
    for (i = 0; i < count && isalpha(kbuf[i]); i++);
    kbuf[i] = '\0';

    if (strcasecmp(kbuf, "uvm") == 0)
    {
        nv_pm_action_depth = NV_PM_ACTION_DEPTH_UVM;
    }
    else if (strcasecmp(kbuf, "modeset") == 0)
    {
        nv_pm_action_depth = NV_PM_ACTION_DEPTH_MODESET;
    }
    else if (strcasecmp(kbuf, "default") == 0)
    {
        nv_pm_action_depth = NV_PM_ACTION_DEPTH_DEFAULT;
    }
    else
    {
        return -EINVAL;
    }

    return count;
}

static int
nv_procfs_open_suspend_depth(
    struct inode *inode,
    struct file *file
)
{
    return single_open(file, nv_procfs_show_suspend_depth, NULL);
}

static nv_proc_ops_t nv_procfs_suspend_depth_fops = {
     NV_PROC_OPS_SET_OWNER()
    .NV_PROC_OPS_OPEN    = nv_procfs_open_suspend_depth,
    .NV_PROC_OPS_READ    = seq_read,
    .NV_PROC_OPS_WRITE   = nv_procfs_write_suspend_depth,
    .NV_PROC_OPS_LSEEK   = seq_lseek,
    .NV_PROC_OPS_RELEASE = single_release
};

static int
nv_procfs_show_suspend(
    struct seq_file *m,
    void *v
)
{
    seq_printf(m, "suspend hibernate resume\n");
    return 0;
}

static ssize_t
nv_procfs_write_suspend(
    struct file *file,
    const char __user *buf,
    size_t count,
    loff_t *pos
)
{
    NV_STATUS status;
    char kbuf[sizeof("hibernate\n")];
    nv_power_state_t power_state;
    unsigned i;

    if (!NV_IS_SUSER())
    {
        return -EPERM;
    }

    if (count < strlen("resume") || count > sizeof(kbuf))
    {
        return -EINVAL;
    }

    if (copy_from_user(kbuf, buf, count))
    {
        return -EFAULT;
    }

    count = min(count, sizeof(kbuf) - 1);
    for (i = 0; i < count && isalpha(kbuf[i]); i++);
    kbuf[i] = '\0';

    if (strcasecmp(kbuf, "suspend") == 0)
    {
        power_state = NV_POWER_STATE_IN_STANDBY;
    }
    else if (strcasecmp(kbuf, "hibernate") == 0)
    {
        power_state = NV_POWER_STATE_IN_HIBERNATE;
    }
    else if (strcasecmp(kbuf, "resume") == 0)
    {
        power_state = NV_POWER_STATE_RUNNING;
    }
    else
    {
        return -EINVAL;
    }

    status = nv_set_system_power_state(power_state, nv_pm_action_depth);

    return (status != NV_OK) ? -EIO : count;
}

static int
nv_procfs_open_suspend(
    struct inode *inode,
    struct file *file
)
{
    return single_open(file, nv_procfs_show_suspend, NULL);
}

static nv_proc_ops_t nv_procfs_suspend_fops = {
     NV_PROC_OPS_SET_OWNER()
    .NV_PROC_OPS_OPEN    = nv_procfs_open_suspend,
    .NV_PROC_OPS_READ    = seq_read,
    .NV_PROC_OPS_WRITE   = nv_procfs_write_suspend,
    .NV_PROC_OPS_LSEEK   = seq_lseek,
    .NV_PROC_OPS_RELEASE = single_release
};

#endif

/*
 * Forwards error to nv_log_error which exposes data to vendor callback
 */
void
exercise_error_forwarding_va(
    nv_state_t *nv,
    NvU32 err,
    const char *fmt,
    ...
)
{
    va_list arguments;

    va_start(arguments, fmt);
    nv_log_error(nv, err, fmt, arguments);
    va_end(arguments);
}

static int
nv_procfs_show_exercise_error_forwarding(
    struct seq_file *m,
    void *v
)
{
    return 0;
}

static int
nv_procfs_open_exercise_error_forwarding(
    struct inode *inode,
    struct file  *file
)
{
    nv_file_private_t *nvfp = NULL;
    int retval;

    retval = nv_procfs_open_file(inode, file, &nvfp);
    if (retval < 0)
    {
        return retval;
    }

    retval = single_open(file, nv_procfs_show_exercise_error_forwarding, nvfp);
    if (retval < 0)
    {
        nv_procfs_close_file(nvfp);
        return retval;
    }

    retval = NV_READ_LOCK_SYSTEM_PM_LOCK_INTERRUPTIBLE();
    if (retval < 0)
    {
        single_release(inode, file);
        nv_procfs_close_file(nvfp);
    }

    return retval;
}

static int
nv_procfs_close_exercise_error_forwarding(
    struct inode *inode,
    struct file *file
)
{
    struct seq_file *s = file->private_data;
    nv_file_private_t *nvfp = s->private;
    nv_state_t *nv = nvfp->proc_data;
    char *proc_buffer = &((char *)nvfp->data)[0];
    size_t count = nvfp->off;
    int i = 0, status = 0;
    NvU32 xid = 0;
    const NvU8 MAX_XID_DIGITS = 3;

    while (i < count && i <= MAX_XID_DIGITS && proc_buffer[i] != ',')
    {
        if (proc_buffer[i] < '0' || proc_buffer[i] > '9')
        {
            status = -EINVAL;
            goto done;
        }

        xid = xid * 10 + (proc_buffer[i++] - '0');
    }

    if (count > (i + 1) && proc_buffer[i] == ',')
        exercise_error_forwarding_va(nv, xid, &proc_buffer[i + 1], 0xdeadbee0,
                0xdeadbee1, 0xdeadbee2, 0xdeadbee3, 0xdeadbee4, 0xdeadbee5);
    else
        status = -EINVAL;

done:
    NV_READ_UNLOCK_SYSTEM_PM_LOCK();

    single_release(inode, file);

    nv_procfs_close_file(nvfp);

    return status;
}

static nv_proc_ops_t nv_procfs_exercise_error_forwarding_fops = {
     NV_PROC_OPS_SET_OWNER()
    .NV_PROC_OPS_OPEN    = nv_procfs_open_exercise_error_forwarding,
    .NV_PROC_OPS_WRITE   = nv_procfs_write_file,
    .NV_PROC_OPS_RELEASE = nv_procfs_close_exercise_error_forwarding,
};

static int
nv_procfs_read_unbind_lock(
    struct seq_file *s,
    void *v
)
{
    nv_file_private_t *nvfp = s->private;
    nv_state_t *nv = nvfp->proc_data;
    nv_linux_state_t *nvl = NV_GET_NVL_FROM_NV_STATE(nv);

    down(&nvl->ldata_lock);
    if (nv->flags & NV_FLAG_UNBIND_LOCK)
    {
        seq_printf(s, "1\n");
    }
    else
    {
        seq_printf(s, "0\n");
    }
    up(&nvl->ldata_lock);

    return 0;
}

static int
nv_procfs_open_unbind_lock(
    struct inode *inode,
    struct file  *file
)
{
    nv_file_private_t *nvfp = NULL;
    int retval;

    retval = nv_procfs_open_file(inode, file, &nvfp);
    if (retval < 0)
    {
        return retval;
    }

    retval = single_open(file, nv_procfs_read_unbind_lock, nvfp);
    if (retval < 0)
    {
        nv_procfs_close_file(nvfp);
        return retval;
    }

    retval = NV_READ_LOCK_SYSTEM_PM_LOCK_INTERRUPTIBLE();
    if (retval < 0)
    {
        single_release(inode, file);
        nv_procfs_close_file(nvfp);
    }

    return retval;
}

static int
nv_procfs_close_unbind_lock(
    struct inode *inode,
    struct file  *file
)
{
    struct seq_file *s = file->private_data;
    nv_file_private_t *nvfp = s->private;
    nv_state_t *nv;
    nvidia_stack_t *sp = nvfp->fops_sp[NV_FOPS_STACK_INDEX_PROCFS];
    int rc = 0;
    nv_linux_state_t * nvl;
    int value;

    if (0 != nvfp->off)
    {
        nv = nvfp->proc_data;
        nvl = NV_GET_NVL_FROM_NV_STATE(nv);

        if (NULL == nvfp->data || NULL == nv)
        {
            rc = -EINVAL;
            goto done;
        }

        if (sscanf((char *)nvfp->data, "%u\n", &value) != 1)
        {
            rc = -EINVAL;
            goto done;
        }

        down(&nvl->ldata_lock);
        if ((value == 1) && !(nv->flags & NV_FLAG_UNBIND_LOCK))
        {
            if (NV_ATOMIC_READ(nvl->usage_count) == 0)
                rm_unbind_lock(sp, nv);

            if (nv->flags & NV_FLAG_UNBIND_LOCK)
            {
                NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "UnbindLock acquired\n");
            }
            else
            {
                NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "Could not acquire UnbindLock\n");
            }
        }
        else if ((value == 0) && (nv->flags & NV_FLAG_UNBIND_LOCK))
        {
            nv->flags &= ~NV_FLAG_UNBIND_LOCK;
            NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "UnbindLock released\n");
        }
        up(&nvl->ldata_lock);
    }

done:
    NV_READ_UNLOCK_SYSTEM_PM_LOCK();

    single_release(inode, file);

    nv_procfs_close_file(nvfp);

    return rc;
}

static nv_proc_ops_t nv_procfs_unbind_lock_fops = {
    NV_PROC_OPS_SET_OWNER()
    .NV_PROC_OPS_OPEN    = nv_procfs_open_unbind_lock,
    .NV_PROC_OPS_READ    = seq_read,
    .NV_PROC_OPS_WRITE   = nv_procfs_write_file,
    .NV_PROC_OPS_LSEEK   = seq_lseek,
    .NV_PROC_OPS_RELEASE = nv_procfs_close_unbind_lock,
};

static const char*
numa_status_describe(nv_numa_status_t state)
{
    if (state < 0 || state >= NV_NUMA_STATUS_COUNT)
        return "invalid";

    return nv_numa_status_messages[state];
}

static NvBool
numa_is_change_allowed(nv_numa_status_t current_state, nv_numa_status_t requested)
{
    NvBool allowed = NV_TRUE;

    switch (requested) {
        case NV_NUMA_STATUS_OFFLINE:
        case NV_NUMA_STATUS_OFFLINE_FAILED:
            allowed = (current_state == NV_NUMA_STATUS_OFFLINE_IN_PROGRESS);
            break;

        /* All except Offline. */
        case NV_NUMA_STATUS_OFFLINE_IN_PROGRESS:
            allowed = (current_state != NV_NUMA_STATUS_OFFLINE);
            break;

        case NV_NUMA_STATUS_ONLINE:
            allowed = (current_state == NV_NUMA_STATUS_ONLINE_IN_PROGRESS);
            break;

        case NV_NUMA_STATUS_ONLINE_FAILED:
            allowed = (current_state == NV_NUMA_STATUS_ONLINE_IN_PROGRESS) ||
                      (current_state == NV_NUMA_STATUS_ONLINE);
            break;

        case NV_NUMA_STATUS_ONLINE_IN_PROGRESS:
            allowed = (current_state == NV_NUMA_STATUS_OFFLINE);
            break;

        /* Fallthrough. */
        case NV_NUMA_STATUS_DISABLED:
        default:
            return NV_FALSE;
    }

    return allowed;
}

static NV_STATUS
numa_status_read(
        nv_state_t *nv,
        nv_stack_t *sp,
        NvS32 *nid,
        NvS32 *status,
        NvU64 *numa_mem_addr,
        NvU64 *numa_mem_size,
        nv_blacklist_addresses_t *blacklist
)
{
    NV_STATUS rm_status;
    nv_linux_state_t *nvl = NV_GET_NVL_FROM_NV_STATE(nv);

    down(&nvl->ldata_lock);

    /*
     * If GPU has not been initialized but NUMA info is valid, populate
     * NUMA node ID and status. Memory range and blacklist cannot be read
     * at this point so fill in dummy values.
     */
    if (!(nv->flags & NV_FLAG_OPEN))
    {
#if defined(NVCPU_PPC64LE)
        if (nv_numa_info_valid(nvl))
        {
            *nid = nvl->npu->numa_info.node_id;
            *status = nv_get_numa_status(nvl);
            *numa_mem_addr = 0;
            *numa_mem_size = 0;
            memset(blacklist, 0x0, sizeof(*blacklist));
        }
#endif

        rm_status = NV_ERR_NOT_READY;
        goto done;
    }

    rm_status = rm_get_gpu_numa_info(sp, nv,
                                     nid, numa_mem_addr, numa_mem_size, blacklist);
    *status = nv_get_numa_status(nvl);

done:
    up(&nvl->ldata_lock);
    return rm_status;
}

static int
nv_procfs_read_offline_pages(
    struct seq_file *s,
    void *v
)
{
    NvU32 i;
    int retval = 0;
    NV_STATUS rm_status;
    nv_ioctl_numa_info_t numa_info;
    nv_file_private_t *nvfp = s->private;
    nv_stack_t *sp = nvfp->fops_sp[NV_FOPS_STACK_INDEX_PROCFS];
    nv_state_t *nv = nvfp->proc_data;

    rm_status = numa_status_read(nv, sp,
                                 &numa_info.nid,
                                 &numa_info.status,
                                 &numa_info.numa_mem_addr,
                                 &numa_info.numa_mem_size,
                                 &numa_info.blacklist_addresses);

    if (rm_status != NV_OK)
        return -EIO;

    for (i = 0; i < numa_info.blacklist_addresses.numEntries; ++i)
    {
        seq_printf(s, "%p\n",
                (void*) numa_info.blacklist_addresses.addresses[i]);
    }

    return retval;
}

static int
nv_procfs_open_offline_pages(
        struct inode *inode,
        struct file  *file
)
{
    int retval;
    nv_file_private_t *nvfp = NULL;

    retval = nv_procfs_open_file(inode, file, &nvfp);
    if (retval < 0)
    {
        return retval;
    }

    retval = single_open(file, nv_procfs_read_offline_pages, nvfp);
    if (retval < 0)
    {
        nv_procfs_close_file(nvfp);
        return retval;
    }

    retval = NV_READ_LOCK_SYSTEM_PM_LOCK_INTERRUPTIBLE();
    if (retval < 0)
    {
        single_release(inode, file);
        nv_procfs_close_file(nvfp);
    }

    return retval;
}

static int
nv_procfs_close_offline_pages(
        struct inode *inode,
        struct file  *file
)
{
    struct seq_file *s = file->private_data;
    nv_file_private_t *nvfp = s->private;

    NV_READ_UNLOCK_SYSTEM_PM_LOCK();

    single_release(inode, file);

    nv_procfs_close_file(nvfp);

    return 0;
}

static int
nv_procfs_read_numa_status(
    struct seq_file *s,
    void *v
)
{
    int retval = 0;
    NV_STATUS rm_status;
    nv_ioctl_numa_info_t numa_info;
    nv_file_private_t *nvfp = s->private;
    nv_stack_t *sp = nvfp->fops_sp[NV_FOPS_STACK_INDEX_PROCFS];
    nv_state_t *nv = nvfp->proc_data;

    rm_status = numa_status_read(nv, sp,
                                 &numa_info.nid,
                                 &numa_info.status,
                                 &numa_info.numa_mem_addr,
                                 &numa_info.numa_mem_size,
                                 &numa_info.blacklist_addresses);

    if ((rm_status != NV_OK) && (rm_status != NV_ERR_NOT_READY))
        return -EIO;

    /* Note: RM clients need to read block size from sysfs. */
    seq_printf(s, "Node:      %d\n", numa_info.nid);
    seq_printf(s, "Status:    %s\n", numa_status_describe(numa_info.status));

    if (rm_status == NV_OK)
    {
        seq_printf(s, "Address:   %llx\n", numa_info.numa_mem_addr);
        seq_printf(s, "Size:      %llx\n", numa_info.numa_mem_size);
    }

    return retval;
}

static int
nv_procfs_open_numa_status(
        struct inode *inode,
        struct file  *file
)
{
    int retval;
    nv_file_private_t *nvfp = NULL;

    retval = nv_procfs_open_file(inode, file, &nvfp);
    if (retval < 0)
    {
        return retval;
    }

    retval = single_open(file, nv_procfs_read_numa_status, nvfp);
    if (retval < 0)
    {
        nv_procfs_close_file(nvfp);
        return retval;
    }

    retval = NV_READ_LOCK_SYSTEM_PM_LOCK_INTERRUPTIBLE();
    if (retval < 0)
    {
        single_release(inode, file);
        nv_procfs_close_file(nvfp);
    }

    return retval;
}

static int
nv_procfs_close_numa_status(
        struct inode *inode,
        struct file  *file
)
{
    int retval = 0;
    struct seq_file *s = file->private_data;
    nv_file_private_t *nvfp = s->private;
    nvidia_stack_t *sp = nvfp->fops_sp[NV_FOPS_STACK_INDEX_PROCFS];
    nv_state_t *nv = nvfp->proc_data;
    nv_linux_state_t *nvl = NV_GET_NVL_FROM_NV_STATE(nv);

    const size_t MAX_STATES = ARRAY_SIZE(nv_numa_status_messages);
    nv_numa_status_t current_state = nv_get_numa_status(nvl);
    char *cmd = nvfp->data;

    down(&nvl->ldata_lock);

    if (nvfp->off != 0)
    {
        NvU32 state;
        nv_numa_status_t requested = NV_NUMA_STATUS_DISABLED;
        NV_STATUS rm_status = NV_OK;

        for (state = 0; state < MAX_STATES; ++state)
        {
            if (strncmp(nv_numa_status_messages[state],
                        cmd,
                        NV_NUMA_STATUS_MSG_LEN) == 0)
            {
                requested = state;
                break;
            }
        }

        if (requested != current_state)
        {
            /* Validate state transition. */
            if (!numa_is_change_allowed(current_state, requested))
            {
                retval = -EINVAL;
                goto done;
            }

            if (requested == NV_NUMA_STATUS_OFFLINE_IN_PROGRESS)
            {
                /*
                 * If this call fails, RM is not ready to offline
                 * memory => retain status.
                 */
                rm_status = rm_gpu_numa_offline(sp, nv);
            }

            if (rm_status == NV_OK)
            {
                retval = nv_set_numa_status(nvl, requested);
                if (retval < 0)
                    goto done;

                if (requested == NV_NUMA_STATUS_ONLINE)
                {
                    rm_status = rm_gpu_numa_online(sp, nv);
                }
            }

            retval = (rm_status == NV_OK) ? retval: -EBUSY;
        }
    }

done:
    up(&nvl->ldata_lock);

    NV_READ_UNLOCK_SYSTEM_PM_LOCK();

    single_release(inode, file);

    nv_procfs_close_file(nvfp);

    return retval;
}

static const nv_proc_ops_t nv_procfs_numa_status_fops = {
    NV_PROC_OPS_SET_OWNER()
    .NV_PROC_OPS_OPEN    = nv_procfs_open_numa_status,
    .NV_PROC_OPS_READ    = seq_read,
    .NV_PROC_OPS_WRITE   = nv_procfs_write_file,
    .NV_PROC_OPS_LSEEK   = seq_lseek,
    .NV_PROC_OPS_RELEASE = nv_procfs_close_numa_status,
};

static const nv_proc_ops_t nv_procfs_offline_pages_fops = {
    NV_PROC_OPS_SET_OWNER()
    .NV_PROC_OPS_OPEN    = nv_procfs_open_offline_pages,
    .NV_PROC_OPS_READ    = seq_read,
    .NV_PROC_OPS_LSEEK   = seq_lseek,
    .NV_PROC_OPS_RELEASE = nv_procfs_close_offline_pages,
};

static int
nv_procfs_read_text_file(
    struct seq_file *s,
    void *v
)
{
    seq_puts(s, s->private);
    return 0;
}

NV_DEFINE_SINGLE_NVRM_PROCFS_FILE(text_file);

static void
nv_procfs_add_text_file(
    struct proc_dir_entry *parent,
    const char *filename,
    const char *text
)
{
    NV_CREATE_PROC_FILE(filename, parent, text_file, (void *)text);
}

static void
nv_procfs_unregister_all(struct proc_dir_entry *entry, struct proc_dir_entry *delimiter)
{
#if defined(NV_PROC_REMOVE_PRESENT)
    proc_remove(entry);
#else
    while (entry)
    {
        struct proc_dir_entry *next = entry->next;
        if (entry->subdir)
            nv_procfs_unregister_all(entry->subdir, delimiter);
        remove_proc_entry(entry->name, entry->parent);
        if (entry == delimiter)
            break;
        entry = next;
    }
#endif
}
#endif

void nv_procfs_add_warning(
    const char *filename,
    const char *text
)
{
#if defined(CONFIG_PROC_FS)
    nv_procfs_add_text_file(proc_nvidia_warnings, filename, text);
#endif
}

int nv_register_procfs(void)
{
#if defined(CONFIG_PROC_FS)
    NvU32 i = 0;
    char nv_dir_name[20];
    struct proc_dir_entry *entry;

    snprintf(nv_dir_name, sizeof(nv_dir_name), "driver/%s", nv_device_name);

    nv_dir_name[sizeof(nv_dir_name) - 1] = '\0';

    proc_nvidia = NV_CREATE_PROC_DIR(nv_dir_name, NULL);

    if (!proc_nvidia)
        goto failed;

    entry = NV_CREATE_PROC_FILE("params", proc_nvidia, params, NULL);
    if (!entry)
        goto failed;

    entry = NV_CREATE_PROC_FILE("registry", proc_nvidia, registry, NULL);
    if (!entry)
        goto failed;

#if defined(CONFIG_PM)
    entry = NV_CREATE_PROC_FILE("suspend_depth", proc_nvidia, suspend_depth, NULL);
    if (!entry)
        goto failed;

    entry = NV_CREATE_PROC_FILE("suspend", proc_nvidia, suspend, NULL);
    if (!entry)
        goto failed;
#endif

    proc_nvidia_warnings = NV_CREATE_PROC_DIR("warnings", proc_nvidia);
    if (!proc_nvidia_warnings)
        goto failed;
    nv_procfs_add_text_file(proc_nvidia_warnings, "README", __README_warning);

    proc_nvidia_patches = NV_CREATE_PROC_DIR("patches", proc_nvidia);
    if (!proc_nvidia_patches)
        goto failed;

    for (i = 0; __nv_patches[i].short_description; i++)
    {
        nv_procfs_add_text_file(proc_nvidia_patches,
            __nv_patches[i].short_description, __nv_patches[i].description);
    }

    nv_procfs_add_text_file(proc_nvidia_patches, "README", __README_patches);

    entry = NV_CREATE_PROC_FILE("version", proc_nvidia, version, NULL);
    if (!entry)
        goto failed;

    proc_nvidia_gpus = NV_CREATE_PROC_DIR("gpus", proc_nvidia);
    if (!proc_nvidia_gpus)
        goto failed;
#endif
    return 0;
#if defined(CONFIG_PROC_FS)
failed:
    nv_procfs_unregister_all(proc_nvidia, proc_nvidia);
    return -ENOMEM;
#endif
}

void nv_unregister_procfs(void)
{
#if defined(CONFIG_PROC_FS)
    nv_procfs_unregister_all(proc_nvidia, proc_nvidia);
#endif
}

int nv_procfs_add_gpu(nv_linux_state_t *nvl)
{
#if defined(CONFIG_PROC_FS)
    nv_state_t *nv;

    /* Buffer size is 32 in order to fit the full name when PCI domain is 32 bit. */
    char name[32];
    struct proc_dir_entry *proc_nvidia_gpu, *entry;

    nv = NV_STATE_PTR(nvl);

    snprintf(name, sizeof(name), "%04x:%02x:%02x.%1x",
                   nv->pci_info.domain, nv->pci_info.bus,
                   nv->pci_info.slot, PCI_FUNC(nvl->pci_dev->devfn));

    proc_nvidia_gpu = NV_CREATE_PROC_DIR(name, proc_nvidia_gpus);
    if (!proc_nvidia_gpu)
        goto failed;

    entry = NV_CREATE_PROC_FILE("information", proc_nvidia_gpu, gpu_info,
                                nv);
    if (!entry)
        goto failed;

    entry = NV_CREATE_PROC_FILE("registry", proc_nvidia_gpu, registry, nv);
    if (!entry)
        goto failed;

    if (IS_EXERCISE_ERROR_FORWARDING_ENABLED())
    {
        entry = NV_CREATE_PROC_FILE("exercise_error_forwarding", proc_nvidia_gpu,
                                    exercise_error_forwarding, nv);
        if (!entry)
            goto failed;
    }

    if (IS_VGX_HYPER())
    {
        entry = NV_CREATE_PROC_FILE("unbindLock", proc_nvidia_gpu, unbind_lock, nv);
        if (!entry)
            goto failed;
    }

    if (nv_get_numa_status(nvl) != NV_IOCTL_NUMA_STATUS_DISABLED)
    {
        entry = NV_CREATE_PROC_FILE("numa_status", proc_nvidia_gpu, numa_status,
                                    nv);
        if (!entry)
            goto failed;

        entry = NV_CREATE_PROC_FILE("offline_pages", proc_nvidia_gpu, offline_pages,
                                    nv);
        if (!entry)
            goto failed;
    }

    nvl->proc_dir = proc_nvidia_gpu;
#endif
    return 0;
#if defined(CONFIG_PROC_FS)
failed:
    if (proc_nvidia_gpu)
    {
        nv_procfs_unregister_all(proc_nvidia_gpu, proc_nvidia_gpu);
    }
    return -1;
#endif
}

void nv_procfs_remove_gpu(nv_linux_state_t *nvl)
{
#if defined(CONFIG_PROC_FS)
    nv_procfs_unregister_all(nvl->proc_dir, nvl->proc_dir);
#endif
}