#ifndef SYSEMU_H
#define SYSEMU_H
/* Misc. things related to the system emulator.  */

/* vl.c */
extern const char *bios_name;
extern const char *bios_dir;

extern int vm_running;
extern const char *qemu_name;

typedef struct vm_change_state_entry VMChangeStateEntry;
typedef void VMChangeStateHandler(void *opaque, int running);
typedef void VMStopHandler(void *opaque, int reason);

VMChangeStateEntry *qemu_add_vm_change_state_handler(VMChangeStateHandler *cb,
                                                     void *opaque);
void qemu_del_vm_change_state_handler(VMChangeStateEntry *e);

int qemu_add_vm_stop_handler(VMStopHandler *cb, void *opaque);
void qemu_del_vm_stop_handler(VMStopHandler *cb, void *opaque);

void vm_start(void);
void vm_stop(int reason);

int64_t cpu_get_ticks(void);
void cpu_enable_ticks(void);
void cpu_disable_ticks(void);

void qemu_system_reset_request(void);
void qemu_system_shutdown_request(void);
void qemu_system_powerdown_request(void);
int qemu_shutdown_requested(void);
int qemu_reset_requested(void);
int qemu_powerdown_requested(void);
#if !defined(TARGET_SPARC) && !defined(TARGET_I386)
// Please implement a power failure function to signal the OS
#define qemu_system_powerdown() do{}while(0)
#else
void qemu_system_powerdown(void);
#endif
void qemu_system_reset(void);

void cpu_save(QEMUFile *f, void *opaque);
int cpu_load(QEMUFile *f, void *opaque, int version_id);

void qemu_get_launch_info(int *argc, char ***argv,
                          int *opt_daemonize, const char **opt_incoming);

void do_savevm(const char *name);
void do_loadvm(const char *name);
void do_delvm(const char *name);
void do_info_snapshots(void);

int qemu_live_savevm_state(QEMUFile *f);
int qemu_live_loadvm_state(QEMUFile *f);

void main_loop_wait(int timeout);

int check_params(char *buf, int buf_size, char **params, const char *str);
int get_param_value(char *buf, int buf_size, const char *tag,
                    const char *str);

/* Polling handling */

/* return TRUE if no sleep should be done afterwards */
typedef int PollingFunc(void *opaque);

int qemu_add_polling_cb(PollingFunc *func, void *opaque);
void qemu_del_polling_cb(PollingFunc *func, void *opaque);

#ifdef _WIN32
/* Wait objects handling */
typedef void WaitObjectFunc(void *opaque);

int qemu_add_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque);
void qemu_del_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque);
#endif

/* TAP win32 */
int tap_win32_init(VLANState *vlan, const char *ifname);

/* SLIRP */
void do_info_slirp(void);

extern int bios_size;
extern int cirrus_vga_enabled;
extern int vmsvga_enabled;
extern int graphic_width;
extern int graphic_height;
extern int graphic_depth;
extern const char *keyboard_layout;
extern int win2k_install_hack;
extern int alt_grab;
extern int usb_enabled;
extern int smp_cpus;
extern int cursor_hide;
extern int graphic_rotate;
extern int no_quit;
extern int semihosting_enabled;
extern int autostart;
extern int old_param;
extern int hpagesize;
extern const char *bootp_filename;


#ifdef USE_KQEMU
extern int kqemu_allowed;
#endif

#define MAX_OPTION_ROMS 16
extern const char *option_rom[MAX_OPTION_ROMS];
extern int nb_option_roms;

#ifdef TARGET_SPARC
#define MAX_PROM_ENVS 128
extern const char *prom_envs[MAX_PROM_ENVS];
extern unsigned int nb_prom_envs;
#endif

#if defined (TARGET_PPC)
#define BIOS_SIZE (1024 * 1024)
#elif defined (TARGET_SPARC64)
#define BIOS_SIZE ((512 + 32) * 1024)
#elif defined(TARGET_MIPS)
#define BIOS_SIZE (4 * 1024 * 1024)
#endif

