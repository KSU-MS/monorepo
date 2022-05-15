/*Testing live rinehart torque control with MCP3204 and teensy 4.1*/

#include <KS2e.h>
/******DEFINES*******/
// #define STARTUP 0
// #define TRACTIVE_SYSTEM_NOT_ACTIVE 1
// #define TRACTIVE_SYSTEM_ACTIVE 2
// #define ENABLING_INVERTER 3
// #define WAITING_RTD 4
// #define RTD 5
// #define mcFaulted 6
// #define rebootMC 7
unsigned long pchgAliveTimer=0;
unsigned long now=0;
int testState;
int pchgState;
bool inverterState,rtdButtonPressed=false;
bool precharge_success=false;
bool rtdFlag=false;
//torque limits
//placeholder pedal position values
uint8_t defaultInverterCmd[]={0,0,0,0,0,0,0,0};
uint8_t MC_internalState[8], MC_voltageInfo[8], MC_faultState[8], MC_motorPosInfo[8];
Metro timer_debug_raw_torque=Metro(100);
Metro pchgMsgTimer=Metro(100);
Metro miscDebugTimer=Metro(1000);
Metro timer_inverter_enable = Metro(2000); // Timeout failed inverter enable
Metro timer_motor_controller_send = Metro(50);
Metro timer_coloumb_count_send = Metro(1000);
Metro timer_ready_sound = Metro(2000); // Time to play RTD sound
Metro timer_can_update = Metro(100);
Metro timer_sensor_can_update = Metro(5);
Metro timer_restart_inverter = Metro(500, 1); // Allow the MCU to restart the inverter
Metro timer_status_send = Metro(100);
Metro timer_watchdog_timer = Metro(500);
PM100Info::MC_internal_states pm100State;
PM100Info::MC_motor_position_information pm100Speed;
PM100Info::MC_voltage_information pm100Voltage;
MCU_status mcu_status{};
//GPIOs
const int rtdButtonPin=33,TorqueControl=34,LaunchControl=35,InverterRelay=14;
const int maxState = 7;
int lastState;
int rinehartState;
int lastRinehartState;
bool inverter_restart = false;
//#define DEBUG 0
elapsedMillis dischargeCountdown;
elapsedMillis RTDcountDown;
elapsedMillis RTDbuzzer; const int RTDPin=15; unsigned long RTD_interval=200;
//objects
#define NUM_TX_MAILBOXES 2
#define NUM_RX_MAILBOXES 6
FlexCAN_T4<CAN2, RX_SIZE_256, TX_SIZE_16> CAN;
FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> DaqCAN;
FlexCAN_T4<CAN3, RX_SIZE_256, TX_SIZE_16> AccumulatorCAN;
uint8_t state = 0;                                      //basic state machine state
uint8_t disableWithZeros[] = {0, 0, 0, 0, 0, 0, 0, 0};  //The message to disable the controller/cancel lockout
uint8_t enableNoTorque[] = {0, 0, 0, 0, 1, 1, 0, 0};    //The message to enable the motor with zero torque
uint8_t enableSmallTorque[] = {0xD2, 0x04, 0, 0, 1, 1, 0, 0}; //The message to enable the motor with small torque
uint8_t maxTorque;

