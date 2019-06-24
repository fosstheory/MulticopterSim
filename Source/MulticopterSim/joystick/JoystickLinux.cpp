/*
 * JoystickLinux.cpp: Linux implementation of joystick/gamepad support for MulticopterSim
 *
 * Copyright (C) 2018 Simon D. Levy
 *
 * MIT License
 */

#ifdef __linux__

#include "Joystick.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/joystick.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#ifndef debug
#define debug printf
#endif

enum {

    AX_THR,
    AX_ROL,
    AX_PIT,
    AX_YAW,
    AX_AU1,
    AX_AU2,
    AX_NIL
};

//                                            0       1       2       3       4       5       6       7
static uint8_t F310_MAP[8]                 = {AX_YAW, AX_THR, AX_ROL, AX_PIT, AX_NIL, AX_NIL, AX_NIL, AX_NIL};
static uint8_t SPEKTRUM_MAP[8]             = {AX_YAW, AX_THR, AX_ROL, AX_PIT, AX_AU2, AX_NIL, AX_AU1, AX_NIL};
static uint8_t XBOX360_WIRELESS_MAP[8]     = {AX_YAW, AX_THR, AX_NIL, AX_ROL, AX_PIT, AX_NIL, AX_NIL, AX_NIL};
static uint8_t REALFLIGHT_INTERLINK_MAP[8] = {AX_ROL, AX_PIT, AX_THR, AX_NIL, AX_YAW, AX_AU1, AX_NIL, AX_NIL};

static char productName[128];

Joystick::Joystick(const char * devname) 
{
    _joystickId = open(devname, O_RDONLY);

    if (_joystickId > 0) {

        fcntl(_joystickId, F_SETFL, O_NONBLOCK);

        *productName = 0;

        if (ioctl(_joystickId, JSIOCGNAME(sizeof(productName)), productName) < 0) {
            return;
        }

        if (strstr(productName, "Taranis") || strstr(productName, "DeviationTx Deviation GamePad")) {
            _productId = PRODUCT_TARANIS;
            _isRcTransmitter = true;
        }
        else if (strstr(productName, "Horizon Hobby SPEKTRUM")) {
            _productId = PRODUCT_SPEKTRUM;
            _isRcTransmitter = true;
        }
        else if (strstr(productName, "GREAT PLANES InterLink Elite")) {
            _productId = PRODUCT_REALFLIGHT_INTERLINK;
            _isRcTransmitter = true;
        }
         else if (strstr(productName, "Extreme 3D")) {
            _productId = PRODUCT_EXTREMEPRO3D;
        }
        else if (strstr(productName, "Generic X-Box pad")) {
            _productId = PRODUCT_XBOX360_CLONE;
        }
        else if (strstr(productName, "Logitech Logitech Dual Action")) {
            _productId = PRODUCT_F310;
        }
        else if (strstr(productName, "Xbox 360 Wireless Receiver")) {
            _productId = PRODUCT_XBOX360_WIRELESS;
        }
   }
}

// On RealFlight InterLink, auxiliary switches appear as buttons
static void handleRealflightInterlinkButtons(uint8_t number, int16_t value, float axes[6]) 
{
    //printf("%d %d\n", number, value);

    if (number == 3 && value == 1) {
        axes[AX_AU1] = 0.0;
    }

    if (number == 3 && value == 0) {
        axes[AX_AU1] = 0.5;
    }

    if (number == 4 && value == 0) {
        axes[AX_AU1] = 0.5;
    }

    if (number == 4 && value == 1) {
        axes[AX_AU1] = 1.0;
    }

}


Joystick::error_t Joystick::poll(float axes[6], uint8_t & buttonState)
{
    if (_joystickId <= 0) return ERROR_PRODUCT;

    struct js_event js;

    read(_joystickId, &js, sizeof(struct js_event));

    if (js.type & JS_EVENT_INIT) return ERROR_NOERROR;

    static float _axes[6];

    uint8_t * axisMap = NULL;

    switch (_productId) {

        case PRODUCT_F310:
            axisMap = F310_MAP;
            break;

        case PRODUCT_SPEKTRUM:
            axisMap = SPEKTRUM_MAP;
            break;

        case PRODUCT_XBOX360_WIRELESS:
            axisMap = XBOX360_WIRELESS_MAP;
            break;

        case PRODUCT_REALFLIGHT_INTERLINK:
            axisMap = REALFLIGHT_INTERLINK_MAP;
            break;

        default:
            debug("JOYSTICK '%s' NOT RECOGNIZED\n", productName);
            return ERROR_PRODUCT;
    }

    switch (js.type) {

        case JS_EVENT_AXIS: 
            _axes[axisMap[js.number]] = js.value / 32768.f;
            break;

        case JS_EVENT_BUTTON:

            // Special handling for RealFlight InterLink
            if (_productId == PRODUCT_REALFLIGHT_INTERLINK) {
                handleRealflightInterlinkButtons(js.number, js.value, _axes);
            }

            break;
    }

    for (uint8_t k=0; k<6; ++k) {
        axes[k] = _axes[k];
    }

    // Invert axes 0, 2 for unless R/C transmitter
    if (_productId == PRODUCT_REALFLIGHT_INTERLINK || !_isRcTransmitter) {
        axes[0] = -axes[0];
        axes[2] = -axes[2];
    }

    return ERROR_NOERROR;
}  

#endif