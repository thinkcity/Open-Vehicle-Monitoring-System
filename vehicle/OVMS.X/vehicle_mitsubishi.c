/*
;    Project:       Open Vehicle Monitor System
;    Date:          6 May 2012
;
;    Changes:
;    1.8  02.10.13 (Thomas)
;         - Added filtering of QC state detection to prevent false QC status
;    1.7  31.10.13 (Thomas)
;         - Changed quick charge detection.
;         - Added charge stale timer to prevent hanging in charging mode.
;    1.6  30.10.13 (Thomas)
;         - Added car_SOCalertlimit = 10 to set a system alert when SOC < 10
;         - Added function for stale temps
;    1.5  26.10.13 (Thomas)
;         - Added Motor and PEM temperature reading (thanks to Matt Beard)
;         - Added calculation of estimated range during QC (thanks to Matt Beard)
;         - Added detection of quick charging
;         - Changed calculation of ideal range to reflect that SOC below 10% not usable.
;         - Changed hard coded charge limit to 16A for standardization.
;         - Fixed temperature stale in app.
;         - Fixed charge status on charge interruption.
;    1.4  27.09.13 (Thomas)
;         - Fixed battery temperature reading.
;         - TODO: Fix charge status on charge interruption.
;    1.3  23.09.13 (Thomas)
;         - Fixed charging notification when battery is full, SOC=100%
;         - TODO: Fix battery temperature reading. 
;    1.2  22.09.13 (Thomas)
;         - Fixed Ideal range
;         - Added parking timer
;         - Added detection of car is asleep.
;    1.1  21.09.13 (Thomas)
;         - Fixed ODO
;         - Fixed filter and mask for poll0 (thanks to Matt Beard)
;         - Verified estimated range
;         - Verified SOC
;         - Verified Charge state
;         - Added battery temperature reading (thanks to Matt Beard)
;    1.0  Initial release
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

#include <stdlib.h>
#include <delays.h>
#include <string.h>
#include "ovms.h"
#include "params.h"
#include "net_msg.h"

// Mitsubishi state variables

#pragma udata overlay vehicle_overlay_data
unsigned char mi_charge_timer;   // A per-second charge timer
unsigned long mi_charge_wm;      // A per-minute watt accumulator
unsigned char mi_candata_timer;  // A per-second timer for CAN bus data
unsigned char mi_stale_charge;   // A charge stale timer
unsigned char mi_quick_charge;   // Quick charge status 
unsigned char mi_qc_counter;     // Counter to filter false QC messages (0xff)
unsigned int  mi_estrange;       // The estimated range from the CAN-bus


signed char mi_batttemps[24]; // Temperature buffer, holds first two temps from the 0x6e1 messages (24 of the 64 values)

// Variables to calculate the estimated range during rapid/quick charge
unsigned char mi_last_good_SOC;    // The last known good SOC
unsigned char mi_last_good_range;  // The last known good range

#pragma udata

////////////////////////////////////////////////////////////////////////
// vehicle_mitsubishi_ticker1()
// This function is an entry point from the main() program loop, and
// gives the CAN framework a ticker call approximately once per second
//
BOOL vehicle_mitsubishi_ticker1(void)
  {

  ////////////////////////////////////////////////////////////////////////
  // Stale tickers
  ////////////////////////////////////////////////////////////////////////

  car_doors3bits.CoolingPump = (car_stale_temps <= 1)?0:1;

  if (mi_stale_charge > 0)
    {
    if (--mi_stale_charge == 0) // Charge stale
      {
      mi_quick_charge = 0;
      car_linevoltage = 0;
      car_chargecurrent = 0;
      }
    }

  if (mi_candata_timer > 0)
    {
    if (--mi_candata_timer == 0) 
      { // Car has gone to sleep
      car_doors3 &= ~0x01;  // Car is asleep
      }
    else
      {
      car_doors3 |= 0x01;   // Car is awake
      }
    }

  car_time++;
  
  
  ////////////////////////////////////////////////////////////////////////
  // Range update
  ////////////////////////////////////////////////////////////////////////
  // Range of 255 is returned during rapid/quick charge
  // We can use this to indicate charge rate, but we will need to
  // calculate a new range based on the last seen SOC and estrange
  
  if (mi_quick_charge == 1) //Quick charging
    {
    // Simple range stimation during quick/rapid charge based on
    // last seen decent values of SOC and estrange and assuming that
    // estrange hits 0 at 10% SOC and is linear from there to 100%

    if (car_SOC <= 10)
      {
      car_estrange = 0;
      }

    else
      {
      // If the last known SOC was too low for accurate guesstimating, 
      // use fundge-figures giving 72 mile (116 km)range as it is probably best
      // to guess low-ish here (but not silly low)
      if (car_SOC < 20)
        {
        mi_last_good_SOC = 20;
        mi_last_good_range = 8;
        }
      car_estrange = ((unsigned int)mi_last_good_range * (unsigned int)(car_SOC - 10)) / ((unsigned int)(mi_last_good_SOC - 10));
      }
    }

  else // Not quick charging
    {
    if (mi_estrange != 255) // Check valid value to prevent false estimated range after ended quick charge
      {
      if (can_mileskm == 'K') 
        {
        car_estrange = (unsigned int)(MiFromKm((unsigned long)mi_estrange));
        }
      else
        {
        car_estrange = mi_estrange; 
        }
      }

    if ((car_SOC >= 20) && (car_estrange >= 5)) // Save last good value
      {
      mi_last_good_SOC = car_SOC;
      mi_last_good_range = car_estrange;
      }
    }

  if (car_SOC <= 10) // Only SOC above 10% usable
    {
    car_idealrange = 0;
    }
  else
    {
    car_idealrange = ((((unsigned int)(car_SOC) - 10) * 104) / 100); //Ideal range: 93 miles (150 Km)
    }
  
  
  ////////////////////////////////////////////////////////////////////////
  // Charge state determination
  ////////////////////////////////////////////////////////////////////////
  // 0x04 Chargeport open, bit 2 set
  // 0x08 Pilot signal on, bit 3 set
  // 0x10 Vehicle charging, bit 4 set
  // 0x0C Chargeport open and pilot signal, bits 2,3 set
  // 0x1C Bits 2,3,4 set
  
  
  
  if ((mi_quick_charge == 1) || ((car_chargecurrent != 0) && (car_linevoltage > 100)))
    { // CAN says the car is charging
    car_doors5bits.Charging12V = 1;  //MJ
    if ((car_doors1 & 0x08) == 0)
      { // Charge has started
      car_doors1 |= 0x1c;     // Set charge, door and pilot bits
      car_chargemode = 0;     // Standard charge mode
      car_chargestate = 1;    // Charging state
      car_chargesubstate = 3; // Charging by request
      if (mi_quick_charge == 1) // Quick charge
        {
        car_chargelimit = 125;// Signal quick charging
        }
      else
        {
          car_chargelimit = 16; // Hard-coded 16A charging
        }
      car_chargeduration = 0; // Reset charge duration
      car_chargekwh = 0;      // Reset charge kWh
      mi_charge_timer = 0;    // Reset the per-second charge timer
      mi_charge_wm = 0;       // Reset the per-minute watt accumulator
      net_req_notification(NET_NOTIFY_STAT);
      }

    else
      { // Charge is ongoing
      car_doors1bits.ChargePort = 1;  //MJ
      mi_charge_timer++;
      if (mi_charge_timer >= 60)
        { // One minute has passed
        mi_charge_timer = 0;
        car_chargeduration++;
        if (mi_quick_charge == 0) // Not QC
          {
          mi_charge_wm += (car_chargecurrent*car_linevoltage);
          if (mi_charge_wm >= 60000L)
            { // Let's move 1kWh to the virtual car
            car_chargekwh += 1;
            mi_charge_wm -= 60000L;
            }
          }
        }
      }
    }
  
  // Special case: The car will take a ~15 min charging brake buring normal charging to level 
  // battery cellsat ~70% SOC. car_chargecurrent 0A and car_chargevoltage > 100V will be reporter
  // reported in this stage.   
  else if ((car_chargecurrent == 0) && (car_linevoltage > 100))
    { // CAN says the car is not charging
    if ((car_doors1 & 0x08) && (car_SOC == 100))
      { // Charge has completed 
      car_doors1 &= ~0x18;    // Clear charge and pilot bits
      car_doors1bits.ChargePort = 1;  //MJ
      car_chargemode = 0;     // Standard charge mode
      car_chargestate = 4;    // Charge DONE
      car_chargesubstate = 3; // Leave it as is
      net_req_notification(NET_NOTIFY_CHARGE);  // Charge done Message MJ
      mi_charge_timer = 0;       // Reset the per-second charge timer
      mi_charge_wm = 0;          // Reset the per-minute watt accumulator
      net_req_notification(NET_NOTIFY_STAT);
      }
    car_doors5bits.Charging12V = 0;  // MJ
    }

  else if ((car_chargecurrent == 0) && (car_linevoltage < 100) && (mi_quick_charge == 0))
    { // CAN says the car is not charging
    if (car_doors1 & 0x08)
      { // Charge has completed / stopped
      car_doors1 &= ~0x18;    // Clear charge and pilot bits
      car_doors1bits.ChargePort = 1;  //MJ
      car_chargemode = 0;     // Standard charge mode
      if (car_SOC < 95)
        { // Assume charge was interrupted
        car_chargestate = 21;    // Charge STOPPED
        car_chargesubstate = 14; // Charge INTERRUPTED
        net_req_notification(NET_NOTIFY_CHARGE);
        }
      else
        { // Assume charge completed normally
        car_chargestate = 4;    // Charge DONE
        car_chargesubstate = 3; // Leave it as is
        net_req_notification(NET_NOTIFY_CHARGE);  // Charge done Message MJ
        }
      mi_charge_timer = 0;       // Reset the per-second charge timer
      mi_charge_wm = 0;          // Reset the per-minute watt accumulator
      net_req_notification(NET_NOTIFY_STAT);
      }
    car_doors5bits.Charging12V = 0;  // MJ
    car_doors1bits.ChargePort = 0;   // Charging cable unplugged, charging door closed.
    }

  return FALSE;
  }


////////////////////////////////////////////////////////////////////////
// vehicle_mitsubishi_ticker10()
// State Model: 10 second ticker
// This function is called approximately once every 10 seconds (since state
// was first entered), and gives the state a timeslice for activity.
//

BOOL vehicle_mitsubishi_ticker10(void)
  {
  ////////////////////////////////////////////////////////////////////////
  // Battery temperature
  ////////////////////////////////////////////////////////////////////////
  
  int i;
  signed int tbattery = 0;

  for(i=0; i<24; i++)
    {
    tbattery += mi_batttemps[i];
    }
  car_tbattery = tbattery / 24;

  return FALSE;
  }

////////////////////////////////////////////////////////////////////////
// can_poll()
// This function is an entry point from the main() program loop, and
// gives the CAN framework an opportunity to poll for data.
//
BOOL vehicle_mitsubishi_poll0(void)
  {
  mi_candata_timer = 60;  // Reset the timer
  
  switch (can_id)
    {
    
    case 0x346: //Range
      {
      mi_estrange = (unsigned int)can_databuffer[7];
      
      // Quick charging indicated by range = 255.
      // To filter out false 255 messages we need 3 subsequent 255 messages to indicate QC. 
  
      if ((mi_estrange == 255) && (car_speed < 5))
        {
        if (mi_qc_counter > 0) mi_qc_counter--;
          
        if (mi_qc_counter == 0)
          {
          mi_quick_charge = 1;
          mi_stale_charge = 30; // Reset stale charging indicator
          }
        }

      else
        {
        if (mi_qc_counter < 3) mi_qc_counter++;
 
        else 
          {
          mi_quick_charge = 0;
          }
        }
        
      break;
      }
    
    /*
    case 0x373:
      // BatCurr & BatVolt
    break;
    */

    case 0x374: //SOC
      {
      car_SOC = (unsigned char)(((unsigned int)can_databuffer[1] - 10) / 2); 
      break;
      }

    case 0x389: //Charge voltage & current
      {
      car_linevoltage = (unsigned char)can_databuffer[1];
      car_chargecurrent = (unsigned char)((unsigned int)can_databuffer[6] / 10);
      mi_stale_charge = 30; // Reset stale charging indicator
      break;
      }
    }

  return TRUE;
  }

