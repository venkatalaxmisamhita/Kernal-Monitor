#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>

#define CPU_LIMIT 80.0
#define MEMORY_LIMIT 80.0

volatile sig_atomic_t running = 1;

pthread_mutex_t monitor_lock;

/* ---------------- SIGNAL HANDLER ---------------- */

void signal_handler(int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
    {
        running = 0;
    }
}

/* ---------------- FILE SYSTEM CALLS ---------------- */
/*
   Handout Session 2 and Session 10:
   open(), read(), write(), close()
*/

int read_file(const char *filename, char *buffer, int size)
{
    int fd;
    int bytes;

    fd = open(filename, O_RDONLY);

    if (fd == -1)
    {
        return -1;
    }

    bytes = read(fd, buffer, size - 1);

    close(fd);

    if (bytes <= 0)
    {
        return -1;
    }

    buffer[bytes] = '\0';

    return bytes;
}

/* ---------------- CPU MONITOR ---------------- */
/*
   CPU information is obtained from /proc/stat
*/

double get_cpu_usage()
{
    char buffer[1024];

    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
    unsigned long long steal;

    static unsigned long long old_total = 0;
    static unsigned long long old_idle = 0;

    if (read_file("/proc/stat", buffer, sizeof(buffer)) == -1)
    {
        return 0;
    }

    sscanf(buffer,
           "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &user,
           &nice,
           &system,
           &idle,
           &iowait,
           &irq,
           &softirq,
           &steal);

    unsigned long long idle_time = idle + iowait;

    unsigned long long total =
        user + nice + system + idle +
        iowait + irq + softirq + steal;

    if (old_total == 0)
    {
        old_total = total;
        old_idle = idle_time;

        return 0;
    }

    unsigned long long total_difference =
        total - old_total;

    unsigned long long idle_difference =
        idle_time - old_idle;

    old_total = total;
    old_idle = idle_time;

    if (total_difference == 0)
    {
        return 0;
    }

    return ((double)(total_difference - idle_difference)
            / total_difference) * 100.0;
}

/* ---------------- MEMORY MONITOR ---------------- */
/*
   Handout CO4:
   Memory management and Linux process/system information
*/

double get_memory_usage()
{
    char buffer[4096];

    long total_memory = 0;
    long available_memory = 0;

    if (read_file("/proc/meminfo",
                  buffer,
                  sizeof(buffer)) == -1)
    {
        return 0;
    }

    char *line = strtok(buffer, "\n");

    while (line != NULL)
    {
        if (sscanf(line,
                   "MemTotal: %ld kB",
                   &total_memory) == 1)
        {
        }

        if (sscanf(line,
                   "MemAvailable: %ld kB",
                   &available_memory) == 1)
        {
        }

        line = strtok(NULL, "\n");
    }

    if (total_memory == 0)
    {
        return 0;
    }

    return ((double)(total_memory - available_memory)
            / total_memory) * 100.0;
}

/* ---------------- PROCESS MONITOR ---------------- */
/*
   Handout Session 3:
   Process creation, PID, PPID and process states.
*/

void display_processes()
{
    DIR *directory;

    directory = opendir("/proc");

    if (directory == NULL)
    {
        perror("opendir");
        return;
    }

    printf("\n================ PROCESS MONITOR ================\n");

    printf("%-8s %-25s %-8s\n",
           "PID",
           "PROCESS",
           "STATE");

    printf("-----------------------------------------------\n");

    struct dirent *entry;

    int count = 0;

    while ((entry = readdir(directory)) != NULL)
    {
        /*
           Process directories inside /proc
           have numeric names.
        */

        if (entry->d_name[0] < '0' ||
            entry->d_name[0] > '9')
        {
            continue;
        }

        if (count >= 15)
        {
            break;
        }

        char filename[512];

        char data[1024];

        snprintf(filename,
                 sizeof(filename),
                 "/proc/%s/stat",
                 entry->d_name);

        if (read_file(filename,
                       data,
                       sizeof(data)) == -1)
        {
            continue;
        }

        int pid;

        char process_name[256];

        char state;

        if (sscanf(data,
                   "%d (%255[^)]) %c",
                   &pid,
                   process_name,
                   &state) == 3)
        {
            printf("%-8d %-25s %-8c\n",
                   pid,
                   process_name,
                   state);

            count++;
        }
    }

    closedir(directory);
}

