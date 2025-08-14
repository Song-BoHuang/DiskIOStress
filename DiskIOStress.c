/*===================================================
Author: Lin Xin-Yu
E-mail: xinyu0123@gmail.com
====================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <linux/fs.h>
#include <linux/falloc.h>
#include <scsi/sg.h>
#include <scsi/scsi.h>
#include <sync.h>

#include "nvme.h"

/*===================================================
| Constant
===================================================*/
#define TRUE                1
#define FALSE               0

#define SIZE_1K             (1024)
#define SIZE_1M             (1024 * SIZE_1K)
#define SIZE_1G             (1024 * SIZE_1M)

#define MAX_THREAD_NUM      256
#define MAX_LOOP_NUM        10000000
#define MAX_TRUNK_SIZE      (20 * SIZE_1M)
#define ONE_MININUTE_IN_SEC 60
#define ONE_HOUR_IN_SEC     (60 * ONE_MININUTE_IN_SEC)
#define ONE_DAY_IN_SEC      (24 * ONE_HOUR_IN_SEC)
#define MAX_TEST_TIME       (24 * ONE_HOUR_IN_SEC) //(86400 * 7)
#define MAX_STREAM_NUM      128
#define MAX_SECTOR_COUNT    2048

#define MAX_SLEEP_TIME      15
#define MAX_SLEEP_DELAY     300
#define MIN_SLEEP_DELAY     30

#define MAX_OTF_DELAY       30
#define MIN_OTF_DELAY       15

#define COLOR_RED           "\x1b[31m"
#define COLOR_GREEN         "\x1b[32m"
#define COLOR_YELLOW        "\x1b[33m"
#define COLOR_BLUE          "\x1b[34m"
#define COLOR_MAGENTA       "\x1b[35m"
#define COLOR_CYAN          "\x1b[36m"
#define COLOR_RESET         "\x1b[0m"

#define U8_MAX              0xFF
#define U16_MAX             0xFFFF
#define U32_MAX             0xFFFFFFFF
#define U64_MAX             0xFFFFFFFFFFFFFFFF

#define S8_MAX              0x7F
#define S16_MAX             0x7FFF
#define S32_MAX             0x7FFFFFFF
#define S64_MAX             0x7FFFFFFFFFFFFFFF

#define LOG2(X)             (31 - __builtin_clz(X))

#define CELSIUS_TO_KELVIN(C) (C + 273)
#define KELVIN_TO_CELSIUS(K) (K - 273)

#define dbg_printf(color, format, ...)   {int tm = get_timeval_sec(gDiskIOInfo.timeval); fprintf(stdout, "\r%sTime(%2dh:%2dm:%2ds) S(%d) O(%d) => ", color, tm / 3600, (tm / 60) % 60, tm % 60, gDiskIOInfo.rtc_delay / 4, gDiskIOInfo.otf_delay / 4);fprintf(stdout, format, ##__VA_ARGS__);fprintf(stdout, "%s", COLOR_RESET); fflush(stdout);}

typedef unsigned long long  U64;
typedef unsigned int        U32;
typedef unsigned short      U16;
typedef unsigned char       U8;

typedef long long           S64;
typedef int                 S32;
typedef short               S16;
typedef char                S8;

enum
{
    STATUS_RUNNING = 0,
    STATUS_PASS,
    STATUS_COMPARE_ERROR,
    STATUS_READ_ERROR,
    STATUS_WRITE_ERROR,
    STATUS_OPEN_ERROR,
    STATUS_FORCE_STOP
};

enum
{
    OTF_DONE = 0,
    OTF_HALT
};

enum
{
    THREAD_STATUS_PAUSE = 0,
    THREAD_STATUS_RUNNING
};

enum
{
    WORKLOAD_SEQ_WRC = 0,
    WORKLOAD_SEQ_WRRC,
    WORKLOAD_SEQ_W1RCN,
    WORKLOAD_RAND_WRC,
    WORKLOAD_RAND_WUR,
    WORKLOAD_MAX
};

enum
{
    PATTERN_ALLZERO = 0,
    PATTERN_ALLONE,
    PATTERN_WORKING_ZERO,
    PATTERN_WORKING_ONE,
    PATTERN_SEQU_INC_BYTE,
    PATTERN_SEQU_DEC_BYTE,
    PATTERN_SEQU_INC_WORD,
    PATTERN_SEQU_DEC_WORD,
    PATTERN_SEQU_INC_DWORD,
    PATTERN_SEQU_DEC_DWORD,
    PATTERN_RANDOM,
    PATTERN_LBA,
    PATTERN_MAX
};

const char* pattern_str[] =
{
    "All Zero",
    "All One",
    "Working Zero",
    "Working One",
    "Sequential Byte Increased",
    "Sequential Byte Decreased",
    "Sequential Word Increased",
    "Sequential Word Decreased",
    "Sequential DWord Increased",
    "Sequential DWord Decreased",
    "Random",
    "LBA with Timestamp",
};

enum
{
    OPS_WRITE = 0,
    OPS_READ,
    OPS_WRITE_UNCOR,
    OPS_NULL,
};

#define IO_ENGINE_SYSTEM    0
#define IO_ENGINE_FILE      1
#define IO_ENGINE_NVME      2
#define IO_ENGINE_USB       3

#define DEFAULT_PATTERN     PATTERN_LBA
#define DEFAULT_WORKLOAD    WORKLOAD_RAND_WRC
#define DISK_IO_ENGINE      IO_ENGINE_USB

#define SUPPORT_BLKDISCARD  TRUE
#define SUPPORT_BLKFLUSH    TRUE
#define SUPPORT_DATA_TAG    FALSE
#define SUPPORT_RE_READ     TRUE
#define SUPPORT_DATA_VERIFY TRUE
#define SUPPORT_SLEEP       TRUE
#define SUPPORT_OTF_FW_UPD  FALSE

// USB power control settings
#define SUPPORT_USB_POWER_CONTROL TRUE
#define USB_AUTOSUSPEND_DELAY     1    // USB auto suspend delay time (seconds)
#define USB_POWER_LOG_FILE        "usb_power.log"

// Sleep mode options - can be combined with bitwise OR
#define SLEEP_MODE_NONE     0
#define SLEEP_MODE_S3       1  // S3 Sleep (suspend to RAM)
#define SLEEP_MODE_S4       2  // S4 Sleep (suspend to disk/hibernate)
#define SLEEP_MODE_BOTH     3  // Both S3 and S4 (alternating)

#define DEFAULT_SLEEP_MODE  SLEEP_MODE_S4  // Default to S4 Sleep (suspend to disk/hibernate)

/*===================================================
| Structure
===================================================*/
typedef struct
{
    U16 enable;
    U16 msl;
    U16 nsa;
    U16 nso;
} StreamDirective_t;

typedef struct
{
    U16 openCnt;
    U16 idTable[MAX_STREAM_NUM];
    U16 rsvd;
} StreamStatus_t;

typedef struct
{
    struct timeval timeval;
    U32 status;
    U32 nr_thread;
    U32 nr_loop;
    U32 nr_trunk;
    U64 sz_trunk;
    U64 nr_block;
    U32 sz_block;
    U32 max_sector;
    U32 slot;
    U32 nsid;
    U32 stream_support;
    struct nvme_id_ctrl id_ctrl;
    struct nvme_id_ns id_ns;
    struct streams_directive_params stream_param;
    StreamStatus_t stream_status;
    U32 rtc_cycle;
    U32 rtc_delay;
    U32 sleep_mode;      // Sleep mode: S3, S4, or both
    U32 current_sleep;   // Current sleep type being used (for alternating mode)
    U32 otf_delay;
    U32 otf_flag;
} DiskIOInfo_t;

typedef struct
{
    U32 id;
    U32 fd;
    U32 ops;
    U32 status;
    U32 nr_loop;
    U32 nr_trunk;
    U32 cr_loop;
    U32 cr_trunk;
    U64 block_per_trunk;
    U64 block_start;
    U64 block_end;
    U32 block_count;
    U32 pattern_type;
    U32 workload;
    unsigned char device_path[256];
    unsigned char* bufR;
    unsigned char* bufW;
} ThreadInfo_t;

typedef int (*CmdFunc_t)(char* device, int argc, char* argv[]);

typedef struct
{
    char* cmdStr;
    char* helpStr;
    char* fmtStr;
    CmdFunc_t pFunc;
} CmdTbl_t;

typedef struct
{
                                               /// indicates critical warnings for the state of the controller (bytes[00])
    U32 criticalWarningSpareSpace:1;           ///< available spare space has fallen below the threshold (bits[00])
    U32 criticalWarningTemperature:1;          ///< temperature has exceeded a critical threshold (bits[01])
    U32 criticalWarningMediaInternalError:1;   ///< device reliability degraded due to media related errors or internal error (bits[02])
    U32 criticalWarningReadOnlyMode:1;         ///< media has been placed in read only mode (bits[03])
    U32 criticalWarningVolatileFail:1;         ///< volatile memory backup device has failed (bits[04])
    U32 reserved:3;                            ///< Reserved (bits[07:05])
    U32 temperature:16;                        ///< Contains the temperature of the overall device (bytes[2:1])
    U32 availableSpare:8;                      ///< Contains normalized percentage of the remaining spare capacity available with (bytes[3])

    U8  availableSpareThreshold;               ///< When the Available Spare falls below the threshold indicated in this field  (bytes[4])
    U8  percentageUsed;                        ///< Contains a vendor specific estimate of the percentage of device life used (bytes[5])
    U8  reserved6[26];                         ///<  Reserved (bytes[31:6])

    U64 dataUnitsRead[2];                      ///<  Contains the number of 512 byte data units the host has read from the controller (bytes[47:32])

    U64 dataUnitsWritten[2];                   ///< the number of 512 byte data units the host has written to the controller (bytes[63:48])
    U64 hostReadCommands[2];                   ///< the number of read commands completed by the controller (bytes[79:64])
    U64 hostWriteCommands[2];                  ///< the number of write commands completed by the controller (bytes[95:80])
    U64 controllerBusyTime[2];                 ///< the amount of time the controller is busy with I/O commands (bytes[111:96])
    U64 powerCycles[2];                        ///< the number of power cycles (bytes[127:112])
    U64 powerOnHours[2];                       ///< the number of power-on hours (bytes[143:128])
    U64 unsafeShutdowns[2];                    ///< the number of unsafe shutdowns (bytes[159:144])
    U64 mediaErrors[2];                        ///< the number of occurrences where controller detected unrecovered data integrity error (bytes[175:160])
    U64 numberofErrorInformationLogEntries[2]; ///< the number of Error Information log entries over life of controller (bytes[191:176])
    U32 WarningTempTime;                       ///< Warning Composite Temperature Time (bytes[195:192])
    U32 CriticalTempTime;                      ///< Critical Composite Temperature Time (bytes[199:196])
    U16 TempSensor1;                           ///< Temperature Sensor 1 (bytes[201:200])
    U16 TempSensor2;                           ///< Temperature Sensor 2 (bytes[203:202])
    U8  reserved202[308];                      ///< Reserved (bytes[511:204])
} LogPageSmart_t;

/*===================================================
| Prototype
===================================================*/
void    sig_handler(int signo);
void*   io_thread_handler(void *data);
void*   timer_thread_handler(void *data);
void*   rtc_thread_handler(void *data);
void*   otf_fwupd_thread_handler(void* data);
U32     get_disk_info(char* device);
void    show_result(struct tm* tm_info);
void    set_process_priority(S32 priority);

// USB power control functions
int     configure_usb_power_for_sleep(void);
int     restore_usb_power_after_wake(void);
int     write_to_sysfs_file(const char* path, const char* value);
void    log_usb_power_status(const char* phase);
U32     get_timeval_sec(struct timeval base_timeval);
double  get_timeval_sec_usec(struct timeval base_timeval);
U32     get_core_number(void);
void    dump_compare_error_buffer(ThreadInfo_t* pThrInfo, unsigned char* bufR, unsigned char* bufW, U64 lba);
void    generate_pattern(unsigned char* bufW, U32 size, U32 pattern_type, U64 curr_block);
void    generate_tag(unsigned char* bufW, ThreadInfo_t* pThrInfo, U64 curr_block);
void    show_usage(int argc, char* argv[]);

int     wl_seq_wrc(ThreadInfo_t* pThrInfo);
int     wl_seq_wrrc(ThreadInfo_t* pThrInfo);
int     wl_seq_w1rcn(ThreadInfo_t* pThrInfo);
int     wl_rand_wrc(ThreadInfo_t* pThrInfo);
int     wl_rand_wur(ThreadInfo_t* pThrInfo);

int     thread_write(ThreadInfo_t* pThrInfo, U64 lba, U32 len, U32 write_hint);
int     thread_read(ThreadInfo_t* pThrInfo, U64 lba, U32 len);
int     thread_writeuncor(ThreadInfo_t* pThrInfo, U64 lba, U32 len);
int     thread_readuncor(ThreadInfo_t* pThrInfo, U64 lba, U32 len);

int     disk_read (int fd, char* buf, U64 lba, U32 len);
int     disk_write(int fd, char* buf, U64 lba, U32 len, U32 write_hint);
int     disk_writeuncor(int fd, U64 lba, U32 len);
int     disk_flush(int fd);
int     disk_trim (int fd, U64 lba, U32 len);
int     disk_reset(int fd);

int     nvme_read (int fd, char* buf, U64 lba, U32 len);
int     nvme_write(int fd, char* buf, U64 lba, U32 len, U32 write_hint);
int     nvme_writeuncor(int fd, U64 lba, U32 len);
int     nvme_flush(int fd, int nsid);
int     nvme_trim(int fd, U64 lba, U32 len, U32 nsid);
int     nvme_reset(int fd);
int     nvme_identify(int fd, int cns, int nsid, void* pBuff);
int     nvme_fw_download(int fd, char* pBuff, int data_len, int offset);
int     nvme_fw_commit(int fd, int action, int slot);
int     nvme_get_log(int fd, int logId, char* pBuff, int data_len);
int     nvme_stream_get_status(int fd, int nsid, StreamStatus_t* status);
int     nvme_stream_get_param(int fd, int nsid, struct streams_directive_params* params);
int     nvme_stream_enable(int fd, int nsid, int enable);
int     nvme_stream_alloc_resource(int fd, int nsid, int num);

// SCSI/UASP USB functions
int     scsi_read (int fd, char* buf, U64 lba, U32 len);
int     scsi_write(int fd, char* buf, U64 lba, U32 len);
int     scsi_writeuncor(int fd, U64 lba, U32 len);
int     scsi_flush(int fd);
int     scsi_trim(int fd, U64 lba, U32 len);
int     scsi_inquiry(int fd, char* buf, int buf_len);
int     scsi_read_capacity(int fd, U64* total_blocks, U32* block_size);
int     scsi_test_unit_ready(int fd);
int     nvme_stream_rel_resource(int fd, int nsid);
int     nvme_stream_rel_id(int fd, int nsid, int id);

