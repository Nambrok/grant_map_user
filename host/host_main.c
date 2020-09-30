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
#include <signal.h>

typedef uint32_t domid_t;
typedef uint32_t grant_ref_t;
// https://lore.kernel.org/patchwork/patch/817972/
#include <xen/gntdev.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE ((1ul << PAGE_SHIFT))

#define PAGE_PER_GRANT 1

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

uint32_t number_of_grant_from_pages(uint32_t nb_pages)
{
    return nb_pages/PAGE_PER_GRANT;
}

volatile int cont;
void sigint_handler(int signum){
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

int parse_args(int argc, char ** argv, domid_t* _domid, grant_ref_t** _refid)
{
    int i;
    /*
     * name of command
     * domid
     * grant_ref
     * ...
     */

    if(argc < 3){
        *_domid = 0;
        *_refid = NULL;
        return 0;
    }

    domid_t domid = strtoul(argv[1], NULL, 10);
    uint32_t nb_grant = argc-2;
    grant_ref_t* refid = malloc(sizeof(grant_ref_t) * nb_grant);
    for (i = 0; i < nb_grant; ++i){
        refid[i] = strtoul(argv[i+2], NULL, 10);
    }

    *_domid = domid;
    *_refid = refid;
    return nb_grant;
}

#define NOTIFY_BYTE 4095

int set_unmap_notify(int gntdev_fd, struct ioctl_gntdev_map_grant_ref* gref)
{
    int err;

    /* Set the unmap notify to be able to finish */
    struct ioctl_gntdev_unmap_notify snot;
    snot.index = gref->index + NOTIFY_BYTE;
    snot.action = UNMAP_NOTIFY_CLEAR_BYTE;
    snot.event_channel_port = 0;

    err = ioctl(gntdev_fd, IOCTL_GNTDEV_SET_UNMAP_NOTIFY, &snot);
    if(err < 0){
        return err;
    }
    return 0;
}

/* Map the grant references */
struct ioctl_gntdev_map_grant_ref* map_grant_ref(int gntdev_fd, domid_t domid,
        int nb_grant, grant_ref_t* refid)
{
    int err, i;

    struct ioctl_gntdev_map_grant_ref* gref = malloc(
            sizeof(struct ioctl_gntdev_map_grant_ref) +
            (nb_grant-1) * sizeof(struct ioctl_gntdev_grant_ref));

    gref->count = nb_grant;
    for(i = 0; i < gref->count; ++i){
        struct ioctl_gntdev_grant_ref* ref = &gref->refs[i];
        ref->domid = domid;
        ref->ref = refid[i];
    }

    err = ioctl(gntdev_fd, IOCTL_GNTDEV_MAP_GRANT_REF, gref);
    if(err < 0){
        free(gref);
        return NULL;
    }
    return gref;
}

/* Unmap grant references */
int unmap_grant_ref(int gntdev_fd, struct ioctl_gntdev_map_grant_ref* gref)
{
    int err;
    struct ioctl_gntdev_unmap_grant_ref ugref;

    ugref.index = gref->index;
    ugref.count = gref->count;

    err = ioctl(gntdev_fd, IOCTL_GNTDEV_UNMAP_GRANT_REF, &ugref);
    if(err < 0){
        return err;
    }
    return 0;
}

int main(int argc, char *argv[]){
    printf("Hello, Xen!\n");

    INIT_WAIT_FOR_USER(cont);

    int err, ret = EXIT_SUCCESS, i, nb_pages = 1, nb_grant;
    domid_t domid;
    grant_ref_t* refid;

    /* We open the gntdev interface */
    int gntdev_fd = open_dev_gntdev();
    if(gntdev_fd < 0){
        ret = EXIT_FAILURE;
        goto exit_all;
    }

    /* Parse the inputs */
    nb_grant = parse_args(argc, argv, &domid, &refid);
    if(nb_grant == 0){
        print_error("The arguments given were incorrect.\n");
        print_error("%s <domid> <grantref> [<grantref> ...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    //We have one page per grant (page = 4k)
    nb_pages = nb_grant;


    /* Open access to the grant reference */
    struct ioctl_gntdev_map_grant_ref* gref =
        map_grant_ref(gntdev_fd, domid, nb_grant, refid);
    if(gref == NULL){
        print_errno();
        ret = EXIT_FAILURE;
        goto exit_free;
    }

    /* We print the opened gref information */
    printf("count: %d\n", gref->count);
    for(i = 0; i < gref->count; ++i){
        struct ioctl_gntdev_grant_ref* ref = &gref->refs[i];
        printf("gref[%d]: domid = %d, ref = %d\n", i, ref->domid, ref->ref);
    }

    /* We set so that when unmap the other side will know */
    set_unmap_notify(gntdev_fd, gref);
    if(err < 0){
        print_errno();
        print_error("Error while setting unmap notification\n");
    }

    /* Map the grant pages */
    char *shbuf = mmap(NULL, nb_pages*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, gntdev_fd, gref->index);
    if(shbuf == MAP_FAILED){
        print_errno();
        ret = EXIT_FAILURE;
        goto exit_free;
    }

    /* Do the thing with the shared pages */
    printf("%s\n", shbuf);

    /* Unmap the grant pages */
    if((err = munmap(shbuf, nb_pages*PAGE_SIZE))){
        print_errno();
        ret = EXIT_FAILURE;
        goto exit_free;
    }

    /* We close access to the grants */
    if(unmap_grant_ref(gntdev_fd, gref)){
        print_errno();
        ret = EXIT_FAILURE;
    }

    free(gref);
exit_free:
    free(refid);
exit_close:
    close(gntdev_fd);
exit_all:
    exit(ret);
}
