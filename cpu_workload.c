#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include<unistd.h>
int main(int argc, char *argv[])
{
    int seconds =200;

    if (argc > 1) {
        seconds = atoi(argv[1]);
    }

    if (seconds < 1 || seconds > 300) {
        fprintf(stderr, "Duration must be between 1 and 300 seconds.\n");
        return 1;
    }

    printf("CPU workload started. PID: %d\n", getpid());
    printf("Running for %d seconds...\n", seconds);
    fflush(stdout);

    time_t start = time(NULL);
    volatile double result = 0.0;
    unsigned long long iterations = 0;

    while (time(NULL) - start < seconds) {
        for (int i = 1; i <= 10000; i++) {
            result += sqrt((double)i) * 0.000001;
        }
        iterations += 10000;
    }

    printf("CPU workload completed.\n");
    printf("Iterations: %llu\n", iterations);
    printf("Result checksum: %.6f\n", result);

    return 0;
}