U64     hex2dec(char* buf);
void    toLowerCase(char* src, char* dest, int len);
void    dump_buffer(char* buf, U32 len);

int     command_parser(int argc, char* argv[]);
int     single_read      (char* device, int argc, char* argv[]);
int     single_write     (char* device, int argc, char* argv[]);
int     single_trim      (char* device, int argc, char* argv[]);
int     sequential_read  (char* device, int argc, char* argv[]);
int     sequential_write (char* device, int argc, char* argv[]);
int     ramdom_read      (char* device, int argc, char* argv[]);
int     ramdom_write     (char* device, int argc, char* argv[]);
int     reset_controller (char* device, int argc, char* argv[]);
int     fw_activation    (char* device, int argc, char* argv[]);
int     log_page         (char* device, int argc, char* argv[]);

/*===================================================
| Static Variables
===================================================*/
pthread_attr_t attr;
pthread_mutex_t mutex_msg;
pthread_mutex_t mutex_ops;

pthread_t* gThreads        = NULL;
pthread_t* gTimerThread    = NULL;
pthread_t* gRtcThread      = NULL;
pthread_t* gOtfFwUpdThread = NULL;

ThreadInfo_t gThreadInfo[MAX_THREAD_NUM];
DiskIOInfo_t gDiskIOInfo;

const CmdTbl_t cmdList[] =
{
    {"w",       "single write cmd",     "[device] [lba] [len]",                   single_write},
    {"r",       "single read cmd",      "[device] [lba] [len]",                   single_read},
    {"t",       "single trim cmd",      "[device] [lba] [len]",                   single_trim},
    {"rw",      "random write",         "[device] [lba start] [lba end] [count]", ramdom_write},
    {"rr",      "random read",          "[device] [lba start] [lba end] [count]", ramdom_read},
    {"sw",      "sequential write",     "[device] [lba start] [lba end] [count]", sequential_write},
    {"sr",      "sequential read",      "[device] [lba start] [lba end] [count]", sequential_read},
    {"rst",     "reset controller",     "[device]",                               reset_controller},
    {"log",     "log page",             "[device] [log id]",                      log_page},
    {"fwact",   "fw activation",        "[device] [action] [slot]",               fw_activation},
    {"",        "",                     "",                                       NULL}
};

int (*gWorkloadList[WORKLOAD_MAX])(ThreadInfo_t* pThrInfo) =
{
    wl_seq_wrc,
    wl_seq_wrrc,
    wl_seq_w1rcn,
    wl_rand_wrc,
    wl_rand_wur,
};

/*===================================================
| MAIN FUNCTION
===================================================*/
int main(int argc, char *argv[])
{
    U32 t, rc;
    void *status;
    time_t timer;
    struct tm* tm_info;
    char option;

    srand(time(NULL));
    memset(&gDiskIOInfo, 0x00, sizeof(gDiskIOInfo));

    if (argc <= 1)
    {
        show_usage(argc, argv);
        exit(1);
    }
    else
    {
        if (strcmp(argv[1], "/dev/sda") == 0)
        {
            system("lsblk");
            printf("%sDo you really want to perform this opertion on /dev/sda? (y/n)%s\n", COLOR_RED, COLOR_RESET);
            option = fgetc(stdin);

            switch(option)
            {
                case 'y':
                case 'Y':
                    break;
                default:
                    exit(1);
            }
        }
    }

    //=== turn off swap memory
    system("swapoff -a");

    time(&timer);
    tm_info = localtime(&timer);
    gettimeofday(&gDiskIOInfo.timeval, NULL);
    signal(SIGINT, sig_handler);
    srand(time(0));

    if (argc > 2)
    {
        if (command_parser(argc, argv))
        {
            printf("Undefined parameter!\n");
            show_usage(argc, argv);
            exit(1);
        }
        else
        {
            // Command parser succeeded, return to avoid running disk stress test
            return 0;
        }
    }
    else
    {
        if (get_disk_info(argv[1]) == -1)
        {
            printf("Device[%s] not found!\n", argv[1]);
            exit(1);
        }

        // Set default sleep mode
        gDiskIOInfo.sleep_mode = DEFAULT_SLEEP_MODE;
        gDiskIOInfo.current_sleep = (DEFAULT_SLEEP_MODE == SLEEP_MODE_BOTH) ? SLEEP_MODE_S3 : DEFAULT_SLEEP_MODE;


        // Common configuration setup for both sleep mode and normal operation
        gDiskIOInfo.nr_loop   = MAX_LOOP_NUM;
        gDiskIOInfo.sz_trunk  = MAX_TRUNK_SIZE;
        gDiskIOInfo.nr_thread = MAX_THREAD_NUM;
        gDiskIOInfo.nr_trunk  = (gDiskIOInfo.nr_block * gDiskIOInfo.sz_block) / gDiskIOInfo.nr_thread / gDiskIOInfo.sz_trunk;
        gDiskIOInfo.status    = STATUS_RUNNING;

        if (gDiskIOInfo.nr_trunk == 0)
        {
            gDiskIOInfo.sz_trunk  = (10 * SIZE_1M);
            gDiskIOInfo.nr_trunk  = 2;
            gDiskIOInfo.nr_thread = (gDiskIOInfo.nr_block * gDiskIOInfo.sz_block) / gDiskIOInfo.nr_trunk / gDiskIOInfo.sz_trunk;
        }

        printf("=== Configuration =========================\n");
        printf("= Loops            : %d\n", gDiskIOInfo.nr_loop);
        printf("= Threads          : %d\n", gDiskIOInfo.nr_thread);
        printf("= Test Time        : %dd %2dh %2dm %2ds\n", MAX_TEST_TIME / 86400, (MAX_TEST_TIME / 3600) % 24, (MAX_TEST_TIME / 60) % 60, MAX_TEST_TIME % 60);
        printf("= Trunk Size       : %lld MB \n", gDiskIOInfo.sz_trunk / SIZE_1M);
        printf("= Data Pattern     : %s\n", pattern_str[DEFAULT_PATTERN]);
        printf("= Device           : %s\n", argv[1]);
        printf("= Max Sector       : %d (%d KB)\n", gDiskIOInfo.max_sector, gDiskIOInfo.max_sector * gDiskIOInfo.sz_block / 1024);
        printf("= Sector Size      : %d Bytes\n", gDiskIOInfo.sz_block);
        printf("= Capacity         : %.2f GB (0x%llX)\n", (double)gDiskIOInfo.nr_block * gDiskIOInfo.sz_block / (1024 * 1024 * 1024), gDiskIOInfo.nr_block);
        printf("= Stream Directive : %s (MSL:%d, SWS:%d KB, SGS:%d MB)\n", (gDiskIOInfo.stream_support)?"SUPPORT":"NOT SUPPORT", gDiskIOInfo.stream_param.msl, gDiskIOInfo.stream_param.sws, gDiskIOInfo.stream_param.sgs);

        // Display sleep mode configuration
        if (SUPPORT_SLEEP)
        {
            const char* sleep_status;
            switch(gDiskIOInfo.sleep_mode)
            {
                case SLEEP_MODE_NONE: sleep_status = "OFF"; break;
                case SLEEP_MODE_S3:   sleep_status = "S3 Only"; break;
                case SLEEP_MODE_S4:   sleep_status = "S4 Only"; break;
                case SLEEP_MODE_BOTH: sleep_status = "S3+S4 Alternating"; break;
                default:              sleep_status = "Unknown"; break;
            }
            printf("= Sleep Mode       : %s (Sleep:%d, Delay:%d ~ %d Sec)\n", sleep_status, MAX_SLEEP_TIME, MIN_SLEEP_DELAY, MAX_SLEEP_DELAY);
            if (DISK_IO_ENGINE == IO_ENGINE_USB && SUPPORT_USB_POWER_CONTROL)
            {
                printf("= USB Power Control: ON (Auto power-off during sleep)\n");
            }
        }
        else
        {
            printf("= Sleep Mode       : OFF (SUPPORT_SLEEP disabled)\n");
        }

        printf("= OTF Update       : %s (Delay:%d ~ %d Sec)\n", (SUPPORT_OTF_FW_UPD)?"ON":"OFF", MIN_OTF_DELAY, MAX_OTF_DELAY);
        printf("=== Start Testing =========================\n");

        //----------------------------------------------------------------------
        pthread_mutex_init(&mutex_msg, NULL);
        pthread_mutex_init(&mutex_ops, NULL);
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

        gThreads = (pthread_t*)calloc(gDiskIOInfo.nr_thread, sizeof(pthread_t));

        for (t = 0; t < gDiskIOInfo.nr_thread; t++)
        {
            gThreadInfo[t].id              = t;
            gThreadInfo[t].nr_trunk        = gDiskIOInfo.nr_trunk;
            gThreadInfo[t].block_per_trunk = (gDiskIOInfo.sz_trunk / gDiskIOInfo.sz_block);
            gThreadInfo[t].block_count     = t + 1;
            gThreadInfo[t].block_count    %= gDiskIOInfo.max_sector;
            if (gThreadInfo[t].block_count == 0) gThreadInfo[t].block_count = gDiskIOInfo.max_sector;
            gThreadInfo[t].block_start     =  t      * gThreadInfo[t].block_per_trunk * gThreadInfo[t].nr_trunk;
            gThreadInfo[t].block_end       = (t + 1) * gThreadInfo[t].block_per_trunk * gThreadInfo[t].nr_trunk;
            gThreadInfo[t].nr_loop         = gDiskIOInfo.nr_loop;
            gThreadInfo[t].pattern_type    = DEFAULT_PATTERN;
            gThreadInfo[t].workload        = DEFAULT_WORKLOAD;

            memcpy(gThreadInfo[t].device_path, argv[1], strlen(argv[1]));

            rc = pthread_create(&gThreads[t], NULL, io_thread_handler, (void*)&gThreadInfo[t]);

            if (rc)
            {
                printf("ERROR(%d) pthread_create() for IO(%d)\n", rc, t);
                exit(-1);
            }
        }

        gTimerThread = (pthread_t*)calloc(1, sizeof(pthread_t));
        rc = pthread_create(gTimerThread, NULL, timer_thread_handler, NULL);

        if (rc)
        {
            printf("ERROR(%d) pthread_create() for Timer\n", rc);
            exit(-1);
        }

    #if SUPPORT_SLEEP == TRUE
        gRtcThread = (pthread_t*)calloc(1, sizeof(pthread_t));
        rc = pthread_create(gRtcThread, NULL, rtc_thread_handler, NULL);

        if (rc)
        {
            printf("ERROR(%d) pthread_create() for RTC\n", rc);
            exit(-1);
        }
    #endif

    #if SUPPORT_OTF_FW_UPD == TRUE
        gOtfFwUpdThread = (pthread_t*)calloc(1, sizeof(pthread_t));
        rc = pthread_create(gOtfFwUpdThread, NULL, otf_fwupd_thread_handler, NULL);

        if (rc)
        {
            printf("ERROR(%d) pthread_create() for OTF\n", rc);
            exit(-1);
        }
    #endif
        //----------------------------------------------------------------------

        for (t = 0; t < gDiskIOInfo.nr_thread; t++)
        {
            rc = pthread_join(gThreads[t], &status);

            if (rc)
            {
                printf("ERROR(%d) pthread_join() for IO(%d)\n", rc, t);
            }
        }

        free(gThreads);

        if (gDiskIOInfo.status == STATUS_RUNNING)
        {
            gDiskIOInfo.status = STATUS_PASS;
        }

        rc = pthread_join(*gTimerThread, &status);

        if (rc)
        {
            printf("ERROR(%d) pthread_join() for Timer\n", rc);
        }

    #if SUPPORT_SLEEP == TRUE
        rc = pthread_join(*gRtcThread, &status);

        if (rc)
        {
            printf("ERROR(%d) pthread_join() for RTC\n", rc);
        }
    #endif

    #if SUPPORT_OTF_FW_UPD == TRUE
        rc = pthread_join(*gOtfFwUpdThread, &status);

        if (rc)
        {
            printf("ERROR(%d) pthread_join() for OTF\n", rc);
        }
    #endif

        pthread_attr_destroy(&attr);
        pthread_mutex_destroy(&mutex_msg);
        pthread_mutex_destroy(&mutex_ops);
        //----------------------------------------------------------------------

        show_result(tm_info);
    }

    return 0;
}

/*===================================================
| THREAD FUNCTION
===================================================*/
void* io_thread_handler(void *data)
{
    ThreadInfo_t* pThrInfo;

    pThrInfo = (ThreadInfo_t*)data;

    pThrInfo->bufR = calloc(1, SIZE_1M);
    pThrInfo->bufW = calloc(1, SIZE_1M);

    pThrInfo->fd = open(pThrInfo->device_path, O_RDWR);

    if (pThrInfo->fd == -1)
    {
        gDiskIOInfo.status = STATUS_OPEN_ERROR;

        pthread_mutex_lock(&mutex_msg);
        dbg_printf(COLOR_RED, "can not open: %s\n", pThrInfo->device_path);
        pthread_mutex_unlock(&mutex_msg);
    }
    else
    {
        pThrInfo->status = THREAD_STATUS_RUNNING;
        gWorkloadList[pThrInfo->workload](pThrInfo);
        close(pThrInfo->fd);
    }

    free(pThrInfo->bufR);
    free(pThrInfo->bufW);
    pthread_exit(NULL);
}

/*===================================================
| Workload: Sequential Write Read Compare
===================================================*/
int wl_seq_wrc(ThreadInfo_t* pThrInfo)
{
    U64 cmds;
    U64 curr_block;
    U32 ret;
    U32 write_hint = 0;

    if (gDiskIOInfo.stream_support)
    {
        write_hint = (pThrInfo->id % (gDiskIOInfo.stream_param.msl + 1));
    }

    for (pThrInfo->cr_loop = 0; pThrInfo->cr_loop < pThrInfo->nr_loop; pThrInfo->cr_loop++)
    {
        for (pThrInfo->cr_trunk = 0; pThrInfo->cr_trunk < pThrInfo->nr_trunk; pThrInfo->cr_trunk++)
        {
            // === Write ================================
            cmds          = 0;
            curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk;
            pThrInfo->ops = OPS_WRITE;

            generate_pattern(pThrInfo->bufW, SIZE_1M, pThrInfo->pattern_type, curr_block);

            while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
            {
                generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

                cmds++;
                curr_block += pThrInfo->block_count;

                if (gDiskIOInfo.status) return 1;
            }

        #if DISK_IO_ENGINE == IO_ENGINE_SYSTEM
            ioctl(pThrInfo->fd, BLKFLSBUF, 0);
        #elif DISK_IO_ENGINE == IO_ENGINE_FILE
            // For file IO, use fsync instead of BLKFLSBUF
            fsync(pThrInfo->fd);
        #elif DISK_IO_ENGINE == IO_ENGINE_USB
            // For USB IO, use fsync and additional sync for reliability
            fsync(pThrInfo->fd);
            sync();
        #endif

        #if (SUPPORT_BLKFLUSH == TRUE)
            if ((rand() % 100) == 0)
            {
                disk_flush(pThrInfo->fd);
            }
        #endif

            // === Verify ===============================
            cmds          = 0;
            curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk;
            pThrInfo->ops = OPS_READ;
            while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
            {
                generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                thread_read(pThrInfo, curr_block, pThrInfo->block_count);

                cmds++;
                curr_block += pThrInfo->block_count;

                if (gDiskIOInfo.status) return 1;
            }

        #if SUPPORT_BLKDISCARD == TRUE
            disk_trim(pThrInfo->fd, pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk, pThrInfo->block_per_trunk);
        #endif
        }
    }

    return 0;
}

