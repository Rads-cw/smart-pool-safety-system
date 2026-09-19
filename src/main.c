/*
 * Smart Pool Safety and Monitoring System
 * Platform: Dragon12-Plus / MC9S12DG256
 * Language: C
 *
 * Features:
 * - Waterproof ultrasonic distance sensing
 * - DS18B20 water temperature sensing
 * - TDS water-quality measurement with temperature compensation
 * - Water-level monitoring
 * - Ambient temperature / fan control
 * - Keypad and LCD interaction
 * - Buzzer, pump, and servo emergency response
 */

#include <hidef.h>
#include <mc9s12dg256.h>

#pragma LINK_INFO DERIVATIVE "mc9s12dg256b"

#include "main_asm.h"

#define TRIG_MASK 0x04u
#define ECHO_MASK 0x08u

#define OW_MASK 0x01u
#define OW_PORT PTT
#define OW_DDR  DDRT

float calc_tds_ppm(float tForCalc);
void servoB_lower(void);
void servoB_raise(void);


/* ============================================================
 * Keypad
 * ============================================================ */

int key_scan(void)
{
    const char keycodes[] = {
        0x7D, 0xEE, 0xED, 0xEB,
        0xDE, 0xDD, 0xDB, 0xBE,
        0xBD, 0xBB, 0xE7, 0xD7,
        0xB7, 0x77, 0x7E, 0x7B
    };

    int i, j, key;
    char readback;
    int found;

    key = 16;  /* Return 16 if no key is pressed. */
    found = 0;
    i = 0;

    while ((i < 16) && (found == 0)) {
        PORTA = keycodes[i];

        for (j = 0; j < 10; j++) {
            /* Short settling delay. */
        }

        readback = PORTA;

        if (readback == keycodes[i]) {
            key = i;
            found = 1;
        } else {
            i++;
        }
    }

    return key;
}

int getkey0(void)
{
    int key;

    do {
        key = key_scan();
    } while (key == 16);

    return key;
}

void wait_for_keyup(void)
{
    while (key_scan() != 16) {
        /* Wait until the pressed key is released. */
    }
}


/* ============================================================
 * Ultrasonic sensor
 * ============================================================ */

static void timer_init(void)
{
    TSCR1 = 0x90;
    TSCR2 = 0x04;
    TIOS  = 0x00;
}

static unsigned int microseconds_to_ticks(unsigned long microseconds)
{
    unsigned long ticks = microseconds * 3u + 1u;
    ticks /= 2u;

    return (unsigned int)ticks;
}

static void delay_microseconds(unsigned int microseconds)
{
    unsigned int start_time = TCNT;
    unsigned int delay_span = microseconds_to_ticks(microseconds);

    while ((unsigned int)(TCNT - start_time) < delay_span) {
        /* Busy wait. */
    }
}

static void trigger_pulse(void)
{
    PTT &= (unsigned char)(~TRIG_MASK);
    delay_microseconds(2);

    PTT |= TRIG_MASK;
    delay_microseconds(10);

    PTT &= (unsigned char)(~TRIG_MASK);
}

static unsigned int echo_pulse_duration(void)
{
    unsigned int start_time;
    unsigned int max_timeout = microseconds_to_ticks(25000u);

    start_time = TCNT;
    while ((PTT & ECHO_MASK) != 0u) {
        if ((unsigned int)(TCNT - start_time) > max_timeout) {
            return 0u;
        }
    }

    start_time = TCNT;
    while ((PTT & ECHO_MASK) == 0u) {
        if ((unsigned int)(TCNT - start_time) > max_timeout) {
            return 0u;
        }
    }

    start_time = TCNT;
    while ((PTT & ECHO_MASK) != 0u) {
        if ((unsigned int)(TCNT - start_time) > max_timeout) {
            return 0u;
        }
    }

    return (unsigned int)(TCNT - start_time);
}

static unsigned int convert_ticks_to_centimeters(unsigned int ticks)
{
    return (unsigned int)((ticks + 43u) / 87u);
}


/* ============================================================
 * DS18B20 water temperature sensor (1-Wire)
 * ============================================================ */

static void onewire_drive_low(void)
{
    OW_PORT &= (unsigned char)(~OW_MASK);
    OW_DDR |= OW_MASK;
}

static void onewire_release(void)
{
    OW_DDR &= (unsigned char)(~OW_MASK);
}

static char onewire_sample(void)
{
    return (PTT & OW_MASK) ? 1 : 0;
}

static char onewire_reset(void)
{
    char device_present;

    onewire_drive_low();
    delay_microseconds(500);

    onewire_release();
    delay_microseconds(70);

    device_present = (onewire_sample() == 0);

    delay_microseconds(420);

    return device_present;
}

static void onewire_write_bit(char bit_value)
{
    if (bit_value) {
        onewire_drive_low();
        delay_microseconds(6);

        onewire_release();
        delay_microseconds(64);
    } else {
        onewire_drive_low();
        delay_microseconds(60);

        onewire_release();
        delay_microseconds(10);
    }
}

