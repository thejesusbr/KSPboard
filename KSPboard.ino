//////////////////////////////////////////
/// Justin Lee <justin at taiz dot me>
//
// Arduino driver for the KSPboard game
// controller.  See LICENSE and README
// for more information.
//
// License: GNU GPLv3 or above
// Copyright: Justin Lee 2017-2018




/////////////
/// libraries
//


#include <Wire.h>
#include <avr/pgmspace.h>



////////////////////////////////////
/// pin configurations and constants
//


#define DEBUG 1 // comment out to disable debug

#define ENABLE true
#define DISABLE false
#define NEUTRAL 2 // the natural extension to HIGH and LOW ;-)

// high byte / low byte macros
#define HI(Q) ((uint8_t) ((0xFF00 & Q) >> 8))
#define LO(Q) ((uint8_t) (0x00FF & Q))

// serial
#define SERIAL_SPEED 115200

// state pins
#define PIN_LED 13 // status
#define PIN_ENABLE A1 // this should be high to run

// helm pins, matrix pins, and shorthand for helm masks
#define PIN_HELM_MAT_DIM  3
#define PIN_HELM_MAT_ROW  4
#define PIN_HELM_MAT_ROWS (PIN_HELM_MAT_ROW + PIN_HELM_MAT_DIM)
#define PIN_HELM_MAT_COL  7
#define PIN_HELM_MAT_COLS (PIN_HELM_MAT_COL + PIN_HELM_MAT_DIM)

#define MASKED_PITCH_U helm & 0b100000000
#define MASKED_PITCH_D helm & 0b010000000
#define MASKED_PITCH_S helm & 0b001000000
#define MASKED_ROLL_R  helm & 0b000100000
#define MASKED_ROLL_L  helm & 0b000010000
#define MASKED_ROLL_S  helm & 0b000001000
#define MASKED_YAW_R   helm & 0b000000100
#define MASKED_YAW_L   helm & 0b000000010
#define MASKED_YAW_S   helm & 0b000000001

#define PIN_THROTTLE A0 // analog read

// rotary pins
#define PIN_ROT_CTRL_CLK 2
#define PIN_ROT_CTRL_DATA A2
#define PIN_ROT_CTRL_SW A3 // when you push the rotary switch in
#define PIN_ROT_CTRL_EN A6 // sas on / off

// i2c addresses / stuff
#define I2C_SUCCESS 0
#define IO_ADDR_BASE 0x20
// also note the wire library uses pins A4, A5

#define IO_OPS_ADDR 0
#define IO_OPS_LENGTH 2

// delays
#define DELAY_LOOP 10 // these are milliseconds
#define DELAY_DEBUG 250
#define DELAY_START 30
#define DELAY_CTRL 100
#define DELAY_OP 100

// misc
#define OPS 16 // Action 1-10, stage, gear, light, rcs, brake, abort

// helm, with velocity ramping
#define RAMP_MAX 100
#define RAMP_MIN -RAMP_MAX
#define DIR_MAX 6000
#define DIR_MIN -DIR_MAX



///////////
/// globals
//


// helm control variables
// convert to structure for storage savings?
int16_t pitch, yaw, roll, throttle;
int8_t pitchAdjust = 0, yawAdjust = 0, rollAdjust = 0; // for ramping the direction input

// sas control variable
volatile uint8_t _vol_rotaryControl = 0; // 0-255 for conversion to sas states
uint8_t controlState = 0, controlLocked = 0; // 0-7 for all sas states
uint8_t controlDebounce = 0; // debounce for the jittery rotary control switch
boolean controlEnable = false; // for the sas on / off switch