/*===================================================
| Workload: Sequential Write Once and Read Compare
===================================================*/
int wl_seq_w1rcn(ThreadInfo_t* pThrInfo)
{
    U64 cmds;
    U64 curr_block;
    U32 ret;
    U32 write_hint = 0;

    if (gDiskIOInfo.stream_support)
    {
        write_hint = (pThrInfo->id % gDiskIOInfo.stream_param.msl) + 1;
    }

    for (pThrInfo->cr_loop = 0; pThrInfo->cr_loop < pThrInfo->nr_loop; pThrInfo->cr_loop++)
    {
        for (pThrInfo->cr_trunk = 0; pThrInfo->cr_trunk < pThrInfo->nr_trunk; pThrInfo->cr_trunk++)
        {
            if (pThrInfo->cr_loop == 0)
            {
                // === Write ================================
                cmds          = 0;
                curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk;
                pThrInfo->ops = OPS_WRITE;

                generate_pattern(pThrInfo->bufW, SIZE_1M, pThrInfo->pattern_type, curr_block);

                while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
                {
                    generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                    thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

                    cmds++;
                    curr_block += pThrInfo->block_count;

                    if (gDiskIOInfo.status) return 1;
                }

            #if DISK_IO_ENGINE == IO_ENGINE_SYSTEM
                ioctl(pThrInfo->fd, BLKFLSBUF, 0);
            #elif DISK_IO_ENGINE == IO_ENGINE_FILE
                // For file IO, use fsync instead of BLKFLSBUF
                fsync(pThrInfo->fd);
            #elif DISK_IO_ENGINE == IO_ENGINE_USB
                // For USB IO, use fsync and additional sync for reliability
                fsync(pThrInfo->fd);
                sync();
            #endif

            #if (SUPPORT_BLKFLUSH == TRUE)
                if ((rand() % 100) == 0)
                {
                    disk_flush(pThrInfo->fd);
                }
            #endif
            }

            // === Verify ===============================
            cmds          = 0;
            curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk;
            pThrInfo->ops = OPS_READ;
            while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
            {
                generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                thread_read(pThrInfo, curr_block, pThrInfo->block_count);

                cmds++;
                curr_block += pThrInfo->block_count;

                if (gDiskIOInfo.status) return 1;
            }

        #if SUPPORT_BLKDISCARD == TRUE
            disk_trim(pThrInfo->fd, pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk, pThrInfo->block_per_trunk);
        #endif
        }
    }

    return 0;
}

/*==================================================================
| Workload: Sequential Write Read(Repeat previous 10 trunks) Compare
===================================================================*/
#define REPEAT_READ_DEPTH   10
int wl_seq_wrrc(ThreadInfo_t* pThrInfo)
{
    U64 cmds;
    U64 curr_block;
    U32 ret;
    U32 idx;
    U32 ridx;
    U32 write_hint;

    if (gDiskIOInfo.stream_support)
    {
        write_hint = (pThrInfo->id % gDiskIOInfo.stream_param.msl) + 1;
    }

    for (pThrInfo->cr_loop = 0; pThrInfo->cr_loop < pThrInfo->nr_loop; pThrInfo->cr_loop++)
    {
        for (pThrInfo->cr_trunk = 0; pThrInfo->cr_trunk < pThrInfo->nr_trunk; pThrInfo->cr_trunk++)
        {
            // === Write ================================
            cmds          = 0;
            curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk;
            pThrInfo->ops = OPS_WRITE;

            generate_pattern(pThrInfo->bufW, SIZE_1M, pThrInfo->pattern_type, curr_block);

            while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
            {
                generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

                cmds++;
                curr_block += pThrInfo->block_count;

                if (gDiskIOInfo.status) return 1;
            }

            if (pThrInfo->cr_trunk >= REPEAT_READ_DEPTH)    ridx = pThrInfo->cr_trunk - REPEAT_READ_DEPTH;
            else                                            ridx = 0;

            // === Verify ===============================
            for (idx = ridx; idx <= pThrInfo->cr_trunk; idx++)
            {
                cmds          = 0;
                curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * idx;
                pThrInfo->ops = OPS_READ;
                while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
                {
                    generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                    thread_read(pThrInfo, curr_block, pThrInfo->block_count);

                    cmds++;
                    curr_block += pThrInfo->block_count;

                    if (gDiskIOInfo.status) return 1;
                }
            }
        }
    }

    return 0;
}

/*===================================================
| Workload: Random Write Read Compare
===================================================*/
int wl_rand_wrc(ThreadInfo_t* pThrInfo)
{
    U64 cmds;
    U64 curr_block;
    U64 start_block;
    U64 end_block;
    U64 cmd_count;
    U32 write_hint = 0;

    if (gDiskIOInfo.stream_support)
    {
        write_hint = (pThrInfo->id % (gDiskIOInfo.stream_param.msl + 1));
    }

    for (pThrInfo->cr_loop = 0; pThrInfo->cr_loop < pThrInfo->nr_loop; pThrInfo->cr_loop++)
    {
        pThrInfo->block_count = (rand() % gDiskIOInfo.max_sector) + 1;

        start_block = pThrInfo->block_start + (rand() % (pThrInfo->block_end - pThrInfo->block_start));

        do
        {
            end_block = start_block + (rand() % pThrInfo->block_per_trunk) + 1;
        } while((start_block + pThrInfo->block_count) > end_block);

        end_block = (end_block > pThrInfo->block_end) ? pThrInfo->block_end: end_block;
        cmd_count = (end_block - start_block) / pThrInfo->block_count;

        generate_pattern(pThrInfo->bufW, SIZE_1M, pThrInfo->pattern_type, start_block);

        // === Sequentail Write ================================
        curr_block    = start_block;
        cmds          = 0;
        pThrInfo->ops = OPS_WRITE;
        while (cmds++ < cmd_count)
        {
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

            curr_block += pThrInfo->block_count;
            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT)
            {
                cmd_count = cmds;
                break;
            }
        }

        // === Random Write ================================
        cmds          = 0;
        pThrInfo->ops = OPS_WRITE;
        while (cmds++ < cmd_count)
        {
            curr_block = start_block + (rand() % cmd_count) * pThrInfo->block_count;
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT) break;
        }

    #if DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        ioctl(pThrInfo->fd, BLKFLSBUF, 0);
    #elif DISK_IO_ENGINE == IO_ENGINE_FILE
        // For file IO, use fsync instead of BLKFLSBUF
        fsync(pThrInfo->fd);
    #elif DISK_IO_ENGINE == IO_ENGINE_USB
        // For USB IO, use fsync and additional sync for reliability
        fsync(pThrInfo->fd);
        sync();
    #endif

    #if (SUPPORT_BLKFLUSH == TRUE)
        if ((rand() % 100) == 0)
        {
            disk_flush(pThrInfo->fd);
        }
    #endif

wl_rand_wrc_halt:

    #if SUPPORT_OTF_FW_UPD == TRUE
        if (gDiskIOInfo.otf_flag == OTF_HALT)
        {
            pThrInfo->status = THREAD_STATUS_PAUSE;

            while (gDiskIOInfo.otf_flag == OTF_HALT && gDiskIOInfo.status == STATUS_RUNNING) usleep(10000);

            pThrInfo->status = THREAD_STATUS_RUNNING;
        }
    #endif

        // === Sequentail read verfiy ===============================
        curr_block    = start_block;
        cmds          = 0;
        pThrInfo->ops = OPS_READ;
        while (cmds++ < cmd_count)
        {
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_read(pThrInfo, curr_block, pThrInfo->block_count);

            curr_block += pThrInfo->block_count;
            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT) goto wl_rand_wrc_halt;
        }

        // === Random read verfiy ===============================
        cmds          = 0;
        pThrInfo->ops = OPS_READ;
        while (cmds++ < cmd_count)
        {
            curr_block = start_block + (rand() % cmd_count) * pThrInfo->block_count;
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_read(pThrInfo, curr_block, pThrInfo->block_count);

            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT) goto wl_rand_wrc_halt;
        }

    #if SUPPORT_BLKDISCARD == TRUE
        disk_trim(pThrInfo->fd, start_block, end_block - start_block);
    #endif
    }

    return 0;
}

/*===================================================
| Workload: Random Write UNC Read
===================================================*/
int wl_rand_wur(ThreadInfo_t* pThrInfo)
{
    U64 cmds;
    U64 curr_block;
    U64 start_block;
    U64 end_block;
    U64 cmd_count;
    U32 write_hint = 0;

    for (pThrInfo->cr_loop = 0; pThrInfo->cr_loop < pThrInfo->nr_loop; pThrInfo->cr_loop++)
    {
        pThrInfo->block_count = (rand() % gDiskIOInfo.max_sector) + 1;
        start_block = pThrInfo->block_start + (rand() % (pThrInfo->block_end - pThrInfo->block_start));

        generate_pattern(pThrInfo->bufW, SIZE_1M, pThrInfo->pattern_type, start_block);

        do
        {
            end_block = start_block + (rand() % pThrInfo->block_per_trunk) + 1;
        } while((start_block + pThrInfo->block_count) > end_block);

        end_block = (end_block > pThrInfo->block_end) ? pThrInfo->block_end: end_block;
        cmd_count = (end_block - start_block) / pThrInfo->block_count;

        // === Sequentail Write UNC ============================
        curr_block    = start_block;
        cmds          = 0;
        pThrInfo->ops = OPS_WRITE_UNCOR;
        while (cmds++ < cmd_count)
        {
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_writeuncor(pThrInfo, curr_block, pThrInfo->block_count);

            curr_block += pThrInfo->block_count;
            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT)
            {
                cmd_count = cmds;
                break;
            }
        }

        // === Sequentail Write ================================
        curr_block    = start_block;
        cmds          = 0;
        pThrInfo->ops = OPS_WRITE;
        while (cmds++ < cmd_count)
        {
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

            curr_block += pThrInfo->block_count;
            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT)
            {
                cmd_count = cmds;
                break;
            }
        }

        // === Random Write ================================
        cmds          = 0;
        pThrInfo->ops = OPS_WRITE;
        while (cmds++ < cmd_count)
        {
            curr_block = start_block + (rand() % cmd_count) * pThrInfo->block_count;
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT) break;
        }

    #if DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        ioctl(pThrInfo->fd, BLKFLSBUF, 0);
    #elif DISK_IO_ENGINE == IO_ENGINE_FILE
        // For file IO, use fsync instead of BLKFLSBUF
        fsync(pThrInfo->fd);
    #elif DISK_IO_ENGINE == IO_ENGINE_USB
        // For USB IO, use fsync and additional sync for reliability
        fsync(pThrInfo->fd);
        sync();
    #endif

    #if (SUPPORT_BLKFLUSH == TRUE)
        if ((rand() % 100) == 0)
        {
            disk_flush(pThrInfo->fd);
        }
    #endif

wl_rand_wur_halt:

    #if SUPPORT_OTF_FW_UPD == TRUE
        if (gDiskIOInfo.otf_flag == OTF_HALT)
        {
            pThrInfo->status = THREAD_STATUS_PAUSE;

            while (gDiskIOInfo.otf_flag == OTF_HALT && gDiskIOInfo.status == STATUS_RUNNING) usleep(10000);

            pThrInfo->status = THREAD_STATUS_RUNNING;
        }
    #endif

        // === Sequentail read verfiy ===============================
        curr_block    = start_block;
        cmds          = 0;
        pThrInfo->ops = OPS_READ;
        while (cmds++ < cmd_count)
        {
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_readuncor(pThrInfo, curr_block, pThrInfo->block_count);

            curr_block += pThrInfo->block_count;
            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT) goto wl_rand_wur_halt;
        }

        // === Random read verfiy ===============================
        cmds          = 0;
        pThrInfo->ops = OPS_READ;
        while (cmds++ < cmd_count)
        {
            curr_block = start_block + (rand() % cmd_count) * pThrInfo->block_count;
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_readuncor(pThrInfo, curr_block, pThrInfo->block_count);

            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT) goto wl_rand_wur_halt;
        }

    #if SUPPORT_BLKDISCARD == TRUE
        disk_trim(pThrInfo->fd, start_block, end_block - start_block);
    #endif
    }

    return 0;
}

void* timer_thread_handler(void *data)
{
    U32 index = 0;
    char* ops_str[] = {"Write", "Read ", "N/A  "};
    int tm;

    while (1)
    {
        usleep(250000);

        if (gDiskIOInfo.status == STATUS_RUNNING)
        {
            dbg_printf(COLOR_RESET, "%d:%d:%d %s   ", gThreadInfo[index].id, gThreadInfo[index].cr_loop, gThreadInfo[index].cr_trunk, ops_str[gThreadInfo[index].ops]);

            index = (++index == gDiskIOInfo.nr_thread) ? 0: index;

            tm = get_timeval_sec(gDiskIOInfo.timeval);

            if (tm >= MAX_TEST_TIME)
            {
                gDiskIOInfo.status = STATUS_FORCE_STOP;
            }
        }
        else
        {
            pthread_exit(NULL);
        }
    }
}

