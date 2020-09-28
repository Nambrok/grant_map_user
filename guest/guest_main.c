#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <xen/gntalloc.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE ((1ul << PAGE_SHIFT))

/*
 * We need first to map the buffer to be shared (w/ len/PAGE_SIZE == 0).
 * Then use the IOCTL interface of gntalloc to grant the page to the other VM.
 *
 * We need to alloc a page of kernel memory, create an entry in the grant table
 * granting access to the page to a domain (by referencing it's domid).
 * Gives the "grant reference identifier" to the other domain.
 * Then we can mmap a part of the of the memory of the gntalloc device.
 *
 * To begin, ioctl(create) on gntalloc to grant the page then mmap
 * To finish munmap, then ioctl (or close)
 *
 */

#define print_error(format, ...) \
    fprintf(stderr, format, ##__VA_ARGS__)

#define print_errno() \
    print_error("Error[%d]: %s:%d: %s\n", errno, __FUNCTION__, __LINE__,  strerror(errno))

int open_dev_gntalloc(void)
{
    // Might need to modprobe `xen-gntalloc` before being able to open
    int fd = open("/dev/xen/gntalloc", O_RDWR);
    if(fd < 0){
        print_errno();
        if(errno == ENOENT){
            print_error("Please load the xen-gntalloc driver\n");
        }
        return -1;
    }

    return fd;
}

int32_t number_page_from_bytes(int32_t len)
{
    int nb = len/PAGE_SIZE;
    if((len%PAGE_SIZE) > 0){
        nb += 1;
    }
    return nb;
}

int main(int argc, char *argv[])
{
    int err, i;
    printf("Hello, Xen!\n");

    static const uint32_t len = PAGE_SIZE*1;

    /* Alloc the grant reference */

    int gntalloc_fd = open_dev_gntalloc();
    if(gntalloc_fd < 0){
        exit(EXIT_FAILURE);
    }

    struct ioctl_gntalloc_alloc_gref gref;
    gref.domid = 0; //dom0, but might be something else in the future
    gref.count = number_page_from_bytes(len);

    err = ioctl(gntalloc_fd, IOCTL_GNTALLOC_ALLOC_GREF, &gref);
    if(err < 0){
        print_errno();
        exit(EXIT_FAILURE);
    }

    for(i = 0; i < gref.count; ++i){
        printf("%d: grefid; %d\n", i, gref.gref_ids[i]);
    }

    /* Map the grant pages */

    char* shpages = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, gntalloc_fd, gref.index);
    if(shpages == MAP_FAILED){
        print_error("The mmap-ing failed\n");
        print_errno();
    }

    char* hostname = "TEST1DAMS";
    char* ret = memcpy(shpages, hostname, strlen(hostname));
    if(ret != shpages){
        print_error("Return value from memcpy is incorrect");
        exit(EXIT_FAILURE);
    }

    printf("%s\n", shpages);

    /* Unmap the grant pages */

    err = munmap(shpages, len);
    if(err < 0){
        print_errno();
    }

    /* Dealloc the grant reference */

    struct ioctl_gntalloc_dealloc_gref dgref;
    dgref.index = gref.index;
    dgref.count = gref.count;

    err = ioctl(gntalloc_fd, IOCTL_GNTALLOC_DEALLOC_GREF, &dgref);
    if(err < 0){
        print_errno();
        exit(EXIT_FAILURE);
    }

    close(gntalloc_fd);
    exit(EXIT_SUCCESS);
}
