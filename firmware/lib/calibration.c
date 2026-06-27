#include "calibration.h"
#include "crc.h"
#include <string.h>

/* CRC covers everything up to but not including the crc32 field itself. */
#define CAL_CRC_SPAN (sizeof(daq_cal_t) - sizeof(uint32_t))

void daq_cal_set_identity(daq_cal_t *cal)
{
    (void)memset(cal, 0, sizeof(*cal));
    cal->magic        = DAQ_CAL_MAGIC;
    cal->version      = DAQ_CAL_VERSION;
    cal->num_channels = (uint16_t)DAQ_NUM_CHANNELS;
    for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
        cal->offset[c]   = 0;
        cal->gain_q16[c] = DAQ_CAL_UNITY_Q16;
    }
}

void daq_cal_finalize(daq_cal_t *cal, uint32_t serial, uint32_t uptime_s)
{
    cal->magic        = DAQ_CAL_MAGIC;
    cal->version      = DAQ_CAL_VERSION;
    cal->num_channels = (uint16_t)DAQ_NUM_CHANNELS;
    cal->serial       = serial;
    cal->uptime_s     = uptime_s;
    cal->reserved[0]  = 0U;
    cal->reserved[1]  = 0U;
    cal->reserved[2]  = 0U;
    cal->crc32        = crc32_iso((const uint8_t *)cal, CAL_CRC_SPAN);
}

bool daq_cal_valid(const daq_cal_t *cal)
{
    if (cal->magic != DAQ_CAL_MAGIC) {
        return false;
    }
    if (cal->version != DAQ_CAL_VERSION) {
        return false;
    }
    if (cal->num_channels != (uint16_t)DAQ_NUM_CHANNELS) {
        return false;
    }
    return cal->crc32 == crc32_iso((const uint8_t *)cal, CAL_CRC_SPAN);
}

bool daq_cal_is_overrange(int32_t raw)
{
    return (raw >= DAQ_CODE_FULL_SCALE) || (raw <= DAQ_CODE_MIN);
}

int32_t daq_cal_apply(const daq_cal_t *cal, uint32_t channel, int32_t raw)
{
    /*
     * int64 intermediate is not optional.  (raw - offset) can reach ~2^24 and
     * gain_q16 is ~2^16, so the product needs 40 bits.  On a 32-bit multiply
     * this silently truncates and the error only shows up near full scale -
     * exactly where a calibration bug is hardest to notice.
     */
    const int64_t centred = (int64_t)raw - (int64_t)cal->offset[channel];
    /* Round to nearest rather than truncating.  A bare >>16 floors, which
       biases every corrected sample by -0.5 LSB on average - a DC offset that
       the offset calibration has just been at pains to remove. */
    const int64_t scaled  = ((centred * (int64_t)cal->gain_q16[channel]) +
                             (int64_t)32768) >> 16;

    if (scaled > (int64_t)DAQ_CODE_FULL_SCALE) {
        return DAQ_CODE_FULL_SCALE;
    }
    if (scaled < (int64_t)DAQ_CODE_MIN) {
        return DAQ_CODE_MIN;
    }
    return (int32_t)scaled;
}

bool daq_cal_from_two_point(daq_cal_t *cal, uint32_t channel,
                            int32_t zero_code, int32_t ref_code,
                            int32_t ref_ideal)
{
    if (channel >= DAQ_NUM_CHANNELS) {
        return false;
    }

    const int64_t span = (int64_t)ref_code - (int64_t)zero_code;
    if (span == 0) {
        return false;
    }

    const int64_t half = (span > 0) ? (span / 2) : (-span / 2);
    const int64_t gain = (((int64_t)ref_ideal * DAQ_CAL_UNITY_Q16) + half) / span;

    /* Sanity band: anything outside 0.5x..2.0x means the two-point procedure
       was run without the reference actually connected.  Refusing here is
       cheaper than shipping a unit whose gain is silently 100x off. */
    if ((gain < (DAQ_CAL_UNITY_Q16 / 2)) || (gain > (DAQ_CAL_UNITY_Q16 * 2))) {
        return false;
    }

    cal->offset[channel]   = zero_code;
    cal->gain_q16[channel] = (int32_t)gain;
    return true;
}