void* rtc_thread_handler(void *data)
{
    char cmd[80];

    gDiskIOInfo.rtc_delay = MIN_SLEEP_DELAY * 4;

    while (1)
    {
        if (gDiskIOInfo.status == STATUS_RUNNING)
        {
            if (gDiskIOInfo.rtc_delay == 0)
            {
                pthread_mutex_lock(&mutex_ops);

                // Determine which sleep mode to use
                // Using simpler redirection to avoid permission issues
                switch (gDiskIOInfo.sleep_mode)
                {
                    case SLEEP_MODE_NONE:
                        // Skip sleep, just update delay
                        break;

                    case SLEEP_MODE_S3:
                        // S3 Sleep (suspend to RAM)
                        // if (DISK_IO_ENGINE == IO_ENGINE_USB)
                        // {
                        //     configure_usb_power_for_sleep();
                        // }
                        sprintf(cmd, "sudo sh -c 'rtcwake -m mem -s %d > rtc.log 2>&1'", MAX_SLEEP_TIME);
                        if (system(cmd) != 0)
                        {
                            dbg_printf(COLOR_RED, "S3 sleep failed\n");
                        }
                        // if (DISK_IO_ENGINE == IO_ENGINE_USB)
                        // {
                        //     restore_usb_power_after_wake();
                        // }
                        break;

                    case SLEEP_MODE_S4:
                        // S4 Sleep (suspend to disk/hibernate)
                        // if (DISK_IO_ENGINE == IO_ENGINE_USB)
                        // {
                        //     configure_usb_power_for_sleep();
                        // }
                        sprintf(cmd, "sudo sh -c 'rtcwake -m disk -s %d > rtc.log 2>&1'", MAX_SLEEP_TIME);
                        if (system(cmd) != 0)
                        {
                            dbg_printf(COLOR_RED, "S4 sleep failed\n");
                        }
                        // if (DISK_IO_ENGINE == IO_ENGINE_USB)
                        // {
                        //     restore_usb_power_after_wake();
                        // }
                        break;

                    case SLEEP_MODE_BOTH:
                        // Alternate between S3 and S4
                        // if (DISK_IO_ENGINE == IO_ENGINE_USB)
                        // {
                        //     configure_usb_power_for_sleep();
                        // }
                        if (gDiskIOInfo.current_sleep == SLEEP_MODE_S3)
                        {
                            sprintf(cmd, "sudo sh -c 'rtcwake -m mem -s %d > rtc.log 2>&1'", MAX_SLEEP_TIME);
                            if (system(cmd) != 0)
                            {
                                dbg_printf(COLOR_RED, "S3 sleep failed\n");
                            }
                            gDiskIOInfo.current_sleep = SLEEP_MODE_S4; // Switch to S4 next time
                        }
                        else
                        {
                            sprintf(cmd, "sudo sh -c 'rtcwake -m disk -s %d > rtc.log 2>&1'", MAX_SLEEP_TIME);
                            if (system(cmd) != 0)
                            {
                                dbg_printf(COLOR_RED, "S4 sleep failed\n");
                            }
                            gDiskIOInfo.current_sleep = SLEEP_MODE_S3; // Switch to S3 next time
                        }
                        // if (DISK_IO_ENGINE == IO_ENGINE_USB)
                        // {
                        //     restore_usb_power_after_wake();
                        // }
                        break;

                    default:
                        // Unknown sleep mode, skip sleep
                        break;
                }

                gDiskIOInfo.rtc_delay = (MIN_SLEEP_DELAY + (rand() % (MAX_SLEEP_DELAY - MIN_SLEEP_DELAY))) * 4;
                gDiskIOInfo.rtc_cycle++;
                pthread_mutex_unlock(&mutex_ops);
            }
            else
            {
                usleep(250000);
                gDiskIOInfo.rtc_delay--;
            }
        }
        else
        {
            pthread_exit(NULL);
        }
    }
}

void* otf_fwupd_thread_handler(void* data)
{
    U32 flag;
    U32 idx;
    U32 slot = 0;
    int fd = open(gThreadInfo[0].device_path, O_RDWR);
    int rst_fd;
    char baseDev[11] = {0};

    strncpy(baseDev, gThreadInfo[0].device_path, 10);
    rst_fd = open(baseDev, O_SYNC | O_RDWR);

    gDiskIOInfo.otf_delay = MIN_OTF_DELAY * 4;

    while (1)
    {
        if (gDiskIOInfo.status == STATUS_RUNNING)
        {
            if (gDiskIOInfo.otf_delay < 2)
            {
                gDiskIOInfo.otf_flag = OTF_HALT;

                flag = TRUE;
                for (idx = 0; idx < gDiskIOInfo.nr_thread; idx++)
                {
                    if (gThreadInfo[idx].status == THREAD_STATUS_RUNNING)
                    {
                        flag = FALSE;
                        break;
                    }
                }

                if (flag)
                {
                    pthread_mutex_lock(&mutex_ops);
                    nvme_fw_commit(fd, 2, ((slot++) % 2) + 1);
                    nvme_reset(rst_fd);

                    gDiskIOInfo.otf_delay = (MIN_OTF_DELAY + (rand() % (MAX_OTF_DELAY - MIN_OTF_DELAY))) * 4;
                    gDiskIOInfo.otf_flag  = OTF_DONE;
                    pthread_mutex_unlock(&mutex_ops);
                }

                usleep(250000);
            }
            else
            {
                usleep(250000);
                gDiskIOInfo.otf_delay--;
            }
        }
        else
        {
            close(fd);
            close(rst_fd);
            pthread_exit(NULL);
        }
    }
}

/*===================================================
| SINGAL FUNCTION
===================================================*/
void sig_handler(int signo)
{
    U32 t, rc;
    void *status;

    gDiskIOInfo.status = STATUS_FORCE_STOP;

    dbg_printf(COLOR_BLUE, "Destroy all threads, please wait a while...");

    if (gThreads)
    {
        for (t = 0; t < gDiskIOInfo.nr_thread; t++)
        {
            rc = pthread_join(gThreads[t], &status);
        }

        free(gThreads);
    }

    if (gTimerThread)
    {
        rc = pthread_join(*gTimerThread, &status);
    }

    dbg_printf(COLOR_BLUE, "Manual Stop!                               \n");

#if SUPPORT_SLEEP == TRUE
    if (gRtcThread)
    {
        rc = pthread_join(*gRtcThread, &status);
    }
#endif

#if SUPPORT_OTF_FW_UPD == TRUE
    if (gOtfFwUpdThread)
    {
        rc = pthread_join(*gOtfFwUpdThread, &status);
    }
#endif

    pthread_attr_destroy(&attr);
    pthread_mutex_destroy(&mutex_msg);
    pthread_mutex_destroy(&mutex_ops);
    exit(1);
}

/*===================================================
| IMPLEMENTATION
===================================================*/
U32 get_disk_info(char* device)
{
    int fd;
    U32 idx;
    U32 sectorSize;
    struct streams_directive_params params;

    #if DISK_IO_ENGINE == IO_ENGINE_NVME
    sscanf(device, "/dev/nvme%dn%d", &gDiskIOInfo.slot, &gDiskIOInfo.nsid);
    #endif

    fd = open(device, O_RDONLY);

    if (fd != -1)
    {
        U32 allocCnt;

        #if DISK_IO_ENGINE == IO_ENGINE_USB
            // For USB SCSI devices, use SCSI commands to get device info
            U64 total_blocks;
            U32 block_size;

            if (scsi_read_capacity(fd, &total_blocks, &block_size) == 0)
            {
                gDiskIOInfo.sz_block = block_size;
                gDiskIOInfo.nr_block = total_blocks;
                // Set conservative max_sector for USB devices (64 sectors = 32KB max transfer)
                gDiskIOInfo.max_sector = 64; // Conservative limit for USB SCSI devices
            }
            else
            {
                // Fallback to safe defaults if SCSI commands fail
                gDiskIOInfo.sz_block = 512;
                gDiskIOInfo.nr_block = 1000000; // 500MB default
                gDiskIOInfo.max_sector = 64; // Conservative USB limit
            }
        #else
            // For non-USB engines, use block device ioctls
            ioctl(fd, BLKSSZGET, &gDiskIOInfo.sz_block);
            ioctl(fd, BLKGETSIZE, &gDiskIOInfo.nr_block);
            ioctl(fd, BLKSECTGET, &gDiskIOInfo.max_sector);

            if (gDiskIOInfo.max_sector > 2048) gDiskIOInfo.max_sector = 2048;

            // Prevent division by zero
            if (gDiskIOInfo.sz_block == 0)
            {
                gDiskIOInfo.sz_block = 512; // Default sector size
            }

            gDiskIOInfo.max_sector = gDiskIOInfo.max_sector * 512 / gDiskIOInfo.sz_block;
        #endif

        #if DISK_IO_ENGINE == IO_ENGINE_NVME
            nvme_identify(fd, 1, 0, (void*)&gDiskIOInfo.id_ctrl);
            nvme_identify(fd, 0, gDiskIOInfo.nsid, (void*)&gDiskIOInfo.id_ns);

            if (gDiskIOInfo.id_ctrl.oacs & NVME_CTRL_OACS_DIRECTIVES)
            {
                gDiskIOInfo.stream_support = TRUE;

                nvme_stream_enable(fd, gDiskIOInfo.nsid, TRUE);
                nvme_stream_get_param(fd, gDiskIOInfo.nsid, &gDiskIOInfo.stream_param);

                gDiskIOInfo.stream_param.sws = (1 << gDiskIOInfo.id_ns.lbaf[gDiskIOInfo.id_ns.flbas & NVME_NS_FLBAS_LBA_MASK].ds) * gDiskIOInfo.stream_param.sws / 1024;
                gDiskIOInfo.stream_param.sgs = gDiskIOInfo.stream_param.sws * gDiskIOInfo.stream_param.sgs / 1024;

                allocCnt = gDiskIOInfo.stream_param.msl - gDiskIOInfo.stream_param.nsa;

                if (allocCnt)
                {
                    nvme_stream_rel_resource(fd, gDiskIOInfo.nsid);
                    nvme_stream_alloc_resource(fd, gDiskIOInfo.nsid, gDiskIOInfo.stream_param.msl);
                }

                if (gDiskIOInfo.stream_param.nso)
                {
                    nvme_stream_get_status(fd,  gDiskIOInfo.nsid, &gDiskIOInfo.stream_status);

                    for (idx = 0; idx < gDiskIOInfo.stream_status.openCnt; idx++)
                    {
                        nvme_stream_rel_id(fd, gDiskIOInfo.nsid, gDiskIOInfo.stream_status.idTable[idx]);
                    }
                }
            }
        #endif

        close(fd);

        #if DISK_IO_ENGINE != IO_ENGINE_USB
            // Only adjust nr_block for non-USB engines
            gDiskIOInfo.nr_block /= (gDiskIOInfo.sz_block / 512);
        #endif
    }

    return fd;
}

void show_result(struct tm* tm_info)
{
    U32 tm;
    FILE* logFp;
    char buffer[26];

    logFp = fopen("log.txt", "a+");

    strftime(buffer, 26, "%Y:%m:%d %H:%M:%S", tm_info);

    tm = get_timeval_sec(gDiskIOInfo.timeval);

    switch (gDiskIOInfo.status)
    {
        case STATUS_PASS:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => COMPARE PASS\n",   buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("\n===%s COMPARE PASS %s==========================\n", COLOR_GREEN, COLOR_RESET);
            break;
        case STATUS_COMPARE_ERROR:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => Compare ERROR!\n", buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s COMPARE ERROR %s===========================\n", COLOR_RED, COLOR_RESET);
            break;
        case STATUS_WRITE_ERROR:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => WRITE ERROR!\n",   buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s WRITE ERROR %s=============================\n", COLOR_RED, COLOR_RESET);
            break;
        case STATUS_READ_ERROR:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => READ ERROR!\n",    buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s READ ERROR %s==============================\n", COLOR_RED, COLOR_RESET);
            break;
        case STATUS_FORCE_STOP:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => TIMOUET COMPLETE!\n",    buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("\n===%s TIMOUET COMPLETE %s======================\n", COLOR_CYAN, COLOR_RESET);
            break;
        default:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => UNKNOWN ERROR!\n",    buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s UNKNOWN ERROR:%d %s=============================\n", COLOR_RED, gDiskIOInfo.status, COLOR_RESET);
            break;
    }

    fclose(logFp);
}

void set_process_priority(S32 priority)
{
    int which = PRIO_PROCESS;
    id_t pid;
    int ret;

    pid = getpid();
    ret = setpriority(which, pid, priority);
}

U32 get_timeval_sec(struct timeval base_timeval)
{
    struct timeval curr_timeval;

    gettimeofday(&curr_timeval, NULL);

    return curr_timeval.tv_sec - base_timeval.tv_sec;
}

double get_timeval_sec_usec(struct timeval base_timeval)
{
    struct timeval curr_timeval;

    gettimeofday(&curr_timeval, NULL);

    return (curr_timeval.tv_sec + curr_timeval.tv_usec / 1000000.0) - (base_timeval.tv_sec + base_timeval.tv_usec / 1000000.0);
}

U32 get_core_number(void)
{
    FILE* fp = popen("cat /proc/cpuinfo | grep processor | wc -l", "r");
    U32 nr_core;

    fscanf(fp, "%d", &nr_core);

    pclose(fp);

    return nr_core;
}

void dump_compare_error_buffer(ThreadInfo_t* pThrInfo, unsigned char* bufR, unsigned char* bufW, U64 lba)
{
    FILE* fp;
    U32 i, offset, lba_offset;
    U32 dump_offset;
    U32 lba_tag, hash_tag;
    char filename[256];
    char* bufXOR;

    bufXOR = calloc(1, SIZE_1M);

    for (i = 0; i < pThrInfo->block_count * gDiskIOInfo.sz_block; i++)
    {
        if (bufW[i] != bufR[i])
        {
            offset = i;
            break;
        }
    }

    #if SUPPORT_DATA_TAG == TRUE
        dump_offset = offset & 0xFFFFFFF0;
    #else
        dump_offset = offset;
    #endif

    sprintf(filename, "%08llX_%03X_%X_%d_%d_%d_good.dat", lba, pThrInfo->block_count, offset, pThrInfo->id, pThrInfo->cr_loop, pThrInfo->cr_trunk);

    fp = fopen(filename, "wb+");
    fwrite(bufW, 1, pThrInfo->block_count * gDiskIOInfo.sz_block, fp);
    fclose(fp);

    sprintf(filename, "%08llX_%03X_%X_%d_%d_%d_bad.dat", lba, pThrInfo->block_count, offset, pThrInfo->id, pThrInfo->cr_loop, pThrInfo->cr_trunk);

    fp = fopen(filename, "wb+");
    fwrite(bufR, 1, pThrInfo->block_count * gDiskIOInfo.sz_block, fp);
    fclose(fp);

    for (i = 0; i < (pThrInfo->block_count * gDiskIOInfo.sz_block); i++)
    {
        bufXOR[i] = bufR[i] ^ bufW[i];
    }

    sprintf(filename, "%08llX_%03X_%X_%d_%d_%d_xor.dat", lba, pThrInfo->block_count, offset, pThrInfo->id, pThrInfo->cr_loop, pThrInfo->cr_trunk);

    fp = fopen(filename, "wb+");
    fwrite(bufXOR, 1, pThrInfo->block_count * gDiskIOInfo.sz_block, fp);
    fclose(fp);

    lba_offset = offset / gDiskIOInfo.sz_block;
    lba += lba_offset;

    pthread_mutex_lock(&mutex_msg);
    printf("%s=== GOOD Pattern === LBA:%08llX (LAA:%08llX) Offset:%08X", COLOR_GREEN, lba, lba / 8, offset);

    for (i = 0; i < 512; i++)
    {
        if ((i % 16) == 0)  printf("\n[%08X]:", dump_offset + i);
        printf(" %02X", bufW[dump_offset + i] & 0xFF);
    }

    printf("%s\n", COLOR_RESET);

    printf("%s=== BAD  Pattern === LBA:%08llX (LAA:%08llX) Offset:%08X", COLOR_MAGENTA, lba, lba / 8, offset);
    for (i = 0; i < 512; i++)
    {
        if ((i % 16) == 0)  printf("\n[%08X]:", dump_offset + i);
        printf(" %02X", bufR[dump_offset + i] & 0xFF);

        #if SUPPORT_DATA_TAG == TRUE
            if (((dump_offset + i) & 0x1FF) == 0xF)
            {
                lba_tag = *((U32*)&bufR[(dump_offset + i - 0xF)]);
                printf("%s|%08X", COLOR_RESET, lba_tag);

                if (lba_tag == lba) printf(":OK%s", COLOR_MAGENTA);
                else                printf(":ERROR%s", COLOR_MAGENTA);
            }

            if (((dump_offset + i) & 0x1FF) == 0x1FF)
            {
                hash_tag = *((U32*)&bufR[(dump_offset + i - 3)]);
                printf("%s|%08X", COLOR_RESET, hash_tag);

                if (hash_tag == ((~((U32)lba + 0x12345678)) & 0xFFFFFFFF))  printf(":OK%s", COLOR_MAGENTA);
                else                                                        printf(":ERROR%s", COLOR_MAGENTA);

                lba++;
            }
        #endif
    }

    printf("%s\n", COLOR_RESET);
    pthread_mutex_unlock(&mutex_msg);

    free(bufXOR);
}

