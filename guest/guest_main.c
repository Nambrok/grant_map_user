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
#include <signal.h>

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


volatile int cont;
void sigint_handler(int signum)
{
    cont = 0;
}
#define WAIT_FOR_USER(var) \
    do{ \
        while(var); \
        var = 1; \
    }while(0)

#define INIT_WAIT_FOR_USER(var) \
    do{ \
        signal(SIGINT, sigint_handler); \
        var = 1; \
    }while(0)

#define WAIT_FOR(var) \
    do { \
        while(var) sleep(1); \
    } while(0)

struct ioctl_gntalloc_alloc_gref* alloc_grefs(int gntalloc_fd, uint16_t domid, uint32_t count)
{
    int err;
    struct ioctl_gntalloc_alloc_gref* gref = malloc(sizeof(struct ioctl_gntalloc_alloc_gref));
    gref->domid = domid; //dom0, but might be something else in the future
    gref->count = count;
    gref->flags = GNTALLOC_FLAG_WRITABLE;

    err = ioctl(gntalloc_fd, IOCTL_GNTALLOC_ALLOC_GREF, gref);
    if(err < 0){
        free(gref);
        return NULL;
    }
    return gref;
}

int dealloc_grefs(int gntalloc_fd, struct ioctl_gntalloc_alloc_gref* gref)
{
    struct ioctl_gntalloc_dealloc_gref dgref;
    dgref.index = gref->index;
    dgref.count = gref->count;

    return ioctl(gntalloc_fd, IOCTL_GNTALLOC_DEALLOC_GREF, &dgref);
}

char* get_hostname()
{
    char* hostname = malloc(sizeof(char) * 255);
    int fd = open("/etc/hostname", O_RDONLY);
    if(fd < 0){
        print_error("File '/etc/hostname' not found\n");
        free(hostname);
        return NULL;
    }
        
    read(fd, hostname, 255);
    //delete the last return line
    *(strrchr(hostname, '\n')) = '\0';
    close(fd);
    return hostname;
}

int main(int argc, char *argv[])
{
    int err, i, ret = EXIT_SUCCESS;
    printf("Hello, Xen!\n");

    INIT_WAIT_FOR_USER(cont);

    static const uint32_t len = PAGE_SIZE*2;

    /* Open access to the gntalloc interface */
    int gntalloc_fd = open_dev_gntalloc();
    if(gntalloc_fd < 0){
        exit(EXIT_FAILURE);
    }

    /* Alloc the grant reference */
    struct ioctl_gntalloc_alloc_gref* gref = alloc_grefs(gntalloc_fd, 0, number_page_from_bytes(len));
    if(gref == NULL){
        print_errno();
        ret = EXIT_FAILURE;
        goto exit_close;
    }

    /* We print the obtained grant references */
    for(i = 0; i < gref->count; ++i){
        printf("gref[%d]: ref = %d\n", i, gref->gref_ids[i]);
    }

    /* Map the grant pages */
    char* shpages = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, gntalloc_fd, gref->index);
    if(shpages == MAP_FAILED){
        print_error("The mmap-ing failed\n");
        print_errno();
        ret = EXIT_FAILURE;
        goto exit_dealloc;
    }

#define NOTIFY_BYTE 4095
    shpages[NOTIFY_BYTE] = 1;

    /* We put our payload in the shared memory */
    char* hostname = get_hostname();
    if(hostname == NULL){
        exit(EXIT_FAILURE);
    }
    char* ret1 = memcpy(shpages, hostname, strlen(hostname));
    if(ret1 != shpages){
        print_error("Return value from memcpy is incorrect");
        exit(EXIT_FAILURE);
    }
    free(hostname);

    printf("%s\n", shpages);

//    WAIT_FOR(shpages[NOTIFY_BYTE]);

    WAIT_FOR_USER(cont);
    printf("After interrupt. Cleaning...\n");

    /* Unmap the grant pages */
    err = munmap(shpages, len);
    if(err < 0){
        print_errno();
    }


exit_dealloc:
    /* Dealloc the grant reference */
    err = dealloc_grefs(gntalloc_fd, gref);
    if(err < 0){
        print_errno();
        ret = EXIT_FAILURE;
    }

exit_close:
    free(gref);
    close(gntalloc_fd);
    exit(ret);
}