BOOL vehicle_mitsubishi_poll1(void)
  {
  mi_candata_timer = 60;  // Reset the timer
  
  switch (can_id)
    {
    
    case 0x285:
      {
      if (can_databuffer[6] == 0x0C) // Car in park
        {
        car_doors1 |= 0x40;     //  PARK
        car_doors1 &= ~0x80;    // CAR OFF
        if (car_parktime == 0)
          {
          car_parktime = car_time-1;    // Record it as 1 second ago, so non zero report
          net_req_notification(NET_NOTIFY_ENV);
          }
        }
      
      else if (can_databuffer[6] == 0x0E) // Car not in park
        {
        car_doors1 &= ~0x40;     //  NOT PARK
        car_doors1 |= 0x80;      // CAR ON
        if (car_parktime != 0)
          {
          car_parktime = 0; // No longer parking
          net_req_notification(NET_NOTIFY_ENV);
          }
        }
      break;
      }

    case 0x286: // Charger temp + 40C?
      {
      car_tpem = (signed char)can_databuffer[3] - 40;
      car_stale_temps = 60; // Reset stale indicator
      break;
      }

    case 0x298: // Motor temp + 40C?
      {
      car_tmotor = (unsigned char)can_databuffer[3] - 40;
      car_stale_temps = 60; // Reset stale indicator
      break;
      }

    case 0x412: //Speed & Odo
      {
      if (can_databuffer[1] > 200) //Speed
        {
        car_speed = (unsigned char)((unsigned int)can_databuffer[1] - 255);
        }
      else
        {
        car_speed = (unsigned char) can_databuffer[1];
        }

      if (can_mileskm == 'K') // Odo
        {
        car_odometer = MiFromKm((((unsigned long) can_databuffer[2] << 16) + ((unsigned long) can_databuffer[3] << 8) + can_databuffer[4]) * 10);
        }
      else
        {
        car_odometer = ((((unsigned long) can_databuffer[2] << 16) + ((unsigned long) can_databuffer[3] << 8) + can_databuffer[4]) * 10);
        }
      break;
      }
    
    case 0x6e1: 
      {
       // Calculate average battery pack temperature based on 24 of the 64 temperature values
       // Message 0x6e1 carries two temperatures for each of 12 banks, bank number (1..12) in byte 0,
       // temperatures in bytes 2 and 3, offset by 50C
       int idx = can_databuffer[0];
       if((idx >= 1) && (idx <= 12))
         {
         idx = ((idx << 1)-2);
         mi_batttemps[idx] = (signed char)(can_databuffer[2] - 50);
         mi_batttemps[idx + 1] = (signed char)(can_databuffer[3] - 50);
         car_stale_temps = 60; // Reset stale indicator
         }
      break;
      }
    }
  
  return TRUE;
  }