static char onewire_read_bit(void)
{
    char bit_value;

    onewire_drive_low();
    delay_microseconds(3);

    onewire_release();
    delay_microseconds(12);

    bit_value = onewire_sample();

    delay_microseconds(50);

    return bit_value;
}

static void onewire_write_byte(char byte_value)
{
    char bit_index;

    for (bit_index = 0; bit_index < 8; bit_index++) {
        onewire_write_bit(byte_value & 1);
        byte_value >>= 1;
    }
}

static char onewire_read_byte(void)
{
    char bit_index;
    char byte_value = 0;

    for (bit_index = 0; bit_index < 8; bit_index++) {
        if (onewire_read_bit()) {
            byte_value |= (1 << bit_index);
        }
    }

    return byte_value;
}

static char temperature_sensor_start_conversion(void)
{
    if (!onewire_reset()) {
        return 0;
    }

    onewire_write_byte(0xCC);
    onewire_write_byte(0x44);

    return 1;
}

static int temperature_sensor_read_raw(void)
{
    char low_byte, high_byte;
    int raw_temperature;

    if (!onewire_reset()) {
        return 9999;
    }

    onewire_write_byte(0xCC);
    onewire_write_byte(0xBE);

    low_byte = onewire_read_byte();
    high_byte = onewire_read_byte();

    raw_temperature =
        (((int)(high_byte & 0xFF)) << 8) |
        ((int)(low_byte & 0xFF));

    if ((raw_temperature == 0xFFFF) ||
        (raw_temperature == 0xFFFE)) {
        return 9999;
    }

    return raw_temperature;
}


/* ============================================================
 * TDS calculation
 * ============================================================ */

float calc_tds_ppm(float tForCalc)
{
    unsigned int raw;
    float voltage;
    float compFactor;
    float tdsPPM;

    raw = ad0conv(4);
    voltage = ((float)raw * 5.0) / 1024.0;

    compFactor = 1.0 + 0.02 * (tForCalc - 25.0);

    tdsPPM =
        (133.42 * voltage * voltage * voltage
        - 255.86 * voltage * voltage
        + 857.39 * voltage) * 0.30;

    tdsPPM = tdsPPM / compFactor;

    return tdsPPM;
}


/* ============================================================
 * Main program
 * ============================================================ */

