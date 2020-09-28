#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

typedef uint32_t domid_t;
typedef uint32_t grant_ref_t;
// https://lore.kernel.org/patchwork/patch/817972/
#include <xen/gntdev.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE ((1ul << PAGE_SHIFT))

/*
 * We need to obtain grant reference id from the guest.
 * Then we can use gntdev to mmap the shared page of guest memory.
 */

#define print_error(format, ...) \
    fprintf(stderr, format, ##__VA_ARGS__)

#define print_errno() \
    print_error("Error[%d]: %s:%d: %s\n", errno, __FUNCTION__, __LINE__,  strerror(errno))

uint32_t open_dev_gntdev()
{
    int fd = open("/dev/xen/gntdev", O_RDWR);
    if(fd < 0){
        print_errno();
        if(errno == ENOENT){
            print_error("Please load the xen-gntdev driver\n");
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

int32_t number_page_from_grant(int32_t nb_grant)
{
    return nb_grant;
}

grant_ref_t* get_grefid(int* nb_grant)
{
    //TODO: get grefid and number from Xenstore ?
    *nb_grant = 1;
    return NULL;
}

int main(int argc, char *argv[]){
    printf("Hello, Xen!\n");

    int err, ret = EXIT_SUCCESS, i, nb_grant = 1;

    domid_t domid = 1; //TODO: get domid, from args ?
//    grant_ref_t* refid = get_grefid(&nb_grant);
    grant_ref_t refid[1];
    refid[0] = 2423;

//    char* rec_buf = malloc(len);
//    if(!rec_buf){
//        print_errno();
//        ret = EXIT_FAILURE;
//        goto exit_2;
//    }


    int gntdev_fd = open_dev_gntdev();
    if(gntdev_fd < 0){
        ret = EXIT_FAILURE;
        goto exit_all;
    }


    /* Map the grant references */

    uint32_t count = number_page_from_grant(nb_grant);
    struct ioctl_gntdev_map_grant_ref* gref = malloc(
            sizeof(struct ioctl_gntdev_map_grant_ref) +
            count * sizeof(struct ioctl_gntdev_grant_ref));

    gref->count = count;
    printf("count: %d\n", gref->count);
    for(i = 0; i < count; ++i){
        struct ioctl_gntdev_grant_ref* ref = &gref->refs[i];
        ref->domid = domid;
        ref->ref = refid[i];
        printf("gref[%d]: domid = %d, ref = %d\n", i, ref->domid, ref->ref);
    }

    err = ioctl(gntdev_fd, IOCTL_GNTDEV_MAP_GRANT_REF, gref);
    if(err < 0){
        print_errno();
        ret = EXIT_FAILURE;
        goto exit_close;
    }

    /* Map the grant pages */

    char *shbuf = mmap(NULL, count*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, gntdev_fd, gref->index);
    if(shbuf == MAP_FAILED){
        printf("Error (%s:%d): %s\n", __FILE__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Do the thing with the shared pages */

    printf("%s\n", shbuf);

    /* Unmap the grant pages */

    if((err = munmap(shbuf, count*PAGE_SIZE))){
        print_errno();
        exit(EXIT_FAILURE);
    }

    /* Unmap grant references */
    struct ioctl_gntdev_unmap_grant_ref ugref;
    ugref.index = gref->index;
    ugref.count = gref->count;
    err = ioctl(gntdev_fd, IOCTL_GNTDEV_UNMAP_GRANT_REF, &ugref);
    if(err < 0){
        print_errno();
        ret = EXIT_FAILURE;
        goto exit_close;
    }

exit_close:
    close(gntdev_fd);
exit_all:
    exit(ret);
}
