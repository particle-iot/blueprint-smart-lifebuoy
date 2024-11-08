/*
 * Copyright (c) 2023 EdgeImpulse Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an "AS
 * IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language
 * governing permissions and limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Supported accelerometers */
#define ACC_ADXL362DMA  1
#define ACC_ADXL345     2

/* Include ----------------------------------------------------------------- */
#include "Particle.h"

/* Edge Impulse supports 2 accelometers out of box; the adxl362 using SPI for
 * Photon devices, the adxl345 using I2C for Boron devices.
 */
#if PLATFORM_ID == PLATFORM_P2
#define ACC_SENSOR  ACC_ADXL362DMA
#elif defined(PARTICLE_BORON) || PLATFORM_ID == PLATFORM_BORON || PLATFORM_ID == PLATFORM_BSOM || PLATFORM_ID == PLATFORM_B5SOM
#define ACC_SENSOR  ACC_ADXL345
#else
#error "No ACC sensor defined for this platform"
#endif

#if ACC_SENSOR == ACC_ADXL345
#include "adxl345.h"
#else
#include "ADXL362DMA.h"
#endif
#include <gesture-detection_inferencing.h>

SYSTEM_THREAD(ENABLED);
SerialLogHandler logHandler(LOG_LEVEL_ERROR);

/* Constants --------------------------------------------------------------- */
#define CONVERT_G_TO_MS2    9.80665f

/* Private variables ------------------------------------------------------- */
#if ACC_SENSOR == ACC_ADXL345
ADXL345 *accel;
#else
ADXL362DMA *accel;
#endif

/* Forward declarations ---------------------------------------------------- */
void print_inference_result(ei_impulse_result_t result);

/**
 * @brief Return the sign of the number
 * 
 * @param number 
 * @return int 1 if positive (or 0) -1 if negative
 */
float ei_get_sign(float number) {
    return (number >= 0.0) ? 1.0 : -1.0;
}

/**
 * @brief      Particle setup function
 */
void setup()
{
    // put your setup code here, to run once:

    // Wait for serial to make it easier to see the serial logs at startup.
    waitFor(Serial.isConnected, 15000);
    delay(2000);

    ei_printf("Edge Impulse inference runner for Particle devices\r\n");

    /* Init & configure sensor */
#if ACC_SENSOR == ACC_ADXL345
    accel = new ADXL345;
#else
    accel = new ADXL362DMA(SPI, D13 /* A2 */);
#endif


#if ACC_SENSOR == ACC_ADXL345
    accel->powerOn();
    accel->setRangeSetting(2);
#else
    accel->softReset();
    delay(100);
    while(accel->readStatus() == 0) {
        ei_printf("no status yet, waiting for accelerometer\r\n");
        ei_sleep(1000);
    }

    accel->writeFilterControl(accel->RANGE_2G, false, false, accel->ODR_200);
    accel->setMeasureMode(true);
#endif
}

/**
 * @brief      Particle main function
 */
void loop()
{
    ei_printf("\nStarting inferencing in 2 seconds...\n");

    delay(2000);

    ei_printf("Sampling...\n");

    // Allocate a buffer here for the values we'll read from the IMU
    static float buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = { 0 };

    for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; ix += 3) {
        // Determine the next tick (and then sleep later)
        uint64_t next_tick = micros() + (EI_CLASSIFIER_INTERVAL_MS * 1000);

        // Read accelerometer data  and convert to ms2 in floats
#if ACC_SENSOR == ACC_ADXL345
        int16_t xyz[3];
        accel->readAccel(xyz);
        for (int i = 0; i < 3; i++) {
            buffer[ix + i] = (((float)xyz[i]) * 0.00389f) * CONVERT_G_TO_MS2;
        }
#else
        int16_t acc[3];
        accel->readXYZ(acc[0], acc[1], acc[2]);
        for (int i = 0; i < 3; i++) {
            buffer[ix + i] = (((float)(acc[i] * 2)) / 2048.f) * CONVERT_G_TO_MS2;
        }
#endif
        delayMicroseconds(next_tick - micros());
    }

    // Turn the raw buffer in a signal which we can the classify
    signal_t signal;
    int err = numpy::signal_from_buffer(buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
    if (err != 0) {
        ei_printf("Failed to create signal from buffer (%d)\n", err);
        return;
    }

    ei_impulse_result_t result = { 0 };

    // invoke the impulse
    EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false /* debug */);
    if (res != EI_IMPULSE_OK) {
        ei_printf("ERR: Failed to run classifier (%d)\n", res);
        return;
    }

    // print inference return code
    ei_printf("run_classifier returned: %d\r\n", res);
    print_inference_result(result);

    delay(1000);
}

void print_inference_result(ei_impulse_result_t result) {

    // Print how long it took to perform inference
    ei_printf("Timing: DSP %d ms, inference %d ms, anomaly %d ms\r\n",
            result.timing.dsp,
            result.timing.classification,
            result.timing.anomaly);

    // Print the prediction results (object detection)
#if EI_CLASSIFIER_OBJECT_DETECTION == 1
    ei_printf("Object detection bounding boxes:\r\n");
    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
        if (bb.value == 0) {
            continue;
        }
        ei_printf("  %s (%f) [ x: %u, y: %u, width: %u, height: %u ]\r\n",
                bb.label,
                bb.value,
                bb.x,
                bb.y,
                bb.width,
                bb.height);
    }

    // Print the prediction results (classification)
#else
    ei_printf("Predictions:\r\n");
    for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        ei_printf("  %s: ", ei_classifier_inferencing_categories[i]);
        ei_printf("%.5f\r\n", result.classification[i].value);
    }
#endif

    // Print anomaly result (if it exists)
#if EI_CLASSIFIER_HAS_ANOMALY == 1
    ei_printf("Anomaly prediction: %.3f\r\n", result.anomaly);
#endif

}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_ACCELEROMETER
#error "Invalid model for current sensor"
#endif