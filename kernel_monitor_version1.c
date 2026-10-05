#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define MAX_CPUS 64
#define DISK_NAME "sdd"

/* =========================
   THRESHOLDS
   ========================= */

#define CPU_WARNING 70.0
#define CPU_CRITICAL 85.0

#define MEMORY_WARNING 75.0
#define MEMORY_CRITICAL 90.0

#define IO_WARNING 10.0
#define IO_CRITICAL 50.0

#define PERSISTENCE_LIMIT 3


/* =========================
   STRUCTURES
   ========================= */

typedef struct
{
    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
    unsigned long long steal;
} CPUStats;


typedef struct
{
    unsigned long long total;
    unsigned long long free;
    unsigned long long available;
} MemoryStats;


typedef struct
{
    unsigned long long read_operations;
    unsigned long long read_sectors;
    unsigned long long write_operations;
    unsigned long long write_sectors;
} DiskStats;


/* =========================
   PROGRAM CONTROL
   ========================= */

volatile sig_atomic_t running = 1;

void stop_monitor(int signal_number)
{
    running = 0;
}


/* =========================================================
   CPU
   ========================================================= */

int read_cpu(CPUStats *cpu)
{
    FILE *file;
    char line[256];

    file = fopen("/proc/stat", "r");

    if (file == NULL)
        return 0;

    while (fgets(line, sizeof(line), file))
    {
        if (strncmp(line, "cpu ", 4) == 0)
        {
            if (sscanf(line,
                       "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &cpu->user,
                       &cpu->nice,
                       &cpu->system,
                       &cpu->idle,
                       &cpu->iowait,
                       &cpu->irq,
                       &cpu->softirq,
                       &cpu->steal) == 8)
            {
                fclose(file);
                return 1;
            }
        }
    }

    fclose(file);
    return 0;
}


int read_all_cpus(CPUStats cpus[])
{
    FILE *file;
    char line[256];

    file = fopen("/proc/stat", "r");

    if (file == NULL)
        return 0;

    while (fgets(line, sizeof(line), file))
    {
        char cpu_name[20];
        int cpu_number;
        CPUStats temp;

        if (sscanf(line,
                   "%19s %llu %llu %llu %llu %llu %llu %llu %llu",
                   cpu_name,
                   &temp.user,
                   &temp.nice,
                   &temp.system,
                   &temp.idle,
                   &temp.iowait,
                   &temp.irq,
                   &temp.softirq,
                   &temp.steal) != 9)
            continue;

        if (strncmp(cpu_name, "cpu", 3) != 0)
            continue;

        if (sscanf(cpu_name, "cpu%d", &cpu_number) != 1)
            continue;

        if (cpu_number < 0 || cpu_number >= MAX_CPUS)
            continue;

        cpus[cpu_number] = temp;
    }

    fclose(file);
    return 1;
}


double calculate_cpu_usage(CPUStats *previous,
                           CPUStats *current)
{
    unsigned long long previous_idle;
    unsigned long long current_idle;

    unsigned long long previous_total;
    unsigned long long current_total;

    unsigned long long total_difference;
    unsigned long long idle_difference;

    previous_idle =
        previous->idle + previous->iowait;

    current_idle =
        current->idle + current->iowait;

    previous_total =
        previous->user +
        previous->nice +
        previous->system +
        previous->idle +
        previous->iowait +
        previous->irq +
        previous->softirq +
        previous->steal;

    current_total =
        current->user +
        current->nice +
        current->system +
        current->idle +
        current->iowait +
        current->irq +
        current->softirq +
        current->steal;

    total_difference =
        current_total - previous_total;

    idle_difference =
        current_idle - previous_idle;

    if (total_difference == 0)
        return 0.0;

    return ((double)(total_difference -
                     idle_difference) /
            total_difference) * 100.0;
}


/* =========================================================
   MEMORY
   ========================================================= */

int read_memory(MemoryStats *memory)
{
    FILE *file;
    char name[50];
    unsigned long long value;

    file = fopen("/proc/meminfo", "r");

    if (file == NULL)
        return 0;

    memory->total = 0;
    memory->free = 0;
    memory->available = 0;

    while (fscanf(file,
                  "%49s %llu kB",
                  name,
                  &value) == 2)
    {
        if (strcmp(name, "MemTotal:") == 0)
            memory->total = value;

        else if (strcmp(name, "MemFree:") == 0)
            memory->free = value;

        else if (strcmp(name, "MemAvailable:") == 0)
            memory->available = value;
    }

    fclose(file);

    return memory->total != 0;
}