/*****PROTOTYPES*****/
void writeControldisableWithZeros();
void writeEnableNoTorque();
void idle();
void writeEnableFWDNoTorque();
void writeEnableSmallTorque();
void doStartup();
void readBroadcast();
void blinkLED();
int calculate_torque();
void sendPrechargeStartMsg();
void keepInverterAlive(bool enable);
int figureOutMCStuff();
int mcBusVoltage();
int mcMotorRPM();
void tryToClearMcFault();
void forceMCdischarge();
void testStateMachine();
void read_pedal_values();
static CAN_message_t tx_msg;
void setup()
{
    mcu_status.set_max_torque(0); //no torque on startup
    mcu_status.set_torque_mode(0);
    Serial.begin(115200);
    pinMode(RTDbutton,INPUT_PULLUP);
    pinMode(BUZZER,OUTPUT);digitalWrite(BUZZER,LOW);
    pinMode(TORQUEMODE, INPUT_PULLUP);
    pinMode(LAUNCHCONTROL, INPUT_PULLUP);
    pinMode(MC_RELAY, OUTPUT);
    pinMode(WSFL,INPUT_PULLUP);pinMode(WSFR,INPUT_PULLUP);
    CAN.begin();
    CAN.setBaudRate(500000);
    DaqCAN.begin();
    DaqCAN.setBaudRate(500000);
    AccumulatorCAN.begin();
    AccumulatorCAN.setBaudRate(250000);
    CAN.setMaxMB(NUM_TX_MAILBOXES+NUM_RX_MAILBOXES);
    for (int i = 0; i<NUM_RX_MAILBOXES; i++){
        CAN.setMB((FLEXCAN_MAILBOX)i,RX,STD);
    }
    for (int i = NUM_RX_MAILBOXES; i<(NUM_TX_MAILBOXES + NUM_RX_MAILBOXES); i++){
        CAN.setMB((FLEXCAN_MAILBOX)i,TX,STD);
    }
    CAN.setMBFilter(REJECT_ALL);
    CAN.setMBFilter(MB0,0x69); //precharge circuit id
    CAN.setMBFilter(MB1,ID_MC_VOLTAGE_INFORMATION);
    CAN.setMBFilter(MB2,ID_MC_FAULT_CODES);
    CAN.setMBFilter(MB3, ID_MC_INTERNAL_STATES);
    CAN.setMBFilter(MB4,ID_MC_MOTOR_POSITION_INFORMATION);
    CAN.mailboxStatus();
    testState=0;
    digitalWrite(MC_RELAY,HIGH); keepInverterAlive(0);
    mcu_status.set_inverter_powered(true);
    mcu_status.set_max_torque(TORQUE_2);
}
void loop()
{
    read_pedal_values();
    if (timer_restart_inverter.check() && inverter_restart) {
        inverter_restart = false;
        digitalWrite(MC_RELAY, HIGH);
        mcu_status.set_inverter_powered(true);
    }
    testStateMachine();
}
void readBroadcast()
{   
    char lineBuffer[50];
    CAN_message_t rxMsg;
    if (CAN.read(rxMsg))
    {   
        
        // Serial.print("  ID: 0x");
        // Serial.print(rxMsg.id, HEX);
        // Serial.print(" DATA: ");
        // for (uint8_t i = 0; i < 8; i++)
        // {
        //     Serial.print(rxMsg.buf[i],HEX);
        //     Serial.print(" ");
        // }
        if (rxMsg.id == ID_MC_INTERNAL_STATES)
        {
            memcpy(MC_internalState,rxMsg.buf,sizeof(MC_internalState));
           // Serial.print("<< Internal States");
        }
        if (rxMsg.id == ID_MC_FAULT_CODES)
        {
            memcpy(MC_faultState,rxMsg.buf,sizeof(MC_faultState));
        }
        if (rxMsg.id == ID_MC_VOLTAGE_INFORMATION)
        {
            memcpy(MC_voltageInfo,rxMsg.buf,sizeof(MC_voltageInfo));
        }
         if (rxMsg.id == ID_MC_MOTOR_POSITION_INFORMATION)
        {
            memcpy(MC_motorPosInfo,rxMsg.buf,sizeof(MC_motorPosInfo));
        }
        if (rxMsg.id == 0x69){
            pchgAliveTimer=millis();
            pchgState=rxMsg.buf[0];
            int accVoltage=rxMsg.buf[1]+(rxMsg.buf[2]*100);
            int tsVoltage=rxMsg.buf[3]+(rxMsg.buf[4]*100);
            sprintf(lineBuffer, "precharging: state: %d ACV: %dv TSV: %dv\n",pchgState,accVoltage,tsVoltage);
            //Serial.print(lineBuffer);
        }
        if(pchgState==2){precharge_success=true;}else if((now-pchgAliveTimer>=100) || pchgState==0 ||pchgState==1||pchgState==3){precharge_success=false;}
        // Serial.println("");
        
    }
}
void writeControldisableWithZeros()
{
    CAN_message_t ctrlMsg;
    ctrlMsg.len = 8;
    ctrlMsg.id = 0xC0; //OUR CONTROLLER
    memcpy(ctrlMsg.buf, disableWithZeros, sizeof(ctrlMsg.buf));
    if (CAN.write(ctrlMsg) > 0)
    {
        Serial.println("****DISABLE****");
        blinkLED();
    }
    // else
    // {
    //     Serial.print("NO CAN state: ");
    //     Serial.println(state);
    //    delay(50);
    // }
}
void writeEnableNoTorque()
{
    CAN_message_t ctrlMsg;
    ctrlMsg.len = 8;
    ctrlMsg.id = 0xC0; //OUR CONTROLLER
    memcpy(ctrlMsg.buf, enableNoTorque, sizeof(ctrlMsg.buf));
    CAN.write(ctrlMsg);
    Serial.println("----ENABLE----");
    blinkLED();
}
void doStartup()
{
    writeEnableNoTorque();
    writeControldisableWithZeros();
    writeEnableNoTorque();
}
void blinkLED()
{
    // digitalWrite(LED_BUILTIN, LOW);
    // digitalWrite(LED_BUILTIN, HIGH);
    // // delay(30);
    // digitalWrite(LED_BUILTIN, LOW);
    // delay(30);
}
inline void state_machine() {
    switch (mcu_status.get_state()) {
        case MCU_STATE::STARTUP: break;
        case MCU_STATE::TRACTIVE_SYSTEM_NOT_ACTIVE:
            inverter_heartbeat(0);
            #if DEBUG
            Serial.println("TS NOT ACTIVE");
            #endif
            // if TS is above HV threshold, move to Tractive System Active
            if (mc_voltage_information.get_dc_bus_voltage() >= MIN_HV_VOLTAGE) {
                #if DEBUG
                Serial.println("Setting state to TS Active from TS Not Active");
                #endif
                set_state(MCU_STATE::TRACTIVE_SYSTEM_ACTIVE);
            }
            break;

        case MCU_STATE::TRACTIVE_SYSTEM_ACTIVE:
            check_TS_active();
            inverter_heartbeat(0);

            // if start button has been pressed and brake pedal is held down, transition to the next state
            if (dashboard_status.get_start_btn() && mcu_status.get_brake_pedal_active()) {
                #if DEBUG
                Serial.println("Setting state to Enabling Inverter");
                #endif
                set_state(MCU_STATE::ENABLING_INVERTER);
            }
            break;

        case MCU_STATE::ENABLING_INVERTER:
            check_TS_active();
            inverter_heartbeat(1);

            // inverter enabling timed out
            if (timer_inverter_enable.check()) {
                #if DEBUG
                Serial.println("Setting state to TS Active from Enabling Inverter");
                #endif
                set_state(MCU_STATE::TRACTIVE_SYSTEM_ACTIVE);
            }
            // motor controller indicates that inverter has enabled within timeout period
            if (mc_internal_states.get_inverter_enable_state()) {
                #if DEBUG
                Serial.println("Setting state to Waiting Ready to Drive Sound");
                #endif
                set_state(MCU_STATE::WAITING_READY_TO_DRIVE_SOUND);
            }
            break;

        case MCU_STATE::WAITING_READY_TO_DRIVE_SOUND:
            check_TS_active();
            check_inverter_disabled();
            inverter_heartbeat(1);

            // if the ready to drive sound has been playing for long enough, move to ready to drive mode
            if (timer_ready_sound.check()) {
                #if DEBUG
                Serial.println("Setting state to Ready to Drive");
                #endif
                set_state(MCU_STATE::READY_TO_DRIVE);
            }
            break;

        case MCU_STATE::READY_TO_DRIVE:
            check_TS_active();
            check_inverter_disabled();

            //update_coulomb_count();
            if (timer_motor_controller_send.check()) {
                // MC_command_message mc_command_message(0, 0, 1, 1, 0, 0);
                MC_command_message mc_command_message(0,0,1,1,1,0);
                // FSAE EV.5.5
                // FSAE T.4.2.10
                if (filtered_accel1_reading < MIN_ACCELERATOR_PEDAL_1 || filtered_accel1_reading > MAX_ACCELERATOR_PEDAL_1) {
                    mcu_status.set_no_accel_implausability(false);
                    #if DEBUG
                    Serial.println("T.4.2.10 1");
                    #endif
                }
                else if (filtered_accel2_reading < MAX_ACCELERATOR_PEDAL_2 ||filtered_accel2_reading > MIN_ACCELERATOR_PEDAL_2) {
                    mcu_status.set_no_accel_implausability(false);
                    #if DEBUG
                    Serial.println("T.4.2.10 2");
                    #endif
                }
                // check that the pedals are reading within 10% of each other
                // sum of the two readings should be within 10% of the average travel
                // T.4.2.4
                else if ((filtered_accel1_reading - (4096 - filtered_accel2_reading)) >
                         (END_ACCELERATOR_PEDAL_1 - START_ACCELERATOR_PEDAL_1 + START_ACCELERATOR_PEDAL_2 - END_ACCELERATOR_PEDAL_2)/20 ){
                    #if DEBUG
                    Serial.println("T.4.2.4");
                    Serial.printf("computed - %f\n", filtered_accel1_reading - (4096 - filtered_accel2_reading));
                    Serial.printf("standard - %d\n", (END_ACCELERATOR_PEDAL_1 - START_ACCELERATOR_PEDAL_1 + START_ACCELERATOR_PEDAL_2 - END_ACCELERATOR_PEDAL_2)/20);
                    #endif
                    mcu_status.set_no_accel_implausability(false);
                }
                else{
                    mcu_status.set_no_accel_implausability(true);
                }

                // BSE check
                // EV.5.6
                // FSAE T.4.3.4
                if (filtered_brake1_reading < 409 || filtered_brake1_reading > 3687) {
                    mcu_status.set_no_brake_implausability(false);
                }
                else{
                    mcu_status.set_no_brake_implausability(true);
                }

                // FSAE EV.5.7
                // APPS/Brake Pedal Plausability Check
                if  (
                        (
                            (filtered_accel1_reading > ((END_ACCELERATOR_PEDAL_1 - START_ACCELERATOR_PEDAL_1)/4 + START_ACCELERATOR_PEDAL_1))
                            ||
                            (filtered_accel2_reading < ((END_ACCELERATOR_PEDAL_2 - START_ACCELERATOR_PEDAL_2)/4 + START_ACCELERATOR_PEDAL_2))
                        )
                        && mcu_status.get_brake_pedal_active()
                    )
                {
                    mcu_status.set_no_accel_brake_implausability(false);
                }
                else if
                (
                    (filtered_accel1_reading < ((END_ACCELERATOR_PEDAL_1 - START_ACCELERATOR_PEDAL_1)/20 + START_ACCELERATOR_PEDAL_1))
                    &&
                    (filtered_accel2_reading > ((END_ACCELERATOR_PEDAL_2 - START_ACCELERATOR_PEDAL_2)/20 + START_ACCELERATOR_PEDAL_2))
                )
                {
                    mcu_status.set_no_accel_brake_implausability(true);
                }

                int calculated_torque = 0;

                if (
                    mcu_status.get_no_brake_implausability() &&
                    mcu_status.get_no_accel_implausability() &&
                    mcu_status.get_no_accel_brake_implausability() &&
                    //mcu_status.get_software_is_ok() && //why was software not ok?
                    mcu_status.get_bms_ok_high() &&
                    mcu_status.get_imd_ok_high()
                    ) {
                    calculated_torque = calculate_torque();
                } else {
                  Serial.println("not calculating torque");
                  Serial.printf("no brake implausibility: %d\n", mcu_status.get_no_brake_implausability());
                  Serial.printf("no accel implausibility: %d\n", mcu_status.get_no_accel_implausability());
                  Serial.printf("no accel brake implausibility: %d\n", mcu_status.get_no_accel_brake_implausability());
                  Serial.printf("software is ok: %d\n", mcu_status.get_software_is_ok());
                  Serial.printf("get bms ok high: %d\n", mcu_status.get_bms_ok_high());
                  Serial.printf("get imd ok high: %d\n", mcu_status.get_imd_ok_high());

                }
                // Implausibility exists, command 0 torque

                #if DEBUG
                if (timer_debug_torque.check()) {
                   /* Serial.print("MCU REQUESTED TORQUE: ");
                    Serial.println(calculated_torque);
                    Serial.print("MCU NO IMPLAUS ACCEL: ");
                    Serial.println(mcu_status.get_no_accel_implausability());
                    Serial.print("MCU NO IMPLAUS BRAKE: ");
                    Serial.println(mcu_status.get_no_brake_implausability());
                    Serial.print("MCU NO IMPLAUS ACCEL BRAKE: ");
                    Serial.println(mcu_status.get_no_accel_brake_implausability());*/
                   /* Serial.printf("ssok: %d\n", mcu_status.get_software_is_ok());
                    Serial.printf("bms: %d\n", mcu_status.get_bms_ok_high());
                    Serial.printf("imd: %d\n", mcu_status.get_imd_ok_high());*/
                }
                #endif

                // Serial.print("RPM: ");
                // Serial.println(mc_motor_position_information.get_motor_speed());
                // Serial.println(calculated_torque);

                mc_command_message.set_torque_command(calculated_torque);

                mc_command_message.write(tx_msg.buf);
                tx_msg.id = ID_MC_COMMAND_MESSAGE;
                tx_msg.len = 8;
                CAN.write(tx_msg);
            }
            break;
    }
}
// void testStateMachine(){
//   switch(mcu_status.get_state()){
//     case MCU_STATE::STARTUP:
//     writeControldisableWithZeros();
//     tryToClearMcFault();
//     //what to do here?
//     break;
//     case(MCU_STATE::TRACTIVE_SYSTEM_NOT_ACTIVE):
//     //what to do here? probably skip
//     // if(mcControlTimer.check()==1){
//     //     writeControldisableWithZeros();
//     // }
//     keepInverterAlive(0);
//     if(pchgMsgTimer.check()==1){sendPrechargeStartMsg();}
//     if(precharge_success==true && mcBusVoltage()>=600){
//     }
//     break;
//     case(MCU_STATE::TRACTIVE_SYSTEM_ACTIVE):
//     //here?
//     break;
//     case(MCU_STATE::ENABLING_INVERTER):
//     if(mcControlTimer.check()){
//     doStartup();}
//     break;
//     case(MCU_STATE::WAITING_READY_TO_DRIVE_SOUND):
//     // if(mcControlTimer.check()==1){
//     //     writeEnableNoTorque();
//     // }
//     {keepInverterAlive(0);
//     int waitingToRtdTorque=calculate_torque();
//       rtdFlag=false;
//     #ifdef DEBUG
//     if(miscDebugTimer.check()){
//         Serial.print("Brake pedal active: "); 
//         Serial.println(mcu_status.get_brake_pedal_active());
//         Serial.print("RTD button pressed(0 is pressed): ");
//         Serial.println(digitalRead(rtdButtonPin));
//     }
//     #endif
//     if(mcu_status.get_brake_pedal_active() && digitalRead(rtdButtonPin)==LOW && (waitingToRtdTorque)<=0){return;}
//     else if(mcBusVoltage()<=600 || (now-pchgAliveTimer>=1000)){
//         #ifdef DEBUG
//         Serial.println("==STATE TRANSITION TO TS NOT ACTIVE ==")
//         Serial.print("Bus V: "); Serial.println(mcBusVoltage()); 
//         Serial.print("Precharge board state: ");Serial.println(pchgState); 
//         Serial.print("Precharge timeout: "); Serial.println(now-pchgAliveTimer); 
//         #endif
//         forceMCdischarge();
//         }
//     break;
//     case(MCU_STATE::READY_TO_DRIVE):
//     if(precharge_success==false){
//         forceMCdischarge();
//         }
//     if(mcControlTimer.check()==1){
//       //implement plausibility checks on the test bench at some point
//       int calculated_torque = calculate_torque(); //plaus checks are built in to the torque calculate function rn
//       uint8_t torquePart1 = calculated_torque % 256;
//       uint8_t torquePart2 = calculated_torque/256;
//       uint8_t angularVelocity1=0,angularVelocity2=0;
//       bool emraxDirection = true; //forward
//       bool inverterEnable = true; //go brrr
//       if((brake1>=22000)||calculated_torque==0|| !mcu_status.get_no_accel_brake_implausability()){
//           torquePart1=0x0C;
//           torquePart2=0xFE; //50nm regen
//       }
//       uint8_t torqueCommand[]={torquePart1,torquePart2,angularVelocity1,angularVelocity2,emraxDirection,inverterEnable,0,0};
//       CAN_message_t ctrlMsg;
//       ctrlMsg.len=8;
//       ctrlMsg.id=ID_MC_COMMAND_MESSAGE;
//       memcpy(ctrlMsg.buf, torqueCommand, sizeof(ctrlMsg.buf));
//       CAN.write(ctrlMsg);
//     }
//     break;
//   }

