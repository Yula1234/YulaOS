#include "yula.h"

int main() {
    char buf[128];
    int n;
    
    printf("Reader started. Waiting for input...\n");

    while ((n = read(0, buf, 127)) > 0) {
        buf[n] = 0;
        printf("[READER]: %s", buf);
    }

    printf("\nReader finished.\n");
    return 0;
}