double calculate_memory_usage(MemoryStats *memory)
{
    unsigned long long used;

    used = memory->total -
           memory->available;

    return ((double)used /
            memory->total) * 100.0;
}


/* =========================================================
   DISK I/O
   ========================================================= */

int read_disk(DiskStats *disk)
{
    FILE *file;
    char line[512];

    file = fopen("/proc/diskstats", "r");

    if (file == NULL)
        return 0;

    disk->read_operations = 0;
    disk->read_sectors = 0;
    disk->write_operations = 0;
    disk->write_sectors = 0;

    while (fgets(line, sizeof(line), file))
    {
        int major;
        int minor;
        char device[50];

        unsigned long long reads_completed;
        unsigned long long reads_merged;
        unsigned long long sectors_read;
        unsigned long long read_time;

        unsigned long long writes_completed;
        unsigned long long writes_merged;
        unsigned long long sectors_written;
        unsigned long long write_time;

        int result;

        result = sscanf(
            line,
            "%d %d %49s "
            "%llu %llu %llu %llu "
            "%llu %llu %llu %llu",
            &major,
            &minor,
            device,
            &reads_completed,
            &reads_merged,
            &sectors_read,
            &read_time,
            &writes_completed,
            &writes_merged,
            &sectors_written,
            &write_time
        );

        if (result < 11)
            continue;

        if (strcmp(device, DISK_NAME) == 0)
        {
            disk->read_operations =
                reads_completed;

            disk->read_sectors =
                sectors_read;

            disk->write_operations =
                writes_completed;

            disk->write_sectors =
                sectors_written;

            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 0;
}


/* =========================================================
   TREND
   ========================================================= */

void display_trend(double current,
                   double previous)
{
    double difference =
        current - previous;

    if (difference > 2.0)
        printf("Increasing ^\n");

    else if (difference < -2.0)
        printf("Decreasing v\n");

    else
        printf("Stable ->\n");
}


/* =========================================================
   STATUS
   ========================================================= */

const char *cpu_status(double usage)
{
    if (usage >= CPU_CRITICAL)
        return "CRITICAL";

    if (usage >= CPU_WARNING)
        return "WARNING";

    return "NORMAL";
}


const char *memory_status(double usage)
{
    if (usage >= MEMORY_CRITICAL)
        return "CRITICAL";

    if (usage >= MEMORY_WARNING)
        return "WARNING";

    return "NORMAL";
}


const char *io_status(double io)
{
    if (io >= IO_CRITICAL)
        return "CRITICAL";

    if (io >= IO_WARNING)
        return "WARNING";

    return "NORMAL";
}


/* =========================================================
   CPU LOAD ANALYSIS
   ========================================================= */

const char *cpu_pattern(int active_cores,
                        int cpu_count,
                        double busiest_core)
{
    if (active_cores <= cpu_count / 4 &&
        busiest_core > 80.0)
        return "Concentrated workload";

    if (active_cores > cpu_count / 2)
        return "Distributed workload";

    return "Moderate distribution";
}


/* =========================================================
   SYSTEM ANALYSIS
   ========================================================= */

void display_analysis(
    double cpu_usage,
    double busiest_core_usage,
    int active_cores,
    int cpu_count,
    double memory_usage,
    double total_io)
{
    const char *cpu_pattern_result;

    cpu_pattern_result =
        cpu_pattern(active_cores,
                    cpu_count,
                    busiest_core_usage);

    printf("\nSYSTEM ANALYSIS\n");
    printf("--------------------------------------------\n");

    printf("CPU Pattern      : %s\n",
           cpu_pattern_result);

    printf("Memory Condition : %s\n",
           memory_status(memory_usage));

    printf("I/O Condition    : %s\n",
           io_status(total_io));


    if (memory_usage >= MEMORY_CRITICAL ||
        total_io >= IO_CRITICAL ||
        cpu_usage >= CPU_CRITICAL)
    {
        printf("Overall Health   : CRITICAL\n");
    }
    else if (memory_usage >= MEMORY_WARNING ||
             total_io >= IO_WARNING ||
             cpu_usage >= CPU_WARNING ||
             busiest_core_usage >= CPU_CRITICAL)
    {
        printf("Overall Health   : ATTENTION\n");
    }
    else
    {
        printf("Overall Health   : HEALTHY\n");
    }


    printf("\nINSIGHT\n");
    printf("--------------------------------------------\n");

    if (active_cores <= cpu_count / 4 &&
        busiest_core_usage > 80.0 &&
        cpu_usage < 20.0)
    {
        printf("CPU load is concentrated on a small number of cores\n");
        printf("while overall CPU utilization remains low.\n");
    }
    else if (cpu_usage >= CPU_CRITICAL)
    {
        printf("Overall CPU utilization is critically high.\n");
    }
    else if (memory_usage >= MEMORY_CRITICAL)
    {
        printf("Memory utilization is critically high.\n");
    }
    else if (memory_usage >= MEMORY_WARNING)
    {
        printf("Memory utilization is elevated.\n");
    }
    else if (total_io >= IO_CRITICAL)
    {
        printf("Disk activity is critically high.\n");
    }
    else if (total_io >= IO_WARNING)
    {
        printf("Disk activity is elevated.\n");
    }
    else
    {
        printf("CPU, memory and I/O activity are within normal ranges.\n");
    }
}


/* =========================================================
   ALERT ENGINE
   ========================================================= */

void display_alerts(
    double cpu_usage,
    double memory_usage,
    double total_io,
    int *cpu_warning_count,
    int *cpu_critical_count,
    int *memory_warning_count,
    int *memory_critical_count,
    int *io_warning_count,
    int *io_critical_count)
{
    printf("\nALERTS\n");
    printf("--------------------------------------------\n");


    /* CPU */

    if (cpu_usage >= CPU_CRITICAL)
    {
        (*cpu_critical_count)++;
        *cpu_warning_count = 0;
    }
    else if (cpu_usage >= CPU_WARNING)
    {
        (*cpu_warning_count)++;
        *cpu_critical_count = 0;
    }
    else
    {
        *cpu_warning_count = 0;
        *cpu_critical_count = 0;
    }


    /* MEMORY */

    if (memory_usage >= MEMORY_CRITICAL)
    {
        (*memory_critical_count)++;
        *memory_warning_count = 0;
    }
    else if (memory_usage >= MEMORY_WARNING)
    {
        (*memory_warning_count)++;
        *memory_critical_count = 0;
    }
    else
    {
        *memory_warning_count = 0;
        *memory_critical_count = 0;
    }


    /* I/O */

    if (total_io >= IO_CRITICAL)
    {
        (*io_critical_count)++;
        *io_warning_count = 0;
    }
    else if (total_io >= IO_WARNING)
    {
        (*io_warning_count)++;
        *io_critical_count = 0;
    }
    else
    {
        *io_warning_count = 0;
        *io_critical_count = 0;
    }


    /* DISPLAY */

    if (*cpu_critical_count >= PERSISTENCE_LIMIT)
        printf("CPU             : CRITICAL ALERT\n");
    else if (*cpu_warning_count >= PERSISTENCE_LIMIT)
        printf("CPU             : WARNING ALERT\n");
    else
        printf("CPU             : NORMAL\n");


    if (*memory_critical_count >= PERSISTENCE_LIMIT)
        printf("MEMORY          : CRITICAL ALERT\n");
    else if (*memory_warning_count >= PERSISTENCE_LIMIT)
        printf("MEMORY          : WARNING ALERT\n");
    else
        printf("MEMORY          : NORMAL\n");


    if (*io_critical_count >= PERSISTENCE_LIMIT)
        printf("I/O             : CRITICAL ALERT\n");
    else if (*io_warning_count >= PERSISTENCE_LIMIT)
        printf("I/O             : WARNING ALERT\n");
    else
        printf("I/O             : NORMAL\n");


    printf("\nAlert rule: abnormal condition must persist for %d samples.\n",
           PERSISTENCE_LIMIT);
}


/* =========================================================
   MAIN
   ========================================================= */

int main()
{
    CPUStats previous_cpu;
    CPUStats current_cpu;

    CPUStats previous_cores[MAX_CPUS];
    CPUStats current_cores[MAX_CPUS];

    MemoryStats memory;

    DiskStats previous_disk;
    DiskStats current_disk;

    int cpu_count;
    int sample_count = 0;

    double previous_cpu_usage = 0.0;
    double previous_memory_usage = 0.0;

    double average_cpu = 0.0;
    double peak_cpu = 0.0;
    double minimum_cpu = 100.0;

    double average_memory = 0.0;
    double peak_memory = 0.0;
    double minimum_memory = 100.0;

    int cpu_warning_count = 0;
    int cpu_critical_count = 0;

    int memory_warning_count = 0;
    int memory_critical_count = 0;

    int io_warning_count = 0;
    int io_critical_count = 0;

    signal(SIGINT, stop_monitor);


    /* =========================
       CPU COUNT
       ========================= */

    cpu_count =
        (int)sysconf(_SC_NPROCESSORS_ONLN);

    if (cpu_count <= 0 ||
        cpu_count > MAX_CPUS)
        cpu_count = 1;


    /* =========================
       INITIAL SNAPSHOT
       ========================= */

    if (!read_cpu(&previous_cpu))
    {
        printf("Could not read CPU statistics.\n");
        return 1;
    }

    if (!read_all_cpus(previous_cores))
    {
        printf("Could not read per-core statistics.\n");
        return 1;
    }

    if (!read_memory(&memory))
    {
        printf("Could not read memory statistics.\n");
        return 1;
    }

    if (!read_disk(&previous_disk))
    {
        printf("Could not find disk: %s\n",
               DISK_NAME);
        return 1;
    }

    previous_memory_usage =
        calculate_memory_usage(&memory);


    /* =========================
       MONITOR LOOP
       ========================= */

    while (running)
    {
        double cpu_usage;
        double memory_usage;

        double read_rate;
        double write_rate;
        double total_io;

        double read_difference;
        double write_difference;

        int active_cores;
        int busiest_core;

        double busiest_core_usage;

        int i;


        sleep(1);


        if (!read_cpu(&current_cpu))
            break;

        if (!read_all_cpus(current_cores))
            break;

        if (!read_memory(&memory))
            break;

        if (!read_disk(&current_disk))
            break;


        /* =========================
           CPU
           ========================= */

        cpu_usage =
            calculate_cpu_usage(
                &previous_cpu,
                &current_cpu
            );

        sample_count++;


        if (sample_count == 1)
        {
            average_cpu = cpu_usage;
            peak_cpu = cpu_usage;
            minimum_cpu = cpu_usage;
        }
        else
        {
            average_cpu =
                ((average_cpu *
                  (sample_count - 1))
                 + cpu_usage)
                / sample_count;

            if (cpu_usage > peak_cpu)
                peak_cpu = cpu_usage;

            if (cpu_usage < minimum_cpu)
                minimum_cpu = cpu_usage;
        }


        /* =========================
           MEMORY
           ========================= */

        memory_usage =
            calculate_memory_usage(&memory);

        if (sample_count == 1)
        {
            average_memory = memory_usage;
            peak_memory = memory_usage;
            minimum_memory = memory_usage;
        }
        else
        {
            average_memory =
                ((average_memory *
                  (sample_count - 1))
                 + memory_usage)
                / sample_count;

            if (memory_usage > peak_memory)
                peak_memory = memory_usage;

            if (memory_usage < minimum_memory)
                minimum_memory = memory_usage;
        }


        /* =========================
           PER CORE
           ========================= */

        active_cores = 0;
        busiest_core = -1;
        busiest_core_usage = 0.0;

        for (i = 0; i < cpu_count; i++)
        {
            double core_usage =
                calculate_cpu_usage(
                    &previous_cores[i],
                    &current_cores[i]
                );

            if (core_usage > 20.0)
                active_cores++;

            if (core_usage >
                busiest_core_usage)
            {
                busiest_core_usage =
                    core_usage;

                busiest_core = i;
            }
        }


        /* =========================
           DISK I/O
           ========================= */

        if (current_disk.read_sectors >=
            previous_disk.read_sectors)
        {
            read_difference =
                (double)(
                    current_disk.read_sectors -
                    previous_disk.read_sectors
                );
        }
        else
        {
            read_difference = 0.0;
        }


        if (current_disk.write_sectors >=
            previous_disk.write_sectors)
        {
            write_difference =
                (double)(
                    current_disk.write_sectors -
                    previous_disk.write_sectors
                );
        }
        else
        {
            write_difference = 0.0;
        }


        read_rate =
            (read_difference * 512.0) /
            (1024.0 * 1024.0);

        write_rate =
            (write_difference * 512.0) /
            (1024.0 * 1024.0);

        total_io =
            read_rate + write_rate;


        /* =========================
           CLEAR SCREEN
           ========================= */

        printf("\033[2J");
        printf("\033[H");


        /* =========================
           HEADER
           ========================= */

        printf("============================================\n");
        printf("             KERNEL MONITOR\n");
        printf("============================================\n");

        printf("Samples         : %d\n",
               sample_count);

        printf("Monitoring      : CPU | MEMORY | I/O\n");

        printf("Press Ctrl+C to stop.\n");


        /* =========================
           CPU
           ========================= */

        printf("\nCPU\n");
        printf("--------------------------------------------\n");

        printf("CPU Usage       : %.2f%%\n",
               cpu_usage);

        printf("Average CPU     : %.2f%%\n",
               average_cpu);

        printf("Peak CPU        : %.2f%%\n",
               peak_cpu);

        printf("Minimum CPU     : %.2f%%\n",
               minimum_cpu);

        printf("CPU Trend       : ");
        display_trend(
            cpu_usage,
            previous_cpu_usage
        );

        printf("CPU Status      : %s\n",
               cpu_status(cpu_usage));


        /* =========================
           PER CORE
           ========================= */

        printf("\nPER-CORE ANALYSIS\n");
        printf("--------------------------------------------\n");

        printf("Active Cores    : %d / %d\n",
               active_cores,
               cpu_count);

        if (busiest_core >= 0)
        {
            printf("Busiest Core    : CPU %d (%.2f%%)\n",
                   busiest_core,
                   busiest_core_usage);
        }

        printf("Load Distribution: %s\n",
               cpu_pattern(
                   active_cores,
                   cpu_count,
                   busiest_core_usage));

        printf("Highly Busy Cores: %d\n",
               active_cores);


        /* =========================
           MEMORY
           ========================= */

        printf("\nMEMORY\n");
        printf("--------------------------------------------\n");

        printf("Total Memory    : %.2f GB\n",
               memory.total /
               (1024.0 * 1024.0));

        printf("Used Memory     : %.2f GB\n",
               (memory.total -
                memory.available) /
               (1024.0 * 1024.0));

        printf("Available       : %.2f GB\n",
               memory.available /
               (1024.0 * 1024.0));

        printf("Free Memory     : %.2f GB\n",
               memory.free /
               (1024.0 * 1024.0));

        printf("Memory Usage    : %.2f%%\n",
               memory_usage);


        printf("\nMEMORY ANALYTICS\n");
        printf("--------------------------------------------\n");

        printf("Current Usage   : %.2f%%\n",
               memory_usage);

        printf("Average Usage   : %.2f%%\n",
               average_memory);

        printf("Peak Usage      : %.2f%%\n",
               peak_memory);

        printf("Minimum Usage   : %.2f%%\n",
               minimum_memory);

        printf("Trend           : ");
        display_trend(
            memory_usage,
            previous_memory_usage
        );

        printf("Status          : %s\n",
               memory_status(memory_usage));


        /* =========================
           FILE I/O
           ========================= */

        printf("\nFILE I/O\n");
        printf("--------------------------------------------\n");

        printf("Device          : %s\n",
               DISK_NAME);

        printf("Read Operations : %llu\n",
               current_disk.read_operations);

        printf("Write Operations: %llu\n",
               current_disk.write_operations);

        printf("Read Rate       : %.2f MB/s\n",
               read_rate);

        printf("Write Rate      : %.2f MB/s\n",
               write_rate);

        printf("Total I/O Rate  : %.2f MB/s\n",
               total_io);

        printf("I/O Activity    : %s\n",
               io_status(total_io));


        /* =========================
           ANALYSIS
           ========================= */

        display_analysis(
            cpu_usage,
            busiest_core_usage,
            active_cores,
            cpu_count,
            memory_usage,
            total_io
        );


        /* =========================
           ALERTS
           ========================= */

        display_alerts(
            cpu_usage,
            memory_usage,
            total_io,
            &cpu_warning_count,
            &cpu_critical_count,
            &memory_warning_count,
            &memory_critical_count,
            &io_warning_count,
            &io_critical_count
        );


        printf("\n");
        printf("============================================\n");


        /* =========================
           UPDATE SNAPSHOTS
           ========================= */

        previous_cpu =
            current_cpu;

        previous_memory_usage =
            memory_usage;

        previous_disk =
            current_disk;

        previous_cpu_usage =
            cpu_usage;

        for (i = 0; i < cpu_count; i++)
        {
            previous_cores[i] =
                current_cores[i];
        }
    }


    printf("\n\n");
    printf("Kernel Monitor stopped.\n");

    return 0;
}
i have this code