// }
// }

int calculate_torque() {
    int calculated_torque = 0;
    const int max_torque = mcu_status.get_max_torque() * 10;
    int torque1 = map(round(accel1), START_ACCELERATOR_PEDAL_1, END_ACCELERATOR_PEDAL_1, 0, max_torque);
    int torque2 = map(round(accel2), START_ACCELERATOR_PEDAL_2, END_ACCELERATOR_PEDAL_2, 0, max_torque);
    // #if DEBUG
    //   Serial.print("max torque: ");
    //   Serial.println(max_torque);
    //   Serial.print("torque1: ");
    //   Serial.println(torque1);
    //   Serial.print("torque2: ");
    //   Serial.println(torque2);
    // #endif

    // torque values are greater than the max possible value, set them to max
    if (torque1 > max_torque) {
        torque1 = max_torque;
    }
    if (torque2 > max_torque) {
        torque2 = max_torque;
    }
    // compare torques to check for accelerator implausibility
    calculated_torque = (torque1 + torque2) / 2;

    if (calculated_torque > max_torque) {
        calculated_torque = max_torque;
    }
    if (calculated_torque < 0) {
        calculated_torque = 0;
    }
    
                // FSAE EV.5.7
                // APPS/Brake Pedal Plausability Check
                if  (
                        (
                            (accel1 > ((END_ACCELERATOR_PEDAL_1 - START_ACCELERATOR_PEDAL_1)/4 + START_ACCELERATOR_PEDAL_1))
                            ||
                            (accel2 < ((END_ACCELERATOR_PEDAL_2 - START_ACCELERATOR_PEDAL_2)/4 + START_ACCELERATOR_PEDAL_2))
                        )
                        && mcu_status.get_brake_pedal_active()
                    )
                {
                    mcu_status.set_no_accel_brake_implausability(false);
                }
                else if
                (
                    (accel1 < ((END_ACCELERATOR_PEDAL_1 - START_ACCELERATOR_PEDAL_1)/20 + START_ACCELERATOR_PEDAL_1))
                    &&
                    (accel2 > ((END_ACCELERATOR_PEDAL_2 - START_ACCELERATOR_PEDAL_2)/20 + START_ACCELERATOR_PEDAL_2))
                )
                {
                    mcu_status.set_no_accel_brake_implausability(true);
                }

    //#if DEBUG
    if (timer_debug_raw_torque.check()) {
        Serial.print("TORQUE REQUEST DELTA PERCENT: "); // Print the % difference between the 2 accelerator sensor requests
        Serial.println(abs(torque1 - torque2) / (double) max_torque * 100);
        Serial.print("MCU RAW TORQUE: ");
        Serial.println(calculated_torque);
        Serial.print("TORQUE 1: ");
        Serial.println(torque1);
        Serial.print("TORQUE 2: ");
        Serial.println(torque2);
        Serial.print("Accel 1: ");
        Serial.println(accel1);
        Serial.print("Accel 2: ");
        Serial.println(accel2);
        Serial.print("Brake1 : ");
        Serial.println(brake1);
    }
    //#endif

     return calculated_torque;
}
inline void read_pedal_values() {
    /* Filter ADC readings */
    accel1 = ALPHA * accel1 + (1 - ALPHA) * ADC.read_adc(ADC_ACCEL_1_CHANNEL);
    accel2 = ALPHA * accel2 + (1 - ALPHA) * ADC.read_adc(ADC_ACCEL_2_CHANNEL);
    brake1 = ALPHA * brake1 + (1 - ALPHA) * ADC.read_adc(ADC_BRAKE_1_CHANNEL);
    //we dont have 2 brake sensors so commented out
    // filtered_brake2_reading = ALPHA * filtered_brake2_reading + (1 - ALPHA) * ADC.read_adc(ADC_BRAKE_2_CHANNEL);

    #if DEBUG
   // Serial.print("ACCEL 1: "); Serial.println(accel1);
   // Serial.print("ACCEL 2: "); Serial.println(accel2);
  //  Serial.print("BRAKE 1: "); Serial.println(brake1);
  //  Serial.print("BRAKE 2: "); Serial.println(filtered_brake2_reading);
    #endif

    // only uses front brake pedal
    mcu_status.set_brake_pedal_active(brake1 >= BRAKE_ACTIVE);

    /* Print values for debugging */
    /*#if DEBUG
    if (timer_debug.check()) {
        Serial.print("MCU PEDAL ACCEL 1: ");
        Serial.println(mcu_pedal_readings.get_accelerator_pedal_1());
        Serial.print("MCU PEDAL ACCEL 2: ");
        Serial.println(mcu_pedal_readings.get_accelerator_pedal_2());
        Serial.print("MCU PEDAL BRAKE: ");
        Serial.println(mcu_pedal_readings.get_brake_transducer_1());
        Serial.print("MCU BRAKE ACT: ");
        Serial.println(mcu_status.get_brake_pedal_active());
        Serial.print("MCU STATE: ");
        Serial.println(mcu_status.get_state());
    }
    #endif*/
}

