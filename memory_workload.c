#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[])
{
    long memory_mb = 100;
    long duration = 300;

    if (argc > 1) {
        char *end;
        errno = 0;
        memory_mb = strtol(argv[1], &end, 10);

        if (errno || *end != '\0' ||
            memory_mb < 1 || memory_mb > 4096) {
            fprintf(stderr,
                    "Usage: %s [memory_MB] [duration_seconds]\n",
                    argv[0]);
            return 1;
        }
    }

    if (argc > 2) {
        char *end;
        errno = 0;
        duration = strtol(argv[2], &end, 10);

        if (errno || *end != '\0' ||
            duration < 1 || duration > 86400) {
            fprintf(stderr, "Duration must be 1-86400 seconds.\n");
            return 1;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    size_t bytes = (size_t)memory_mb * 1024 * 1024;

    printf("\n=== MEMORY WORKLOAD ===\n");
    printf("PID: %d\n", getpid());
    printf("Requested memory: %ld MB\n", memory_mb);
    printf("Duration: %ld seconds\n", duration);
    fflush(stdout);

    unsigned char *memory = malloc(bytes);

    if (memory == NULL) {
        perror("malloc failed");
        return 1;
    }

    /* Touch each memory page to make the allocation resident. */
    long page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0)
        page_size = 4096;

    for (size_t i = 0; i < bytes; i += (size_t)page_size)
        memory[i] = (unsigned char)(i % 251 + 1);

    memory[bytes - 1] = 1;

    printf("Memory initialized successfully.\n");
    printf("Keeping memory allocated. Monitor PID %d.\n", getpid());
    fflush(stdout);

    time_t start = time(NULL);

    while (running && difftime(time(NULL), start) < duration)
        sleep(1);

    free(memory);

    printf("Memory released. Workload finished.\n");

    return 0;
}