void main(void)
{
    unsigned int pulse_ticks, distance_cm;
    unsigned char danger_flag = 0;

    int temperature_value = 9999;
    unsigned int temperature_elapsed = 0;
    unsigned char temperature_conversion_active = 0;

    float tds_ppm;
    unsigned int tds_integer;
    float temperature_for_tds = 25.0;

    char key;
    int room_temp;
    int waterLevel;

    PLL_init();
    lcd_init();
    timer_init();
    ad0_enable();

    motor0_init();
    motor1_init();
    keypad_enable();

    DDRT |= TRIG_MASK;
    DDRT |= 0x02;

    PTT &= (unsigned char)(~TRIG_MASK);

    onewire_release();

    DDRH |= 0x03;

    for (;;) {

        /* Start a new DS18B20 conversion if one is not already active. */
        if (!temperature_conversion_active) {
            if (temperature_sensor_start_conversion()) {
                temperature_conversion_active = 1;
                temperature_elapsed = 0;
            }
        }

        ms_delay(60);
        temperature_elapsed += 60;

        /* Continuously measure distance using the ultrasonic sensor. */
        trigger_pulse();
        pulse_ticks = echo_pulse_duration();

        if (pulse_ticks == 0u) {

            /* No echo received. Clear the distance field. */
            set_lcd_addr(0x45);
            type_lcd("       ");

            if (danger_flag) {
                set_lcd_addr(0x00);
                type_lcd("                ");
                danger_flag = 0;
            }

        } else {

            distance_cm = convert_ticks_to_centimeters(pulse_ticks);

            set_lcd_addr(0x45);
            write_int_lcd((int)distance_cm);
            type_lcd(" cm");

            /*
             * Drowning-detection confirmation:
             * the object must remain at <= 25 cm for ~3 seconds.
             */
            if ((distance_cm <= 25) && !danger_flag) {

                unsigned int confirmation_time = 0;
                unsigned char still_close = 1;

                while (confirmation_time < 3000) {

                    ms_delay(200);
                    confirmation_time += 200;
                    temperature_elapsed += 200;

                    if (temperature_conversion_active &&
                        temperature_elapsed >= 750) {

                        temperature_value = temperature_sensor_read_raw();

                        if (temperature_value != 9999) {
                            temperature_for_tds =
                                (float)(temperature_value / 16);
                        }

                        temperature_conversion_active = 0;
                    }

                    trigger_pulse();
                    pulse_ticks = echo_pulse_duration();

                    if (pulse_ticks == 0u) {
                        still_close = 0;
                        break;
                    }

                    distance_cm =
                        convert_ticks_to_centimeters(pulse_ticks);

                    set_lcd_addr(0x45);
                    write_int_lcd((int)distance_cm);
                    type_lcd(" cm");

                    if (distance_cm > 25) {
                        still_close = 0;
                        break;
                    }
                }

                if (still_close) {

                    set_lcd_addr(0x00);
                    type_lcd("!!! DANGER !!!    ");
                    danger_flag = 1;

                    /* Buzzer ON (active-low module). */
                    PTT &= ~0x02;

                    /* Run the water pump at full power. */
                    motor1(255);

                    /* Raise servo to release the lifebuoy. */
                    servoB_raise();

                    ms_delay(2000);
                    temperature_elapsed += 2000;

                    /* Buzzer OFF. */
                    PTT |= 0x02;

                    /* Keep pump active briefly after the alarm. */
                    ms_delay(3000);
                    temperature_elapsed += 3000;

                    motor1(0);

                    set_lcd_addr(0x00);
                    type_lcd("                ");
                    danger_flag = 0;
                }

            } else if ((distance_cm > 25) && danger_flag) {

                PTT |= 0x02;

                set_lcd_addr(0x00);
                type_lcd("                ");

                danger_flag = 0;
            }
        }

        ms_delay(200);
        temperature_elapsed += 200;

        /* Ambient-temperature fan control. */
        room_temp = ad0conv(5);
        room_temp = room_temp / 2;

        if (room_temp > 24) {
            motor0(255);
        } else {
            motor0(128);
        }

        /* Read the water temperature after conversion is complete. */
        if (temperature_conversion_active &&
            temperature_elapsed >= 750) {

            temperature_value = temperature_sensor_read_raw();

            if (temperature_value != 9999) {
                temperature_for_tds =
                    (float)(temperature_value / 16);
            }

            temperature_conversion_active = 0;
        }

        /* Calculate temperature-compensated TDS. */
        tds_ppm = calc_tds_ppm(temperature_for_tds);
        tds_integer = (unsigned int)tds_ppm;

        /*
         * Non-blocking keypad scan so the sensors continue updating
         * while waiting for user input.
         */
        key = key_scan();

        if (key != 16) {

            wait_for_keyup();

            if (key == 0) {

                clear_lcd();

                set_lcd_addr(0x00);
                type_lcd("Lowering servos...");

                servoB_lower();

                clear_lcd();

                set_lcd_addr(0x00);
                type_lcd("Servos lowered  ");

                ms_delay(1000);
                clear_lcd();

            } else if (key == 1) {

                clear_lcd();

                room_temp = ad0conv(5);
                room_temp = room_temp / 2;

                set_lcd_addr(0x00);
                type_lcd("Room Temp:      ");

                set_lcd_addr(0x40);
                write_int_lcd(room_temp);
                type_lcd(" C              ");

                ms_delay(500);

            } else if (key == 2) {

                clear_lcd();

                set_lcd_addr(0x00);
                type_lcd("Water Temp:     ");

                set_lcd_addr(0x40);
                write_int_lcd(temperature_value / 16);
                type_lcd(" C              ");

                ms_delay(500);

            } else if (key == 3) {

                clear_lcd();

                set_lcd_addr(0x00);
                type_lcd("TDS:            ");

                set_lcd_addr(0x40);
                write_int_lcd(tds_integer);
                type_lcd(" ppm            ");

                ms_delay(500);

            } else if (key == 4) {

                clear_lcd();

                waterLevel = ad0conv(2);
                waterLevel = waterLevel >> 1;

                if ((waterLevel >= 100) && (waterLevel <= 215)) {

                    clear_lcd();

                    set_lcd_addr(0x00);
                    type_lcd("Water Level:    ");

                    set_lcd_addr(0x40);
                    type_lcd("Water level:low");

                    ms_delay(1000);

                } else if ((waterLevel >= 216) &&
                           (waterLevel <= 250)) {

                    clear_lcd();

                    set_lcd_addr(0x00);
                    type_lcd("Water Level:    ");

                    set_lcd_addr(0x40);
                    type_lcd("Water level: med");

                    ms_delay(1000);

                } else if ((waterLevel >= 251) &&
                           (waterLevel <= 360)) {

                    clear_lcd();

                    set_lcd_addr(0x00);
                    type_lcd("Water Level:    ");

                    set_lcd_addr(0x40);
                    type_lcd("Water level: Hi");

                    ms_delay(1000);

                } else if (waterLevel < 100) {

                    clear_lcd();

                    set_lcd_addr(0x00);
                    type_lcd("Water Level:    ");

                    set_lcd_addr(0x40);
                    type_lcd("No water");

                    ms_delay(1000);
                }

                set_lcd_addr(0x40);
                ms_delay(500);
            }
        }
    }
}


/* ============================================================
 * Lifebuoy servo control
 * ============================================================ */

void servoB_lower(void)
{
    int i;

    for (i = 0; i < 50; i++) {
        PTH |= 0x02;
        ms_delay(2);

        PTH &= ~0x02;
        ms_delay(30);
    }
}

void servoB_raise(void)
{
    int i;

    for (i = 0; i < 50; i++) {
        PTH |= 0x02;
        ms_delay(1);

        PTH &= ~0x02;
        ms_delay(19);
    }
}