void sendPrechargeStartMsg(){
    uint8_t prechargeCmd[]={0,0,0,0,0,0,0,0};
      CAN_message_t ctrlMsg;
      ctrlMsg.len=8;
      ctrlMsg.id=420;

      // MC_command_message mc_command_message(0,0,1,1,1,0);
      
      // mc_command_message.set_torque_command(calculated_torque);
      // mc_command_message.set_discharge_enable(true);
      // mc_command_message.set_inverter_enable(true);
      memcpy(ctrlMsg.buf, prechargeCmd, sizeof(ctrlMsg.buf));
      CAN.write(ctrlMsg);
}
void keepInverterAlive(bool enable){ //do u want the MC on or not?
    if(mcControlTimer.check()){
    CAN_message_t ctrlMsg;
      ctrlMsg.len=8;
      ctrlMsg.id=ID_MC_COMMAND_MESSAGE;
      uint8_t heartbeatMsg[]={0,0,0,0,1,enable,0,0};
      memcpy(ctrlMsg.buf, heartbeatMsg, sizeof(ctrlMsg.buf));
      CAN.write(ctrlMsg);
    }
}
int figureOutMCStuff(){
    int mcState;
    mcState=MC_internalState[0];
    return mcState;
}
int mcBusVoltage(){
    int busVoltage=MC_voltageInfo[0]+MC_voltageInfo[1]*256;
    return busVoltage;
}
int mcMotorRPM(){
    int emraxSpeed=MC_motorPosInfo[2]+MC_motorPosInfo[3]*256;
    if(emraxSpeed>65000){
        emraxSpeed=0;
    }
    return emraxSpeed;
}
void  tryToClearMcFault(){
    if(mcControlTimer.check()){
    CAN_message_t ctrlMsg;
      ctrlMsg.len=8;
      ctrlMsg.id=ID_MC_READ_WRITE_PARAMETER_COMMAND;
      uint8_t clearFaultMsg[]={20,0,1,0,0,0,0,0};
      memcpy(ctrlMsg.buf, clearFaultMsg, sizeof(ctrlMsg.buf));
      CAN.write(ctrlMsg);
}
}
void forceMCdischarge(){
    dischargeCountdown=0;
    while(dischargeCountdown<=100){
        if(mcControlTimer.check()==1){
            CAN_message_t ctrlMsg;
        ctrlMsg.len=8;
        ctrlMsg.id=ID_MC_COMMAND_MESSAGE;
        uint8_t dischgMsg[]={0,0,0,0,1,0b0000010,0,0};//bit one?
        memcpy(ctrlMsg.buf, dischgMsg, sizeof(ctrlMsg.buf));
        CAN.write(ctrlMsg);
            //writeEnableNoTorque();
            }
        } 
        for(int i=0;i<=10;i++){
            writeControldisableWithZeros();
        }
}