void generate_tag(unsigned char* bufW, ThreadInfo_t* pThrInfo, U64 curr_block)
{
#if SUPPORT_DATA_TAG == TRUE
    U32 *ptr;
    U32 tag;
    U32 idx;

    for (idx = 0; idx < pThrInfo->block_count; idx++)
    {
        tag  = curr_block + idx;
        ptr  = (U32*)(bufW + gDiskIOInfo.sz_block * idx);
        *ptr = tag;

        ptr  = (U32*)(bufW + gDiskIOInfo.sz_block * idx + gDiskIOInfo.sz_block - 4);
        *ptr = ~(tag + 0x12345678);
    }
#endif
}

void generate_pattern(unsigned char* bufW, U32 size, U32 pattern_type, U64 curr_block)
{
    U32 i, j;
    struct timeval tv;
    U64 timestamp_ms;

    // Get timestamp at the beginning of all pattern types
    gettimeofday(&tv, NULL);
    timestamp_ms = (U64)tv.tv_sec * 1000 + (U64)tv.tv_usec / 1000;

    switch (pattern_type)
    {
        case PATTERN_ALLZERO:
            memset(bufW, 0x00, size);
            break;
        case PATTERN_ALLONE:
            memset(bufW, 0xFF, size);
            break;
        case PATTERN_WORKING_ONE:
            for (i = 0; i < 32; i++)
            {
                *(((U32*)bufW) + i) = 1 << i;
            }

            for (i = 1; i < size / 32 / 4; i++)
            {
                memcpy(&bufW[i * 32 * 4], &bufW[0], 32 * 4);
            }

            break;
        case PATTERN_WORKING_ZERO:
            for (i = 0; i < 32; i++)
            {
                *(((U32*)bufW) + i) = ~(1 << i);
            }

            for (i = 1; i < size / 32 / 4; i++)
            {
                memcpy(&bufW[i * 32 * 4], &bufW[0], 32 * 4);
            }

            break;
        case PATTERN_SEQU_INC_DWORD:
            for (i = 0; i < size / 4; i++)
            {
                *(((U32*)bufW) + i) = i;
            }
            break;
        case PATTERN_SEQU_DEC_DWORD:
            for (i = 0; i < size / 4; i++)
            {
                *(((U32*)bufW) + i) = U32_MAX - i;
            }
            break;
        case PATTERN_SEQU_INC_WORD:
            for (i = 0; i < size / 2; i++)
            {
                *(((U16*)bufW) + i) = i;
            }
            break;
        case PATTERN_SEQU_DEC_WORD:
            for (i = 0; i < size / 2; i++)
            {
                *(((U16*)bufW) + i) = U32_MAX - i;
            }
            break;
        case PATTERN_SEQU_INC_BYTE:
            for (i = 0; i < size; i++)
            {
                *(bufW + i) = i;
            }
            break;
        case PATTERN_SEQU_DEC_BYTE:
            for (i = 0; i < size; i++)
            {
                *(bufW + i) = U32_MAX - i;
            }
            break;
        case PATTERN_RANDOM:
        {
            for (i = 0; i < (32 * SIZE_1K); i++)
            {
                bufW[i] = rand() % 0xFF;
            }

            for (i = 1; i < size / 32 / SIZE_1K; i++)
            {
                memcpy(&bufW[i * SIZE_1K * 32], &bufW[0], SIZE_1K * 32);
            }
            break;
        }
        case PATTERN_LBA:
        {
            U32 block_size = gDiskIOInfo.sz_block;
            U32 blocks_in_buffer = size / block_size;

            // Clear buffer
            memset(bufW, 0, size);

            // Fill offset-based LBA pattern for each block
            for (i = 0; i < blocks_in_buffer; i++)
            {
                U32 block_offset = i * block_size;
                U64 current_lba = curr_block + i;
                U32* ptr32;
                U64* ptr64;

                // First 8 bytes: timestamp (milliseconds since epoch)
                ptr64 = (U64*)(bufW + block_offset);
                *ptr64 = timestamp_ms;

                // Next 4 bytes: current block's LBA (starting from offset 8)
                ptr32 = (U32*)(bufW + block_offset + 8);
                *ptr32 = (U32)current_lba;

                // From byte 12 onwards: fill with offset-based LBA value to end of block
                ptr32 = (U32*)(bufW + block_offset + 12);
                U32 remaining_bytes = block_size - 12;
                U32 dword_count = remaining_bytes / 4;
                U32 extra_bytes = remaining_bytes % 4;

                // Fill remaining complete DWORDs with offset-based LBA value
                for (j = 0; j < dword_count; j++)
                {
                    ptr32[j] = (U32)current_lba;
                }

                // Handle remaining bytes less than 4
                if (extra_bytes > 0)
                {
                    U32 offset_lba = (U32)current_lba;
                    unsigned char* last_bytes = (unsigned char*)&ptr32[dword_count];
                    unsigned char* lba_bytes = (unsigned char*)&offset_lba;

                    for (j = 0; j < extra_bytes; j++)
                    {
                        last_bytes[j] = lba_bytes[j];
                    }
                }
            }
            break;
        }
        default:
            printf("Error:Invalid Pattern Type\n");
            break;
    }
}

int thread_write(ThreadInfo_t* pThrInfo, U64 lba, U32 len, U32 write_hint)
{
    if (disk_write(pThrInfo->fd, pThrInfo->bufW, lba, len, write_hint))
    {
        pthread_mutex_lock(&mutex_msg);
        gDiskIOInfo.status = STATUS_WRITE_ERROR;
        dbg_printf(COLOR_YELLOW, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] WRITE ERROR\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
        pthread_mutex_unlock(&mutex_msg);

        return 1;
    }

    return 0;
}

int thread_read(ThreadInfo_t* pThrInfo, U64 lba, U32 len)
{
    U32 ret;

    if (disk_read(pThrInfo->fd, pThrInfo->bufR, lba, len))
    {
        pthread_mutex_lock(&mutex_msg);
        gDiskIOInfo.status = STATUS_READ_ERROR;
        dbg_printf(COLOR_YELLOW, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] READ ERROR\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
        pthread_mutex_unlock(&mutex_msg);

        return 1;
    }

#if SUPPORT_DATA_VERIFY == TRUE
    ret = memcmp(pThrInfo->bufR, pThrInfo->bufW, len * gDiskIOInfo.sz_block);

    if (ret)
    {
        pthread_mutex_lock(&mutex_msg);
        gDiskIOInfo.status = STATUS_COMPARE_ERROR;
        dbg_printf(COLOR_RED, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] COMPARE ERROR\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
        pthread_mutex_unlock(&mutex_msg);

        dump_compare_error_buffer(pThrInfo, pThrInfo->bufR, pThrInfo->bufW, lba);

    #if SUPPORT_RE_READ == TRUE
        if (disk_read(pThrInfo->fd, pThrInfo->bufR, lba, pThrInfo->block_count))
        {
            pthread_mutex_lock(&mutex_msg);
            gDiskIOInfo.status = STATUS_READ_ERROR;
            dbg_printf(COLOR_YELLOW, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] READ ERROR\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
            pthread_mutex_unlock(&mutex_msg);

            return 1;
        }

        ret = memcmp(pThrInfo->bufR, pThrInfo->bufW, len * gDiskIOInfo.sz_block);

        if (ret)
        {
            pthread_mutex_lock(&mutex_msg);
            dbg_printf(COLOR_RED, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] COMPARE ERROR(2)\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
            pthread_mutex_unlock(&mutex_msg);

            dump_compare_error_buffer(pThrInfo, pThrInfo->bufR, pThrInfo->bufW, lba);
        }
        else
        {
            dbg_printf(COLOR_GREEN, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] RE-READ PASS\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
        }
    #endif

        return 2;
    }
#endif

    return 0;
}

int thread_writeuncor(ThreadInfo_t* pThrInfo, U64 lba, U32 len)
{
    U32 ret;

    if (disk_writeuncor(pThrInfo->fd, lba, len))
    {
        pthread_mutex_lock(&mutex_msg);
        gDiskIOInfo.status = STATUS_WRITE_ERROR;
        dbg_printf(COLOR_YELLOW, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] WRITE UNC ERROR\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
        pthread_mutex_unlock(&mutex_msg);

        return 1;
    }

    return 0;
}

int thread_readuncor(ThreadInfo_t* pThrInfo, U64 lba, U32 len)
{
    U32 ret;

    if (disk_read(pThrInfo->fd, pThrInfo->bufR, lba, len))
    {
        return 0;
    }

    return 1;
}

int disk_read(int fd, char* buf, U64 lba, U32 len)
{
    #if   DISK_IO_ENGINE == IO_ENGINE_NVME
        if (nvme_read(fd, buf, lba, len)) return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        if (pread(fd, buf, len * gDiskIOInfo.sz_block, lba * gDiskIOInfo.sz_block) != len * gDiskIOInfo.sz_block)  return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_FILE
        // File-based IO: use fseek + fread for file operations
        off_t offset = lba * gDiskIOInfo.sz_block;
        size_t bytes_to_read = len * gDiskIOInfo.sz_block;

        if (lseek(fd, offset, SEEK_SET) == -1) return TRUE;
        if (read(fd, buf, bytes_to_read) != bytes_to_read) return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_USB
        // USB SCSI/UASP optimized IO
        if (scsi_read(fd, buf, lba, len)) return TRUE;
    #else
        printf("read not supported for IO engine:%d\n", DISK_IO_ENGINE);
        exit(1);
    #endif

    return FALSE;
}

int disk_write(int fd, char* buf, U64 lba, U32 len, U32 write_hint)
{
    #if   DISK_IO_ENGINE == IO_ENGINE_NVME
        if (nvme_write(fd, buf, lba, len, write_hint)) return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        if (pwrite(fd, buf, len * gDiskIOInfo.sz_block, lba * gDiskIOInfo.sz_block) != len * gDiskIOInfo.sz_block)  return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_FILE
        // File-based IO: use lseek + write for file operations
        off_t offset = lba * gDiskIOInfo.sz_block;
        size_t bytes_to_write = len * gDiskIOInfo.sz_block;

        if (lseek(fd, offset, SEEK_SET) == -1) return TRUE;
        if (write(fd, buf, bytes_to_write) != bytes_to_write) return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_USB
        // USB SCSI/UASP optimized IO
        if (scsi_write(fd, buf, lba, len)) return TRUE;
    #else
        printf("write not supported for IO engine:%d\n", DISK_IO_ENGINE);
        exit(1);
    #endif

    return FALSE;
}

int disk_writeuncor(int fd, U64 lba, U32 len)
{
    #if   DISK_IO_ENGINE == IO_ENGINE_NVME
        if (nvme_writeuncor(fd, lba, len)) return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        // Write uncorrectable is not supported for system IO engine
        // Return FALSE to indicate success (no-op)
        return FALSE;
    #elif DISK_IO_ENGINE == IO_ENGINE_FILE
        // Write uncorrectable is not supported for file IO engine
        // Return FALSE to indicate success (no-op)
        return FALSE;
    #elif DISK_IO_ENGINE == IO_ENGINE_USB
        // USB SCSI/UASP write uncorrectable
        if (scsi_writeuncor(fd, lba, len)) return TRUE;
    #else
        printf("writeuncor not supported for IO engine:%d\n", DISK_IO_ENGINE);
        exit(1);
    #endif

    return FALSE;
}

int disk_flush(int fd)
{
    #if   DISK_IO_ENGINE == IO_ENGINE_NVME
        if (nvme_flush(fd, gDiskIOInfo.nsid)) return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        fdatasync(fd);
    #elif DISK_IO_ENGINE == IO_ENGINE_FILE
        // File-based IO: use fsync for file operations
        if (fsync(fd) != 0) return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_USB
        // USB SCSI/UASP flush
        if (scsi_flush(fd)) return TRUE;
    #else
        printf("flush not supported for IO engine:%d\n", DISK_IO_ENGINE);
        exit(1);
    #endif

    return FALSE;
}

int disk_trim(int fd, U64 lba, U32 len)
{
    #if   DISK_IO_ENGINE == IO_ENGINE_NVME
        if (nvme_trim(fd, lba, len, gDiskIOInfo.nsid))    return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        // TRIM/DISCARD for system IO engine
        // Using BLKDISCARD ioctl for block devices
        U64 range[2];
        range[0] = lba * gDiskIOInfo.sz_block;  // offset in bytes
        range[1] = len * gDiskIOInfo.sz_block;  // length in bytes

        if (ioctl(fd, BLKDISCARD, range) != 0)
        {
            // TRIM operation failed, but don't treat as fatal error
            return FALSE;
        }
    #elif DISK_IO_ENGINE == IO_ENGINE_FILE
        // TRIM/DISCARD for file IO engine
        // Use fallocate with FALLOC_FL_PUNCH_HOLE to create holes in file
        off_t offset = lba * gDiskIOInfo.sz_block;
        off_t length = len * gDiskIOInfo.sz_block;

        if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, length) != 0)
        {
            // TRIM operation failed, but don't treat as fatal error
            return FALSE;
        }
    #elif DISK_IO_ENGINE == IO_ENGINE_USB
        // USB SCSI/UASP TRIM/UNMAP
        if (scsi_trim(fd, lba, len)) return TRUE;
    #else
        printf("trim not supported for IO engine:%d\n", DISK_IO_ENGINE);
        exit(1);
    #endif

    return FALSE;
}