////////////////////////////////////////////////////////////////////////
// vehicle_mitsubishi_initialise()
// This function is an entry point from the main() program loop, and
// gives the CAN framework an opportunity to initialise itself.
//
BOOL vehicle_mitsubishi_initialise(void)
  {
  char *p;
  int i;
  
  car_type[0] = 'M'; // Car is type MI - Mitsubishi iMiev
  car_type[1] = 'I';
  car_type[2] = 0;
  car_type[3] = 0;
  car_type[4] = 0;

  // Vehicle specific data initialisation
  car_stale_timer = -1; // Timed charging is not supported for OVMS MI
  car_time = 0;
  mi_candata_timer = 0;
  car_SOCalertlimit = 10;
  mi_stale_charge = 0;  
  mi_quick_charge = 0;
  mi_qc_counter = 3;
  mi_estrange = 0;
  
   // Clear the battery temperatures
  for(i=0; i<24; i++) mi_batttemps[i] = 0;

  CANCON = 0b10010000; // Initialize CAN
  while (!CANSTATbits.OPMODE2); // Wait for Configuration mode

  // We are now in Configuration Mode

  // Filters: low byte bits 7..5 are the low 3 bits of the filter, and bits 4..0 are all zeros
  //          high byte bits 7..0 are the high 8 bits of the filter
  //          So total is 11 bits

  // Buffer 0 (filters 0, 1) for extended PID responses
  RXB0CON = 0b00000000;
  // Mask0 = 0b11110000000 (0x700), filterbit 0-7 deactivated
  RXM0SIDL = 0b00000000;
  RXM0SIDH = 0b11100000;

// Filter0 0b01101000000 (0x340..0x347 to 0x370..0x377 - includes 0x346, 0x373 and 0x374)
  RXF0SIDL = 0b00000000;
  RXF0SIDH = 0b01101000;
  
  // Filter1 0b01110001001 (0x389)
  RXF1SIDL = 0b00100000;
  RXF1SIDH = 0b01110001;


  // Buffer 1 (filters 2, 3, 4 and 5) for direct can bus messages
  RXB1CON  = 0b00000000;   // RX buffer1 uses Mask RXM1 and filters RXF2, RXF3, RXF4, RXF5

  // Mask1 = 0b11111111100 (0x7FC)
  // Note: We deactivate bits 0 and 1 to group 0x285 and 0x286 into the same buffer
  RXM1SIDL = 0b10000000;
  RXM1SIDH = 0b11111111;
  
  // Filter2 0b01010000101 (0x285 & 0x286)
  RXF2SIDL = 0b10000000;
  RXF2SIDH = 0b01010000;

  // Filter3 0b10000010010 (0x412)
  RXF3SIDL = 0b01000000;
  RXF3SIDH = 0b10000010;

  // Filter4 0b11011100001 (0x6E1)
  // Sample the first of the battery values
  RXF4SIDL = 0b00100000;
  RXF4SIDH = 0b11011100;
  
  // Filter5 0b01010011000 (0x298)
  RXF5SIDL = 0b00000000;
  RXF5SIDH = 0b01010011;
  

  // CAN bus baud rate

  BRGCON1 = 0x01; // SET BAUDRATE to 500 Kbps
  BRGCON2 = 0xD2;
  BRGCON3 = 0x02;

  CIOCON = 0b00100000; // CANTX pin will drive VDD when recessive
  if (sys_features[FEATURE_CANWRITE]>0)
    {
    CANCON = 0b00000000;  // Normal mode
    }
  else
    {
    CANCON = 0b01100000; // Listen only mode, Receive bufer 0
    }

  // Hook in...
  vehicle_fn_poll0 = &vehicle_mitsubishi_poll0;
  vehicle_fn_poll1 = &vehicle_mitsubishi_poll1;
  vehicle_fn_ticker1 = &vehicle_mitsubishi_ticker1;
  vehicle_fn_ticker10 = &vehicle_mitsubishi_ticker10;

  net_fnbits |= NET_FN_INTERNALGPS;   // Require internal GPS
  net_fnbits |= NET_FN_12VMONITOR;    // Require 12v monitor

  return TRUE;
  }
