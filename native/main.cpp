#include <cstdio>
#include <unistd.h>

int main() {
    std::printf("Hello from android!\n");
    std::printf("PID: %d\n", getpid());
    return 0;
}