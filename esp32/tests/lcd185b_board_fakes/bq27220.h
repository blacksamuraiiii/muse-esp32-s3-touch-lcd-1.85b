/* Fake espressif/bq27220 0.1.3 header for the lcd185b board host test: the
 * structs and getter signatures read_power() uses, with the production
 * bit layout (copied from the component's bq27220.h). The harness implements
 * the getters against scriptable values. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void *bq27220_handle_t;

typedef union {
    struct {
        // Low byte, Low bit first
        bool DSG : 1;       /* The device is in DISCHARGE */
        bool SYSDWN : 1;    /* System down bit indicating the system should shut down */
        bool TDA : 1;       /* Terminate Discharge Alarm */
        bool BATTPRES : 1;  /* Battery Present detected */
        bool AUTH_GD : 1;   /* Detect inserted battery */
        bool OCVGD : 1;    /* Good OCV measurement taken */
        bool TCA : 1;       /* Terminate Charge Alarm */
        bool RSVD : 1;      /* Reserved */
        // High byte, Low bit first
        bool CHGINH : 1;    /* Charge inhibit */
        bool FC : 1;        /* Full-charged is detected */
        bool OTD : 1;       /* Overtemperature in discharge condition is detected */
        bool OTC : 1;       /* Overtemperature in charge condition is detected */
        bool SLEEP : 1;     /* Device is operating in SLEEP mode when set */
        bool OCVFAIL : 1;   /* Status bit indicating that the OCV reading failed due to current */
        bool OCVCOMP : 1;   /* An OCV measurement update is complete */
        bool FD : 1;        /* Full-discharge is detected */
    };
    uint16_t full;
} battery_status_t;

typedef struct {
    // Low byte, Low bit first
    bool CALMD : 1;         /* Calibration mode enabled */
    uint8_t SEC : 2;        /* Current security access */
    bool EDV2 : 1;          /* EDV2 threshold exceeded */
    bool VDQ : 1;           /* Indicates if Current discharge cycle is NOT qualified or qualified for an FCC updated */
    bool INITCOMP : 1;      /* gauge initialization is complete */
    bool SMTH : 1;          /* RemainingCapacity is scaled by smooth engine */
    bool BTPINT : 1;        /* BTP threshold has been crossed */
    // High byte, Low bit first
    uint8_t RSVD1 : 2;
    bool CFGUPDATE : 1;     /* Gauge is in CONFIG UPDATE mode */
    uint8_t RSVD0 : 5;
} operation_status_t;

uint16_t bq27220_get_voltage(bq27220_handle_t handle);
int16_t bq27220_get_avgcurrent(bq27220_handle_t bq_handle);
uint16_t bq27220_get_temperature(bq27220_handle_t handle);
uint16_t bq27220_get_state_of_charge(bq27220_handle_t handle);
esp_err_t bq27220_get_battery_status(bq27220_handle_t handle, battery_status_t *battery_status);
esp_err_t bq27220_get_operation_status(bq27220_handle_t handle, operation_status_t *operation_status);