typedef enum {
    IF_IDE, IF_SCSI, IF_FLOPPY, IF_PFLASH, IF_MTD, IF_SD, IF_VIRTIO
} BlockInterfaceType;

typedef struct DriveInfo {
    BlockDriverState *bdrv;
    BlockInterfaceType type;
    int bus;
    int unit;
    int used;
    int drive_opt_idx;
} DriveInfo;

#define MAX_IDE_DEVS	2
#define MAX_SCSI_DEVS	7
#define MAX_DRIVES 32

extern int nb_drives;
extern DriveInfo drives_table[MAX_DRIVES+1];
extern int extboot_drive;

extern int drive_get_index(BlockInterfaceType type, int bus, int unit);
extern int drive_get_max_bus(BlockInterfaceType type);

extern void drive_uninit(BlockDriverState *bdrv);
extern void drive_remove(int index);

struct drive_opt {
    const char *file;
    char opt[1024];
    int used;
};

extern struct drive_opt drives_opt[MAX_DRIVES];
extern int nb_drives_opt;

struct QEMUMachine;
extern int drive_add(const char *file, const char *fmt, ...);
extern int drive_init(struct drive_opt *arg, int snapshot,
                      struct QEMUMachine *machine);

/* acpi */
void qemu_system_cpu_hot_add(int cpu, int state);
void qemu_system_hot_add_init(const char *cpu_model);
void qemu_system_device_hot_add(int pcibus, int slot, int state);

/* device-hotplug */
void device_hot_add(int pcibus, const char *type, const char *opts);
void drive_hot_add(int pcibus, const char *devfn_string, const char *opts);
void device_hot_remove(int pcibus, int slot);
void device_hot_remove_success(int pcibus, int slot);

/* vmchannel devices */

#define MAX_VMCHANNEL_DEVICES 4
void pci_hypercall_init(PCIBus *bus);
void vmchannel_init(CharDriverState *hd, uint32_t deviceid, uint32_t index);

/* serial ports */

#define MAX_SERIAL_PORTS 4

extern CharDriverState *serial_hds[MAX_SERIAL_PORTS];

/* parallel ports */

#define MAX_PARALLEL_PORTS 3

extern CharDriverState *parallel_hds[MAX_PARALLEL_PORTS];

#ifdef NEED_CPU_H
/* loader.c */
int get_image_size(const char *filename);
int load_image(const char *filename, uint8_t *addr); /* deprecated */
int load_image_targphys(const char *filename, target_phys_addr_t, int max_sz);
int load_elf(const char *filename, int64_t virt_to_phys_addend,
             uint64_t *pentry, uint64_t *lowaddr, uint64_t *highaddr);
int load_aout(const char *filename, target_phys_addr_t addr, int max_sz);
int load_uboot(const char *filename, target_ulong *ep, int *is_linux);
int load_uimage(const char *filename, target_ulong *ep,
             target_ulong *la, target_ulong *loaded_image_size,
             int *is_linux);

int fread_targphys(target_phys_addr_t dst_addr, size_t nbytes, FILE *f);
int fread_targphys_ok(target_phys_addr_t dst_addr, size_t nbytes, FILE *f);
int read_targphys(int fd, target_phys_addr_t dst_addr, size_t nbytes);
void pstrcpy_targphys(target_phys_addr_t dest, int buf_size,
                      const char *source);
#endif

#ifdef HAS_AUDIO
struct soundhw {
    const char *name;
    const char *descr;
    int enabled;
    int isa;
    union {
        int (*init_isa) (AudioState *s, qemu_irq *pic);
        int (*init_pci) (PCIBus *bus, AudioState *s);
    } init;
};

extern struct soundhw soundhw[];
#endif

void do_usb_add(const char *devname);
void do_usb_del(const char *devname);
void usb_info(void);

#endif