// op control variables
// state == 0 to DELAY_OP-1 -> off / debounce, state == DELAY_OP -> on
// Basically if this is not equal to DELAY_OP then it is in debounce state
// or off (zero).  When it is equal to DELAY_OP then it fires the op code out
// over serial. This allows multi switch debounce in a convenient package.
uint8_t opDebounce[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
uint16_t opState = 0; // holds current switch state

// this controls whether the python script closes or not, 1 == run
boolean state = 1;



/////////////////////
//// helper functions
//


// interrupt for the SAS rotary dial & helper
void rotaryInterruptHandler()
{
  if (digitalRead(PIN_ROT_CTRL_CLK))
    _vol_rotaryControl += digitalRead(PIN_ROT_CTRL_DATA) ? 1 : -1; // cw
  else
    _vol_rotaryControl -= digitalRead(PIN_ROT_CTRL_DATA) ? 1 : -1; // ccw
}

uint8_t rotaryControl2State(uint8_t c) // this should be moved to python client
{
  return ((uint8_t)((6.0 * (float)c) / (float)(UINT8_MAX - 1)));
}

// i/o expander helpers
boolean i2cRequestDevice(uint8_t address) // ping the device / send start command
{
  Wire.beginTransmission(IO_ADDR_BASE | address);
  return (Wire.endTransmission() == I2C_SUCCESS); // returns success / fail only
}

boolean i2cRequestData(uint8_t address, uint8_t len)
{
  return (Wire.requestFrom(IO_ADDR_BASE | address, len) == len); // returns success fail
}

// get the data bytes and stuff them into a 16-bit unsigned value
// call more than once if you have more than 2 bytes
uint16_t i2cReadData()
{
  return (~(Wire.read() | (Wire.read() << 8)));
}

// direction helpers
uint16_t helmReadMatrix()
{
  uint16_t matrix = 0;
  uint8_t row = 0, col = 0;

  // read the 3x3 multiplexed switch matrix
  for (row = PIN_HELM_MAT_ROW; row < PIN_HELM_MAT_ROWS; row++)
  {
    digitalWrite(row, LOW); // turn on the active-low row

    for (col = PIN_HELM_MAT_COL; col < PIN_HELM_MAT_COLS; col++)
    {
      // get inverted result (matrix is active low) and shift it onto the matrix
      matrix = (matrix << 1) | !digitalRead(col); // wish there was <<=
    }

    digitalWrite(row, HIGH); // turn off the active-low row
  }

  return (matrix);
}

// get control input and set the new adjust value
int16_t helmGetAdjustment(boolean high, boolean low, int8_t adj)
{
  switch (high ? HIGH : (low ? LOW : NEUTRAL)) // high trumps low
  {
    case HIGH: // pitch up / roll right / yaw right
      if (adj++ > RAMP_MAX)
        adj = RAMP_MAX;
      break;

    case LOW: // pitch down / roll left / yaw left
      if (adj-- < RAMP_MIN)
        adj = RAMP_MIN;
      break;

    default:
    case NEUTRAL: // stuck in the middle with you
      adj = 0;
  }

  return (adj);
}

// takes the current dir vector, the adjust value, and the sticky flag and gets the new dir
int16_t helmGetDirection(int16_t dir, int8_t adj, boolean stick)
{
  // reset current if no adj&stick flag or if adj and current are opposite signs
  if (!(adj || stick) || (dir > 0 && adj < 0) || (dir < 0 && adj > 0))
  {
    dir = 0; // reset current direction to neutral
  }
  else
  {
    dir += adj; // adjust current direction

    if (dir > DIR_MAX) // keep the current direction in bounds
      dir = DIR_MAX;
    else if (dir < DIR_MIN)
      dir = DIR_MIN;
  }

  return (dir);
}

uint16_t getThrottle(uint8_t pin) // get throttle input
{
  return (analogRead(pin)); // this used to do more...now it doesn't
}



////////////////////
//// setup fucntions
//


void setupSerial()
{
  // start up serial interface
  Serial.begin(SERIAL_SPEED);
  Serial.println();
  Serial.flush();

  // serial enable code, super secure
  Serial.print(1);
  Serial.print(1);
  Serial.print(1);
}

void setupControl()
{
  // when you click the dial
  pinMode(PIN_ROT_CTRL_SW, INPUT_PULLUP);

  // sas on/off switch
  pinMode(PIN_ROT_CTRL_EN, INPUT_PULLUP);

  // set the data line as input as well (has internal pull up)
  pinMode(PIN_ROT_CTRL_DATA, INPUT);

  // the rotary encoder uses two wire differential signalling
  attachInterrupt(
    digitalPinToInterrupt(PIN_ROT_CTRL_CLK),
    rotaryInterruptHandler,
    FALLING);
}

void setupState()
{
  pinMode(PIN_LED, OUTPUT); // debug led

  pinMode(PIN_ENABLE, INPUT_PULLUP);
}

void setupHelm()
{
  // rows are output, and cols are input
  uint8_t index = 0;

  // here we setup the input_pullup pinMode for the cols
  for (index = PIN_HELM_MAT_COL; index < PIN_HELM_MAT_COLS; index++)
  {
    pinMode(index, INPUT_PULLUP); // active low matrix
  }

  // then pull these up to 5V so current cannot flow
  for (index = PIN_HELM_MAT_ROW; index < PIN_HELM_MAT_ROWS; index++)
  {
    pinMode(index, OUTPUT);
    digitalWrite(index, HIGH);
  }
}

void setupOps()
{
  // start up I/O expander(s)
  Wire.begin();

  // ping the expanders to make sure they are operational
}



/////////////////////
//// update fucntions
//


void updateControl()
{
  boolean newControlEnable = !digitalRead(PIN_ROT_CTRL_EN); // update active low switch

  // if it was on and now its off
  if (controlEnable && !newControlEnable)
  {
    Serial.println(DISABLE); // send disable sequence
  }
  else if (controlEnable || newControlEnable)
  {
    uint8_t roter = 0; // setup space

    noInterrupts(); // shutdown interrupts for this volatile operation
    roter = _vol_rotaryControl; // copy the volatile control variable
    interrupts(); // reenable interrupts immediately

    uint8_t newState = rotaryControl2State(roter); // convert to sas number

    if (newState != controlState)
    {
      controlState = newState; // update the hilighted state

      // do hiligting stuff //
      // here the currently hovered state will be a blinking
      // led, there should be a timer started now that if not
      // dealt with by pressing the dial button resets the
      // controlState variable back to what is actually selected
    }

    if (controlDebounce > 0)
    {
      controlDebounce--; // do dat sweet digi debounce
    }
    else
    {
      // if you press dis switch lock the sas control state (active low)
      if (!digitalRead(PIN_ROT_CTRL_SW))
      {
        controlLocked = controlState; // update locked variable

        Serial.println(ENABLE); // send enable
        Serial.println(controlLocked); // send new sas state

        controlDebounce = DELAY_CTRL; // reset delay
      }
    }
  }

  controlEnable = newControlEnable; // finally save the new state for next time around

  Serial.println(DISABLE); // skip or end transmission
}

void updateState()
{
  // tells the python client when to close
  state = digitalRead(PIN_ENABLE);
  digitalWrite(PIN_LED, state);
  Serial.println(state);
}

void updateHelm()
{
  // get the matrix state used in the pitch/roll/yaw masks below
  uint16_t helm = helmReadMatrix();

  // update control surface inputs
  // the MASKED_ contants have "helm & DIR_MASK_x" format for convenience
  pitchAdjust = helmGetAdjustment(MASKED_PITCH_U, MASKED_PITCH_D, pitchAdjust);
  pitch = helmGetDirection(pitch, pitchAdjust, MASKED_PITCH_S);
  Serial.println(pitch);

  rollAdjust = helmGetAdjustment(MASKED_ROLL_R, MASKED_ROLL_L, rollAdjust);
  roll = helmGetDirection(roll, rollAdjust, MASKED_ROLL_S);
  Serial.println(roll);

  yawAdjust = helmGetAdjustment(MASKED_YAW_R, MASKED_YAW_L, yawAdjust);
  yaw = helmGetDirection(yaw, yawAdjust, MASKED_YAW_S);
  Serial.println(yaw);

  // update our current throttle input level
  throttle = getThrottle(PIN_THROTTLE);
  Serial.println(throttle);
}

void updateOps()
{
  uint8_t ops = 0;
  uint8_t index = 0;

  if (i2cRequestDevice(IO_OPS_ADDR)) // send start signal, skip if not success
  {
    if (i2cRequestData(IO_OPS_ADDR, IO_OPS_LENGTH)) // ask for data, skip if failure
    {
      uint16_t data = i2cReadData(); // finally get the 2 byte data packet

      for (index = 0; index < OPS; index++) // scan through ops 1-16 (everything but sas)
      {
        if (opDebounce[index] > 0) // check for debounce on current pin
        {
          opDebounce[index]--; // decrement debounce counter for current state
        }
        else // if not we can check to see if it's triggered
        {
          uint16_t opMask = 1 << index; // get and save the current mask

          // scan through 0b0000000000000001 to 0b1000000000000000 masks
          // if that switch is 1 then set it's state to DELAY_OP to flag
          // it to be sent out over serial, otherwise set it to zero
          opDebounce[index] =  (opMask & data) ? DELAY_OP : 0; // DELAY_OP signals state change

          if (opState & opMask) // if it's on turn it off
            opState &= ~opMask;
          else                  // or vice versa
            opState |= opMask;

          // increase the operations counter
          ops++;
        }
      }
    }
  }

  if (ops > 0) // if we have at least one operation to send
  {
    Serial.println(ENABLE); // send ops enable because we have at least one op

    for (index = 0; index < OPS; index++)
    {
      // for each state that's flagged...
      if (opDebounce[index] == DELAY_OP)
      {
        Serial.println(index); // print it
        Serial.println((boolean)(opState & (1 << index))); // print it's state
      }
    }
  }

  Serial.println(DISABLE); // send skip / end tramission flag
}



///////////////////////////////
//// built-in arduino functions
//


void setup()
{
  setupSerial();

  setupControl();
  setupState();
  setupHelm();
  setupOps();

  delay(DELAY_START);
}

void loop()
{
  updateState();

  if (state)
  {
    updateControl();
    updateHelm();
    updateOps();
  }

#ifndef DEBUG
  delay(DELAY_LOOP);
#else
  delay(DELAY_DEBUG);
  Serial.println(); // separates the data packets with a newline
#endif

  Serial.flush();
}

