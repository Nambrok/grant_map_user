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

// https://lore.kernel.org/patchwork/patch/817972/
#include <xen/grant_table.h>
#include <xen/gntdev.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE ((1ul << PAGE_SHIFT))

#define PAGE_PER_GRANT 1

typedef enum{
    FALSE = 0,
    TRUE = 1
} bool_t;

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

void print_gnttab_copy(struct ioctl_gntdev_grant_copy* gcopy)
{
    int i;

    printf("%u segments\n", gcopy->count);

    for(i = 0; i < gcopy->count; ++i){
        struct gntdev_grant_copy_segment* cur = &( gcopy->segments[i] );
        printf("Seg %d:\n", i);
        printf("\tlen: %u\n", cur->len);
        printf("\tflags: %x\n", cur->flags);

        if(cur->flags & GNTCOPY_source_gref) {
            printf("\tsource: domid: %u, gref: %u, off: %u\n",
                    cur->source.foreign.domid, cur->source.foreign.ref,
                    cur->source.foreign.offset);
        } else {
            printf("\tsource: virt: %p\n", cur->source.virt);
        }

        if(cur->flags & GNTCOPY_dest_gref){
            printf("\tdest: domid: %u, gref: %u, off: %u\n",
                cur->dest.foreign.domid, cur->dest.foreign.ref,
                cur->dest.foreign.offset);
        } else {
            printf("\tdest: virt: %p\n", cur->dest.virt);
        }
    }
}


int grant_copy(int gntdev_fd, void* buf, domid_t domid,
        grant_ref_t* refid, int nb_grant)
{

    int i, err;
    bool_t verbose = TRUE;
    struct ioctl_gntdev_grant_copy gcopy;
    struct gntdev_grant_copy_segment* seg =
        calloc(nb_grant, sizeof(struct gntdev_grant_copy_segment));

    for(i = 0; i < nb_grant; ++i){
        struct gntdev_grant_copy_segment* cur = &(seg[i]);

        cur->flags = GNTCOPY_source_gref;
        cur->len = PAGE_SIZE;

        cur->source.foreign.domid = domid;
        cur->source.foreign.ref = refid[i];
        cur->source.foreign.offset = 0;

        cur->dest.virt = buf + (i << PAGE_SHIFT);
    }

    gcopy.count = nb_grant;
    gcopy.segments = seg;

    if(verbose)
        print_gnttab_copy(&gcopy);

    err = ioctl(gntdev_fd, IOCTL_GNTDEV_GRANT_COPY, &gcopy);
    if(err < 0){
        print_errno();
        return -1;
    }

    for(i = 0; i < nb_grant; ++i){
        struct gntdev_grant_copy_segment* cur = &(seg[i]);
        if(cur->status != GNTST_okay){
            print_error("Error grant copy (gref %d)\n", cur->source.foreign.ref);
            //Maybe needing to retry ?
            return -1;
        }
    }
    free(seg);
    return 0;
}

struct grant_map_ret
{
    struct ioctl_gntdev_map_grant_ref* gref;
    void* buf;
    int ret;
};

struct grant_map_ret
grant_map(int gntdev_fd, domid_t domid, grant_ref_t* refid, int nb_grant)
{

    int i, nb_pages = number_page_from_grant(nb_grant), err;
    bool_t verbose = TRUE;
    struct grant_map_ret ret = {NULL, NULL, 0};

    /* Open access to the grant reference */
    struct ioctl_gntdev_map_grant_ref* gref =
        map_grant_ref(gntdev_fd, domid, nb_grant, refid);
    if(gref == NULL){
        print_errno();
        goto exit_gmap;
    }
    ret.gref = gref;

    if(verbose){
        /* We print the opened gref information */
        printf("count: %d\n", gref->count);
        for(i = 0; i < gref->count; ++i){
            struct ioctl_gntdev_grant_ref* ref = &gref->refs[i];
            printf("gref[%d]: domid = %d, ref = %d\n", i, ref->domid, ref->ref);
        }
    }

    /* We set so that when unmap the other side will know */
    set_unmap_notify(gntdev_fd, gref);
    if(err < 0){
        print_errno();
        print_error("Error while setting unmap notification\n");
    }

    /* Map the grant pages */
    void* shbuf = mmap(NULL, nb_pages*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, gntdev_fd, gref->index);
    if(shbuf == MAP_FAILED){
        print_errno();
        goto exit_mmap;
    }
    ret.buf = shbuf;

    return ret;

exit_mmap:
    free(gref);
    ret.gref = NULL;
exit_gmap:
    ret.ret = EXIT_FAILURE;
    return ret;
}

int grant_map_free(int gntdev_fd, int nb_grant, struct grant_map_ret gmap_ret)
{
    int ret = EXIT_SUCCESS, err, nb_pages = number_page_from_grant(nb_grant);
    /* Unmap the grant pages */
    if((err = munmap(gmap_ret.buf, nb_pages*PAGE_SIZE))){
        print_errno();
        print_error("Unmap of the grants has failed\n");
    }

    /* We close access to the grants */
    if(unmap_grant_ref(gntdev_fd, gmap_ret.gref)){
        print_errno();
        ret = EXIT_FAILURE;
    }

    free(gmap_ret.gref);
    return ret;
}

int main(int argc, char *argv[])
{
    printf("Hello, Xen!\n");

    bool_t use_copy = FALSE;
    int err, ret = EXIT_SUCCESS, i, nb_pages = 1, nb_grant;
    domid_t domid;
    grant_ref_t* refid;

    /* We open the gntdev interface */
    int gntdev_fd = open_dev_gntdev();
    if(gntdev_fd < 0){
        ret = EXIT_FAILURE;
        goto exit_1;
    }

    /* Parse the inputs */
    nb_grant = parse_args(argc, argv, &domid, &refid);
    if(nb_grant == 0){
        print_error("The arguments given were incorrect.\n");
        print_error("%s <domid> <grantref> [<grantref> ...]\n", argv[0]);
        goto exit_2;
    }
    //We have one page per grant (page = 4k)
    nb_pages = nb_grant;

    if(!use_copy){
    //Solution where we mmap the other buffer

        struct grant_map_ret map_ret = grant_map(gntdev_fd, domid, refid, nb_grant);
        if(map_ret.ret < 0){
            print_error("Error while mapping grant references\n");
            goto exit_3;
        }
        char* shbuf = map_ret.buf;

        /* Do the thing with the shared pages */
        printf("%s\n", shbuf);

        grant_map_free(gntdev_fd, nb_grant, map_ret);

    } else {
    //Solution where we grant_copy the buffer

        uint32_t len = nb_pages*PAGE_SIZE;
        char* buf = malloc(len);
        if(buf == NULL){
            print_errno();
            ret = EXIT_FAILURE;
            goto exit_3;
        }

        err = grant_copy(gntdev_fd, (void*) buf, domid, refid, nb_grant);
        if(err < 0){
            print_error("Error with grant copy\n");
            goto exit_copy;
        }

        printf("%s\n", buf);

exit_copy:
        free(buf);
    }

exit_3:
    free(refid);
exit_2:
    close(gntdev_fd);
exit_1:
    exit(ret);
}