/* ---------------- SYSTEM INFORMATION ---------------- */

void display_system_information()
{
    char hostname[100];

    if (gethostname(hostname,
                    sizeof(hostname)) == 0)
    {
        printf("System Hostname : %s\n",
               hostname);
    }

    printf("Monitor PID     : %d\n",
           getpid());

    printf("Parent PID      : %d\n",
           getppid());
}

/* ---------------- CPU TEMPERATURE ---------------- */
/*
   /sys is used according to the project abstract.
*/

void display_temperature()
{
    char buffer[100];

    if (read_file(
            "/sys/class/thermal/thermal_zone0/temp",
            buffer,
            sizeof(buffer)) == -1)
    {
        printf("CPU Temperature : Not Available\n");
        return;
    }

    long temperature = atol(buffer);

    printf("CPU Temperature : %.2f C\n",
           temperature / 1000.0);
}

/* ---------------- ALERT SYSTEM ---------------- */

void check_alerts(double cpu,
                  double memory)
{
    printf("\n================ ALERTS ================\n");

    int alert = 0;

    if (cpu >= CPU_LIMIT)
    {
        printf("WARNING: CPU usage is HIGH!\n");
        alert = 1;
    }

    if (memory >= MEMORY_LIMIT)
    {
        printf("WARNING: Memory usage is HIGH!\n");
        alert = 1;
    }

    if (!alert)
    {
        printf("System resources are within limits.\n");
    }
}

/* ---------------- FORK DEMONSTRATION ---------------- */
/*
   Handout CO2:
   fork(), getpid(), getppid(), waitpid()
*/

void process_creation_demo()
{
    printf("\n========== PROCESS CREATION ==========\n");

    pid_t pid = fork();

    if (pid < 0)
    {
        perror("fork");
        return;
    }

    if (pid == 0)
    {
        printf("Child Process\n");

        printf("Child PID  : %d\n",
               getpid());

        printf("Parent PID : %d\n",
               getppid());

        exit(0);
    }
    else
    {
        printf("Parent Process\n");

        printf("Parent PID : %d\n",
               getpid());

        waitpid(pid,
                NULL,
                0);

        printf("Child process terminated.\n");
    }
}

/* ---------------- MONITOR THREAD ---------------- */
/*
   Handout CO6:
   pthread_create(), pthread_join()
   and mutex synchronization.
*/

void *monitor(void *arg)
{
    while (running)
    {
        double cpu;
        double memory;

        cpu = get_cpu_usage();

        memory = get_memory_usage();

        pthread_mutex_lock(&monitor_lock);

        /*
           Clear terminal
        */

        printf("\033[2J");
        printf("\033[H");

        printf("====================================================\n");
        printf("              LINUX KERNEL MONITOR\n");
        printf("====================================================\n\n");

        display_system_information();

        printf("\n---------------- SYSTEM RESOURCES ----------------\n");

        printf("CPU Usage       : %.2f %%\n",
               cpu);

        printf("Memory Usage    : %.2f %%\n",
               memory);

        display_temperature();

        display_processes();

        check_alerts(cpu, memory);

        printf("\n====================================================\n");
        printf("Refreshing every 2 seconds...\n");
        printf("Press Ctrl+C to stop.\n");

        pthread_mutex_unlock(&monitor_lock);

        sleep(2);
    }

    return NULL;
}

/* ---------------- MAIN ---------------- */

int main()
{
    pthread_t monitor_thread;

    /*
       Register signals
       CO3 - Signal handling
    */

    signal(SIGINT,
           signal_handler);

    signal(SIGTERM,
           signal_handler);

    /*
       Initialize mutex
    */

    pthread_mutex_init(&monitor_lock,
                       NULL);

    printf("Starting Linux Kernel Monitor...\n");

    /*
       Demonstrate process creation
    */

    process_creation_demo();

    /*
       Create monitoring thread
    */

    if (pthread_create(&monitor_thread,
                       NULL,
                       monitor,
                       NULL) != 0)
    {
        perror("pthread_create");

        pthread_mutex_destroy(&monitor_lock);

        return 1;
    }

    /*
       Wait for monitoring thread
    */

    pthread_join(monitor_thread,
                 NULL);

    /*
       Destroy mutex
    */

    pthread_mutex_destroy(&monitor_lock);

    printf("\nKernel Monitor stopped successfully.\n");

    return 0;
}