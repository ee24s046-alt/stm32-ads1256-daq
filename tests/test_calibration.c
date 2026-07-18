/**
 * @file test_calibration.c
 * @brief Correction maths, saturation, and record integrity.
 */
#include "test_harness.h"
#include "calibration.h"

static void test_identity_is_transparent(void)
{
    daq_cal_t cal;

    TEST("identity calibration does not change any code");
    daq_cal_set_identity(&cal);

    CHECK_EQ(daq_cal_apply(&cal, 0U, 0), 0);
    CHECK_EQ(daq_cal_apply(&cal, 1U, 123456), 123456);
    CHECK_EQ(daq_cal_apply(&cal, 2U, -123456), -123456);
    CHECK_EQ(daq_cal_apply(&cal, 3U, DAQ_CODE_FULL_SCALE), DAQ_CODE_FULL_SCALE);
}

static void test_offset_and_gain(void)
{
    daq_cal_t cal;

    TEST("offset subtracts, gain scales in Q16.16");
    daq_cal_set_identity(&cal);
    cal.offset[0]   = 500;
    cal.gain_q16[0] = DAQ_CAL_UNITY_Q16 * 2;      /* exactly 2.0x */

    CHECK_EQ(daq_cal_apply(&cal, 0U, 500), 0);
    CHECK_EQ(daq_cal_apply(&cal, 0U, 1500), 2000);
    CHECK_EQ(daq_cal_apply(&cal, 0U, -500), -2000);
}

static void test_saturation_not_wraparound(void)
{
    daq_cal_t cal;

    TEST("out-of-range results saturate; they must never wrap sign");
    daq_cal_set_identity(&cal);
    cal.gain_q16[0] = DAQ_CAL_UNITY_Q16 * 8;

    CHECK_EQ(daq_cal_apply(&cal, 0U, 8000000), DAQ_CODE_FULL_SCALE);
    CHECK_EQ(daq_cal_apply(&cal, 0U, -8000000), DAQ_CODE_MIN);
    /* The bug this guards against: a 32-bit intermediate would make a large
       positive input come out negative. */
    CHECK(daq_cal_apply(&cal, 0U, 8388607) > 0);
}

static void test_two_point_fit(void)
{
    daq_cal_t cal;

    TEST("two-point fit recovers a known gain error");
    daq_cal_set_identity(&cal);

    /* A channel reading 1 % low: 1.000 V reference lands at 4152000 codes
       where it should be 4194304. */
    CHECK(daq_cal_from_two_point(&cal, 0U, 120, 4152120, 4194304));
    CHECK_EQ(cal.offset[0], 120);
    CHECK(cal.gain_q16[0] > DAQ_CAL_UNITY_Q16);

    {
        const int32_t corrected = daq_cal_apply(&cal, 0U, 4152120);
        const int32_t err = (corrected > 4194304) ? (corrected - 4194304)
                                                  : (4194304 - corrected);
        /*
         * The bound is set by the gain word, not by sloppiness.  Q16.16
         * resolves gain to 1/65536 = 15.3 ppm, and half a step of that on a
         * 4.19e6-code reading is +/-32 codes.  Asserting anything tighter
         * would be asserting a property the representation does not have.
         * 32 codes is 3.8 ppm of full scale - roughly 30x below the ADS1256's
         * own +/-0.1 % gain-error spec, so the fixed-point choice is nowhere
         * near the dominant error term.
         */
        CHECK(err <= 32);
    }
}

static void test_two_point_rejects_nonsense(void)
{
    daq_cal_t cal;

    TEST("two-point fit refuses degenerate and implausible inputs");
    daq_cal_set_identity(&cal);

    CHECK(!daq_cal_from_two_point(&cal, 0U, 100, 100, 4194304));   /* zero span */
    CHECK(!daq_cal_from_two_point(&cal, 0U, 0, 100, 4194304));     /* gain 41943x */
    CHECK(!daq_cal_from_two_point(&cal, 9U, 0, 1000, 1000));       /* bad channel */
    /* And the record must be untouched by a rejected fit. */
    CHECK_EQ(cal.gain_q16[0], DAQ_CAL_UNITY_Q16);
}

static void test_record_integrity(void)
{
    daq_cal_t cal;

    TEST("CRC detects tampering anywhere in the record");
    daq_cal_set_identity(&cal);
    cal.offset[2] = -4242;
    daq_cal_finalize(&cal, 7U, 1234U);

    CHECK(daq_cal_valid(&cal));
    CHECK_EQ(cal.serial, 7U);

    cal.offset[2] = -4243;
    CHECK(!daq_cal_valid(&cal));

    cal.offset[2] = -4242;
    CHECK(daq_cal_valid(&cal));

    cal.magic = 0xFFFFFFFFU;            /* what an erased flash slot reads as */
    CHECK(!daq_cal_valid(&cal));
}

int main(void)
{
    test_identity_is_transparent();
    test_offset_and_gain();
    test_saturation_not_wraparound();
    test_two_point_fit();
    test_two_point_rejects_nonsense();
    test_record_integrity();
    TEST_SUMMARY();
}