int disk_reset(int fd)
{
    #if   DISK_IO_ENGINE == IO_ENGINE_NVME
        nvme_reset(fd);
    #elif DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        // Controller reset is not applicable for system IO engine
        // Return FALSE to indicate success (no-op)
        return FALSE;
    #elif DISK_IO_ENGINE == IO_ENGINE_FILE
        // Controller reset is not applicable for file IO engine
        // Return FALSE to indicate success (no-op)
        return FALSE;
    #elif DISK_IO_ENGINE == IO_ENGINE_USB
        // USB device reset: close and reopen the device
        // This simulates a USB device reset/reconnect
        // Note: This is a logical reset, actual USB reset would require root privileges
        sync();  // Ensure all pending data is written
        return FALSE;  // Return success (actual reset not implemented for safety)
    #else
        printf("reset controller not supported for IO engine:%d\n", DISK_IO_ENGINE);
        exit(1);
    #endif

    return FALSE;
}

int nvme_read(int fd, char* buf, U64 lba, U32 len)
{
    int ret;
    struct nvme_user_io io;

    memset(&io, 0x00, sizeof(io));
    io.opcode  = nvme_cmd_read;
    io.slba    = lba;
    io.addr    = (unsigned long)buf;
    io.nblocks = len - 1;

    ret = ioctl(fd, NVME_IOCTL_SUBMIT_IO, (struct nvme_passthru_cmd*)&io);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_write(int fd, char* buf, U64 lba, U32 len, U32 write_hint)
{
    int ret;
    struct nvme_user_io io;

    memset(&io, 0x00, sizeof(io));
    io.opcode   = nvme_cmd_write;
    io.slba     = lba;
    io.addr     = (unsigned long)buf;
    io.nblocks  = len - 1;

    if (write_hint)
    {
        io.control |= NVME_RW_DTYPE_STREAMS;
        io.dsmgmt  |= (write_hint << 16);
    }

    ret = ioctl(fd, NVME_IOCTL_SUBMIT_IO, (struct nvme_passthru_cmd*)&io);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_writeuncor(int fd, U64 lba, U32 len)
{
    int ret;
    struct nvme_passthru_cmd io;

    memset(&io, 0x00, sizeof(io));
    io.opcode  = nvme_cmd_write_uncor;
    io.nsid    = 1,
    io.cdw10   = lba & 0xFFFFFFFF;
    io.cdw11   = lba >> 32;
    io.cdw12   = len - 1;

    ret = ioctl(fd, NVME_IOCTL_IO_CMD, &io);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_flush(int fd, int nsid)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));
    cmd.opcode = nvme_cmd_flush;
    cmd.nsid   = nsid;

    ret = ioctl(fd, NVME_IOCTL_IO_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_trim(int fd, U64 lba, U32 len, U32 nsid)
{
    int ret;
    U32 nr_range = 1;
    struct nvme_dsm_range dsm_range;
    struct nvme_passthru_cmd cmd;

    dsm_range.slba = lba;
    dsm_range.nlb  = len;

    memset(&cmd, 0x0, sizeof(cmd));

    cmd.opcode   = nvme_cmd_dsm;
    cmd.nsid     = nsid;
    cmd.cdw10    = nr_range - 1;
    cmd.cdw11    = NVME_DSMGMT_AD;
    cmd.data_len = nr_range * sizeof(struct nvme_dsm_range);
    cmd.addr     = (unsigned long)&dsm_range;

    ret = ioctl(fd, NVME_IOCTL_IO_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_reset(int fd)
{
    return ioctl(fd, NVME_IOCTL_RESET);
}

int nvme_identify(int fd, int cns, int nsid, void* pBuff)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_identify;
    cmd.nsid     = nsid;
    cmd.data_len = 4096;
    cmd.cdw10    = cns;
    cmd.addr     = (unsigned long)pBuff;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_fw_download(int fd, char* pBuff, int data_len, int offset)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_download_fw;
    cmd.data_len = data_len;
    cmd.cdw10    = (data_len >> 2) - 1,
    cmd.cdw11    = offset >> 2,
    cmd.addr     = (unsigned long)pBuff;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_fw_commit(int fd, int action, int slot)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_activate_fw;
    cmd.cdw10    = (action << 3) | slot,

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_get_log(int fd, int logId, char* pBuff, int data_len)
{
    int ret;
    struct nvme_passthru_cmd cmd;
    U32 numd  = (data_len >> 2) - 1;
    U16 numdu = numd >> 16;
    U16 numdl = numd & 0xffff;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_get_log_page;
    cmd.nsid     = 0xFFFFFFFF;
    cmd.data_len = data_len;
    cmd.cdw10    = (numdl << 16) | logId;
    cmd.cdw11    = numdu;
    cmd.addr     = (unsigned long)&pBuff[0];

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_get_status(int fd, int nsid, StreamStatus_t* status)
{
    int ret;
    struct nvme_directive_recv_cmd cmd;
    struct nvme_passthru_cmd* pcmd;

    pcmd = (struct nvme_passthru_cmd*)&cmd;
    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode     = nvme_admin_directive_recv;
    cmd.numd       = sizeof(StreamStatus_t) / sizeof(U32) - 1;
    cmd.nsid       = nsid;
    pcmd->addr     = (unsigned long)status;
    pcmd->data_len = sizeof(StreamStatus_t);
    cmd.doper      = NVME_DIR_RCV_ST_OP_STATUS;
    cmd.dtype      = NVME_DIR_STREAMS;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }
}

int nvme_stream_get_param(int fd, int nsid, struct streams_directive_params* params)
{
    int ret;
    struct nvme_directive_recv_cmd cmd;
    struct nvme_passthru_cmd* pcmd;

    pcmd = (struct nvme_passthru_cmd*)&cmd;
    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode     = nvme_admin_directive_recv;
    cmd.numd       = sizeof(struct streams_directive_params) / sizeof(U32) - 1;
    cmd.nsid       = nsid;
    pcmd->addr     = (unsigned long)params;
    pcmd->data_len = sizeof(struct streams_directive_params);
    cmd.doper      = NVME_DIR_RCV_ST_OP_PARAM;
    cmd.dtype      = NVME_DIR_STREAMS;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }
}

int nvme_stream_enable(int fd, int nsid, int enable)
{
    int ret;
    struct nvme_directive_send_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_send;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_SND_ID_OP_ENABLE;
    cmd.dtype  = NVME_DIR_IDENTIFY;
    cmd.tdtype = NVME_DIR_STREAMS;
    cmd.endir  = enable;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_alloc_resource(int fd, int nsid, int num)
{
    int ret;
    struct nvme_directive_recv_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_recv;
    cmd.numd   = 0;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_RCV_ST_OP_RESOURCE;
    cmd.dtype  = NVME_DIR_STREAMS;
    cmd.nsr    = num;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }
}

int nvme_stream_rel_resource(int fd, int nsid)
{
    int ret;
    struct nvme_directive_send_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_send;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_SND_ST_OP_REL_RSC;
    cmd.dtype  = NVME_DIR_STREAMS;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_rel_id(int fd, int nsid, int id)
{
    int ret;
    struct nvme_directive_send_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_send;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_SND_ST_OP_REL_ID;
    cmd.dtype  = NVME_DIR_STREAMS;
    cmd.dspec  = id;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

/*===================================================
| SCSI/UASP USB Functions
===================================================*/

int scsi_read(int fd, char* buf, U64 lba, U32 len)
{
    struct sg_io_hdr io_hdr;
    unsigned char cdb[10];
    unsigned char sense_buffer[32];
    int ret;

    // Check for LBA range - use READ(10) for better compatibility
    if (lba > 0xFFFFFFFF || len > 0xFFFF)
    {
        // For large LBA, we should split the operation
        // For now, return error to avoid issues
        return -1;
    }

    // Setup SCSI READ(10) command - more compatible than READ(16)
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    memset(cdb, 0, sizeof(cdb));
    memset(sense_buffer, 0, sizeof(sense_buffer));

    // Use READ(10) for better device compatibility
    cdb[0] = 0x28;  // READ(10) command
    cdb[1] = 0x00;  // No special flags

    // LBA (4 bytes, big endian)
    cdb[2] = (lba >> 24) & 0xff;
    cdb[3] = (lba >> 16) & 0xff;
    cdb[4] = (lba >> 8) & 0xff;
    cdb[5] = lba & 0xff;

    // Transfer length (2 bytes, big endian)
    cdb[7] = (len >> 8) & 0xff;
    cdb[8] = len & 0xff;

    // Setup sg_io_hdr structure
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = 10;
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.dxfer_len = len * gDiskIOInfo.sz_block;
    io_hdr.dxferp = buf;
    io_hdr.cmdp = cdb;
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;  // 20 second timeout
    io_hdr.flags = SG_FLAG_DIRECT_IO;

    ret = ioctl(fd, SG_IO, &io_hdr);

    if (ret < 0 || io_hdr.status != 0)
    {
        // Add debug information for read errors
        if (io_hdr.status != 0)
        {
            unsigned char sense_key = sense_buffer[2] & 0x0f;
            unsigned char asc = sense_buffer[12];
            unsigned char ascq = sense_buffer[13];

            // Only print debug info for first few errors to avoid spam
            static int read_error_count = 0;
            if (read_error_count < 3)
            {
                printf("SCSI READ error: status=0x%02X, sense=0x%02X, asc=0x%02X, ascq=0x%02X, LBA=%llu, len=%u\n",
                       io_hdr.status, sense_key, asc, ascq, (unsigned long long)lba, len);
                read_error_count++;
            }
        }
        return -1;
    }

    return 0;
}

int scsi_write(int fd, char* buf, U64 lba, U32 len)
{
    struct sg_io_hdr io_hdr;
    unsigned char cdb[10];
    unsigned char sense_buffer[32];
    int ret;

    // Check for LBA range - use WRITE(10) for better compatibility
    if (lba > 0xFFFFFFFF || len > 0xFFFF)
    {
        // For large LBA, we should split the operation
        // For now, return error to avoid issues
        return -1;
    }

    // Setup SCSI WRITE(10) command - more compatible than WRITE(16)
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    memset(cdb, 0, sizeof(cdb));
    memset(sense_buffer, 0, sizeof(sense_buffer));

    // Use WRITE(10) for better device compatibility
    cdb[0] = 0x2A;  // WRITE(10) command
    cdb[1] = 0x00;  // No special flags

    // LBA (4 bytes, big endian)
    cdb[2] = (lba >> 24) & 0xff;
    cdb[3] = (lba >> 16) & 0xff;
    cdb[4] = (lba >> 8) & 0xff;
    cdb[5] = lba & 0xff;

    // Transfer length (2 bytes, big endian)
    cdb[7] = (len >> 8) & 0xff;
    cdb[8] = len & 0xff;

    // Setup sg_io_hdr structure
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = 10;
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
    io_hdr.dxfer_len = len * gDiskIOInfo.sz_block;
    io_hdr.dxferp = buf;
    io_hdr.cmdp = cdb;
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;  // 20 second timeout
    io_hdr.flags = SG_FLAG_DIRECT_IO;

    ret = ioctl(fd, SG_IO, &io_hdr);

    if (ret < 0 || io_hdr.status != 0)
    {
        // Add debug information for write errors
        if (io_hdr.status != 0)
        {
            unsigned char sense_key = sense_buffer[2] & 0x0f;
            unsigned char asc = sense_buffer[12];
            unsigned char ascq = sense_buffer[13];

            // Only print debug info for first few errors to avoid spam
            static int error_count = 0;
            if (error_count < 3)
            {
                printf("SCSI WRITE error: status=0x%02X, sense=0x%02X, asc=0x%02X, ascq=0x%02X, LBA=%llu, len=%u\n",
                       io_hdr.status, sense_key, asc, ascq, (unsigned long long)lba, len);
                error_count++;
            }
        }
        return -1;
    }

    return 0;
}

int scsi_writeuncor(int fd, U64 lba, U32 len)
{
    struct sg_io_hdr io_hdr;
    unsigned char cdb[16];
    unsigned char sense_buffer[32];
    int ret;

    // Setup SCSI WRITE UNCORRECTABLE(16) command
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    memset(cdb, 0, sizeof(cdb));
    memset(sense_buffer, 0, sizeof(sense_buffer));

    cdb[0] = 0x9E;  // WRITE UNCORRECTABLE(16) command
    cdb[1] = 0x02;  // WRUNCOR = 1, ANCHOR = 0

    // LBA (8 bytes, big endian)
    cdb[2] = (lba >> 56) & 0xff;
    cdb[3] = (lba >> 48) & 0xff;
    cdb[4] = (lba >> 40) & 0xff;
    cdb[5] = (lba >> 32) & 0xff;
    cdb[6] = (lba >> 24) & 0xff;
    cdb[7] = (lba >> 16) & 0xff;
    cdb[8] = (lba >> 8) & 0xff;
    cdb[9] = lba & 0xff;

    // Number of blocks (4 bytes, big endian)
    cdb[10] = (len >> 24) & 0xff;
    cdb[11] = (len >> 16) & 0xff;
    cdb[12] = (len >> 8) & 0xff;
    cdb[13] = len & 0xff;

    // Setup sg_io_hdr structure
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = 16;
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_NONE;
    io_hdr.dxfer_len = 0;
    io_hdr.dxferp = NULL;
    io_hdr.cmdp = cdb;
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;

    ret = ioctl(fd, SG_IO, &io_hdr);

    if (ret < 0 || io_hdr.status != 0)
    {
        return -1;
    }

    return 0;
}

int scsi_flush(int fd)
{
    struct sg_io_hdr io_hdr;
    unsigned char cdb[10];
    unsigned char sense_buffer[32];
    int ret;

    // Setup SCSI SYNCHRONIZE CACHE(10) command
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    memset(cdb, 0, sizeof(cdb));
    memset(sense_buffer, 0, sizeof(sense_buffer));

    cdb[0] = 0x35;  // SYNCHRONIZE CACHE(10) command
    cdb[1] = 0x00;  // No special flags

    // Setup sg_io_hdr structure
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = 10;
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_NONE;
    io_hdr.dxfer_len = 0;
    io_hdr.dxferp = NULL;
    io_hdr.cmdp = cdb;
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;

    ret = ioctl(fd, SG_IO, &io_hdr);

    if (ret < 0 || io_hdr.status != 0)
    {
        return -1;
    }

    return 0;
}

int scsi_trim(int fd, U64 lba, U32 len)
{
    struct sg_io_hdr io_hdr;
    unsigned char cdb[10];  // UNMAP uses 10-byte CDB
    unsigned char sense_buffer[32];
    unsigned char param_data[24];  // Parameter data for UNMAP
    int ret;

    // Setup SCSI UNMAP command
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    memset(cdb, 0, sizeof(cdb));
    memset(sense_buffer, 0, sizeof(sense_buffer));
    memset(param_data, 0, sizeof(param_data));

    // UNMAP command (10-byte CDB)
    cdb[0] = 0x42;  // UNMAP command
    cdb[1] = 0x00;  // No special flags

    // Parameter list length (2 bytes)
    cdb[7] = 0x00;
    cdb[8] = 0x18;  // 24 bytes

    // Setup parameter data
    // UNMAP parameter list header (8 bytes)
    param_data[0] = 0x00;
    param_data[1] = 0x16;  // UNMAP data length = 22 bytes
    param_data[2] = 0x00;
    param_data[3] = 0x10;  // UNMAP block descriptor data length = 16 bytes

    // UNMAP block descriptor (16 bytes)
    // LBA (8 bytes, big endian)
    param_data[8] = (lba >> 56) & 0xff;
    param_data[9] = (lba >> 48) & 0xff;
    param_data[10] = (lba >> 40) & 0xff;
    param_data[11] = (lba >> 32) & 0xff;
    param_data[12] = (lba >> 24) & 0xff;
    param_data[13] = (lba >> 16) & 0xff;
    param_data[14] = (lba >> 8) & 0xff;
    param_data[15] = lba & 0xff;

    // Number of blocks (4 bytes, big endian)
    param_data[16] = (len >> 24) & 0xff;
    param_data[17] = (len >> 16) & 0xff;
    param_data[18] = (len >> 8) & 0xff;
    param_data[19] = len & 0xff;

    // Setup sg_io_hdr structure
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = 10;
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
    io_hdr.dxfer_len = sizeof(param_data);
    io_hdr.dxferp = param_data;
    io_hdr.cmdp = cdb;
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;

    ret = ioctl(fd, SG_IO, &io_hdr);

    if (ret < 0 || io_hdr.status != 0)
    {
        // Add debug information for trim errors
        if (io_hdr.status != 0)
        {
            unsigned char sense_key = sense_buffer[2] & 0x0f;
            unsigned char asc = sense_buffer[12];
            unsigned char ascq = sense_buffer[13];

            // Only print debug info for first few errors to avoid spam
            static int trim_error_count = 0;
            if (trim_error_count < 3)
            {
                printf("SCSI TRIM error: status=0x%02X, sense=0x%02X, asc=0x%02X, ascq=0x%02X, LBA=%llu, len=%u\n",
                       io_hdr.status, sense_key, asc, ascq, (unsigned long long)lba, len);
                trim_error_count++;
            }
        }
        return -1;
    }

    return 0;
}

int scsi_inquiry(int fd, char* buf, int buf_len)
{
    struct sg_io_hdr io_hdr;
    unsigned char cdb[6];
    unsigned char sense_buffer[32];
    int ret;

    // Setup SCSI INQUIRY command
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    memset(cdb, 0, sizeof(cdb));
    memset(sense_buffer, 0, sizeof(sense_buffer));

    cdb[0] = 0x12;  // INQUIRY command
    cdb[1] = 0x00;  // Standard inquiry data
    cdb[2] = 0x00;  // Page code
    cdb[3] = 0x00;  // Reserved
    cdb[4] = buf_len & 0xff;  // Allocation length
    cdb[5] = 0x00;  // Control

    // Setup sg_io_hdr structure
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = 6;
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.dxfer_len = buf_len;
    io_hdr.dxferp = buf;
    io_hdr.cmdp = cdb;
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;

    ret = ioctl(fd, SG_IO, &io_hdr);

    if (ret < 0 || io_hdr.status != 0)
    {
        return -1;
    }

    return 0;
}

int scsi_read_capacity(int fd, U64* total_blocks, U32* block_size)
{
    struct sg_io_hdr io_hdr;
    unsigned char cdb[16];
    unsigned char sense_buffer[32];
    unsigned char capacity_data[32];
    int ret;

    // Setup SCSI READ CAPACITY(16) command
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    memset(cdb, 0, sizeof(cdb));
    memset(sense_buffer, 0, sizeof(sense_buffer));
    memset(capacity_data, 0, sizeof(capacity_data));

    cdb[0] = 0x9E;  // SERVICE ACTION IN(16)
    cdb[1] = 0x10;  // READ CAPACITY(16) service action

    // Allocation length (4 bytes)
    cdb[10] = 0x00;
    cdb[11] = 0x00;
    cdb[12] = 0x00;
    cdb[13] = 32;   // 32 bytes

    // Setup sg_io_hdr structure
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = 16;
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.dxfer_len = sizeof(capacity_data);
    io_hdr.dxferp = capacity_data;
    io_hdr.cmdp = cdb;
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;

    ret = ioctl(fd, SG_IO, &io_hdr);

    if (ret < 0 || io_hdr.status != 0)
    {
        return -1;
    }

    // Parse capacity data
    *total_blocks = ((U64)capacity_data[0] << 56) |
                   ((U64)capacity_data[1] << 48) |
                   ((U64)capacity_data[2] << 40) |
                   ((U64)capacity_data[3] << 32) |
                   ((U64)capacity_data[4] << 24) |
                   ((U64)capacity_data[5] << 16) |
                   ((U64)capacity_data[6] << 8) |
                   (U64)capacity_data[7];

    *block_size = ((U32)capacity_data[8] << 24) |
                  ((U32)capacity_data[9] << 16) |
                  ((U32)capacity_data[10] << 8) |
                  (U32)capacity_data[11];

    return 0;
}

int scsi_test_unit_ready(int fd)
{
    struct sg_io_hdr io_hdr;
    unsigned char cdb[6];
    unsigned char sense_buffer[32];
    int ret;

    // Setup SCSI TEST UNIT READY command
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    memset(cdb, 0, sizeof(cdb));
    memset(sense_buffer, 0, sizeof(sense_buffer));

    cdb[0] = 0x00;  // TEST UNIT READY command

    // Setup sg_io_hdr structure
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = 6;
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_NONE;
    io_hdr.dxfer_len = 0;
    io_hdr.dxferp = NULL;
    io_hdr.cmdp = cdb;
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;

    ret = ioctl(fd, SG_IO, &io_hdr);

    if (ret < 0 || io_hdr.status != 0)
    {
        return -1;
    }

    return 0;
}

int command_parser(int argc, char** argv)
{
    char  option[256];
    const CmdTbl_t* pCmd;
    CmdFunc_t pFunc = NULL;

    toLowerCase(argv[2], option, strlen(argv[2]));

    pCmd = &cmdList[0];

    while (pCmd->pFunc != NULL)
    {
        if (strcmp(pCmd->cmdStr, option) == 0)
        {
            pFunc = pCmd->pFunc;
            break;
        }

        pCmd++;
    }

    if (pFunc)
    {
        if (strcmp(pCmd->cmdStr, "rst"))
        {
            if (get_disk_info(argv[1]) == -1)
            {
                printf("Device[%s] not found!\n", argv[1]);
                exit(1);
            }
        }

        pFunc(argv[1], argc - 3, (char**)&argv[3]);
    }

    return 0;
}

int single_read(char* device, int argc, char* argv[])
{
    int fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 32);
        U64 lba   = hex2dec(argv[0]);
        U32 len   = hex2dec(argv[1]);

        printf("=== Read LBA[0x%08llX] Len[0x%04X] ==========\n", lba, len);

        disk_read(fd, buf, lba, len);
        dump_buffer(buf, len * gDiskIOInfo.sz_block);

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int single_write(char* device, int argc, char* argv[])
{
    int fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 32);
        U64 lba        = hex2dec(argv[0]);
        U32 len        = hex2dec(argv[1]);
        U32 write_hint = 0;

        printf("=== Write LBA[0x%08llX] Len[0x%04X] ==========\n", lba, len);

        disk_write(fd, buf, lba, len, write_hint);
        dump_buffer(buf, len * gDiskIOInfo.sz_block);

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int single_trim(char* device, int argc, char* argv[])
{
    int fd;

    fd  = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        U64 lba = hex2dec(argv[0]);
        U32 len = hex2dec(argv[1]);

        printf("=== Trim LBA[0x%08llX] Len[0x%04X] ==========\n", lba, len);

        disk_trim(fd, lba, len);

        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int sequential_read(char* device, int argc, char* argv[])
{
    int fd;

    fd = open(device, O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 4);
        U64 block_start;
        U64 block_end;
        U32 block_count;
        U64 lba;
        U32 len;

        U64 idx;
        U64 cmd_count;
        U64 host_read = 0;
        U32 tm;

        if (argc == 3)
        {
            block_start = hex2dec(argv[0]);
            block_end   = hex2dec(argv[1]);
            block_count = hex2dec(argv[2]);
        }
        else
        {
            block_start = 0;
            block_end   = gDiskIOInfo.nr_block;
            block_count = 0x100;
        }

        if (block_count) cmd_count = (block_end - block_start) / block_count;
        else             cmd_count = (block_end - block_start) / gDiskIOInfo.max_sector;

        printf("=== Sequential Read Profile =============\n");
        printf("Block Start : %08llX\n", block_start);
        printf("Block End   : %08llX\n", block_end);
        printf("Block Count : %X (%d Bytes)\n", block_count, block_count * gDiskIOInfo.sz_block);
        printf("Range       : %08llX (%lld MB)\n", (block_end - block_start), (block_end - block_start) / 2 / 1024);
        printf("Cmd Count   : %lld\n", cmd_count);
        printf("=== Start Testing ===================\n");

        lba = block_start;
        len = block_count;

        for (idx = 0; idx < cmd_count; idx++)
        {
            if (block_count == 0) len = (rand() % gDiskIOInfo.max_sector) + 1;

            if ((lba + len) >= gDiskIOInfo.nr_block) break;

            disk_read(fd, buf, lba, len);

            if ((idx & 0xFF) == 0) fprintf(stderr, "\r%lld%c", idx * 100 / cmd_count, '%');

            host_read += len;
        }

        tm = get_timeval_sec(gDiskIOInfo.timeval);

        printf("\r=== Test Completed (%2dh:%2dm:%2ds) ====\n", tm / 3600, (tm / 60) % 60, tm % 60);
        printf("Host Read: %lld MB, (%.1f MB/s)\n", host_read / 2 / 1024, host_read / 2 / 1024 / get_timeval_sec_usec(gDiskIOInfo.timeval));

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int sequential_write(char* device, int argc, char* argv[])
{
    int fd;

    fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 4);
        U64 block_start;
        U64 block_end;
        U32 block_count;
        U64 lba;
        U32 len;

        U64 idx;
        U64 cmd_count;
        U64 host_write = 0;
        U32 tm;
        U32 write_hint = 0;

        memset(buf, 0x5A, 1 * SIZE_1M);

        if (argc == 3)
        {
            block_start = hex2dec(argv[0]);
            block_end   = hex2dec(argv[1]);
            block_count = hex2dec(argv[2]);
        }
        else
        {
            block_start = 0;
            block_end   = gDiskIOInfo.nr_block;
            block_count = 0x100;
        }

        if (block_count) cmd_count = (block_end - block_start) / block_count;
        else             cmd_count = (block_end - block_start) / gDiskIOInfo.max_sector;

        printf("=== Sequential Write Profile =============\n");
        printf("Block Start : %08llX\n", block_start);
        printf("Block End   : %08llX\n", block_end);
        printf("Block Count : %X (%d Bytes)\n", block_count, block_count * gDiskIOInfo.sz_block);
        printf("Range       : %08llX (%lld MB)\n", (block_end - block_start), (block_end - block_start) / 2 / 1024);
        printf("Cmd Count   : %lld\n", cmd_count);
        printf("=== Start Testing ========================\n");

        lba = block_start;
        len = block_count;

        for (idx = 0; idx < cmd_count; idx++)
        {
            if (block_count == 0) len = (rand() % gDiskIOInfo.max_sector) + 1;

            if ((lba + len) >= gDiskIOInfo.nr_block) break;

            disk_write(fd, buf, lba, len, write_hint);

            if ((idx & 0xFF) == 0) fprintf(stderr, "\r%lld%c", idx * 100 / cmd_count, '%');

            host_write += len;
            lba        += len;
        }

        tm = get_timeval_sec(gDiskIOInfo.timeval);

        printf("\r=== Test Completed (%2dh:%2dm:%2ds) ====\n", tm / 3600, (tm / 60) % 60, tm % 60);
        printf("Host Write: %lld MB, (%.1f MB/s)\n", host_write / 2 / 1024, host_write / 2 / 1024 / get_timeval_sec_usec(gDiskIOInfo.timeval));

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int ramdom_read(char* device, int argc, char* argv[])
{
    int fd;

    fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 4);
        U64 block_start;
        U64 block_end;
        U32 block_count;
        U64 lba;
        U32 len;

        U64 idx;
        U64 cmd_count;
        U64 host_read = 0;
        U32 tm;

        if (argc == 3)
        {
            block_start = hex2dec(argv[0]);
            block_end   = hex2dec(argv[1]);
            block_count = hex2dec(argv[2]);
        }
        else
        {
            block_start = 0;
            block_end   = gDiskIOInfo.nr_block;
            block_count = 0x4;
        }

        if (block_count) cmd_count = (block_end - block_start) / block_count;
        else             cmd_count = (block_end - block_start) / gDiskIOInfo.max_sector;

        printf("=== Sequential Read Profile =============\n");
        printf("Block Start : %08llX\n", block_start);
        printf("Block End   : %08llX\n", block_end);
        printf("Block Count : %X (%d Bytes)\n", block_count, block_count * gDiskIOInfo.sz_block);
        printf("Range       : %08llX (%lld MB)\n", (block_end - block_start), (block_end - block_start) / 2 / 1024);
        printf("Cmd Count   : %lld\n", cmd_count);
        printf("=== Start Testing =======================\n");

        lba = block_start;
        len = block_count;

        for (idx = 0; idx < cmd_count; idx++)
        {
            if (block_count == 0) len = (rand() % gDiskIOInfo.max_sector) + 1;

            do
            {
                lba = rand() % gDiskIOInfo.nr_block;
            } while ((lba + len) >= gDiskIOInfo.nr_block);

            disk_read(fd, buf, lba, len);

            if ((idx & 0xFF) == 0) fprintf(stderr, "\r%lld%c", idx * 100 / cmd_count, '%');

            host_read += len;
            lba += len;
        }

        tm = get_timeval_sec(gDiskIOInfo.timeval);

        printf("\r=== Test Completed (%2dh:%2dm:%2ds) ====\n", tm / 3600, (tm / 60) % 60, tm % 60);
        printf("Host Read: %lld MB, (%.1f MB/s)\n", host_read / 2 / 1024, host_read / 2 / 1024 / get_timeval_sec_usec(gDiskIOInfo.timeval));

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int ramdom_write(char* device, int argc, char* argv[])
{
    int fd;

    fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 4);
        U64 block_start;
        U64 block_end;
        U32 block_count;
        U64 lba;
        U32 len;

        U64 idx;
        U64 cmd_count;
        U64 host_write = 0;
        U32 tm;
        U32 write_hint = 0;

        if (argc == 3)
        {
            block_start = hex2dec(argv[0]);
            block_end   = hex2dec(argv[1]);
            block_count = hex2dec(argv[2]);
        }
        else
        {
            block_start = 0;
            block_end   = gDiskIOInfo.nr_block;
            block_count = 0x4;
        }

        if (block_count) cmd_count = (block_end - block_start) / block_count;
        else             cmd_count = (block_end - block_start) / gDiskIOInfo.max_sector;

        printf("=== Random Read Profile =============\n");
        printf("Block Start : %8llX\n", block_start);
        printf("Block End   : %8llX\n", block_end);
        printf("Block Count : %X (%d Bytes)\n", block_count, block_count * gDiskIOInfo.sz_block);
        printf("Range       : %08llX (%lld MB)\n", (block_end - block_start), (block_end - block_start) / 2 / 1024);
        printf("Cmd Count   : %lld\n", cmd_count);
        printf("=== Start Testing ===================\n");

        lba = block_start;
        len = block_count;

        for (idx = 0; idx < cmd_count; idx++)
        {
            if (block_count == 0) len = (rand() % gDiskIOInfo.max_sector) + 1;

            do
            {
                lba = rand() % gDiskIOInfo.nr_block;
            } while ((lba + len) >= gDiskIOInfo.nr_block);

            disk_write(fd, buf, lba, len, write_hint);

            if ((idx & 0xFF) == 0) fprintf(stderr, "\r%lld%c", idx * 100 / cmd_count, '%');

            host_write += len;
        }

        tm = get_timeval_sec(gDiskIOInfo.timeval);

        printf("\r=== Test Completed (%2dh:%2dm:%2ds) ====\n", tm / 3600, (tm / 60) % 60, tm % 60);
        printf("Host Write: %lld MB, (%.1f MB/s)\n", host_write / 2 / 1024, host_write / 2 / 1024 / get_timeval_sec_usec(gDiskIOInfo.timeval));

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int fw_activation(char* device, int argc, char* argv[])
{
    int fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        struct timeval work_tm;
        struct nvme_firmware_log_page fw_log;
        char baseDev[11];
        int rst_fd;
        U32 loop   = atoi(argv[0]);
        U32 action = atoi(argv[1]);
        U32 slot   = atoi(argv[2]);
        FILE* fp;
        U32 binSize;
        char buffer[1 * SIZE_1M];
        U32 idx;
        U32 nr_slots = 0;

        strncpy(baseDev, device, 10);

        rst_fd = open(baseDev, O_SYNC | O_RDWR);

        if (action == 1)
        {
            if (argc == 4)
            {
                fp = fopen(argv[3], "rb");

                fseek(fp, 0, SEEK_END);
                binSize = ftell(fp);
                rewind(fp);

                fread(buffer, 1, binSize, fp);
            }
            else
            {
                printf ("L%d: parameter error\n", __LINE__);
                return 1;
            }
        }

        nvme_get_log(fd, 3, (char*)&fw_log, sizeof(fw_log));

        for (idx = 0; idx < 7; idx++)
        {
            if (fw_log.frs[idx]) nr_slots++;
            else break;
        }

        for (idx = 0; idx < loop; idx++)
        {
            if (action == 1)
            {
                U32 currSize = 0;
                U32 bIdx;

                for (bIdx = 0; bIdx < binSize / (128 * SIZE_1K); bIdx++)
                {
                    nvme_fw_download(fd, &buffer[currSize], 128 * SIZE_1K, currSize);
                    currSize += (128 * SIZE_1K);
                }

                if (currSize != binSize)
                {
                    nvme_fw_download(fd, &buffer[currSize], binSize - currSize, currSize);
                }
            }

            slot = (slot - 1) % nr_slots + 1;
            printf ("%4d) fw commit action:%d slot:%d ", idx + 1, action, slot);
            gettimeofday(&work_tm, NULL);
            nvme_fw_commit(fd, action, slot);
            nvme_reset(rst_fd);

            printf ("=> %.2f seconds\n", get_timeval_sec_usec(work_tm));
            if (loop > 1) sleep(1);
            slot++;
        }

        if (fp != NULL) fclose(fp);

        close(fd);
        close(rst_fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int reset_controller(char* device, int argc, char* argv[])
{
    int fd;
    int idx;
    int loop;

    fd = open(device, O_RDWR);

    if (fd != -1)
    {
        if (argc)   loop = atoi(argv[0]);
        else        loop = 1;

        for (idx = 0; idx < loop; idx++)
        {
            printf("%3d) controller reset:%s\n", idx + 1, device);
            disk_reset(fd);
            usleep(500000);
        }

        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int log_page(char* device, int argc, char* argv[])
{
    int fd;
    int i;
    int logId = hex2dec(argv[0]);
    char buff[4096];

    memset(buff, 0xFF, sizeof(buff));

    fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        nvme_get_log(fd, logId, &buff[0], sizeof(buff));

        switch (logId)
        {
            case 0x02:
            {
                LogPageSmart_t* pSmartLog = (LogPageSmart_t*)&buff[0];

                printf("TempSensor1:%d\n", KELVIN_TO_CELSIUS(pSmartLog->TempSensor1));
                printf("TempSensor2:%d\n", KELVIN_TO_CELSIUS(pSmartLog->TempSensor2));

                break;
            }
            default:
                dump_buffer((char*)&buff[0], sizeof(buff));
                break;
        }

        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

void toLowerCase(char* src, char* dest, int len)
{
    int i;

    for (i = 0; i < len ; i++)
    {
        if (src[i] >= 'A' && src[i] <= 'Z') dest[i] = 'a' + (src[i] - 'A');
        else                                dest[i] = src[i];
    }

    dest[i] = '\0';
}

U64 hex2dec(char* buf)
{
    U32 len;
    S32 i;
    U64 base = 1;
    U64 num = 0;

    len = strlen(buf);

    // to lower case
    for (i = 0; i < len; i++)
    {
        if (buf[i] >= 'A' && buf[i] <= 'Z')
        {
            buf[i] = 'a' + buf[i] - 'A';
        }
    }

    for (i = len - 1; i >= 0; i--)
    {
        if (buf[i] >= '0' && buf[i] <= '9')
        {
            num += (buf[i] - '0') * base;
        }
        else
        {
            num += (buf[i] - 'a' + 10) * base;
        }

        base *= 16;
    }

    return num;
}

void dump_buffer(char* buf, U32 len)
{
    U32 i, j;

    for (i = 0; i < len / 16; i++)
    {
        for (j = 0; j < 16; j++)
        {
            printf(" %02X", buf[i * 16 + j] & 0xFF);
        }

        printf (" | ");

        for (j = 0; j < 16; j++)
        {
            if (buf[i * 16 + j] > 0x1F && buf[i * 16 + j] < 0x7F)
            {
                printf("%c", buf[i * 16 + j] & 0xFF);
            }
            else
            {
                printf(".");
            }
        }

        printf("\n");
    }

    printf("===========================================================\n");
}

void show_usage(int argc, char* argv[])
{
    const CmdTbl_t* pCmd;

    printf("Usage: %s [device_path] (example: %s /dev/sdb)\n", argv[0], argv[0]);
    printf("Usage: %s [device_path] [options]\n", argv[0]);

    pCmd = &cmdList[0];

    printf("-------------------------------------------------------------------------------\n");
    printf("  %-10s%-20s%-15s\n", "[Option]", "[Description]", "[Parameters]");

    while (pCmd->pFunc != NULL)
    {
        printf("  %-10s%-20s%-45s\n", pCmd->cmdStr, pCmd->helpStr, pCmd->fmtStr);
        pCmd++;
    }

    printf("-------------------------------------------------------------------------------\n");
}

/*===================================================
| USB Power Control Functions Implementation
===================================================*/

/*===================================================
| USB Power Control Functions
===================================================*/
int write_to_sysfs_file(const char* path, const char* value)
{
    int fd;
    ssize_t bytes_written;
    size_t value_len;

    fd = open(path, O_WRONLY);
    if (fd == -1)
    {
        return -1;
    }

    value_len = strlen(value);
    bytes_written = write(fd, value, value_len);
    close(fd);

    return (bytes_written == value_len) ? 0 : -1;
}

void log_usb_power_status(const char* phase)
{
    char cmd[256];
    sprintf(cmd, "sudo sh -c 'echo \"=== USB Power Status %s ===\" >> %s'", phase, USB_POWER_LOG_FILE);
    system(cmd);

    sprintf(cmd, "sudo sh -c 'date >> %s'", USB_POWER_LOG_FILE);
    system(cmd);

    sprintf(cmd, "sudo sh -c 'lsusb >> %s 2>&1'", USB_POWER_LOG_FILE);
    system(cmd);
}

int configure_usb_power_for_sleep(void)
{
    char path[256];
    char cmd[512];
    FILE* fp;
    int success_count;
    int total_count;

    success_count = 0;
    total_count = 0;

    // dbg_printf(COLOR_CYAN, "Configuring USB power management for sleep...\n");

    // Log power status before sleep
    log_usb_power_status("before_sleep");

    // 1. Disable USB wake functionality
    fp = popen("find /sys/bus/usb/devices -name 'wakeup' -path '*/power/wakeup' 2>/dev/null", "r");
    if (fp != NULL)
    {
        while (fgets(path, sizeof(path), fp) != NULL)
        {
            // Remove newline character
            path[strcspn(path, "\n")] = '\0';
            total_count++;

            // Use write_to_sysfs_file instead of system() for better error handling
            if (write_to_sysfs_file(path, "disabled") == 0)
            {
                success_count++;
                dbg_printf(COLOR_GREEN, "Disabled wake: %s\n", path);
            }
            else
            {
                dbg_printf(COLOR_YELLOW, "Failed to disable wake: %s\n", path);
            }
        }
        pclose(fp);
    }

    // 2. Set USB auto suspend mode
    fp = popen("find /sys/bus/usb/devices -name 'control' -path '*/power/control' 2>/dev/null", "r");
    if (fp != NULL)
    {
        while (fgets(path, sizeof(path), fp) != NULL)
        {
            path[strcspn(path, "\n")] = '\0';
            total_count++;

            if (write_to_sysfs_file(path, "auto") == 0)
            {
                success_count++;
                dbg_printf(COLOR_GREEN, "Set auto suspend: %s\n", path);
            }
            else
            {
                dbg_printf(COLOR_YELLOW, "Failed to set auto suspend: %s\n", path);
            }
        }
        pclose(fp);
    }

    // 3. Set short auto suspend timeout (1 second)
    fp = popen("find /sys/bus/usb/devices -name 'autosuspend' -path '*/power/autosuspend' 2>/dev/null", "r");
    if (fp != NULL)
    {
        while (fgets(path, sizeof(path), fp) != NULL)
        {
            path[strcspn(path, "\n")] = '\0';
            total_count++;

            if (write_to_sysfs_file(path, "1") == 0)
            {
                success_count++;
                dbg_printf(COLOR_GREEN, "Set suspend timeout: %s\n", path);
            }
            else
            {
                dbg_printf(COLOR_YELLOW, "Failed to set suspend timeout: %s\n", path);
            }
        }
        pclose(fp);
    }

    // 4. Configure USB controller power management
    fp = popen("find /sys/bus/pci/devices -name 'control' -path '*/power/control' 2>/dev/null", "r");
    if (fp != NULL)
    {
        while (fgets(path, sizeof(path), fp) != NULL)
        {
            path[strcspn(path, "\n")] = '\0';

            // Check if this is a USB controller (Class 0x0c03xx)
            char class_path[520];  // Increase buffer size to accommodate additional "/class" string
            char device_dir[512];
            FILE* class_fp;
            char class_value[16];

            // Extract device directory from control path
            strncpy(device_dir, path, sizeof(device_dir) - 1);
            device_dir[sizeof(device_dir) - 1] = '\0';

            char* last_slash = strrchr(device_dir, '/');
            if (last_slash != NULL)
            {
                *last_slash = '\0';

                // Use safer string combination method
                int ret = snprintf(class_path, sizeof(class_path), "%s/class", device_dir);
                if (ret >= sizeof(class_path))
                {
                    // Path too long, skip this item
                    continue;
                }

                class_fp = fopen(class_path, "r");
                if (class_fp != NULL)
                {
                    if (fgets(class_value, sizeof(class_value), class_fp) != NULL)
                    {
                        // Check if it's a USB controller (class 0x0c03xx)
                        if (strncmp(class_value, "0x0c03", 6) == 0)
                        {
                            total_count++;
                            if (write_to_sysfs_file(path, "auto") == 0)
                            {
                                success_count++;
                                dbg_printf(COLOR_GREEN, "Set USB controller auto: %s\n", path);
                            }
                            else
                            {
                                dbg_printf(COLOR_YELLOW, "Failed to set USB controller auto: %s\n", path);
                            }
                        }
                    }
                    fclose(class_fp);
                }
            }
        }
        pclose(fp);
    }

    // 5. Set USB core module parameters (this doesn't count towards success/total)
    system("sudo sh -c 'echo \"options usbcore autosuspend=1\" > /etc/modprobe.d/usb-power-save.conf 2>/dev/null'");

    // dbg_printf(COLOR_GREEN, "USB power management configured successfully (%d/%d)\n", success_count, total_count);

    return 0;
}

int restore_usb_power_after_wake(void)
{
    // dbg_printf(COLOR_CYAN, "Restoring USB power settings after wake...\n");

    // Log power status after wake
    log_usb_power_status("after_wake");

    // Rescan USB devices
    system("echo '1' > /sys/bus/usb/drivers_probe 2>/dev/null");
    system("for hub in /sys/bus/usb/devices/usb*/authorized; do "
           "echo '1' > \"$hub\" 2>/dev/null; done");

    // Wait for device re-initialization
    usleep(2000000); // 2 seconds    dbg_printf(COLOR_GREEN, "USB power restoration completed\n");

    // Check USB storage device status
    system("sudo sh -c 'lsblk | grep -i usb > /tmp/usb_devices_after_wake.log 2>&1'");

    return 0;
}
