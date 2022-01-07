/*
 * Copyright (c) 2021-2022, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "mcuboot_integration_tests.h"

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "bootutil/image.h"
#include "region_defs.h"
#include "flash_map/flash_map.h"
#include "flash_map_backend/flash_map_backend.h"
#include "bootutil/bootutil.h"
#include "bootutil/boot_status.h"
#include "bootutil_priv.h"
#include "test_framework_helpers.h"

#define MCUBOOT_INTEG_TEST_BLOCK_SIZE 256

static int read_image_header(int idx, struct image_header *hdr)
{
    int rc;
    const struct flash_area *fap;

    rc = flash_area_open(flash_area_id_from_image_slot(idx), &fap);
    if (rc) {
        return rc;
    }

    rc = flash_area_read(fap, 0, hdr, sizeof(struct image_header));

    flash_area_close(fap);

    return rc;
}

static int write_image_header(int idx, struct image_header *hdr)
{
    int rc;
    const struct flash_area *fap;

    rc = flash_area_open(flash_area_id_from_image_slot(idx), &fap);
    if (rc) {
        return rc;
    }

    rc = flash_area_write(fap, 0, hdr, sizeof(struct image_header));
    if (rc) {
        return rc;
    }

    flash_area_close(fap);

    return 0;
}

static int copy_image_to_slot(int source_idx, int dest_idx)
{
    int rc;
    const struct flash_area *source_fap;
    const struct flash_area *dest_fap;
    size_t idx;
    size_t copy_size;
    uint8_t block[MCUBOOT_INTEG_TEST_BLOCK_SIZE];

    rc = flash_area_open(flash_area_id_from_image_slot(source_idx), &source_fap);
    if (rc) {
        return rc;
    }

    rc = flash_area_open(flash_area_id_from_image_slot(dest_idx), &dest_fap);
    if (rc) {
        flash_area_close(source_fap);
        return rc;
    }

    rc = flash_area_erase(dest_fap, 0, dest_fap->fa_size);
    if (rc) {
        goto out;
    }

    copy_size = source_fap->fa_size > dest_fap->fa_size ?
                                        dest_fap->fa_size :
                                        source_fap->fa_size;

    /* Note that this copy leaves the header erased, so it can be set by the
     * test without doing any more flash erasing.
     */
    for (idx = sizeof(struct image_header); idx < (copy_size - sizeof(block));
         idx += sizeof(block)) {
        rc = flash_area_read(source_fap, idx, block, sizeof(block));
        if (rc) {
            goto out;
        }

        rc = flash_area_write(dest_fap, idx, block, sizeof(block));
        if (rc) {
            goto out;
        }
    }

    rc = flash_area_read(source_fap, idx, block, copy_size - idx);
    if (rc) {
        goto out;
    }

    rc = flash_area_write(dest_fap, idx, block, copy_size - idx);
    if (rc) {
        goto out;
    }

out:
    flash_area_close(source_fap);
    flash_area_close(dest_fap);

    return rc;
}

static int test_setup(int *original_image_idx)
{
    int rc;
    struct image_header hdr;

    rc = read_image_header(0, &hdr);
    if (rc) {
        return rc;
    }

    /* If there's a valid image in slot 0 copy that to slot 1 */
    if (hdr.ih_magic == IMAGE_MAGIC) {
        *original_image_idx = 0;
        return copy_image_to_slot(0, 1);
    } else {
        /* Else copy slot 1 to slot 0. We assume that one of the slots has a
         * valid image.
         */
        *original_image_idx = 1;
        return copy_image_to_slot(1, 0);
    }
}

static void reset_shared_area()
{
    struct shared_data_tlv_header *header =
        (struct shared_data_tlv_header*)MCUBOOT_SHARED_DATA_BASE;

    memset((uint8_t*)MCUBOOT_SHARED_DATA_BASE + SHARED_DATA_HEADER_SIZE, 0,
           MCUBOOT_SHARED_DATA_SIZE - SHARED_DATA_HEADER_SIZE);

    header->tlv_tot_len = SHARED_DATA_HEADER_SIZE;
}

static int test_teardown(int original_image_idx)
{
    int rc;
    int test_image_idx;
    const struct flash_area *fap;

    /* Erase the test image */
    test_image_idx = original_image_idx == 0 ? 1 : 0;
    rc = flash_area_open(flash_area_id_from_image_slot(test_image_idx), &fap);
    if (rc) {
        return rc;
    }

    rc = flash_area_erase(fap, 0, fap->fa_size);
    flash_area_close(fap);

    reset_shared_area();

    return rc;
}

static void tfm_mcuboot_integration_test_0001(struct test_result_t *ret)
{
    int rc;
    int original_image_idx = 0;
    struct image_header hdr;
    struct boot_rsp rsp;

    rc = test_setup(&original_image_idx);
    if (rc) {
        TEST_FAIL("Failed to setup test");
        goto out;
    }

    rc = read_image_header(0, &hdr);
    if (rc) {
        TEST_FAIL("Failed to read image header");
        goto out;
    }

    /* Increasing the version both causes the image to boot preferentially and
     * also invalidates the signature
     */
    hdr.ih_ver.iv_major += 1;

    rc = write_image_header(1, &hdr);
    if (rc) {
        TEST_FAIL("Failed to write image header");
        goto out;
    }

    rc = fih_int_decode(boot_go(&rsp));
    if (memcmp(rsp.br_hdr, &hdr, sizeof(struct image_header)) == 0) {
        TEST_FAIL("Invalid image boot succeeded");
        goto out;
    }

    ret->val = TEST_PASSED;

out:
    rc = test_teardown(original_image_idx);
    if (rc) {
        TEST_FAIL("Failed to teardown test");
    }
}

static struct test_t integration_tests[] = {
    {&tfm_mcuboot_integration_test_0001, "TFM_MCUBOOT_INTEGRATION_TEST_0001",
     "Integration invalid image signature test", {TEST_PASSED} },
};

void register_testsuite_mcuboot_integration(struct test_suite_t *p_test_suite)
{
    uint32_t list_size = (sizeof(integration_tests) / sizeof(integration_tests[0]));

    set_testsuite("MCUboot Integration test (TFM_MCUBOOT_INTEGRATION_TEST_0XXX)",
                  integration_tests, list_size, p_test_suite);
}
