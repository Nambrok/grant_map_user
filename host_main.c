#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

#include <xen/grant_table.h>

int main(int argc, char *argv[]){
    printf("Hello, Xen!\n");

    static const uint32_t len = 4096;
    char *buf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, 0, 0);
    if(buf == MAP_FAILED){
        printf("Error (%s:%d): %s\n", __FILE__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }

    memset(buf, 0, len);
    exit(EXIT_SUCCESS);
}
