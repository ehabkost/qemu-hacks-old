/*
 * Virtio Support
 *
 * Copyright IBM, Corp. 2007-2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Rusty Russell     <rusty@rustcorp.com.au>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef _QEMU_VIRTIO_BALLOON_H
#define _QEMU_VIRTIO_BALLOON_H

/* from Linux's linux/virtio_balloon.h */

/* The ID for virtio_balloon */
#define VIRTIO_ID_BALLOON	5

/* The feature bitmap for virtio balloon */
#define VIRTIO_BALLOON_F_MUST_TELL_HOST	0 /* Tell before reclaiming pages */

struct virtio_balloon_config
{
    /* Number of pages host wants Guest to give up. */
    uint32_t num_pages;
    /* Number of pages we've actually got in balloon. */
    uint32_t actual;
};

#endif
