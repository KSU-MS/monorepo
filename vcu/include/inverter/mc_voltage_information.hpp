#ifndef MC_VOLTAGE_INFORMATION_H
#define MC_VOLTAGE_INFORMATION_H

#include <Arduino.h>
class MC_voltage_information {
    public:
        MC_voltage_information() = default;
        MC_voltage_information(uint8_t buf[8]) { load(buf); }

        inline void load(uint8_t buf[]){ 
            memcpy(this, buf, sizeof(*this));
        }
        inline void write(uint8_t buf[])  const { memcpy(buf, this, sizeof(*this)); }

        inline int16_t get_dc_bus_voltage()   const { return dc_bus_voltage; }
        inline int16_t get_output_voltage()   const { return output_voltage; }
        inline int16_t get_phase_ab_voltage() const { return phase_ab_voltage; }
        inline int16_t get_phase_bc_voltage() const { return phase_bc_voltage; }

    private:
        int16_t dc_bus_voltage;     // @Parse @Scale(10) @Unit(V)
        int16_t output_voltage;     // @Parse @Scale(10) @Unit(V)
        int16_t phase_ab_voltage;   // @Parse @Scale(10) @Unit(V)
        int16_t phase_bc_voltage;   // @Parse @Scale(10) @Unit(V)
    };
#endif